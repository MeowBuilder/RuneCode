#include "stdafx.h"
#include "RockFallAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "RenderComponent.h"
#include "ColliderComponent.h"
#include "PlayerComponent.h"
#include "Room.h"
#include "Scene.h"
#include "Camera.h"
#include "Shader.h"
#include "Mesh.h"
#include "MeshLoader.h"
#include "MapLoader.h"
#include "Dx12App.h"
#include "AnimationComponent.h"
#include <functional>

// 공유 Ring / Disc 메시 — 첫 Execute 에서 lazy init, 전역에서 재사용
static RingMesh* s_pRockIndicatorRing = nullptr;
static RingMesh* s_pRockIndicatorDisc = nullptr;

static RingMesh* GetOrCreateIndicatorRing(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd)
{
    if (!s_pRockIndicatorRing)
        s_pRockIndicatorRing = new RingMesh(pDevice, pCmd, 1.0f, 0.90f, 32);
    return s_pRockIndicatorRing;
}
static RingMesh* GetOrCreateIndicatorDisc(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd)
{
    if (!s_pRockIndicatorDisc)
        s_pRockIndicatorDisc = new RingMesh(pDevice, pCmd, 1.0f, 0.0f, 32);
    return s_pRockIndicatorDisc;
}

RockFallAttackBehavior::RockFallAttackBehavior(int nRockCount, float fDamagePerRock,
                                               float fRockAoeRadius,
                                               float fSpawnMinRadius, float fSpawnMaxRadius,
                                               float fWindupTime, float fDropDuration, float fRecoveryTime,
                                               float fCameraShakeIntensity, float fCameraShakeDuration,
                                               const char* pClipOverride)
    : m_nRockCount(nRockCount)
    , m_fDamagePerRock(fDamagePerRock)
    , m_fRockAoeRadius(fRockAoeRadius)
    , m_fSpawnMinRadius(fSpawnMinRadius)
    , m_fSpawnMaxRadius(fSpawnMaxRadius)
    , m_fWindupTime(fWindupTime)
    , m_fDropDuration(fDropDuration)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fCameraShakeIntensity(fCameraShakeIntensity)
    , m_fCameraShakeDuration(fCameraShakeDuration)
    , m_strClipOverride(pClipOverride)
{
}

void RockFallAttackBehavior::SetNetworkEffectData(const std::vector<DirectX::XMFLOAT3>& positions, uint32 seed)
{
    // 1. 서버가 계산한 낙석 착지 위치를 저장한다.
    m_vNetworkLandingPositions = positions;

    // 2. 서버가 보내준 seed를 저장한다.
    m_uNetworkSeed = seed;

    // 3. 위치 목록이 하나라도 있으면 네트워크 위치 사용 모드로 전환한다.
    m_bUseNetworkLandingPositions = !m_vNetworkLandingPositions.empty();
}

void RockFallAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();
    if (!pEnemy) return;

    m_pRoom = pEnemy->GetRoom();
    if (!m_pRoom) return;
    m_pScene = m_pRoom->GetScene();
    if (!m_pScene) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;

    XMFLOAT3 bossPos = pOwner->GetTransform()->GetPosition();

    // 착지 위치 결정 — 4인 멀티 전장 전체 area denial
    //   무작위 각도 + 무작위 반경 (min~max) → 방 전역에 분산
    //   외곽 가중치 부여 (sqrt 분포) 로 플레이어가 주로 머무는 원거리 쪽에 더 많은 바위
    m_vRocks.clear();
    m_vRocks.reserve(m_nRockCount);

    float radiusRange = m_fSpawnMaxRadius - m_fSpawnMinRadius;

    // 착지 위치 결정
