#include "stdafx.h"
#include "RockBarrageAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "RenderComponent.h"
#include "PlayerComponent.h"
#include "Room.h"
#include "Scene.h"
#include "Camera.h"
#include "Shader.h"
#include "Mesh.h"
#include "MeshLoader.h"
#include "Dx12App.h"
#include "AnimationComponent.h"
#include <functional>

RockBarrageAttackBehavior::RockBarrageAttackBehavior(
    int nRockCount, float fDamagePerRock, float fProjectileRadius, float fProjectileSpeed,
    float fOrbitRadius, float fOrbitHeight,
    float fSummonTime, float fChargeTime, float fFireInterval,
    float fFlightTimeout, float fRecoveryTime,
    float fCameraShakeIntensity, float fCameraShakeDuration,
    float fHomingStrength)
    : m_nRockCount(nRockCount)
    , m_fDamagePerRock(fDamagePerRock)
    , m_fProjectileRadius(fProjectileRadius)
    , m_fProjectileSpeed(fProjectileSpeed)
    , m_fOrbitRadius(fOrbitRadius)
    , m_fOrbitHeight(fOrbitHeight)
    , m_fSummonTime(fSummonTime)
    , m_fChargeTime(fChargeTime)
    , m_fFireInterval(fFireInterval)
    , m_fFlightTimeout(fFlightTimeout)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fCameraShakeIntensity(fCameraShakeIntensity)
    , m_fCameraShakeDuration(fCameraShakeDuration)
    , m_fHomingStrength(fHomingStrength)
{
}

void RockBarrageAttackBehavior::SetNetworkTarget(GameObject* pTargetPlayer)
{
    // 서버가 정한 targetPlayerId에 해당하는 클라 GameObject를 저장한다.
    m_pNetworkTargetPlayer = pTargetPlayer;
    m_bUseNetworkTarget = (m_pNetworkTargetPlayer != nullptr);
}

void RockBarrageAttackBehavior::SetNetworkEffectSeed(uint32 seed)
{
    // 바위 회전/스케일 같은 시각적 랜덤값 동기화용 seed
    m_uNetworkSeed = seed;
    m_bUseNetworkSeed = true;
}

void RockBarrageAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();
    if (!pEnemy) return;

    m_pRoom = pEnemy->GetRoom();
    if (!m_pRoom) return;
    m_pScene = m_pRoom->GetScene();
    if (!m_pScene) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;

    m_xmf3BossCenter = pOwner->GetTransform()->GetPosition();
    m_xmf3BossCenter.y += m_fOrbitHeight;  // 궤도는 보스 위쪽에

    // 바위 생성 + 초기 궤도 각도 균등 분배
    m_vRocks.clear();
    m_vRocks.reserve(m_nRockCount);

    auto RandRange = [](float a, float b) {
        return a + (b - a) * ((float)rand() / RAND_MAX);
    };

    for (int i = 0; i < m_nRockCount; ++i)
    {
        RockProjectile rock;
        rock.orbitAngle = ((float)i / (float)m_nRockCount) * XM_2PI;
        rock.fireDelay  = i * m_fFireInterval;  // 순차 발사
        rock.rotationSpeed = {
            RandRange(-240.0f, 240.0f),
            RandRange(-160.0f, 160.0f),
            RandRange(-240.0f, 240.0f)
        };
        rock.scaleMultiplier = RandRange(0.8f, 1.2f);
        m_vRocks.push_back(rock);
    }

    SpawnRocks(pEnemy);

    m_ePhase = Phase::Summon;
}

