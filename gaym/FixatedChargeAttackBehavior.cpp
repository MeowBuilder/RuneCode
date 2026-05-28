#include "stdafx.h"
#include "FixatedChargeAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"
#include "AnimationComponent.h"
#include "MathUtils.h"
#include "Dx12App.h"
#include "Scene.h"
#include "Camera.h"
#include "VFXManager.h"
#include "NetworkManager.h"
#include <algorithm>

namespace {
    CCamera* GetActiveCamera()
    {
        if (auto* pApp = Dx12App::GetInstance())
            if (auto* pScene = pApp->GetScene())
                return pScene->GetCamera();
        return nullptr;
    }
    VFXManager* GetVFX()
    {
        if (auto* pApp = Dx12App::GetInstance())
            if (auto* pScene = pApp->GetScene())
                return pScene->GetVFXManager();
        return nullptr;
    }
}

FixatedChargeAttackBehavior::FixatedChargeAttackBehavior(
    float fDamage, float fTelegraphTime, float fLockedAimTime,
    float fDashSpeed, float fMaxDashDistance,
    float fPlayerHitRadius, float fPillarHitRadius, float fIndicatorHalfW,
    float fGroggyDuration, float fNormalRecovery)
    : m_fDamage(fDamage)
    , m_fTelegraphTime(fTelegraphTime)
    , m_fLockedAimTime(fLockedAimTime)
    , m_fDashSpeed(fDashSpeed)
    , m_fMaxDashDistance(fMaxDashDistance)
    , m_fPlayerHitRadius(fPlayerHitRadius)
    , m_fPillarHitRadius(fPillarHitRadius)
    , m_fIndicatorHalfW(fIndicatorHalfW)
    , m_fGroggyDuration(fGroggyDuration)
    , m_fNormalRecovery(fNormalRecovery)
{
}

int FixatedChargeAttackBehavior::GetIndicatorTypeOverride() const
{
    return static_cast<int>(IndicatorType::ForwardBox);
}

void FixatedChargeAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();
    if (!pEnemy) return;

    // Escalation: miss 가 누적될수록 텔레그래프 짧아지고 dash 속도 빨라짐 (3 회까지 강화)
    int nMisses = (std::min)(pEnemy->GetMissedFixatedCharges(), 3);
    m_fTelegraphTime = (std::max)(m_fTelegraphTime - 0.5f * nMisses, 1.2f);
    m_fDashSpeed    += 6.0f * nMisses;

    pEnemy->FaceTarget();   // 시작 즉시 한 번 즉발 회전
}

void FixatedChargeAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;

    m_fPhaseTimer += dt;

    switch (m_ePhase)
    {
    case Phase::Telegraph:
    {
        // 매 프레임 부드럽게 타겟 추적 → 보스 yaw 가 갱신되며 ForwardBox 인디케이터도 따라감
        pEnemy->FaceTarget(dt, false);

        // 카메라 pull-back — 드라마틱한 telegraph 표현
        if (auto* pCam = GetActiveCamera()) pCam->SetExtraOrbitDistanceTarget(28.0f);

        if (m_fPhaseTimer >= m_fTelegraphTime)
        {
            m_ePhase = Phase::LockedAim;
            m_fPhaseTimer = 0.0f;
        }
        break;
    }

    case Phase::LockedAim:
        // 직전 텔레그래프 종료 시점의 보스 yaw 를 그대로 유지 — 이미 멈춰있음
        if (m_fPhaseTimer >= m_fLockedAimTime)
        {
            StartDash(pEnemy);
        }
        break;

    case Phase::Dash:
    {
        GameObject* pOwner = pEnemy->GetOwner();
        if (!pOwner) { EnterMissRecovery(pEnemy); break; }
        TransformComponent* pT = pOwner->GetTransform();
        if (!pT)     { EnterMissRecovery(pEnemy); break; }

        float moveAmt = m_fDashSpeed * dt;
        XMFLOAT3 pos = pT->GetPosition();
        pos.x += m_xmf3DashDir.x * moveAmt;
        pos.z += m_xmf3DashDir.z * moveAmt;

        // 온라인 모드에서는 서버 S_MONSTER_MOVE가 보스 위치를 갱신한다.
        NetworkManager* pNet = NetworkManager::GetInstance();
        bool bOnline = (pNet && pNet->IsConnected());

        if (!bOnline)
        {
            pT->SetPosition(pos);
        }

        m_fDashTraveled += moveAmt;

        // 좌/우 트레일 위치 갱신 (보스 따라 ± perpOffset)
        if (auto* pVFX = GetVFX())
        {
            const float kOffset = 4.5f;
            XMFLOAT3 bp = pos;
            bp.y += 1.0f;
            XMFLOAT3 perpRight( m_xmf3DashDir.z, 0.0f, -m_xmf3DashDir.x);
            XMFLOAT3 trailDir (-m_xmf3DashDir.x, 0.0f, -m_xmf3DashDir.z);
            XMFLOAT3 leftPos { bp.x - perpRight.x * kOffset, bp.y, bp.z - perpRight.z * kOffset };
            XMFLOAT3 rightPos{ bp.x + perpRight.x * kOffset, bp.y, bp.z + perpRight.z * kOffset };
            if (m_nWindVFXIdL >= 0) pVFX->Track(m_nWindVFXIdL, leftPos,  trailDir);
            if (m_nWindVFXIdR >= 0) pVFX->Track(m_nWindVFXIdR, rightPos, trailDir);
        }

        // 충돌 우선순위: 기둥 > 플레이어 (기둥에 박는 게 핵심 기믹이므로)
        if (TryHitPillar(pEnemy))  { EnterGroggy(pEnemy); break; }
        if (TryHitPlayer(pEnemy))  { EnterPlayerHitRecovery(pEnemy); break; }

        if (m_fDashTraveled >= m_fMaxDashDistance)
        {
            EnterMissRecovery(pEnemy);
        }
        break;
    }

    case Phase::Recovery:
        if (m_fPhaseTimer >= m_fRecoveryDuration)
        {
            m_bFinished = true;
        }
        break;
    }
}

