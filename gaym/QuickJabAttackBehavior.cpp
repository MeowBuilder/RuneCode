#include "stdafx.h"
#include "QuickJabAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"

QuickJabAttackBehavior::QuickJabAttackBehavior(float fDamagePerHit, float fWindupTime,
                                               float fHitInterval, int nHitCount,
                                               float fRecoveryTime, float fHitRange)
    : m_fDamagePerHit(fDamagePerHit)
    , m_fWindupTime(fWindupTime)
    , m_fHitInterval(fHitInterval)
    , m_nHitCount(nHitCount)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fHitRange(fHitRange)
{
}

void QuickJabAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();

    if (pEnemy)
    {
        pEnemy->FaceTarget(0.0f, true);
    }

    m_ePhase = Phase::Windup;
}

void QuickJabAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished) return;

    m_fTimer += dt;

    switch (m_ePhase)
    {
    case Phase::Windup:
        // 청록 인디케이터 fill — windup 동안 표시. 너무 짧아서 거의 즉시 차오름
        if (m_fTimer >= m_fWindupTime)
        {
            m_ePhase = Phase::Burst;
            m_fTimer = 0.0f;
            // 첫 타 즉시 처리
            TryDealHit(pEnemy);
            m_nHitsDone = 1;
        }
        break;

    case Phase::Burst:
        // 후속타 — interval 마다 거리 체크해서 데미지. 플레이어가 후퇴하면 빗나감
        if (m_nHitsDone < m_nHitCount && m_fTimer >= m_fHitInterval)
        {
            TryDealHit(pEnemy);
            m_nHitsDone++;
            m_fTimer = 0.0f;

            // burst 중에도 적당히 회전 갱신 — 너무 굳지 않도록 (다만 windup 이후라 회피 가능)
            if (pEnemy) pEnemy->FaceTarget(dt, false);
        }
        if (m_nHitsDone >= m_nHitCount)
        {
            m_ePhase = Phase::Recovery;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::Recovery:
        if (m_fTimer >= m_fRecoveryTime)
        {
            m_bFinished = true;
        }
        break;
    }
}

void QuickJabAttackBehavior::Reset()
{
    m_ePhase    = Phase::Windup;
    m_fTimer    = 0.0f;
    m_nHitsDone = 0;
    m_bFinished = false;
}

void QuickJabAttackBehavior::TryDealHit(EnemyComponent* pEnemy)
{
    if (!pEnemy) return;

    float distance = pEnemy->GetDistanceToTarget();
    if (distance > m_fHitRange) return;

    GameObject* pTarget = pEnemy->GetTarget();
    if (!pTarget) return;

    PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>();
    if (pPlayer)
    {
        pPlayer->TakeDamage(m_fDamagePerHit);
    }
}
