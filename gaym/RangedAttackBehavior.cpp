#include "stdafx.h"
#include "RangedAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "ProjectileManager.h"

RangedAttackBehavior::RangedAttackBehavior(ProjectileManager* pProjectileManager,
                                           float fDamage, float fProjectileSpeed,
                                           float fWindupTime, float fShootTime, float fRecoveryTime)
    : m_pProjectileManager(pProjectileManager)
    , m_fDamage(fDamage)
    , m_fProjectileSpeed(fProjectileSpeed)
    , m_fWindupTime(fWindupTime)
    , m_fShootTime(fShootTime)
    , m_fRecoveryTime(fRecoveryTime)
{
}

void RangedAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();

    if (pEnemy)
    {
        pEnemy->FaceTarget();
    }

    m_ePhase = Phase::Windup;
}

void RangedAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished) return;

    m_fTimer += dt;

    switch (m_ePhase)
    {
    case Phase::Windup:
        // Keep facing target during windup
        if (pEnemy) pEnemy->FaceTarget();
        if (m_fTimer >= m_fWindupTime)
        {
            m_ePhase = Phase::Shoot;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::Shoot:
        if (!m_bShotFired)
        {
            ShootProjectile(pEnemy);
            m_bShotFired = true;
        }
        if (m_fTimer >= m_fShootTime)
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

bool RangedAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void RangedAttackBehavior::Reset()
{
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
    m_bShotFired = false;
    m_bFinished = false;
}

void RangedAttackBehavior::ShootProjectile(EnemyComponent* pEnemy)
{
    if (!pEnemy || !m_pProjectileManager) return;

    GameObject* pOwner = pEnemy->GetOwner();
    GameObject* pTarget = pEnemy->GetTarget();
    if (!pOwner || !pTarget) return;

    TransformComponent* pMyTransform = pOwner->GetTransform();
    TransformComponent* pTargetTransform = pTarget->GetTransform();
    if (!pMyTransform || !pTargetTransform) return;

    XMFLOAT3 startPos = pMyTransform->GetPosition();
    startPos.y += 2.0f; // Spawn projectile at roughly chest height

    XMFLOAT3 targetPos = pTargetTransform->GetPosition();
    targetPos.y += 1.0f; // Aim at player center

    float damage = m_bNetworkVisualOnly ? 0.0f : m_fDamage;

    m_pProjectileManager->SpawnProjectile(
        startPos,
        targetPos,
        damage,
        m_fProjectileSpeed,
        0.5f,           // radius
        0.0f,           // no explosion
        ElementType::None,
        pOwner,
        false,          // isPlayerProjectile = false (enemy projectile)
        0.8f            // scale
    );

}
