#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

class GameObject;
class Scene;
class CRoom;

// 회오리 장판: 보스 주변 N 지점에 토네이도 소환 → 지속 틱 데미지 + 이동 통로 좁힘
//   Windup: 각 지점에 빨간 경고 링 표시 + Wind_TornadoWarning VFX
//   Active: Demon_Tornado VFX 스폰, 지속 동안 반경 안 플레이어에게 틱 데미지
//   Recovery: VFX/인디케이터 정리
class TornadoFieldAttackBehavior : public IAttackBehavior
{
public:
    TornadoFieldAttackBehavior(
        int   nTornadoCount       = 4,
        float fTickDamage         = 18.0f,
        float fTickInterval       = 0.45f,
        float fTornadoRadius      = 5.0f,    // AoE 반경 (토네이도 굵기)
        float fSpawnMinRadius     = 12.0f,   // 보스로부터 최소 거리
        float fSpawnMaxRadius     = 28.0f,
        float fWindupTime         = 1.8f,
        float fActiveDuration     = 4.0f,    // 토네이도 지속 시간
        float fRecoveryTime       = 1.0f,
        float fCameraShakeIntensity = 1.0f,
        float fCameraShakeDuration  = 0.4f
    );
    virtual ~TornadoFieldAttackBehavior() = default;

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

    virtual const char* GetAnimClipName() const override { return "attack2"; }
    virtual float GetTimeToHit() const override { return m_fWindupTime; }
    virtual bool  ShouldShowHitZone() const override { return false; }
    virtual bool  ShouldLoopAnim() const override { return false; }

private:
    struct TornadoInstance
    {
        XMFLOAT3    pos = { 0, 0, 0 };
        GameObject* pBorder = nullptr;     // 경고 링 (테두리)
        GameObject* pFill   = nullptr;     // 차오름 fill
        int         nWarningVFX = -1;       // Wind_TornadoWarning 슬롯
        int         nTornadoVFX = -1;       // Demon_Tornado 슬롯
    };

    void SpawnIndicators(EnemyComponent* pEnemy);
    void ActivateTornadoes(EnemyComponent* pEnemy);
    void TickDamage(float dt, EnemyComponent* pEnemy);
    void CleanupAll();

    // Parameters
    int   m_nTornadoCount;
    float m_fTickDamage;
    float m_fTickInterval;
    float m_fTornadoRadius;
    float m_fSpawnMinRadius;
    float m_fSpawnMaxRadius;
    float m_fWindupTime;
    float m_fActiveDuration;
    float m_fRecoveryTime;
    float m_fCameraShakeIntensity;
    float m_fCameraShakeDuration;

    // Runtime
    enum class Phase { Windup, Active, Recovery };
    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;
    float m_fTickTimer = 0.0f;
    bool  m_bFinished = false;
    bool  m_bAnimReturnedToIdle = false;

    std::vector<TornadoInstance> m_vTornadoes;
    Scene* m_pScene = nullptr;
    CRoom* m_pRoom  = nullptr;
};