void RockBarrageAttackBehavior::SpawnRocks(EnemyComponent* pEnemy)
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

    for (size_t i = 0; i < m_vRocks.size(); ++i)
    {
        auto& rock = m_vRocks[i];

        // SM_Rocks_01 과 02 번갈아 사용 (외관 다양성)
        const char* pMeshPath = (i % 2 == 0)
            ? "Assets/Enemies/Rock&Golem/SM_Rocks_01.bin"
            : "Assets/Enemies/Rock&Golem/SM_Rocks_02.bin";

        GameObject* pRock = MeshLoader::LoadGeometryFromFile(
            m_pScene, pDevice, pCmdList, nullptr, pMeshPath);

        if (pRock)
        {
            auto* pT = pRock->GetTransform();

            // 초기 위치 = 궤도 상 (보스 주변)
            XMFLOAT3 pos;
            pos.x = m_xmf3BossCenter.x + cosf(rock.orbitAngle) * m_fOrbitRadius;
            pos.y = m_xmf3BossCenter.y;
            pos.z = m_xmf3BossCenter.z + sinf(rock.orbitAngle) * m_fOrbitRadius;
            pT->SetPosition(pos);

            // 스케일 — 투사체 반경 × 1.9 (가시성↑) + 개별 ±20% 변동
            float baseScale = m_fProjectileRadius * 1.9f;
            float scale = baseScale * rock.scaleMultiplier;
            pT->SetScale(scale, scale, scale);

            // 초기 자세 랜덤
            pT->SetRotation(
                ((float)rand() / RAND_MAX) * 360.0f,
                ((float)rand() / RAND_MAX) * 360.0f,
                ((float)rand() / RAND_MAX) * 360.0f);

            // 재질 — 기본 돌 + 약한 붉은 emissive 로 "위협" 신호 + 실루엣 대비
            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.30f, 0.24f, 0.20f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(0.60f, 0.50f, 0.42f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.15f, 0.12f, 0.1f, 16.0f);
            mat.m_cEmissive = XMFLOAT4(0.35f, 0.12f, 0.05f, 1.0f);  // 은은한 붉은 발광
            pRock->SetMaterial(mat);

            // Hierarchy 탐색하며 RenderComponent 등록
            std::function<void(GameObject*)> RegisterRender = [&](GameObject* p) {
                if (!p) return;
                auto* pRC = p->GetComponent<RenderComponent>();
                if (!pRC && p->GetMesh()) {
                    pRC = p->AddComponent<RenderComponent>();
                    pRC->SetMesh(p->GetMesh());
                }
                if (pRC) pShader->AddRenderComponent(pRC);
                if (p->m_pChild) RegisterRender(p->m_pChild);
                if (p->m_pSibling) RegisterRender(p->m_pSibling);
            };
            RegisterRender(pRock);

            rock.pRock = pRock;
        }
    }

    m_pScene->SetCurrentRoom(pPrevRoom);
}

void RockBarrageAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;
    m_fTimer += dt;

    // 공격 애니 1회 재생 후 idle 전환
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
    case Phase::Summon:
        UpdateFloating(dt, pEnemy);
        if (m_fTimer >= m_fSummonTime)
        {
            m_ePhase = Phase::Charge;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::Charge:
        UpdateFloating(dt, pEnemy);
        // Charge 동안 바위들이 떨리며 급격히 커짐 (시각적 예고 강화)
        {
            float chargeT = m_fTimer / m_fChargeTime;
            // 떨림: 빠른 sin 진동 + 크기 증가 (가중 위협)
            float wobble = sinf(chargeT * 50.0f) * 0.12f;
            float growUp = 1.0f + chargeT * 0.25f;  // 차지 끝날 때 +25% 크기
            for (auto& rock : m_vRocks)
            {
                if (!rock.pRock) continue;
                auto* pT = rock.pRock->GetTransform();
                if (!pT) continue;
                float base = m_fProjectileRadius * 1.9f * rock.scaleMultiplier;
                float s = base * (1.0f + wobble) * growUp;
                pT->SetScale(s, s, s);
            }
        }
        if (m_fTimer >= m_fChargeTime)
        {
            AssignTargetsAndFire(pEnemy);
            m_ePhase = Phase::Fire;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::Fire:
        // Fire phase 진입 첫 프레임 — 전체 일제 사격 박력용 강한 쉐이크 1회
        if (m_fTimer < dt + 0.001f)
        {
            if (m_pScene)
            {
                if (CCamera* pCam = m_pScene->GetCamera())
                    pCam->StartShake(3.0f, 0.5f);  // 강하게, 짧지 않게
            }
        }
        UpdateFlying(dt, pEnemy);
        // 모든 바위가 Flying 되거나 Done 이 되면 Recovery 로 전환
        {
            bool allDone = true;
            for (auto& rock : m_vRocks)
            {
                if (rock.state == RockState::Floating) { allDone = false; break; }
                if (rock.state == RockState::Flying) { allDone = false; break; }
            }
            if (allDone || m_fTimer >= m_fFlightTimeout + (m_nRockCount * m_fFireInterval))
            {
                m_ePhase = Phase::Recovery;
                m_fTimer = 0.0f;
            }
        }
        break;

    case Phase::Recovery:
        if (m_fTimer >= m_fRecoveryTime)
        {
            CleanupAll();
            m_bFinished = true;
        }
        break;
    }
}

