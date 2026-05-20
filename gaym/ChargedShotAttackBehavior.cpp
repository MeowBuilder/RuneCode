#include "stdafx.h"
#include "ChargedShotAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "ProjectileManager.h"

ChargedShotAttackBehavior::ChargedShotAttackBehavior(ProjectileManager* pProjectileManager,
                                                     float fDamage, float fProjectileSpeed,
                                                     float fChargeTime, float fRecoveryTime,
                                                     float fIndicatorLength, float fIndicatorHalfW)
    : m_pProjectileManager(pProjectileManager)
    , m_fDamage(fDamage)
    , m_fProjectileSpeed(fProjectileSpeed)
    , m_fChargeTime(fChargeTime)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fIndicatorLength(fIndicatorLength)
    , m_fIndicatorHalfW(fIndicatorHalfW)
{
}

void ChargedShotAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();

    if (!pEnemy) return;

    // 시작 시점에 타겟 방향으로 즉시 회전(잠금). 이후 windup 중에는 회전 갱신 안 함 → 라인 회피 가능
    pEnemy->FaceTarget(0.0f, true);

    GameObject* pOwner  = pEnemy->GetOwner();
    GameObject* pTarget = pEnemy->GetTarget();
    if (pOwner && pTarget)
    {
        TransformComponent* pMy = pOwner->GetTransform();
        TransformComponent* pTg = pTarget->GetTransform();
        if (pMy && pTg)
        {
            XMFLOAT3 myPos = pMy->GetPosition();
            XMFLOAT3 tgPos = pTg->GetPosition();
            float dx = tgPos.x - myPos.x;
            float dz = tgPos.z - myPos.z;
            float len = sqrtf(dx * dx + dz * dz);
            if (len > 0.0001f)
            {
                m_xmf3LockedDir = XMFLOAT3(dx / len, 0.0f, dz / len);
            }
        }
    }

    m_ePhase = Phase::Charging;
}

void ChargedShotAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished) return;

    m_fTimer += dt;

    switch (m_ePhase)
    {
    case Phase::Charging:
        // windup 중에는 FaceTarget 호출 안 함 — 방향 잠긴 상태 유지 (회피 가능 메커니즘 핵심)
        if (m_fTimer >= m_fChargeTime)
        {
            m_ePhase = Phase::Fire;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::Fire:
        if (!m_bFired)
        {
            FireProjectile(pEnemy);
            m_bFired = true;
        }
        // 발사 직후 바로 recovery 전환 (셔터링 없음)
        m_ePhase = Phase::Recovery;
        m_fTimer = 0.0f;
        break;

    case Phase::Recovery:
        if (m_fTimer >= m_fRecoveryTime)
        {
            m_bFinished = true;
        }
        break;
    }
}

void ChargedShotAttackBehavior::Reset()
{
    m_ePhase   = Phase::Charging;
    m_fTimer   = 0.0f;
    m_bFired   = false;
    m_bFinished = false;
    m_xmf3LockedDir = XMFLOAT3(0.0f, 0.0f, 1.0f);
}

void ChargedShotAttackBehavior::FireProjectile(EnemyComponent* pEnemy)
{
    if (!pEnemy || !m_pProjectileManager) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;

    TransformComponent* pMy = pOwner->GetTransform();
    if (!pMy) return;

    XMFLOAT3 startPos = pMy->GetPosition();
    startPos.y += 2.0f;

    // 잠긴 방향으로 indicatorLength 만큼 앞 지점을 target 으로 — 라인 끝까지 직선 발사
    XMFLOAT3 targetPos = XMFLOAT3(
        startPos.x + m_xmf3LockedDir.x * m_fIndicatorLength,
        startPos.y,
        startPos.z + m_xmf3LockedDir.z * m_fIndicatorLength);

    m_pProjectileManager->SpawnProjectile(
        startPos,
        targetPos,
        m_fDamage,
        m_fProjectileSpeed,
        0.9f,           // 일반 ranged 보다 큰 hitbox (정조준 보상)
        0.0f,           // 폭발 X
        ElementType::None,
        pOwner,
        false,          // enemy projectile
        1.4f            // 시각 스케일 ↑ — "센 단발" 체감
    );
}
