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
                                               float fCameraShakeIntensity, float fCameraShakeDuration)
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
{
}

void RockFallAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();
    OutputDebugStringA("[RockFall] Execute called\n");
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

    for (int i = 0; i < m_nRockCount; ++i)
    {
        // 각도: 완전 랜덤 (특정 방향 치우침 없음)
        float angle = ((float)rand() / RAND_MAX) * XM_2PI;

        // 반경: sqrt(t) 분포 → 균등 면적 분포 (외곽일수록 자리가 많아 더 자주 나옴)
        float t = (float)rand() / RAND_MAX;
        float radius = m_fSpawnMinRadius + sqrtf(t) * radiusRange;

        RockInstance rock;
        rock.landingPos.x = bossPos.x + cosf(angle) * radius;
        rock.landingPos.y = 0.0f;
        rock.landingPos.z = bossPos.z + sinf(angle) * radius;
        // 시작 위치 = 착지 지점 바로 위
        rock.skyStartPos.x = rock.landingPos.x;
        rock.skyStartPos.y = rock.landingPos.y + 18.0f;
        rock.skyStartPos.z = rock.landingPos.z;

        // 개별 랜덤 — 바위마다 다른 모습 & 움직임
        auto RandRange = [](float minV, float maxV) {
            return minV + (maxV - minV) * ((float)rand() / RAND_MAX);
        };
        // 초기 자세 — 모든 축 0~360° 랜덤
        rock.initialRotation = {
            RandRange(0.0f, 360.0f),
            RandRange(0.0f, 360.0f),
            RandRange(0.0f, 360.0f)
        };
        // 회전 속도 — 축마다 다른 속도/방향 (deg/s)
        rock.rotationSpeed = {
            RandRange(-280.0f, 280.0f),
            RandRange(-180.0f, 180.0f),
            RandRange(-280.0f, 280.0f)
        };
        // 스케일 ±20% 변동
        rock.scaleMultiplier = RandRange(0.8f, 1.2f);
        // 아치 높이 ±3 변동 (어떤 바위는 낮은 커브, 어떤 건 높게)
        rock.archHeight = RandRange(5.0f, 12.0f);

        m_vRocks.push_back(rock);
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
            rock.pIndicatorFill = pFill;
        }
    }

    m_pScene->SetCurrentRoom(pPrevRoom);
}

void RockFallAttackBehavior::SpawnRocks(EnemyComponent* pEnemy)
{
    OutputDebugStringA("[RockFall] SpawnRocks called\n");

    if (!m_pScene || !m_pRoom)
    {
        OutputDebugStringA("[RockFall] SpawnRocks failed: scene or room null\n");
        return;
    }

    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp)
    {
        OutputDebugStringA("[RockFall] SpawnRocks failed: Dx12App null\n");
        return;
    }

    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmdList = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmdList || !pShader)
    {
        OutputDebugStringA("[RockFall] SpawnRocks failed: device/cmd/shader null\n");
        return;
    }

    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    for (auto& rock : m_vRocks)
    {
        GameObject* pRock = MeshLoader::LoadGeometryFromFile(
            m_pScene, pDevice, pCmdList, nullptr,
            "Assets/Enemies/Rock&Golem/SM_Rocks_03.bin");

        if (!pRock)
        {
            OutputDebugStringA("[RockFall] Rock mesh load failed\n");
            continue;
        }

        OutputDebugStringA("[RockFall] Rock mesh loaded success\n");

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
            OutputDebugStringA("[RockFall] Windup finished -> SpawnRocks\n");

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

    if (!m_vRocks.empty() && m_vRocks[0].pRock)
    {
        auto* pT0 = m_vRocks[0].pRock->GetTransform();
        if (pT0)
        {
            XMFLOAT3 p = pT0->GetPosition();

            char buf[160];
            sprintf_s(buf, "[RockFall] rock0 pos=(%.2f, %.2f, %.2f)\n", p.x, p.y, p.z);
            OutputDebugStringA(buf);
        }
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
