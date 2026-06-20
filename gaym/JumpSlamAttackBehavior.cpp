#include "stdafx.h"
#include "JumpSlamAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"
#include "AnimationComponent.h"
#include "Room.h"
#include "Scene.h"
#include "Camera.h"
#include "RenderComponent.h"
#include "Shader.h"
#include "MeshLoader.h"
#include "Dx12App.h"
#include <functional>

JumpSlamAttackBehavior::JumpSlamAttackBehavior(float fDamage,
                                               float fJumpHeight,
                                               float fJumpDuration,
                                               float fSlamRadius,
                                               float fWindupTime,
                                               float fRecoveryTime,
                                               bool bTrackTarget,
                                               float fCameraShakeIntensity,
                                               float fCameraShakeDuration,
                                               const char* pClipOverride,
                                               float fAnimPlaybackSpeed)
    : m_fDamage(fDamage)
    , m_fJumpHeight(fJumpHeight)
    , m_fJumpDuration(fJumpDuration)
    , m_fSlamRadius(fSlamRadius)
    , m_fWindupTime(fWindupTime)
    , m_fRecoveryTime(fRecoveryTime)
    , m_bTrackTarget(bTrackTarget)
    , m_fCameraShakeIntensity(fCameraShakeIntensity)
    , m_fCameraShakeDuration(fCameraShakeDuration)
    , m_strClipOverride(pClipOverride)
    , m_fAnimPlaybackSpeed(fAnimPlaybackSpeed)
{
}

void JumpSlamAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();

    if (pEnemy)
    {
        GameObject* pOwner = pEnemy->GetOwner();
        if (pOwner && pOwner->GetTransform())
        {
            XMFLOAT3 pos = pOwner->GetTransform()->GetPosition();
            m_fOriginalY = pos.y;
            m_xmf3StartPosition = pos;
        }

        // Set target position
        if (m_bTrackTarget)
        {
            GameObject* pTarget = pEnemy->GetTarget();
            if (pTarget && pTarget->GetTransform())
            {
                m_xmf3TargetPosition = pTarget->GetTransform()->GetPosition();
                m_xmf3TargetPosition.y = m_fOriginalY;  // Keep same Y level
            }
            else
            {
                m_xmf3TargetPosition = m_xmf3StartPosition;
            }
        }
        else
        {
            m_xmf3TargetPosition = m_xmf3StartPosition;
        }

        // Face target before jumping
        pEnemy->FaceTarget();

        AnimationComponent* pAnimComp = pEnemy->GetAnimationComponent();
        if (pAnimComp)
        {
            pAnimComp->CrossFade("Take Off", 0.1f, false);
            // 애니 재생속도 override
            if (m_fAnimPlaybackSpeed > 0.0f)
                pAnimComp->SetPlaybackSpeed(m_fAnimPlaybackSpeed);
        }
    }

    m_ePhase = Phase::Windup;
}

void JumpSlamAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;

    TransformComponent* pTransform = pOwner->GetTransform();
    if (!pTransform) return;

    m_fTimer += dt;

    switch (m_ePhase)
    {
    case Phase::Windup:
        if (m_fTimer >= m_fWindupTime)
        {
            m_ePhase = Phase::Jump;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::Jump:
        {
            float t = m_fTimer / m_fJumpDuration;
            if (t > 1.0f) t = 1.0f;

            // Parabolic jump arc
            float heightT = 4.0f * t * (1.0f - t);  // Peaks at t=0.5
            float newY = m_fOriginalY + m_fJumpHeight * heightT;

            // Lerp horizontal position
            XMFLOAT3 pos;
            pos.x = m_xmf3StartPosition.x + (m_xmf3TargetPosition.x - m_xmf3StartPosition.x) * t;
            pos.y = newY;
            pos.z = m_xmf3StartPosition.z + (m_xmf3TargetPosition.z - m_xmf3StartPosition.z) * t;
            pTransform->SetPosition(pos);

            if (m_fTimer >= m_fJumpDuration)
            {
                m_ePhase = Phase::Slam;
                m_fTimer = 0.0f;

                // Snap to ground at target
                pos.y = m_fOriginalY;
                pTransform->SetPosition(pos);

                AnimationComponent* pAnimComp = pEnemy->GetAnimationComponent();
                if (pAnimComp)
                {
                    pAnimComp->CrossFade("Land", 0.05f, false);
                }

                // Slam impact 카메라 쉐이크 (강도 > 0 일 때만)
                if (m_fCameraShakeIntensity > 0.0f && pEnemy->GetRoom())
                {
                    if (Scene* pScene = pEnemy->GetRoom()->GetScene())
                        if (CCamera* pCam = pScene->GetCamera())
                            pCam->StartShake(m_fCameraShakeIntensity, m_fCameraShakeDuration);
                }
            }
        }
        break;

    case Phase::Slam:
        {
            // Deal damage + 파편 비산 (첫 프레임 1회)
            if (!m_bSlamDealt)
            {
                DealSlamDamage(pEnemy);
                SpawnDebris(pEnemy);
                m_bSlamDealt = true;
            }

            UpdateDebris(dt);

            // Short slam animation
            if (m_fTimer >= 0.2f)
            {
                m_ePhase = Phase::Recovery;
                m_fTimer = 0.0f;
            }
        }
        break;

    case Phase::Recovery:
        UpdateDebris(dt);  // 회복 동안에도 파편 계속 움직임
        if (m_fTimer >= m_fRecoveryTime)
        {
            CleanupDebris();
            m_bFinished = true;
        }
        break;
    }
}

