#include "stdafx.h"
#include "GrenadeThrowAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"
#include "ProjectileManager.h"
#include "Camera.h"
#include "Scene.h"
#include "Dx12App.h"

GrenadeThrowAttackBehavior::GrenadeThrowAttackBehavior(ProjectileManager* pProjectileManager,
                                                       float fDamage, float fAoERadius,
                                                       float fWindupTime, float fAirTime,
                                                       float fRecoveryTime)
    : m_pProjectileManager(pProjectileManager)
    , m_fDamage(fDamage)
    , m_fAoERadius(fAoERadius)
    , m_fWindupTime(fWindupTime)
    , m_fAirTime(fAirTime)
    , m_fRecoveryTime(fRecoveryTime)
{
}

void GrenadeThrowAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();

    if (!pEnemy) return;

    pEnemy->FaceTarget(0.0f, true);

    // 시작 시점에 플레이어 위치를 착지 지점으로 잠금 — 이후 플레이어가 도망쳐도 표적 안 따라감
    GameObject* pTarget = pEnemy->GetTarget();
    if (pTarget && pTarget->GetTransform())
    {
        m_xmf3LandingPos = pTarget->GetTransform()->GetPosition();
        // 지면 인디케이터이므로 Y 는 지면 기준 — ShowIndicators 가 indY 보정함
    }
    else
    {
        // 폴백 — 적 자신 위치
        GameObject* pOwner = pEnemy->GetOwner();
        if (pOwner && pOwner->GetTransform())
        {
            m_xmf3LandingPos = pOwner->GetTransform()->GetPosition();
        }
    }

    m_ePhase = Phase::Windup;
}

void GrenadeThrowAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished) return;

    m_fTimer += dt;

    switch (m_ePhase)
    {
    case Phase::Windup:
        if (m_fTimer >= m_fWindupTime)
        {
            m_ePhase = Phase::AirTime;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::AirTime:
        if (m_fTimer >= m_fAirTime)
        {
            if (!m_bExploded)
            {
                Explode(pEnemy);
                m_bExploded = true;
            }
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

void GrenadeThrowAttackBehavior::Reset()
{
    m_ePhase    = Phase::Windup;
    m_fTimer    = 0.0f;
    m_bExploded = false;
    m_bFinished = false;
    m_xmf3LandingPos = XMFLOAT3(0.0f, 0.0f, 0.0f);
}

bool GrenadeThrowAttackBehavior::GetIndicatorWorldPos(EnemyComponent* /*pEnemy*/, XMFLOAT3& outPos) const
{
    outPos = m_xmf3LandingPos;
    return true;
}

void GrenadeThrowAttackBehavior::Explode(EnemyComponent* pEnemy)
{
    if (!pEnemy) return;

    // 광역 데미지 — 잠긴 착지 지점 기준
    GameObject* pTarget = pEnemy->GetTarget();
    if (pTarget && pTarget->GetTransform())
    {
        XMFLOAT3 targetPos = pTarget->GetTransform()->GetPosition();
        float dx = targetPos.x - m_xmf3LandingPos.x;
        float dz = targetPos.z - m_xmf3LandingPos.z;
        float dist = sqrtf(dx * dx + dz * dz);

        if (dist <= m_fAoERadius)
        {
            PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>();
            if (pPlayer)
            {
                // 가까울수록 강함 (60% ~ 100%)
                float damageMul = 1.0f - (dist / m_fAoERadius) * 0.4f;
                float actualDamage = m_fDamage * damageMul;

                // 네트워크 연출 전용 모드에서는 데미지는 서버가 처리한다.
                if (!m_bNetworkVisualOnly)
                {
                    pPlayer->TakeDamage(actualDamage);
                }
            }
        }
    }

    // 폭발 VFX — Fire 원소 컬러
    if (m_pProjectileManager)
    {
        XMFLOAT3 fxPos = m_xmf3LandingPos;
        fxPos.y += 0.4f;
        m_pProjectileManager->SpawnExplosionParticles(fxPos, ElementType::Fire);
    }

    // 카메라 쉐이크
    if (Dx12App* pApp = Dx12App::GetInstance())
    {
        if (Scene* pScene = pApp->GetScene())
        {
            if (CCamera* pCam = pScene->GetCamera())
            {
                pCam->StartShake(0.18f, 0.25f);
            }
        }
    }
}
