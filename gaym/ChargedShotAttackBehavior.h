#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>

using namespace DirectX;

class ProjectileManager;

// 저격수형 공격 — 긴 wind-up(charge) 동안 전방 직사각형 라인 인디케이터 표시 →
//   라인 방향 잠근 상태에서 강한 단발 투사체 발사. 회피는 라인 이탈.
//   같은 메쉬 다른 행동 시연용 — Ranged 변형.
class ChargedShotAttackBehavior : public IAttackBehavior
{
public:
    ChargedShotAttackBehavior(ProjectileManager* pProjectileManager,
                              float fDamage          = 28.0f,
                              float fProjectileSpeed = 32.0f,
                              float fChargeTime      = 1.6f,
                              float fRecoveryTime    = 0.8f,
                              float fIndicatorLength = 34.0f,
                              float fIndicatorHalfW  = 1.2f);
    virtual ~ChargedShotAttackBehavior() = default;

	// 네트워크 연출 전용 모드
    void SetNetworkVisualOnly(bool bEnable) { m_bNetworkVisualOnly = bEnable; }

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override { return m_bFinished; }
    virtual void Reset() override;

    // 인디케이터 fill 진행도 계산용 — charge 시간 그대로
    virtual float GetTimeToHit() const override { return m_fChargeTime; }
    // 발사 후엔 인디케이터 숨김 — 잔존 라인이 "또 발사할까" 오해 방지
    virtual bool  ShouldShowHitZone() const override { return m_ePhase == Phase::Charging; }
    // ForwardBox 모양 강제 — preset 이 Circle 이어도 라인 형태로 표시
    virtual int   GetIndicatorTypeOverride() const override { return 4; /* IndicatorType::ForwardBox */ }
    virtual float GetIndicatorRadius() const override { return m_fIndicatorHalfW; }
    virtual float GetIndicatorLength() const override { return m_fIndicatorLength; }
    // 노랑→주황 톤으로 다른 공격과 시각 차별 (기본 빨강과 구분)
    virtual XMFLOAT3 GetIndicatorTint() const override { return XMFLOAT3(1.3f, 0.9f, 0.15f); }
    virtual bool  ShouldLoopAnim() const override { return false; }

private:
    enum class Phase { Charging, Fire, Recovery };
    void FireProjectile(EnemyComponent* pEnemy);

private:
    ProjectileManager* m_pProjectileManager = nullptr;

    float m_fDamage          = 28.0f;
    float m_fProjectileSpeed = 32.0f;
    float m_fChargeTime      = 1.6f;
    float m_fRecoveryTime    = 0.8f;
    float m_fIndicatorLength = 34.0f;
    float m_fIndicatorHalfW  = 1.2f;

    Phase    m_ePhase   = Phase::Charging;
    float    m_fTimer   = 0.0f;
    bool     m_bFired   = false;
    bool     m_bFinished = false;
    // Execute 시점에 잠근 방향 — windup 중 FaceTarget 호출 안 함 (라인 회피 가능)
    XMFLOAT3 m_xmf3LockedDir = { 0.0f, 0.0f, 1.0f };

    // 네트워크 연출 전용 모드
    bool m_bNetworkVisualOnly = false;
};