bool JumpSlamAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void JumpSlamAttackBehavior::Reset()
{
    CleanupDebris();
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
    m_fOriginalY = 0.0f;
    m_xmf3StartPosition = { 0.0f, 0.0f, 0.0f };
    m_xmf3TargetPosition = { 0.0f, 0.0f, 0.0f };
    m_bSlamDealt = false;
    m_bFinished = false;
}

void JumpSlamAttackBehavior::DealSlamDamage(EnemyComponent* pEnemy)
{
    if (!pEnemy) return;
    // 네트워크 모드: 서버 권위 데미지. 클라는 VFX/인디케이터/카메라 쉐이크만.
    if (m_bNetworkVisualOnly) return;

    GameObject* pTarget = pEnemy->GetTarget();
    if (!pTarget) return;

    // Check distance from slam point to target
    float distance = pEnemy->GetDistanceToTarget();

    if (distance > m_fSlamRadius) return;

    // Deal damage to player
    PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>();
    if (pPlayer)
    {
        // Damage scales with proximity (closer = more damage)
        float damageMultiplier = 1.0f - (distance / m_fSlamRadius) * 0.5f;  // 50% - 100% damage
        float actualDamage = m_fDamage * damageMultiplier;

        pPlayer->TakeDamage(actualDamage);
    }
}

// ── 바위 파편 비산 (slam 임팩트 시 VFX) ────────────────────────────────────
void JumpSlamAttackBehavior::SpawnDebris(EnemyComponent* pEnemy)
{
    if (!pEnemy) return;
    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;
    CRoom* pRoom = pEnemy->GetRoom();
    if (!pRoom) return;
    Scene* pScene = pRoom->GetScene();
    if (!pScene) return;
    m_pDebrisScene = pScene;   // Cleanup 시 사용할 Scene 참조 저장
    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    // ── 내려찍기 주먹 위치 계산 ──
    //   보스의 yaw 기준 forward × N 지점이 주먹 내려친 곳
    //   점프 진동 (jumpHeight > 0) 은 보스 정중앙이 착지점이므로 offset 없음
    XMFLOAT3 bossPos = pOwner->GetTransform()->GetPosition();
    XMFLOAT3 bossRot = pOwner->GetTransform()->GetRotation();
    float yawRad = bossRot.y * (XM_PI / 180.0f);
    float fwdX = sinf(yawRad);
    float fwdZ = cosf(yawRad);

    // 보스 스케일 14 → 몸통 반경이 큼. forward 6 은 여전히 몸 내부.
    // 점프는 보스 발 주변 (offset 0 but 높은 시작 y)
    // 내려찍기는 forward 12 (보스 몸 밖으로 확실히)
    float fwdOffset = (m_fJumpHeight > 0.1f) ? 0.0f : 12.0f;
    XMFLOAT3 center;
    center.x = bossPos.x + fwdX * fwdOffset;
    center.y = 0.0f;
    center.z = bossPos.z + fwdZ * fwdOffset;

    auto RandRange = [](float a, float b) {
        return a + (b - a) * ((float)rand() / RAND_MAX);
    };

    // 파편 개수 — 슬램 반경에 비례, 기본값 많이 확대
    int nDebris = (int)(12 + m_fSlamRadius / 6.0f);
    if (nDebris > 24) nDebris = 24;

    CRoom* pPrevRoom = pScene->GetCurrentRoom();
    pScene->SetCurrentRoom(pRoom);

    for (int i = 0; i < nDebris; ++i)
    {
        // 작은 바위 — SM_Rocks_01/02 번갈아
        const char* pMeshPath = (i % 2 == 0)
            ? "Assets/Enemies/Rock&Golem/SM_Rocks_01.bin"
            : "Assets/Enemies/Rock&Golem/SM_Rocks_02.bin";

        GameObject* pGO = MeshLoader::LoadGeometryFromFile(
            pScene, pDevice, pCmd, nullptr, pMeshPath);
        if (!pGO) continue;

        auto* pT = pGO->GetTransform();

        // 초기 위치 — 주먹 impact 지점에 tight scatter
        //   반경 1~5 (주먹 반경). y 0.5~2.5 (주먹 접촉면 주변)
        XMFLOAT3 pos;
        float startR = RandRange(1.0f, 5.0f);
        float startAng = RandRange(0.0f, XM_2PI);
        pos.x = center.x + cosf(startAng) * startR;
        pos.y = RandRange(0.5f, 2.5f);
        pos.z = center.z + sinf(startAng) * startR;
        pT->SetPosition(pos);

        // 스케일 — 작게 (파편 느낌)
        float scale = RandRange(0.8f, 1.6f);
        pT->SetScale(scale, scale, scale);

        // 초기 자세 랜덤
        pT->SetRotation(
            RandRange(0.0f, 360.0f),
            RandRange(0.0f, 360.0f),
            RandRange(0.0f, 360.0f));

        // 재질 (돌)
        MATERIAL mat;
        mat.m_cAmbient  = XMFLOAT4(0.25f, 0.22f, 0.20f, 1.0f);
        mat.m_cDiffuse  = XMFLOAT4(0.55f, 0.48f, 0.42f, 1.0f);
        mat.m_cSpecular = XMFLOAT4(0.1f, 0.1f, 0.1f, 12.0f);
        mat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        pGO->SetMaterial(mat);

        // Hierarchy 탐색하며 RenderComponent 등록
        std::function<void(GameObject*)> Reg = [&](GameObject* p) {
            if (!p) return;
            auto* pRC = p->GetComponent<RenderComponent>();
            if (!pRC && p->GetMesh()) {
                pRC = p->AddComponent<RenderComponent>();
                pRC->SetMesh(p->GetMesh());
            }
            if (pRC) pShader->AddRenderComponent(pRC);
            if (p->m_pChild) Reg(p->m_pChild);
            if (p->m_pSibling) Reg(p->m_pSibling);
        };
        Reg(pGO);

        // 비산 속도 — 바깥쪽 + 위쪽
        DebrisPiece piece;
        piece.pObj = pGO;
        float outAng = startAng + RandRange(-0.4f, 0.4f);
        float outSpeed = RandRange(8.0f, 16.0f);
        piece.velocity.x = cosf(outAng) * outSpeed;
        piece.velocity.y = RandRange(7.0f, 12.0f);   // 위로 튐
        piece.velocity.z = sinf(outAng) * outSpeed;

        // 뒹구는 회전
        piece.rotSpeed.x = RandRange(-540.0f, 540.0f);
        piece.rotSpeed.y = RandRange(-360.0f, 360.0f);
        piece.rotSpeed.z = RandRange(-540.0f, 540.0f);

        piece.lifetime = RandRange(1.1f, 1.8f);
        piece.age = 0.0f;

        m_vDebris.push_back(piece);
    }

    pScene->SetCurrentRoom(pPrevRoom);
}

