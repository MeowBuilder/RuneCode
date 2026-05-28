#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>

using namespace DirectX;

class RushFrontAttackBehavior : public IAttackBehavior
{
public:
    RushFrontAttackBehavior(float fDamage = 20.0f, float fRushSpeed = 18.0f, float fRushDuration = 0.4f,
                            float fWindupTime = 0.2f, float fHitTime = 0.2f, float fRecoveryTime = 0.3f,
                            float fHitRange = 4.0f, float fConeAngleDeg = 90.0f,
                            float fTelegraphTime = 0.45f);
    virtual ~RushFrontAttackBehavior() = default;

    // 네트워크 연출 전용 모드
// 서버 권위 일반 몬스터는 클라에서 직접 위치 이동/데미지 처리를 하지 않는다.
    void SetNetworkVisualOnly(bool bEnable) { m_bNetworkVisualOnly = bEnable; }

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const char* GetAnimClipName() const override { return "Run"; }

    // 인디케이터: 돌진 corridor 직사각형 (ForwardBox). Telegraph 동안 차오르고 Rush 시작 시 숨김
    virtual int   GetIndicatorTypeOverride() const override; // ForwardBox
    virtual float GetIndicatorRadius() const override;       // corridor 절반 너비
    virtual float GetIndicatorLength() const override;       // 돌진 거리 + 콘 사거리 (전체 위험구간)
    virtual float GetTimeToHit() const override { return m_fTelegraphTime; }
    virtual bool  ShouldShowHitZone() const override
    {
        return m_ePhase == Phase::Telegraph;
    }

private:
    enum class Phase { Telegraph, Rush, Windup, Hit, Recovery };

    void UpdateRush(float dt, EnemyComponent* pEnemy);
    void DealConeDamage(EnemyComponent* pEnemy);

private:
    // Parameters
    float m_fDamage = 20.0f;
    float m_fRushSpeed = 18.0f;
    float m_fRushDuration = 0.4f;
    float m_fWindupTime = 0.2f;
    float m_fHitTime = 0.2f;
    float m_fRecoveryTime = 0.3f;
    float m_fHitRange = 4.0f;
    float m_fConeAngleDeg = 90.0f;
    float m_fCosHalfCone = 0.0f; // Precomputed cos(halfCone)
    float m_fTelegraphTime = 0.45f;

    // Runtime state
    Phase m_ePhase = Phase::Telegraph;
    float m_fTimer = 0.0f;
    bool m_bHitDealt = false;
    bool m_bRushHitDealt = false;  // Separate flag for rush collision damage
    bool m_bFinished = false;
    XMFLOAT3 m_xmf3RushDirection = { 0.0f, 0.0f, 0.0f };

    static constexpr float RUSH_HIT_RADIUS = 2.5f;  // Collision radius during rush
    
    // 네트워크 연출 전용 모드
    bool m_bNetworkVisualOnly = false;
};