void RockBarrageAttackBehavior::UpdateFloating(float dt, EnemyComponent* pEnemy)
{
    // 궤도 회전 속도 — 전체 대형 한바퀴 약 8초
    const float orbitSpeed = XM_2PI / 8.0f;

    for (auto& rock : m_vRocks)
    {
        if (!rock.pRock || rock.state != RockState::Floating) continue;
        auto* pT = rock.pRock->GetTransform();
        if (!pT) continue;

        rock.orbitAngle += orbitSpeed * dt;

        XMFLOAT3 pos;
        pos.x = m_xmf3BossCenter.x + cosf(rock.orbitAngle) * m_fOrbitRadius;
        pos.y = m_xmf3BossCenter.y;
        pos.z = m_xmf3BossCenter.z + sinf(rock.orbitAngle) * m_fOrbitRadius;
        pT->SetPosition(pos);

        // 자체 회전 (개별 속도)
        XMFLOAT3 rot = pT->GetRotation();
        rot.x += rock.rotationSpeed.x * dt;
        rot.y += rock.rotationSpeed.y * dt;
        rot.z += rock.rotationSpeed.z * dt;
        pT->SetRotation(rot);
    }
}

void RockBarrageAttackBehavior::AssignTargetsAndFire(EnemyComponent* pEnemy)
{
    if (!m_pScene) return;

    // 모든 플레이어 목록 — 멀티 플레이어 지원
    std::vector<GameObject*> vPlayers = m_pScene->GetAllPlayers();
    if (vPlayers.empty())
    {
        // 플레이어 없음 — 전부 앞 방향 발사 (fallback)
        for (auto& rock : m_vRocks)
        {
            rock.velocity = { 0.0f, 0.0f, m_fProjectileSpeed };
        }
        return;
    }

    // 각 바위가 랜덤 플레이어 타겟 (중복 허용)
    for (auto& rock : m_vRocks)
    {
        if (!rock.pRock) continue;
        auto* pT = rock.pRock->GetTransform();
        if (!pT) continue;

        // 온라인에서는 서버가 정한 targetPlayerId 대상만 사용한다.
        // 오프라인에서는 기존처럼 랜덤 플레이어를 선택한다.
        GameObject* pTarget = nullptr;

        if (m_bUseNetworkTarget && m_pNetworkTargetPlayer)
        {
            pTarget = m_pNetworkTargetPlayer;
        }
        else
        {
            int idx = rand() % (int)vPlayers.size();
            pTarget = vPlayers[idx];
        }

        if (!pTarget || !pTarget->GetTransform())
        {
            rock.velocity = { 0.0f, 0.0f, m_fProjectileSpeed };
            continue;
        }

        // 유도 대상 저장 — Flying 중 이 플레이어를 약한 유도로 따라감
        rock.pTargetPlayer = pTarget;

        XMFLOAT3 rockPos = pT->GetPosition();
        XMFLOAT3 targetPos = pTarget->GetTransform()->GetPosition();
        targetPos.y += 1.5f;  // 플레이어 몸통 중심 지향

        float dx = targetPos.x - rockPos.x;
        float dy = targetPos.y - rockPos.y;
        float dz = targetPos.z - rockPos.z;
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > 0.001f)
        {
            rock.velocity.x = (dx / len) * m_fProjectileSpeed;
            rock.velocity.y = (dy / len) * m_fProjectileSpeed;
            rock.velocity.z = (dz / len) * m_fProjectileSpeed;
        }
    }
}