void FixatedChargeAttackBehavior::Reset()
{
    m_ePhase = Phase::Telegraph;
    m_fPhaseTimer = 0.0f;
    m_fRecoveryDuration = 0.0f;
    m_fDashTraveled = 0.0f;
    m_bFinished = false;
    m_xmf3DashDir = XMFLOAT3(0.0f, 0.0f, 0.0f);

    // 잔존 VFX 슬롯이 있다면 정리
    if (auto* pVFX = GetVFX())
    {
        if (m_nWindVFXIdL >= 0) { pVFX->Stop(m_nWindVFXIdL); m_nWindVFXIdL = -1; }
        if (m_nWindVFXIdR >= 0) { pVFX->Stop(m_nWindVFXIdR); m_nWindVFXIdR = -1; }
    }

    // 안전장치: 카메라 pull-back 가 남아있을 수 있으니 0 으로 리셋 신호
    if (auto* pCam = GetActiveCamera()) pCam->SetExtraOrbitDistanceTarget(0.0f);
}

void FixatedChargeAttackBehavior::StartDash(EnemyComponent* pEnemy)
{
    m_ePhase = Phase::Dash;
    m_fPhaseTimer = 0.0f;

    // 보스의 현재 yaw 로부터 방향 락 (텔레그래프 종료 시점의 face 방향)
    GameObject* pOwner = pEnemy ? pEnemy->GetOwner() : nullptr;
    if (pOwner)
    {
        if (TransformComponent* pT = pOwner->GetTransform())
        {
            float yawRad = XMConvertToRadians(pT->GetRotation().y);
            m_xmf3DashDir = XMFLOAT3(sinf(yawRad), 0.0f, cosf(yawRad));

            // 좌/우 평행 트레일 — 핵심 기믹이라 Big 버전 사용
            //   spawn pos = bossPos ± perpendicular * offset, dir = 진행 반대
            if (auto* pVFX = GetVFX())
            {
                const float kOffset = 4.5f;
                XMFLOAT3 bp = pT->GetPosition();
                bp.y += 1.0f;
                XMFLOAT3 perpRight( m_xmf3DashDir.z, 0.0f, -m_xmf3DashDir.x);
                XMFLOAT3 trailDir (-m_xmf3DashDir.x, 0.0f, -m_xmf3DashDir.z);
                XMFLOAT3 leftPos { bp.x - perpRight.x * kOffset, bp.y, bp.z - perpRight.z * kOffset };
                XMFLOAT3 rightPos{ bp.x + perpRight.x * kOffset, bp.y, bp.z + perpRight.z * kOffset };
                m_nWindVFXIdL = pVFX->Spawn("Demon_RushWind_Big", leftPos,  trailDir, 0u, false);
                m_nWindVFXIdR = pVFX->Spawn("Demon_RushWind_Big", rightPos, trailDir, 0u, false);
            }
        }
    }

    // 카메라 효과: pull-back 해제(빠르게 복귀) + 가벼운 shake (출발 임팩트)
    if (auto* pCam = GetActiveCamera())
    {
        pCam->SetExtraOrbitDistanceTarget(0.0f);
        pCam->StartShake(0.6f, 0.25f);
    }
}

