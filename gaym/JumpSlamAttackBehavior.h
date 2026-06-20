#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

// Jump slam attack: boss jumps to target location and creates AoE damage on landing
class JumpSlamAttackBehavior : public IAttackBehavior
{
public:
    JumpSlamAttackBehavior(float fDamage = 25.0f,
                           float fJumpHeight = 10.0f,
                           float fJumpDuration = 0.6f,
                           float fSlamRadius = 7.0f,
                           float fWindupTime = 0.3f,
                           float fRecoveryTime = 0.5f,
                           bool bTrackTarget = true,  // Jump to where target is
                           float fCameraShakeIntensity = 0.0f,  // 0 = 쉐이크 없음
                           float fCameraShakeDuration  = 0.35f,
                           const char* pClipOverride   = nullptr, // 특정 애니 클립 지정
                           float fAnimPlaybackSpeed    = 0.0f);   // 0 이하 = 기본값 유지
    virtual ~JumpSlamAttackBehavior() = default;

    // 네트워크 모드: 데미지는 서버 권위, 클라는 점프 아크/슬램 VFX/인디케이터만.
    void SetNetworkVisualOnly(bool b) { m_bNetworkVisualOnly = b; }

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    // 클립 override (있을 때만)
    virtual const char* GetAnimClipName() const override { return m_strClipOverride ? m_strClipOverride : ""; }
    // 지면 인디케이터 반경 = slam 실제 반경
    virtual float GetIndicatorRadius() const override { return m_fSlamRadius; }
    // windup 이 끝나고 slam 순간에 데미지 발생 → fill 는 windup 동안 진행
    virtual float GetTimeToHit() const override { return m_fWindupTime + m_fJumpDuration; }
    // 슬램 애니는 loop X — 클립 끝(슬램 자세) 에 freeze 되어 데미지 임팩트와 자세 일치.
    //   loop on 시 클립 끝나자마자 wind-up 모션이 다시 재생돼 임팩트 자세와 어긋남.
    virtual bool ShouldLoopAnim() const override { return false; }

private:
    void DealSlamDamage(EnemyComponent* pEnemy);
    void SpawnDebris(EnemyComponent* pEnemy);
    void UpdateDebris(float dt);
    void CleanupDebris();

private:
    // Parameters
    float m_fDamage = 25.0f;
    float m_fJumpHeight = 10.0f;
    float m_fJumpDuration = 0.6f;
    float m_fSlamRadius = 7.0f;
    float m_fWindupTime = 0.3f;
    float m_fRecoveryTime = 0.5f;
    bool m_bTrackTarget = true;
    float m_fCameraShakeIntensity = 0.0f;
    float m_fCameraShakeDuration  = 0.35f;
    const char* m_strClipOverride = nullptr;
    float m_fAnimPlaybackSpeed    = 0.0f;

    // Runtime state
    enum class Phase { Windup, Jump, Slam, Recovery };
    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;
    float m_fOriginalY = 0.0f;
    XMFLOAT3 m_xmf3StartPosition = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 m_xmf3TargetPosition = { 0.0f, 0.0f, 0.0f };
    bool m_bSlamDealt = false;
    bool m_bFinished = false;
    bool m_bNetworkVisualOnly = false;

    // 땅 파편 — slam 임팩트 시 바위 조각 비산 (간단 VFX)
    struct DebrisPiece
    {
        class GameObject* pObj = nullptr;
        XMFLOAT3 velocity = { 0, 0, 0 };
        XMFLOAT3 rotSpeed = { 0, 0, 0 };
        float    lifetime = 0.0f;
        float    age      = 0.0f;
    };
    std::vector<DebrisPiece> m_vDebris;
    class Scene* m_pDebrisScene = nullptr;  // Cleanup 시 MarkForDeletion 에 사용
};