void RockBarrageAttackBehavior::UpdateFlying(float dt, EnemyComponent* pEnemy)
{
    if (!m_pScene) return;

    std::vector<GameObject*> vPlayers = m_pScene->GetAllPlayers();

    for (auto& rock : m_vRocks)
    {
        if (!rock.pRock || rock.state == RockState::Done) continue;

        // 순차 발사 — fireDelay 아직 안 지났으면 Float 상태 유지
        if (rock.state == RockState::Floating)
        {
            if (m_fTimer >= rock.fireDelay)
            {
                rock.state = RockState::Flying;
                rock.flightTimer = 0.0f;
                // 개별 launch 쉐이크는 Fire 진입 한 번의 강한 쉐이크로 대체 (중복/마스킹 방지)
            }
            else
            {
                continue;  // 아직 대기
            }
        }

        // Flying — 위치 갱신
        if (rock.state == RockState::Flying)
        {
            auto* pT = rock.pRock->GetTransform();
            if (!pT) continue;

            XMFLOAT3 pos = pT->GetPosition();

            // ── 약한 유도 — 현재 velocity 방향을 타겟 방향으로 서서히 보간 ──
            //   m_fHomingStrength = 0 이면 완전 직선, 크면 빠르게 따라감
            //   타겟이 여전히 존재하고 비행 초반일 때만 활성 (너무 오래 따라가면 spiral)
            if (m_fHomingStrength > 0.0f && rock.pTargetPlayer && rock.flightTimer < 1.5f)
            {
                auto* pTP = rock.pTargetPlayer->GetTransform();
                if (pTP)
                {
                    XMFLOAT3 tp = pTP->GetPosition();
                    tp.y += 1.5f;
                    float dx = tp.x - pos.x;
                    float dy = tp.y - pos.y;
                    float dz = tp.z - pos.z;
                    float toLen = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (toLen > 0.001f)
                    {
                        float vLen = sqrtf(rock.velocity.x * rock.velocity.x
                                         + rock.velocity.y * rock.velocity.y
                                         + rock.velocity.z * rock.velocity.z);
                        if (vLen > 0.001f)
                        {
                            // 현재 방향
                            float curDx = rock.velocity.x / vLen;
                            float curDy = rock.velocity.y / vLen;
                            float curDz = rock.velocity.z / vLen;
                            // 타겟 방향
                            float tgtDx = dx / toLen;
                            float tgtDy = dy / toLen;
                            float tgtDz = dz / toLen;

                            // 보간율 — dt 기반 + strength
                            float k = m_fHomingStrength * dt;
                            if (k > 1.0f) k = 1.0f;
                            float nx = curDx + (tgtDx - curDx) * k;
                            float ny = curDy + (tgtDy - curDy) * k;
                            float nz = curDz + (tgtDz - curDz) * k;
                            // 재정규화 → 속도 유지
                            float newLen = sqrtf(nx * nx + ny * ny + nz * nz);
                            if (newLen > 0.001f)
                            {
                                rock.velocity.x = (nx / newLen) * m_fProjectileSpeed;
                                rock.velocity.y = (ny / newLen) * m_fProjectileSpeed;
                                rock.velocity.z = (nz / newLen) * m_fProjectileSpeed;
                            }
                        }
                    }
                }
            }

            pos.x += rock.velocity.x * dt;
            pos.y += rock.velocity.y * dt;
            pos.z += rock.velocity.z * dt;
            pT->SetPosition(pos);

            // 비행 중 회전 — 훨씬 빠르게 (박력)
            XMFLOAT3 rot = pT->GetRotation();
            rot.x += rock.rotationSpeed.x * 3.5f * dt;
            rot.y += rock.rotationSpeed.y * 3.5f * dt;
            rot.z += rock.rotationSpeed.z * 3.5f * dt;
            pT->SetRotation(rot);

            // Launch pulse — 발사 후 초반 0.12s 동안 scale 커졌다 원상복귀 (임팩트 체감)
            {
                float pulseT = rock.flightTimer / 0.12f;
                float pulse = (pulseT < 1.0f) ? (1.0f + (1.0f - pulseT) * 0.45f) : 1.0f;
                float base = m_fProjectileRadius * 1.9f * rock.scaleMultiplier;
                pT->SetScale(base * pulse, base * pulse, base * pulse);
            }

            rock.flightTimer += dt;

            // 플레이어 충돌 판정
            if (!rock.bHitDealt)
            {
                for (GameObject* pPlayerObj : vPlayers)
                {
                    if (!pPlayerObj) continue;
                    auto* pPT = pPlayerObj->GetTransform();
                    if (!pPT) continue;
                    PlayerComponent* pPlayer = pPlayerObj->GetComponent<PlayerComponent>();
                    if (!pPlayer) continue;

                    XMFLOAT3 pp = pPT->GetPosition();
                    pp.y += 1.5f;  // 몸통 중심
                    float dx = pos.x - pp.x;
                    float dy = pos.y - pp.y;
                    float dz = pos.z - pp.z;
                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (dist <= m_fProjectileRadius + 1.0f)  // 플레이어 반경 ~1
                    {
                        pPlayer->TakeDamage(m_fDamagePerRock);
                        rock.bHitDealt = true;
                        rock.state = RockState::Done;
                        break;
                    }
                }
            }

            // 타임아웃 — 너무 오래 날아가면 종료
            if (rock.flightTimer >= m_fFlightTimeout)
            {
                rock.state = RockState::Done;
            }
        }
    }
}

void RockBarrageAttackBehavior::CleanupAll()
{
    if (!m_pScene) return;

    for (auto& rock : m_vRocks)
    {
        if (rock.pRock)
            m_pScene->MarkForDeletion(rock.pRock);
    }
    m_vRocks.clear();
}

bool RockBarrageAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void RockBarrageAttackBehavior::Reset()
{
    m_ePhase = Phase::Summon;
    m_fTimer = 0.0f;
    m_bFinished = false;
    CleanupAll();
}