// 온라인 모드에서 서버가 effectPositions를 보내준 경우:
//   1. 서버 위치를 그대로 사용한다.
//   2. 클라별 rand() 위치 생성을 하지 않는다.
//   3. 회전/스케일/아치 높이는 서버 seed 기반으로 계산해서 모든 클라가 동일하게 보이도록 한다.
//
// 오프라인 또는 서버 위치가 없는 경우:
//   기존처럼 클라에서 rand()로 위치와 연출값을 만든다.
    m_vRocks.clear();

    auto Rand01FromSeed = [](uint32& seed) -> float
        {
            // XorShift 기반 간단한 결정론적 난수
            // 같은 seed와 같은 호출 순서라면 모든 클라에서 같은 값이 나온다.
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;

            return (seed & 0x00FFFFFF) / static_cast<float>(0x01000000);
        };

    auto RandRangeFromSeed = [&](uint32& seed, float minV, float maxV) -> float
        {
            return minV + (maxV - minV) * Rand01FromSeed(seed);
        };

    if (m_bUseNetworkLandingPositions && !m_vNetworkLandingPositions.empty())
    {
        // 1. 서버가 보내준 낙석 위치 사용
        m_vRocks.reserve(m_vNetworkLandingPositions.size());

        // 2. seed가 0이면 XorShift가 계속 0이 될 수 있으므로 기본 seed를 사용한다.
        uint32 seed = (m_uNetworkSeed != 0) ? m_uNetworkSeed : 0x9E3779B9u;

        for (const auto& serverPos : m_vNetworkLandingPositions)
        {
            RockInstance rock;

            // 3. 착지 위치는 서버 위치 그대로 사용한다.
            rock.landingPos = serverPos;
            rock.landingPos.y = 0.0f;

            // 4. 시작 위치는 착지 지점 바로 위로 설정한다.
            rock.skyStartPos.x = rock.landingPos.x;
            rock.skyStartPos.y = rock.landingPos.y + 18.0f;
            rock.skyStartPos.z = rock.landingPos.z;

            // 5. 회전/스케일/아치 높이는 seed 기반으로 생성한다.
            //    모든 클라가 같은 seed와 같은 순서로 계산하므로 결과가 동일하다.
            rock.initialRotation = {
                RandRangeFromSeed(seed, 0.0f, 360.0f),
                RandRangeFromSeed(seed, 0.0f, 360.0f),
                RandRangeFromSeed(seed, 0.0f, 360.0f)
            };

            rock.rotationSpeed = {
                RandRangeFromSeed(seed, -280.0f, 280.0f),
                RandRangeFromSeed(seed, -180.0f, 180.0f),
                RandRangeFromSeed(seed, -280.0f, 280.0f)
            };

            rock.scaleMultiplier = RandRangeFromSeed(seed, 0.8f, 1.2f);
            rock.archHeight = RandRangeFromSeed(seed, 5.0f, 12.0f);

            m_vRocks.push_back(rock);
        }
    }
    else
    {
        // 오프라인/서버 데이터 없음
        // 기존 방식 유지: 클라에서 rand()로 낙석 위치와 연출값을 생성한다.
        m_vRocks.reserve(m_nRockCount);

        float radiusRange = m_fSpawnMaxRadius - m_fSpawnMinRadius;

        for (int i = 0; i < m_nRockCount; ++i)
        {
            // 1. 각도: 완전 랜덤
            float angle = ((float)rand() / RAND_MAX) * XM_2PI;

            // 2. 반경: sqrt(t) 분포
            float t = (float)rand() / RAND_MAX;
            float radius = m_fSpawnMinRadius + sqrtf(t) * radiusRange;

            RockInstance rock;

            // 3. 착지 위치
            rock.landingPos.x = bossPos.x + cosf(angle) * radius;
            rock.landingPos.y = 0.0f;
            rock.landingPos.z = bossPos.z + sinf(angle) * radius;

            // 4. 시작 위치
            rock.skyStartPos.x = rock.landingPos.x;
            rock.skyStartPos.y = rock.landingPos.y + 18.0f;
            rock.skyStartPos.z = rock.landingPos.z;

            auto RandRange = [](float minV, float maxV)
                {
                    return minV + (maxV - minV) * ((float)rand() / RAND_MAX);
                };

            // 5. 개별 랜덤 연출값
            rock.initialRotation = {
                RandRange(0.0f, 360.0f),
                RandRange(0.0f, 360.0f),
                RandRange(0.0f, 360.0f)
            };

            rock.rotationSpeed = {
                RandRange(-280.0f, 280.0f),
                RandRange(-180.0f, 180.0f),
                RandRange(-280.0f, 280.0f)
            };

            rock.scaleMultiplier = RandRange(0.8f, 1.2f);
            rock.archHeight = RandRange(5.0f, 12.0f);

            m_vRocks.push_back(rock);
        }
    }

    SpawnIndicators(pEnemy);

    m_ePhase = Phase::Windup;
}

