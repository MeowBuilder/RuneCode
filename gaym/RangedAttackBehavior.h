#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>

using namespace DirectX;

class ProjectileManager;

class RangedAttackBehavior : public IAttackBehavior
{
public:
    RangedAttackBehavior(ProjectileManager* pProjectileManager,
                         float fDamage = 10.0f, float fProjectileSpeed = 20.0f,
                         float fWindupTime = 0.5f, float fShootTime = 0.1f, float fRecoveryTime = 0.5f);
    virtual ~RangedAttackBehavior() = default;

    // 네트워크 연출 전용 모드
    // 서버 권위 일반 몬스터는 클라에서 직접 데미지를 주지 않는다.
    void SetNetworkVisualOnly(bool bEnable) { m_bNetworkVisualOnly = bEnable; }

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

private:
    enum class Phase { Windup, Shoot, Recovery };

    void ShootProjectile(EnemyComponent* pEnemy);

private:
    // Dependencies
    ProjectileManager* m_pProjectileManager = nullptr;

    // Parameters
    float m_fDamage = 10.0f;
    float m_fProjectileSpeed = 20.0f;
    float m_fWindupTime = 0.5f;
    float m_fShootTime = 0.1f;
    float m_fRecoveryTime = 0.5f;

    // Runtime state
    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;
    bool m_bShotFired = false;
    bool m_bFinished = false;

    // 네트워크 연출 전용 모드
    bool m_bNetworkVisualOnly = false;
};
