#include "stdafx.h"
#include "RushAoEAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"
#include "MathUtils.h"

RushAoEAttackBehavior::RushAoEAttackBehavior(float fDamage, float fRushSpeed, float fRushDuration,
                                             float fWindupTime, float fHitTime, float fRecoveryTime,
                                             float fAoERadius, float fTelegraphTime)
    : m_fDamage(fDamage)
    , m_fRushSpeed(fRushSpeed)
    , m_fRushDuration(fRushDuration)
    , m_fWindupTime(fWindupTime)
    , m_fHitTime(fHitTime)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fAoERadius(fAoERadius)
    , m_fTelegraphTime(fTelegraphTime)
{
}

float RushAoEAttackBehavior::GetIndicatorRadius() const
{
    // RushFront 와 동일 시각 폭 — 4.5 ~ 6.0
    //   AoE 반경이 너무 크면 corridor 가 좁아 보일 수 있으므로 AoE 반경으로 베이스 잡음
    float halfW = m_fAoERadius;
    if (halfW < 4.5f) halfW = 4.5f;
    if (halfW > 6.0f) halfW = 6.0f;
    return halfW;
}

float RushAoEAttackBehavior::GetIndicatorLength() const
{
    // 전체 위험 구간 = 돌진 거리 + AoE 반경 (착지 폭발까지 표시)
    return m_fRushSpeed * m_fRushDuration + m_fAoERadius;
}

void RushAoEAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();

    if (!pEnemy) return;

    // Face and lock direction to target
    pEnemy->FaceTarget();

    // Store rush direction
    GameObject* pOwner = pEnemy->GetOwner();
    GameObject* pTarget = pEnemy->GetTarget();
    if (pOwner && pTarget)
    {
        TransformComponent* pMyTransform = pOwner->GetTransform();
        TransformComponent* pTargetTransform = pTarget->GetTransform();
        if (pMyTransform && pTargetTransform)
        {
            XMFLOAT3 myPos = pMyTransform->GetPosition();
            XMFLOAT3 targetPos = pTargetTransform->GetPosition();
            XMFLOAT2 dir = MathUtils::Direction2D(myPos, targetPos);
            m_xmf3RushDirection = XMFLOAT3(dir.x, 0.0f, dir.y);
        }
    }

    m_ePhase = Phase::Telegraph;
    m_fPhaseDuration = m_fTelegraphTime;
}

void RushAoEAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished) return;

    m_fTimer += dt;

    switch (m_ePhase)
    {
    case Phase::Telegraph:
        // 정지 상태로 ForwardBox 차오름. 잠긴 방향이 명확히 표시됨 → 회피 가능
        if (m_fTimer >= m_fTelegraphTime)
        {
            m_ePhase = Phase::Rush;
            m_fTimer = 0.0f;
            m_fPhaseDuration = m_fRushDuration;
        }
        break;

    case Phase::Rush:
        UpdateRush(dt, pEnemy);
        if (m_fTimer >= m_fRushDuration)
        {
            m_ePhase = Phase::Windup;
            m_fTimer = 0.0f;
            m_fPhaseDuration = m_fWindupTime;
        }
        break;

    case Phase::Windup:
        if (m_fTimer >= m_fWindupTime)
        {
            m_ePhase = Phase::Hit;
            m_fTimer = 0.0f;
            m_fPhaseDuration = m_fHitTime;
        }
        break;

    case Phase::Hit:
        if (!m_bHitDealt)
        {
            DealAoEDamage(pEnemy);
            m_bHitDealt = true;
        }
        if (m_fTimer >= m_fHitTime)
        {
            m_ePhase = Phase::Recovery;
            m_fTimer = 0.0f;
            m_fPhaseDuration = m_fRecoveryTime;
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

bool RushAoEAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void RushAoEAttackBehavior::Reset()
{
    m_ePhase = Phase::Telegraph;
    m_fTimer = 0.0f;
    m_bHitDealt = false;
    m_bRushHitDealt = false;
    m_bFinished = false;
    m_xmf3RushDirection = XMFLOAT3(0.0f, 0.0f, 0.0f);
}

void RushAoEAttackBehavior::UpdateRush(float dt, EnemyComponent* pEnemy)
{
    if (!pEnemy) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;

    TransformComponent* pTransform = pOwner->GetTransform();
    if (!pTransform) return;

    // Move in the locked rush direction
    // 네트워크 연출 전용 모드에서는 서버 MOVE 패킷이 위치를 갱신하므로
    // 클라에서 직접 SetPosition 하지 않는다.
    if (!m_bNetworkVisualOnly)
    {
        XMFLOAT3 pos = pTransform->GetPosition();
        float moveAmount = m_fRushSpeed * dt;
        pos.x += m_xmf3RushDirection.x * moveAmount;
        pos.z += m_xmf3RushDirection.z * moveAmount;
        pTransform->SetPosition(pos);
    }

    // 네트워크 연출 전용 모드에서는 데미지도 서버가 처리한다.
    if (m_bNetworkVisualOnly)
        return;

    // Check collision with player during rush
    if (!m_bRushHitDealt)
    {
        float distance = pEnemy->GetDistanceToTarget();
        if (distance <= RUSH_HIT_RADIUS)
        {
            GameObject* pTarget = pEnemy->GetTarget();
            if (pTarget)
            {
                PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>();
                if (pPlayer)
                {
                    pPlayer->TakeDamage(m_fDamage);
                    m_bRushHitDealt = true;
                }
            }
        }
    }
}

void RushAoEAttackBehavior::DealAoEDamage(EnemyComponent* pEnemy)
{
    // 네트워크 연출 전용 모드에서는 광역 데미지도 서버가 처리한다.
    if (m_bNetworkVisualOnly)
        return;

    if (!pEnemy) return;

    GameObject* pTarget = pEnemy->GetTarget();
    if (!pTarget) return;

    // 360-degree AoE: only check distance, no angle check
    float distance = pEnemy->GetDistanceToTarget();
    if (distance > m_fAoERadius) return;

    // Deal damage to player
    PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>();
    if (pPlayer)
        pPlayer->TakeDamage(m_fDamage);
}
