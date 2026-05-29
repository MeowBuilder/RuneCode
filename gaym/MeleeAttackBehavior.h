#pragma once
#include "IAttackBehavior.h"

class MeleeAttackBehavior : public IAttackBehavior
{
public:
    MeleeAttackBehavior(float fDamage = 10.0f, float fWindupTime = 0.3f, float fHitTime = 0.5f, float fRecoveryTime = 0.2f);
    virtual ~MeleeAttackBehavior() = default;
    // 네트워크 연출 전용 모드
    // 서버 권위 일반 몬스터는 클라에서 직접 데미지를 주지 않는다.
    void SetNetworkVisualOnly(bool bEnable) { m_bNetworkVisualOnly = bEnable; }
   
    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual float GetTimeToHit() const override { return m_fWindupTime; }
	// 근접 공격은 투사체가 없고 인디케이터로 타이밍과 범위만 표시하므로, 히트존은 windup 동안 보여준다.
    virtual bool ShouldShowHitZone() const override { return m_fTimer < m_fWindupTime; }

    // Configuration
    void SetDamage(float fDamage) { m_fDamage = fDamage; }
    void SetWindupTime(float fTime) { m_fWindupTime = fTime; }
    void SetHitTime(float fTime) { m_fHitTime = fTime; }
    void SetRecoveryTime(float fTime) { m_fRecoveryTime = fTime; }
    void SetHitRange(float fRange) { m_fHitRange = fRange; }

private:
    void DealDamage(EnemyComponent* pEnemy);

private:
    // Attack parameters
    float m_fDamage = 10.0f;
    float m_fWindupTime = 0.3f;   // Time before hit (wind-up animation)
    float m_fHitTime = 0.5f;      // Time when hit occurs
    float m_fRecoveryTime = 0.2f; // Time after hit (recovery)
    float m_fHitRange = 3.0f;     // Range at which hit can connect

    // Runtime state
    float m_fTimer = 0.0f;
    float m_fTotalDuration = 0.0f;
    bool m_bHitDealt = false;
    bool m_bFinished = false;

    // 네트워크 연출 전용 모드
    bool m_bNetworkVisualOnly = false;
};