void JumpSlamAttackBehavior::UpdateDebris(float dt)
{
    const float kGravity = 28.0f;  // 중력 가속도 (u/s²)
    const float kGroundY = 0.3f;   // 착지 판정 높이

    for (auto& p : m_vDebris)
    {
        if (!p.pObj) continue;
        auto* pT = p.pObj->GetTransform();
        if (!pT) continue;

        p.age += dt;

        // 위치 업데이트
        XMFLOAT3 pos = pT->GetPosition();
        pos.x += p.velocity.x * dt;
        pos.y += p.velocity.y * dt;
        pos.z += p.velocity.z * dt;

        // 중력
        p.velocity.y -= kGravity * dt;

        // 착지 — 튕기거나 그대로 멈춤
        if (pos.y < kGroundY)
        {
            pos.y = kGroundY;
            if (p.velocity.y < -2.0f)
            {
                // 낮은 bounce (에너지 70% 유지)
                p.velocity.y = -p.velocity.y * 0.3f;
                p.velocity.x *= 0.6f;
                p.velocity.z *= 0.6f;
                // 회전도 감쇠
                p.rotSpeed.x *= 0.5f;
                p.rotSpeed.y *= 0.5f;
                p.rotSpeed.z *= 0.5f;
            }
            else
            {
                // 거의 멈춤
                p.velocity.x *= 0.3f;
                p.velocity.y = 0.0f;
                p.velocity.z *= 0.3f;
            }
        }

        pT->SetPosition(pos);

        // 회전
        XMFLOAT3 rot = pT->GetRotation();
        rot.x += p.rotSpeed.x * dt;
        rot.y += p.rotSpeed.y * dt;
        rot.z += p.rotSpeed.z * dt;
        pT->SetRotation(rot);
    }
}

void JumpSlamAttackBehavior::CleanupDebris()
{
    for (auto& p : m_vDebris)
    {
        if (!p.pObj) continue;
        if (m_pDebrisScene)
            m_pDebrisScene->MarkForDeletion(p.pObj);
        else if (auto* pT = p.pObj->GetTransform())
            pT->SetPosition(0.0f, -1000.0f, 0.0f);  // fallback 숨김
    }
    m_vDebris.clear();
    m_pDebrisScene = nullptr;
}
