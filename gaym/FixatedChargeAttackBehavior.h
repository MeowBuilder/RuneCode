#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>

using namespace DirectX;

// 4스테이지 데몬의 핵심 기믹 — 어글자 추적 텔레그래프 후 장거리 고속 돌진
//   페이즈: Telegraph(3.0s, 타겟 추적/인디케이터 차오름) → LockedAim(0.2s)
//          → Dash(고속 장거리, 충돌 체크) → Outcome(player/pillar/miss 별 후딜)
//   기둥 충돌 시 그로기 (긴 후딜 + 무방비), 허공 돌진 시 miss 카운터 증가 → 다음 charge 단축
class FixatedChargeAttackBehavior : public IAttackBehavior
{
public:
    FixatedChargeAttackBehavior(float fDamage           = 80.0f,
                                float fTelegraphTime    = 3.0f,
                                float fLockedAimTime    = 0.2f,
                                float fDashSpeed        = 50.0f,
                                float fMaxDashDistance  = 80.0f,
                                float fPlayerHitRadius  = 5.0f,
                                float fPillarHitRadius  = 6.0f,
                                float fIndicatorHalfW   = 4.0f,
                                float fGroggyDuration   = 4.5f,
                                float fNormalRecovery   = 0.9f);

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override { return m_bFinished; }
    virtual void Reset() override;

    // 텔레그래프~락 동안에는 Run 애니로 "추적 자세". 그로기 진입 시 직접 gethit3 로 스왑
    virtual const char* GetAnimClipName() const override { return "Run"; }
    virtual bool        ShouldLoopAnim() const override  { return true; }
    virtual float       GetTimeToHit() const override    { return m_fTelegraphTime; }

    // 인디케이터: ForwardBox (어글자 방향으로 길게 뻗는 직사각형). 보스 yaw 따라감 → 추적 자동
    virtual int   GetIndicatorTypeOverride() const override; // ForwardBox
    virtual float GetIndicatorRadius() const override { return m_fIndicatorHalfW; }
    virtual float GetIndicatorLength() const override { return m_fMaxDashDistance; }
    // 핵심 위협 패턴 — 보라/마젠타로 구분 (가장 눈에 띄는 색)
    virtual XMFLOAT3 GetIndicatorTint() const override { return XMFLOAT3(0.7f, 0.10f, 1.2f); }

    // Dash/Recovery 단계에서는 인디케이터 숨김 (보스가 이미 출발한 뒤)
    virtual bool ShouldShowHitZone() const override
    {
        return m_ePhase == Phase::Telegraph || m_ePhase == Phase::LockedAim;
    }

private:
    enum class Phase { Telegraph, LockedAim, Dash, Recovery };

    void StartDash(EnemyComponent* pEnemy);
    bool TryHitPlayer(EnemyComponent* pEnemy);                 // 플레이어 근접 → true 면 끝
    bool TryHitPillar(EnemyComponent* pEnemy);                 // 기둥 근접 → true 면 그로기
    void EnterGroggy(EnemyComponent* pEnemy);
    void EnterPlayerHitRecovery(EnemyComponent* pEnemy);
    void EnterMissRecovery(EnemyComponent* pEnemy);

private:
    // Parameters
    float m_fDamage;
    float m_fTelegraphTime;
    float m_fLockedAimTime;
    float m_fDashSpeed;
    float m_fMaxDashDistance;
    float m_fPlayerHitRadius;
    float m_fPillarHitRadius;
    float m_fIndicatorHalfW;
    float m_fGroggyDuration;
    float m_fNormalRecovery;

    // Runtime
    Phase    m_ePhase = Phase::Telegraph;
    float    m_fPhaseTimer = 0.0f;
    float    m_fRecoveryDuration = 0.0f;
    float    m_fDashTraveled = 0.0f;
    bool     m_bFinished = false;
    XMFLOAT3 m_xmf3DashDir = { 0.0f, 0.0f, 0.0f };
    int      m_nWindVFXIdL = -1;  // Dash 좌/우 윈드월 슬롯
    int      m_nWindVFXIdR = -1;
};
