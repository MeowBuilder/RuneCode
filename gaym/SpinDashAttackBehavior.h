#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>

using namespace DirectX;

// 회전하면서 돌진하며 주변에 지속 데미지 — 데몬의 기본 압박기
//   Phase: Windup → SpinDash(이동 + 주기 데미지) → Recovery(딜 윈도우)
//   인디케이터: ForwardBox (돌진 경로의 직사각형)
class SpinDashAttackBehavior : public IAttackBehavior
{
public:
    SpinDashAttackBehavior(float fTickDamage = 18.0f,
                           float fTickInterval = 0.22f,
                           float fRushSpeed = 16.0f,
                           float fRushDuration = 1.0f,
                           float fWindupTime = 0.3f,
                           float fRecoveryTime = 0.6f,
                           float fAoERadius = 4.0f);

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

    virtual const char* GetAnimClipName() const override { return "attack3"; }
    virtual bool        ShouldLoopAnim() const override  { return true; }
    virtual float       GetTimeToHit() const override    { return m_fWindupTime; }

    // 인디케이터: 돌진 경로 직사각형 (ForwardBox)
    //   half-width = AoE 반경, 길이 = 돌진 거리
    virtual int   GetIndicatorTypeOverride() const override; // returns (int)IndicatorType::ForwardBox
    virtual float GetIndicatorRadius() const override { return m_fAoERadius; }
    virtual float GetIndicatorLength() const override { return m_fRushSpeed * m_fRushDuration; }
    // 기본 회전 공격 — 따뜻한 호박색 (덜 위협적, 자주 나옴)
    virtual XMFLOAT3 GetIndicatorTint() const override { return XMFLOAT3(1.0f, 0.55f, 0.10f); }

private:
    enum class Phase { Windup, SpinDash, Recovery };

    void TickAoEDamage(EnemyComponent* pEnemy);

private:
    float m_fTickDamage   = 18.0f;
    float m_fTickInterval = 0.22f;
    float m_fRushSpeed    = 16.0f;
    float m_fRushDuration = 1.0f;
    float m_fWindupTime   = 0.3f;
    float m_fRecoveryTime = 0.6f;
    float m_fAoERadius    = 4.0f;

    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;
    float m_fTickTimer = 0.0f;
    bool  m_bFinished = false;
    XMFLOAT3 m_xmf3RushDirection = { 0.0f, 0.0f, 0.0f };

    // 회오리 wind VFX (sub_wind 이펙트). -1 = 활성 슬롯 없음
    int m_nWindVFXId = -1;
    // 좌/우 윈드월 (궤적 양옆 바람 벽)
    int m_nBoosterVFXIdL = -1;
    int m_nBoosterVFXIdR = -1;
};