void RockFallAttackBehavior::SpawnIndicators(EnemyComponent* pEnemy)
{
    if (!m_pScene || !m_pRoom) return;

    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmdList = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmdList || !pShader) return;

    RingMesh* pRingMesh = GetOrCreateIndicatorRing(pDevice, pCmdList);
    RingMesh* pDiscMesh = GetOrCreateIndicatorDisc(pDevice, pCmdList);

    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    for (auto& rock : m_vRocks)
    {
        // 테두리 링
        GameObject* pRing = m_pScene->CreateGameObject(pDevice, pCmdList);
        if (pRing)
        {
            auto* pT = pRing->GetTransform();
            pT->SetPosition(rock.landingPos.x, 0.15f, rock.landingPos.z);
            pT->SetScale(m_fRockAoeRadius, 1.0f, m_fRockAoeRadius);

            pRing->SetMesh(pRingMesh);
            pRingMesh->AddRef();

            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.5f, 0.02f, 0.02f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.2f, 0.1f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = XMFLOAT4(2.0f, 0.3f, 0.1f, 1.0f);
            pRing->SetMaterial(mat);

            auto* pRC = pRing->AddComponent<RenderComponent>();
            pRC->SetMesh(pRingMesh);
            pRC->SetOverlay(true);  // 바닥에 겹쳐 그려지게
            pShader->AddRenderComponent(pRC);
            pRing->SetDecal(true);   // 셰이더 indicator path 활성화
            rock.pIndicator = pRing;
        }

        // 내부 차오름 Fill
        GameObject* pFill = m_pScene->CreateGameObject(pDevice, pCmdList);
        if (pFill)
        {
            auto* pT = pFill->GetTransform();
            pT->SetPosition(rock.landingPos.x, 0.10f, rock.landingPos.z);
            pT->SetScale(0.01f, 1.0f, 0.01f);  // 0 시작

            pFill->SetMesh(pDiscMesh);
            pDiscMesh->AddRef();

            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.3f, 0.02f, 0.0f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.35f, 0.05f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = XMFLOAT4(1.3f, 0.5f, 0.08f, 1.0f);
            pFill->SetMaterial(mat);

            auto* pRC = pFill->AddComponent<RenderComponent>();
            pRC->SetMesh(pDiscMesh);
            pRC->SetOverlay(true);
            pShader->AddRenderComponent(pRC);
            pFill->SetDecal(true);   // 셰이더 indicator path 활성화
            rock.pIndicatorFill = pFill;
        }
    }

    m_pScene->SetCurrentRoom(pPrevRoom);
}

