#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>

using namespace DirectX;

class ProjectileManager;

// 자폭병형 — 사거리 진입 시 정지 + 카운트다운 (Circle 인디케이터 차오름) → 광역 폭발 후 자기 사망.
//   회피 = 인디케이터 밖으로 후퇴. "빠른 접근" 은 preset moveSpeed 로 조절.
class SuicideExplodeAttackBehavior : public IAttackBehavior
{
public:
    SuicideExplodeAttackBehavior(ProjectileManager* pProjectileManager,
                                 float fDamage         = 45.0f,
                                 float fAoERadius      = 4.5f,
                                 float fCountdownTime  = 1.0f);
    virtual ~SuicideExplodeAttackBehavior() = default;

    // 네트워크 연출 전용 모드
    void SetNetworkVisualOnly(bool bEnable) { m_bNetworkVisualOnly = bEnable; }

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override { return m_bFinished; }
    virtual void Reset() override;

    virtual float GetTimeToHit() const override { return m_fCountdownTime; }
    virtual bool  ShouldShowHitZone() const override { return m_ePhase == Phase::Countdown; }
    virtual int   GetIndicatorTypeOverride() const override { return 1; /* Circle */ }
    virtual float GetIndicatorRadius() const override { return m_fAoERadius; }
    // 진한 적색 — "위험" 신호. 다른 변종(노랑/주황/자주/청록) 과 차별
    virtual XMFLOAT3 GetIndicatorTint() const override { return XMFLOAT3(1.6f, 0.1f, 0.05f); }
    virtual bool  ShouldLoopAnim() const override { return false; }

private:
    enum class Phase { Countdown, Done };
    void Explode(EnemyComponent* pEnemy);

private:
    ProjectileManager* m_pProjectileManager = nullptr;

    float m_fDamage        = 45.0f;
    float m_fAoERadius     = 4.5f;
    float m_fCountdownTime = 1.0f;

    Phase m_ePhase     = Phase::Countdown;
    float m_fTimer     = 0.0f;
    bool  m_bExploded  = false;
    bool  m_bFinished  = false;

    // 네트워크 연출 전용 모드
    bool m_bNetworkVisualOnly = false;
};
