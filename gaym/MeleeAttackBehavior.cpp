#include "stdafx.h"
#include "MeleeAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"

MeleeAttackBehavior::MeleeAttackBehavior(float fDamage, float fWindupTime, float fHitTime, float fRecoveryTime)
    : m_fDamage(fDamage)
    , m_fWindupTime(fWindupTime)
    , m_fHitTime(fHitTime)
    , m_fRecoveryTime(fRecoveryTime)
{
    m_fTotalDuration = m_fWindupTime + m_fHitTime + m_fRecoveryTime;
}

void MeleeAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();

    // Face target at start of attack
    if (pEnemy)
    {
        pEnemy->FaceTarget();
    }
}

void MeleeAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished) return;

    m_fTimer += dt;

    // Check phases
    if (m_fTimer < m_fWindupTime)
    {
        // Windup phase - could play animation here
    }
    else if (m_fTimer < m_fWindupTime + m_fHitTime)
    {
        // Hit phase - deal damage once
        if (!m_bHitDealt)
        {
            DealDamage(pEnemy);
            m_bHitDealt = true;
        }
    }
    else if (m_fTimer >= m_fTotalDuration)
    {
        // Attack finished
        m_bFinished = true;
    }
}

bool MeleeAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void MeleeAttackBehavior::Reset()
{
    m_fTimer = 0.0f;
    m_bHitDealt = false;
    m_bFinished = false;
    m_fTotalDuration = m_fWindupTime + m_fHitTime + m_fRecoveryTime;
}

void MeleeAttackBehavior::DealDamage(EnemyComponent* pEnemy)
{
    // 네트워크 연출 전용 모드에서는 데미지는 서버가 처리한다.
    if (m_bNetworkVisualOnly)
        return;

    if (!pEnemy) return;

    GameObject* pTarget = pEnemy->GetTarget();
    if (!pTarget) return;

    // 공격 원점 기준 거리 체크 (크라켄처럼 몸 앞쪽 촉수가 공격 원점인 경우 고려)
    auto* pTargetT = pTarget->GetTransform();
    if (!pTargetT) return;
    XMFLOAT3 origin = pEnemy->GetAttackOrigin();
    XMFLOAT3 tp = pTargetT->GetPosition();
    float dx = tp.x - origin.x, dz = tp.z - origin.z;
    float distance = sqrtf(dx * dx + dz * dz);
    if (distance > m_fHitRange) return;

    // Deal damage to player
    PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>();
    if (pPlayer)
        pPlayer->TakeDamage(m_fDamage);
}