void RockFallAttackBehavior::SpawnRocks(EnemyComponent* pEnemy)
{
    if (!m_pScene || !m_pRoom) return;

    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;

    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmdList = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmdList || !pShader) return;

    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    for (auto& rock : m_vRocks)
    {
        GameObject* pRock = MeshLoader::LoadGeometryFromFile(
            m_pScene, pDevice, pCmdList, nullptr,
            "Assets/Enemies/Rock&Golem/SM_Rocks_03.bin");
        if (!pRock) continue;

        {
            auto* pT = pRock->GetTransform();
            pT->SetPosition(rock.skyStartPos);
            // 초기 자세 적용 (바위마다 다른 각도)
            pT->SetRotation(rock.initialRotation);
            // 스케일 — 임팩트 체감 + 바위마다 ±20% 변동
            float scale = m_fRockAoeRadius * 0.8f * rock.scaleMultiplier;
            pT->SetScale(scale, scale, scale);

            // 재질 (회색 돌 느낌)
            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.28f, 0.25f, 0.22f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(0.55f, 0.48f, 0.42f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.1f, 0.1f, 0.1f, 16.0f);
            mat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            pRock->SetMaterial(mat);

            // Hierarchy render components 등록 (rock .bin 은 child 가 있을 수 있음)
            // 기존처럼 즉시 pShader에도 등록하고,
            // 이후 Scene::UpdateRenderList()가 다시 등록할 수 있도록 RenderComponent도 확실히 붙인다.
            std::function<void(GameObject*)> RegisterRender = [&](GameObject* p)
                {
                    if (!p) return;

                    if (p->GetMesh())
                    {
                        auto* pRC = p->GetComponent<RenderComponent>();
                        if (!pRC)
                            pRC = p->AddComponent<RenderComponent>();

                        pRC->SetMesh(p->GetMesh());
                        pRC->SetCastsShadow(true);

                        if (pShader)
                            pShader->AddRenderComponent(pRC);
                    }

                    if (p->m_pChild) RegisterRender(p->m_pChild);
                    if (p->m_pSibling) RegisterRender(p->m_pSibling);
                };

            RegisterRender(pRock);

            rock.pRock = pRock;
        }
    }

    m_pScene->SetCurrentRoom(pPrevRoom);
}

void RockFallAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;

    m_fTimer += dt;

    // 공격 애니 1회 재생 후 idle 로 자동 전환 (보스는 idle, 바위는 계속 진행)
    if (!m_bAnimReturnedToIdle && m_fTimer > 0.1f)
    {
        if (auto* pAnim = pEnemy->GetAnimationComponent())
        {
            if (!pAnim->IsPlaying())
            {
                pAnim->CrossFade("Golem_battle_stand_ge", 0.25f, true, true);
                m_bAnimReturnedToIdle = true;
            }
        }
    }

    switch (m_ePhase)
    {
    case Phase::Windup:
    {
        // 인디케이터 fill 차오름 (windup 동안 0 → m_fRockAoeRadius)
        float fillProgress = (std::min)(m_fTimer / m_fWindupTime, 1.0f);
        float fillR = m_fRockAoeRadius * fillProgress;
        if (fillR < 0.01f) fillR = 0.01f;
        for (auto& rock : m_vRocks)
        {
            if (rock.pIndicatorFill)
            {
                auto* pT = rock.pIndicatorFill->GetTransform();
                if (pT) pT->SetScale(fillR, 1.0f, fillR);
            }
        }

        if (m_fTimer >= m_fWindupTime)
        {
            SpawnRocks(pEnemy);
            m_ePhase = Phase::Drop;
            m_fTimer = 0.0f;
        }
        break;
    }

    case Phase::Drop:
    {
        UpdateRockFall(dt);

        if (m_fTimer >= m_fDropDuration)
        {
            // 착지 데미지 일괄 적용 + 카메라 쉐이크
            DealImpactDamage(pEnemy);

            if (m_fCameraShakeIntensity > 0.0f)
            {
                if (CCamera* pCam = m_pScene->GetCamera())
                    pCam->StartShake(m_fCameraShakeIntensity, m_fCameraShakeDuration);
            }

            // 인디케이터 제거 (착지 후엔 불필요)
            for (auto& rock : m_vRocks)
            {
                if (rock.pIndicator)     m_pScene->MarkForDeletion(rock.pIndicator);
                if (rock.pIndicatorFill) m_pScene->MarkForDeletion(rock.pIndicatorFill);
                rock.pIndicator = nullptr;
                rock.pIndicatorFill = nullptr;
            }

            m_ePhase = Phase::Recovery;
            m_fTimer = 0.0f;
        }
        break;
    }

    case Phase::Recovery:
        if (m_fTimer >= m_fRecoveryTime)
        {
            CleanupAll();
            m_bFinished = true;
        }
        break;
    }
}