bool FixatedChargeAttackBehavior::TryHitPlayer(EnemyComponent* pEnemy)
{
    GameObject* pTarget = pEnemy->GetTarget();
    if (!pTarget) return false;
    if (pEnemy->GetDistanceToTarget() > m_fPlayerHitRadius) return false;

    if (PlayerComponent* pPC = pTarget->GetComponent<PlayerComponent>())
        pPC->TakeDamage(m_fDamage);
    return true;
}

bool FixatedChargeAttackBehavior::TryHitPillar(EnemyComponent* pEnemy)
{
    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return false;
    TransformComponent* pT = pOwner->GetTransform();
    if (!pT) return false;

    XMFLOAT3 myPos = pT->GetPosition();
    float r2 = m_fPillarHitRadius * m_fPillarHitRadius;

    for (GameObject* pPillar : pEnemy->GetEnvironmentObstacles())
    {
        if (!pPillar) continue;
        TransformComponent* pPT = pPillar->GetTransform();
        if (!pPT) continue;
        XMFLOAT3 p = pPT->GetPosition();
        float dx = p.x - myPos.x;
        float dz = p.z - myPos.z;
        if (dx * dx + dz * dz <= r2) return true;
    }
    return false;
}

void FixatedChargeAttackBehavior::EnterGroggy(EnemyComponent* pEnemy)
{
    // Dash 회오리 VFX 정리
    if (auto* pVFX = GetVFX())
    {
        if (m_nWindVFXIdL >= 0) { pVFX->Stop(m_nWindVFXIdL); m_nWindVFXIdL = -1; }
        if (m_nWindVFXIdR >= 0) { pVFX->Stop(m_nWindVFXIdR); m_nWindVFXIdR = -1; }
    }
    m_ePhase = Phase::Recovery;
    m_fPhaseTimer = 0.0f;
    m_fRecoveryDuration = m_fGroggyDuration;
    pEnemy->ResetMissedFixatedCharges();   // 기둥 박았으니 escalation 리셋

    // 그로기 애니: gethit3 (1.37s) 를 루프로 깔아서 정신 못차리는 모션
    if (GameObject* pOwner = pEnemy->GetOwner())
    {
        if (auto* pAnim = pOwner->GetComponent<AnimationComponent>())
        {
            pAnim->CrossFade("gethit3", 0.15f, true, true);
        }
    }

    // 강한 화면 흔들림 — 기둥 박음 임팩트
    if (auto* pCam = GetActiveCamera())
    {
        pCam->SetExtraOrbitDistanceTarget(0.0f);
        pCam->StartShake(1.6f, 0.55f);
    }
}

void FixatedChargeAttackBehavior::EnterPlayerHitRecovery(EnemyComponent* pEnemy)
{
    if (auto* pVFX = GetVFX())
    {
        if (m_nWindVFXIdL >= 0) { pVFX->Stop(m_nWindVFXIdL); m_nWindVFXIdL = -1; }
        if (m_nWindVFXIdR >= 0) { pVFX->Stop(m_nWindVFXIdR); m_nWindVFXIdR = -1; }
    }
    m_ePhase = Phase::Recovery;
    m_fPhaseTimer = 0.0f;
    m_fRecoveryDuration = m_fNormalRecovery;
    pEnemy->IncrementMissedFixatedCharges();   // 기둥 못 박았으면 다음 charge 강화

    // 플레이어 적중 임팩트 — 강한 shake
    if (auto* pCam = GetActiveCamera())
        pCam->StartShake(1.2f, 0.35f);
}

void FixatedChargeAttackBehavior::EnterMissRecovery(EnemyComponent* pEnemy)
{
    if (auto* pVFX = GetVFX())
    {
        if (m_nWindVFXIdL >= 0) { pVFX->Stop(m_nWindVFXIdL); m_nWindVFXIdL = -1; }
        if (m_nWindVFXIdR >= 0) { pVFX->Stop(m_nWindVFXIdR); m_nWindVFXIdR = -1; }
    }
    m_ePhase = Phase::Recovery;
    m_fPhaseTimer = 0.0f;
    m_fRecoveryDuration = m_fNormalRecovery;
    pEnemy->IncrementMissedFixatedCharges();
}
