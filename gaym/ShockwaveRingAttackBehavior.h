#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>

using namespace DirectX;

class GameObject;
class Scene;
class CRoom;

// 충격파 링: 보스 중심에서 원형 충격파가 확장
//   Windup: 최종 반경에 빨간 경고 링 + fill 차오름
//   Expand: 진행 wave 가 안→밖으로 이동, wave front 에 닿은 플레이어 단발 데미지
//   Recovery: 정리
// 회피 = 시작 전 멀리 벗어나기 / wave 통과 후 안쪽 머물기 — 위치 통제 강요
class ShockwaveRingAttackBehavior : public IAttackBehavior
{
public:
    ShockwaveRingAttackBehavior(
        float fDamage             = 90.0f,
        float fMaxRadius          = 35.0f,    // 충격파 최종 반경
        float fWaveThickness      = 4.0f,     // wave 두께 (안쪽 반경 ~ 바깥쪽 반경)
        float fWindupTime         = 1.6f,
        float fExpandDuration     = 1.0f,     // 0 → MaxRadius 까지
        float fRecoveryTime       = 1.0f,
        float fCameraShakeIntensity = 2.0f,
        float fCameraShakeDuration  = 0.5f
    );
    virtual ~ShockwaveRingAttackBehavior() = default;

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

    virtual const char* GetAnimClipName() const override { return "attack1"; }
    virtual float GetTimeToHit() const override { return m_fWindupTime; }
    virtual bool  ShouldShowHitZone() const override { return false; }
    virtual bool  ShouldLoopAnim() const override { return false; }

private:
    void SpawnIndicators(EnemyComponent* pEnemy);
    void FireRingVFX(EnemyComponent* pEnemy);
    void UpdateWaveFront(float dt, EnemyComponent* pEnemy);
    void CleanupAll();

    float m_fDamage;
    float m_fMaxRadius;
    float m_fWaveThickness;
    float m_fWindupTime;
    float m_fExpandDuration;
    float m_fRecoveryTime;
    float m_fCameraShakeIntensity;
    float m_fCameraShakeDuration;

    enum class Phase { Windup, Expand, Recovery };
    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;
    float m_fCurrentWaveRadius = 0.0f;
    bool  m_bFinished = false;
    bool  m_bAnimReturnedToIdle = false;

    XMFLOAT3 m_xmf3BossCenter = { 0, 0, 0 };

    // 시각적 인디케이터
    GameObject* m_pBorder = nullptr;     // 최종 반경 경고 링
    GameObject* m_pFill   = nullptr;     // windup 동안 차오름
    GameObject* m_pWaveBorder = nullptr; // expand 동안 wave front (안쪽 inner ring)
    int         m_nRingVFX = -1;

    // 동일 플레이어 중복 타격 방지
    std::vector<class GameObject*> m_vAlreadyHit;

    Scene* m_pScene = nullptr;
    CRoom* m_pRoom  = nullptr;
};
