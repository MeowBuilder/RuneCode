#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>

using namespace DirectX;

// 속공형 근접 — 짧은 wind-up 후 3타 다단. 각 타격은 저데미지지만 빠른 압박.
//   회피는 후속 타 전에 후퇴해야 가능 (회피 타이밍 차별화). 청록색 인디케이터로 시각 구분.
class QuickJabAttackBehavior : public IAttackBehavior
{
public:
    QuickJabAttackBehavior(float fDamagePerHit    = 8.0f,
                           float fWindupTime      = 0.18f,
                           float fHitInterval     = 0.18f,
                           int   nHitCount        = 3,
                           float fRecoveryTime    = 0.35f,
                           float fHitRange        = 3.2f);
    virtual ~QuickJabAttackBehavior() = default;

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override { return m_bFinished; }
    virtual void Reset() override;

    virtual float GetTimeToHit() const override { return m_fWindupTime; }
    // 첫 타 이후엔 인디케이터 숨김 — fill 다시 차오르지 않도록 (혼동 방지)
    virtual bool  ShouldShowHitZone() const override { return m_ePhase == Phase::Windup; }
    // 청록 — 노랑(ChargedShot)/주황(Grenade)/빨강(기본 Melee) 과 차별
    virtual XMFLOAT3 GetIndicatorTint() const override { return XMFLOAT3(0.3f, 1.4f, 1.4f); }
    virtual bool  ShouldLoopAnim() const override { return false; }

private:
    enum class Phase { Windup, Burst, Recovery };
    void TryDealHit(EnemyComponent* pEnemy);

private:
    float m_fDamagePerHit = 8.0f;
    float m_fWindupTime   = 0.18f;
    float m_fHitInterval  = 0.18f;
    int   m_nHitCount     = 3;
    float m_fRecoveryTime = 0.35f;
    float m_fHitRange     = 3.2f;

    Phase m_ePhase     = Phase::Windup;
    float m_fTimer     = 0.0f;
    int   m_nHitsDone  = 0;
    bool  m_bFinished  = false;
};