void RockFallAttackBehavior::UpdateRockFall(float dt)
{
    float t = (std::min)(m_fTimer / m_fDropDuration, 1.0f);

    for (auto& rock : m_vRocks)
    {
        if (!rock.pRock) continue;
        auto* pT = rock.pRock->GetTransform();
        if (!pT) continue;

        // 포물선 흩뿌리기:
        //   XZ 는 선형 (보스 → 착지점으로 균등)
        //   Y 는 아치 — 던져올라갔다가 낙하. peak 높이 = start.y + arc
        XMFLOAT3 pos;
        pos.x = rock.skyStartPos.x + (rock.landingPos.x - rock.skyStartPos.x) * t;
        pos.z = rock.skyStartPos.z + (rock.landingPos.z - rock.skyStartPos.z) * t;

        // 수직: t=0 시작 높이, t=1 착지 (0). 중간에 arc 추가로 던지는 느낌
        //   arc 높이는 바위마다 다름 (rock.archHeight)
        float linearY = rock.skyStartPos.y + (rock.landingPos.y - rock.skyStartPos.y) * t;
        float arc = 4.0f * t * (1.0f - t) * rock.archHeight;
        pos.y = linearY + arc;
        pT->SetPosition(pos);

        // 낙하 중 회전 — 바위마다 개별 속도/방향
        XMFLOAT3 rot = pT->GetRotation();
        rot.x += rock.rotationSpeed.x * dt;
        rot.y += rock.rotationSpeed.y * dt;
        rot.z += rock.rotationSpeed.z * dt;
        pT->SetRotation(rot);
    }
}

void RockFallAttackBehavior::DealImpactDamage(EnemyComponent* pEnemy)
{
    if (!pEnemy || !m_pScene) return;

    // 멀티 플레이어 지원: 모든 플레이어에 대해 개별적으로 AOE 체크
    std::vector<GameObject*> vPlayers = m_pScene->GetAllPlayers();

    for (GameObject* pPlayerObj : vPlayers)
    {
        if (!pPlayerObj) continue;
        auto* pPT = pPlayerObj->GetTransform();
        if (!pPT) continue;
        PlayerComponent* pPlayer = pPlayerObj->GetComponent<PlayerComponent>();
        if (!pPlayer) continue;

        XMFLOAT3 tp = pPT->GetPosition();

        bool bHitAny = false;
        for (auto& rock : m_vRocks)
        {
            float dx = tp.x - rock.landingPos.x;
            float dz = tp.z - rock.landingPos.z;
            float dist = sqrtf(dx * dx + dz * dz);
            if (dist <= m_fRockAoeRadius)
            {
                bHitAny = true;
                rock.bImpacted = true;
            }
        }

        // 동일 플레이어가 여러 바위에 동시 맞아도 단일 데미지 (중복 타격 방지)
        if (bHitAny)
            pPlayer->TakeDamage(m_fDamagePerRock);
    }
}

void RockFallAttackBehavior::CleanupAll()
{
    if (!m_pScene) return;

    for (auto& rock : m_vRocks)
    {
        if (rock.pRock)          m_pScene->MarkForDeletion(rock.pRock);
        if (rock.pIndicator)     m_pScene->MarkForDeletion(rock.pIndicator);
        if (rock.pIndicatorFill) m_pScene->MarkForDeletion(rock.pIndicatorFill);
    }
    m_vRocks.clear();
}

bool RockFallAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void RockFallAttackBehavior::Reset()
{
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
    m_bFinished = false;
    // 이전 공격의 바위가 남아있으면 정리
    CleanupAll();
}
