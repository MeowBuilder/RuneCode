#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>

using namespace DirectX;

class RushAoEAttackBehavior : public IAttackBehavior
{
public:
    RushAoEAttackBehavior(float fDamage = 15.0f, float fRushSpeed = 15.0f, float fRushDuration = 0.5f,
                          float fWindupTime = 0.3f, float fHitTime = 0.2f, float fRecoveryTime = 0.3f,
                          float fAoERadius = 5.0f, float fTelegraphTime = 0.45f);
    virtual ~RushAoEAttackBehavior() = default;
    // 네트워크 연출 전용 모드
    // 서버 권위 일반 몬스터는 클라에서 직접 위치 이동/데미지 처리를 하지 않는다.
    void SetNetworkVisualOnly(bool bEnable) { m_bNetworkVisualOnly = bEnable; }

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

    // 인디케이터: 돌진 경로 + 착지 AoE 모두 포함한 ForwardBox 텔레그래프
    virtual int   GetIndicatorTypeOverride() const override { return 4; /* ForwardBox */ }
    virtual float GetIndicatorRadius() const override;       // corridor 절반 너비
    virtual float GetIndicatorLength() const override;       // 돌진 거리 + AoE 사거리
    virtual float GetTimeToHit() const override { return m_fTelegraphTime; }
    // Telegraph phase 동안만 인디케이터 표시
    virtual bool  ShouldShowHitZone() const override { return m_ePhase == Phase::Telegraph; }
    // 주황빛 — 돌진형 식별 (RushFront 와 동일 톤)
    virtual XMFLOAT3 GetIndicatorTint() const override { return XMFLOAT3(1.0f, 0.6f, 0.2f); }

private:
    enum class Phase { Telegraph, Rush, Windup, Hit, Recovery };

    void UpdateRush(float dt, EnemyComponent* pEnemy);
    void DealAoEDamage(EnemyComponent* pEnemy);

private:
    // Parameters
    float m_fDamage = 15.0f;
    float m_fRushSpeed = 15.0f;
    float m_fRushDuration = 0.5f;
    float m_fWindupTime = 0.3f;
    float m_fHitTime = 0.2f;
    float m_fRecoveryTime = 0.3f;
    float m_fAoERadius = 5.0f;
    float m_fTelegraphTime = 0.45f;

    // Runtime state
    Phase m_ePhase = Phase::Telegraph;
    float m_fTimer = 0.0f;
    float m_fPhaseDuration = 0.0f;
    bool m_bHitDealt = false;
    bool m_bRushHitDealt = false;  // Separate flag for rush collision damage
    bool m_bFinished = false;
    XMFLOAT3 m_xmf3RushDirection = { 0.0f, 0.0f, 0.0f };

    static constexpr float RUSH_HIT_RADIUS = 2.5f;  // Collision radius during rush

    // 네트워크 연출 전용 모드
    bool m_bNetworkVisualOnly = false;
};
