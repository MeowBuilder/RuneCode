#include "stdafx.h"
#include "SpinDashAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"
#include "MathUtils.h"
#include "Dx12App.h"
#include "Scene.h"
#include "VFXManager.h"

namespace {
    VFXManager* GetVFX()
    {
        if (auto* pApp = Dx12App::GetInstance())
            if (auto* pScene = pApp->GetScene())
                return pScene->GetVFXManager();
        return nullptr;
    }
}

SpinDashAttackBehavior::SpinDashAttackBehavior(float fTickDamage, float fTickInterval,
                                               float fRushSpeed, float fRushDuration,
                                               float fWindupTime, float fRecoveryTime,
                                               float fAoERadius)
    : m_fTickDamage(fTickDamage)
    , m_fTickInterval(fTickInterval)
    , m_fRushSpeed(fRushSpeed)
    , m_fRushDuration(fRushDuration)
    , m_fWindupTime(fWindupTime)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fAoERadius(fAoERadius)
{
}

int SpinDashAttackBehavior::GetIndicatorTypeOverride() const
{
    return static_cast<int>(IndicatorType::ForwardBox);
}

void SpinDashAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();
    if (!pEnemy) return;

    // 시작 시점에 타겟 방향으로 돌진 방향 잠금
    pEnemy->FaceTarget();

    GameObject* pOwner  = pEnemy->GetOwner();
    GameObject* pTarget = pEnemy->GetTarget();
    if (pOwner && pTarget)
    {
        TransformComponent* pMyT  = pOwner->GetTransform();
        TransformComponent* pTgtT = pTarget->GetTransform();
        if (pMyT && pTgtT)
        {
            XMFLOAT2 dir = MathUtils::Direction2D(pMyT->GetPosition(), pTgtT->GetPosition());
            m_xmf3RushDirection = XMFLOAT3(dir.x, 0.0f, dir.y);
        }
    }

    m_ePhase = Phase::Windup;

    // 회오리 + 부스터 VFX 동시 스폰
    //   - Demon_Tornado: column 중앙, 회전 컨셉 강조
    //   - Demon_RushWind: 진행 방향 반대로 깔리는 부스터 트레일 (전진 속도감)
    if (auto* pVFX = GetVFX())
    {
        if (GameObject* pOwner = pEnemy->GetOwner())
        {
            if (TransformComponent* pT = pOwner->GetTransform())
            {
                XMFLOAT3 base = pT->GetPosition();
                XMFLOAT3 tornadoPos = base;
                tornadoPos.y += 0.5f;
                m_nWindVFXId = pVFX->Spawn("Demon_Tornado", tornadoPos, XMFLOAT3(0.0f, 1.0f, 0.0f), 0u, false);

                const float kOffset = 4.0f;
                XMFLOAT3 bp = base;
                bp.y += 1.0f;
                XMFLOAT3 perpRight( m_xmf3RushDirection.z, 0.0f, -m_xmf3RushDirection.x);
                XMFLOAT3 trailDir (-m_xmf3RushDirection.x, 0.0f, -m_xmf3RushDirection.z);
                XMFLOAT3 leftPos { bp.x - perpRight.x * kOffset, bp.y, bp.z - perpRight.z * kOffset };
                XMFLOAT3 rightPos{ bp.x + perpRight.x * kOffset, bp.y, bp.z + perpRight.z * kOffset };
                m_nBoosterVFXIdL = pVFX->Spawn("Demon_RushWind", leftPos,  trailDir, 0u, false);
                m_nBoosterVFXIdR = pVFX->Spawn("Demon_RushWind", rightPos, trailDir, 0u, false);
            }
        }
    }
}

void SpinDashAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;

    m_fTimer += dt;

    // 매 프레임 VFX 위치 추적 (회오리 + 부스터)
    if (auto* pVFX = GetVFX())
    {
        if (GameObject* pOwner = pEnemy->GetOwner())
        {
            if (TransformComponent* pT = pOwner->GetTransform())
            {
                XMFLOAT3 base = pT->GetPosition();
                if (m_nWindVFXId >= 0)
                {
                    XMFLOAT3 tp = base; tp.y += 0.5f;
                    pVFX->Track(m_nWindVFXId, tp, XMFLOAT3(0.0f, 1.0f, 0.0f));
                }
                const float kOffset = 4.0f;
                XMFLOAT3 bp = base; bp.y += 1.0f;
                XMFLOAT3 perpRight( m_xmf3RushDirection.z, 0.0f, -m_xmf3RushDirection.x);
                XMFLOAT3 trailDir (-m_xmf3RushDirection.x, 0.0f, -m_xmf3RushDirection.z);
                XMFLOAT3 leftPos { bp.x - perpRight.x * kOffset, bp.y, bp.z - perpRight.z * kOffset };
                XMFLOAT3 rightPos{ bp.x + perpRight.x * kOffset, bp.y, bp.z + perpRight.z * kOffset };
                if (m_nBoosterVFXIdL >= 0) pVFX->Track(m_nBoosterVFXIdL, leftPos,  trailDir);
                if (m_nBoosterVFXIdR >= 0) pVFX->Track(m_nBoosterVFXIdR, rightPos, trailDir);
            }
        }
    }

    switch (m_ePhase)
    {
    case Phase::Windup:
        if (m_fTimer >= m_fWindupTime)
        {
            m_ePhase = Phase::SpinDash;
            m_fTimer = 0.0f;
            m_fTickTimer = 0.0f;
            // 첫 틱 즉시 발동
            TickAoEDamage(pEnemy);
        }
        break;

    case Phase::SpinDash:
    {
        // 잠긴 방향으로 이동
        if (GameObject* pOwner = pEnemy->GetOwner())
        {
            if (TransformComponent* pT = pOwner->GetTransform())
            {
                XMFLOAT3 pos = pT->GetPosition();
                float moveAmt = m_fRushSpeed * dt;
                pos.x += m_xmf3RushDirection.x * moveAmt;
                pos.z += m_xmf3RushDirection.z * moveAmt;
                pT->SetPosition(pos);
            }
        }

        // 주기 데미지
        m_fTickTimer += dt;
        if (m_fTickTimer >= m_fTickInterval)
        {
            m_fTickTimer -= m_fTickInterval;
            TickAoEDamage(pEnemy);
        }

        if (m_fTimer >= m_fRushDuration)
        {
            m_ePhase = Phase::Recovery;
            m_fTimer = 0.0f;
        }
        break;
    }

    case Phase::Recovery:
        if (m_fTimer >= m_fRecoveryTime)
        {
            m_bFinished = true;
            // VFX 정리 (회오리 + 좌/우 윈드월)
            if (auto* pVFX = GetVFX())
            {
                if (m_nWindVFXId     >= 0) { pVFX->Stop(m_nWindVFXId);     m_nWindVFXId     = -1; }
                if (m_nBoosterVFXIdL >= 0) { pVFX->Stop(m_nBoosterVFXIdL); m_nBoosterVFXIdL = -1; }
                if (m_nBoosterVFXIdR >= 0) { pVFX->Stop(m_nBoosterVFXIdR); m_nBoosterVFXIdR = -1; }
            }
        }
        break;
    }
}

bool SpinDashAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void SpinDashAttackBehavior::Reset()
{
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
    m_fTickTimer = 0.0f;
    m_bFinished = false;
    m_xmf3RushDirection = XMFLOAT3(0.0f, 0.0f, 0.0f);

    // 안전장치: 이전 VFX 슬롯이 살아있으면 정리
    if (auto* pVFX = GetVFX())
    {
        if (m_nWindVFXId     >= 0) { pVFX->Stop(m_nWindVFXId);     m_nWindVFXId     = -1; }
        if (m_nBoosterVFXIdL >= 0) { pVFX->Stop(m_nBoosterVFXIdL); m_nBoosterVFXIdL = -1; }
        if (m_nBoosterVFXIdR >= 0) { pVFX->Stop(m_nBoosterVFXIdR); m_nBoosterVFXIdR = -1; }
    }
}

void SpinDashAttackBehavior::TickAoEDamage(EnemyComponent* pEnemy)
{
    if (!pEnemy) return;
    GameObject* pTarget = pEnemy->GetTarget();
    if (!pTarget) return;

    if (pEnemy->GetDistanceToTarget() > m_fAoERadius) return;

    if (PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>())
        pPlayer->TakeDamage(m_fTickDamage);
}
