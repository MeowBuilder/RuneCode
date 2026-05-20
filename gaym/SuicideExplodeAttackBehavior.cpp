#include "stdafx.h"
#include "SuicideExplodeAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"
#include "ProjectileManager.h"
#include "Camera.h"
#include "Scene.h"
#include "Dx12App.h"

SuicideExplodeAttackBehavior::SuicideExplodeAttackBehavior(ProjectileManager* pProjectileManager,
                                                           float fDamage, float fAoERadius,
                                                           float fCountdownTime)
    : m_pProjectileManager(pProjectileManager)
    , m_fDamage(fDamage)
    , m_fAoERadius(fAoERadius)
    , m_fCountdownTime(fCountdownTime)
{
}

void SuicideExplodeAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();

    if (pEnemy)
    {
        pEnemy->FaceTarget(0.0f, true);
    }

    m_ePhase = Phase::Countdown;
}

void SuicideExplodeAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished) return;

    m_fTimer += dt;

    switch (m_ePhase)
    {
    case Phase::Countdown:
        if (m_fTimer >= m_fCountdownTime)
        {
            if (!m_bExploded)
            {
                Explode(pEnemy);
                m_bExploded = true;
            }
            m_ePhase = Phase::Done;
            m_bFinished = true;
        }
        break;

    case Phase::Done:
        m_bFinished = true;
        break;
    }
}

void SuicideExplodeAttackBehavior::Reset()
{
    m_ePhase    = Phase::Countdown;
    m_fTimer    = 0.0f;
    m_bExploded = false;
    m_bFinished = false;
}

void SuicideExplodeAttackBehavior::Explode(EnemyComponent* pEnemy)
{
    if (!pEnemy) return;

    GameObject* pOwner = pEnemy->GetOwner();
    XMFLOAT3 boomPos = { 0.0f, 0.0f, 0.0f };
    if (pOwner && pOwner->GetTransform())
    {
        boomPos = pOwner->GetTransform()->GetPosition();
    }

    // 광역 데미지 — 자폭 위치 기준 거리 체크
    GameObject* pTarget = pEnemy->GetTarget();
    if (pTarget && pTarget->GetTransform())
    {
        XMFLOAT3 tp = pTarget->GetTransform()->GetPosition();
        float dx = tp.x - boomPos.x;
        float dz = tp.z - boomPos.z;
        float dist = sqrtf(dx * dx + dz * dz);

        if (dist <= m_fAoERadius)
        {
            PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>();
            if (pPlayer)
            {
                // 가까울수록 강함 (60% ~ 100%)
                float damageMul = 1.0f - (dist / m_fAoERadius) * 0.4f;
                pPlayer->TakeDamage(m_fDamage * damageMul);
            }
        }
    }

    // 폭발 VFX
    if (m_pProjectileManager)
    {
        XMFLOAT3 fxPos = boomPos;
        fxPos.y += 0.4f;
        m_pProjectileManager->SpawnExplosionParticles(fxPos, ElementType::Fire);
    }

    // 카메라 쉐이크 — 자폭은 데미지 큼, GrenadeThrow 보다 강하게
    if (Dx12App* pApp = Dx12App::GetInstance())
    {
        if (Scene* pScene = pApp->GetScene())
        {
            if (CCamera* pCam = pScene->GetCamera())
            {
                pCam->StartShake(0.28f, 0.35f);
            }
        }
    }

    // 자기 사망 — 99999 데미지로 instakill (stagger 트리거 X)
    pEnemy->TakeDamage(99999.0f, false);
}
