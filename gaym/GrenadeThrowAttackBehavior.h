#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>

using namespace DirectX;

class ProjectileManager;

// 투척병형 공격 — 플레이어 현 위치를 착지점으로 잠그고 wind-up + airTime 동안
//   해당 지점에 주황색 원형 인디케이터 표시 → 시간 경과 후 광역 폭발.
//   회피 = 인디케이터 밖으로 이동. ChargedShot(라인 회피) 과 메커니즘 다름.
class GrenadeThrowAttackBehavior : public IAttackBehavior
{
public:
    GrenadeThrowAttackBehavior(ProjectileManager* pProjectileManager,
                               float fDamage       = 32.0f,
                               float fAoERadius    = 4.5f,
                               float fWindupTime   = 0.5f,
                               float fAirTime      = 1.1f,   // throw → land (텔레그래프 총 1.6s)
                               float fRecoveryTime = 0.6f);
    virtual ~GrenadeThrowAttackBehavior() = default;

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override { return m_bFinished; }
    virtual void Reset() override;

    // fill 진행도 = windup + airTime
    virtual float GetTimeToHit() const override { return m_fWindupTime + m_fAirTime; }
    // 폭발 후(Recovery)에는 인디케이터 숨김 — 다음 폭발로 오인 방지
    virtual bool  ShouldShowHitZone() const override { return m_ePhase != Phase::Recovery; }
    // Circle 모양 강제 — preset 이 None 이어도 작동
    virtual int   GetIndicatorTypeOverride() const override { return 1; /* IndicatorType::Circle */ }
    virtual float GetIndicatorRadius() const override { return m_fAoERadius; }
    // 주황 — ChargedShot(노랑), Melee(빨강)와 시각 차별
    virtual XMFLOAT3 GetIndicatorTint() const override { return XMFLOAT3(1.4f, 0.55f, 0.05f); }
    virtual bool  ShouldLoopAnim() const override { return false; }

    // Circle 인디케이터 위치를 적이 아닌 착지 지점에 잠금
    virtual bool GetIndicatorWorldPos(class EnemyComponent* pEnemy, XMFLOAT3& outPos) const override;

private:
    enum class Phase { Windup, AirTime, Recovery };
    void Explode(EnemyComponent* pEnemy);

private:
    ProjectileManager* m_pProjectileManager = nullptr;

    float m_fDamage       = 32.0f;
    float m_fAoERadius    = 4.5f;
    float m_fWindupTime   = 0.5f;
    float m_fAirTime      = 1.1f;
    float m_fRecoveryTime = 0.6f;

    Phase    m_ePhase     = Phase::Windup;
    float    m_fTimer     = 0.0f;
    bool     m_bExploded  = false;
    bool     m_bFinished  = false;
    // Execute 시 잠근 착지 지점 — 플레이어가 도망쳐도 표적은 같은 자리에 그대로
    XMFLOAT3 m_xmf3LandingPos = { 0.0f, 0.0f, 0.0f };
};
