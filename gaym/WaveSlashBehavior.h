#pragma once

#include "ISkillBehavior.h"
#include "SkillData.h"
#include <vector>
#include <unordered_set>

class FluidSkillVFXManager;
class Scene;
class EnemyComponent;
class DecalManager;

// Q 슬롯 - 웨이브 슬래시 (파도 본체 단타 + 불꽃 자국 DoT)
class WaveSlashBehavior : public ISkillBehavior
{
public:
    WaveSlashBehavior();
    WaveSlashBehavior(const SkillData& customData);
    virtual ~WaveSlashBehavior() = default;

    void SetVFXManager(FluidSkillVFXManager* mgr)    { m_pVFXManager   = mgr; }
    void SetScene(Scene* pScene)                     { m_pScene        = pScene; }
    void SetDecalManager(DecalManager* m) override   { m_pDecalManager = m; }

    // ISkillBehavior 인터페이스
    virtual SkillCategory GetCategory() const override { return SkillCategory::Wave; }
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void OnChargeBegin(GameObject* caster) override;
    virtual void OnChargeUpdate(GameObject* caster, float chargeRatio) override;
    virtual void OnEnhanceActivate(GameObject* caster) override;
    virtual void OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) override;
    virtual void OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) override;
    virtual void OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float tickMult) override;
    virtual void OnChannelEnd(GameObject* caster) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    uint32_t GetRuneFlags(GameObject* caster) const;

    // 파도 본체: 처음 닿은 적만 단타
    void HitEnemiesInWave(float damage);

    // 불꽃 자국: 파도가 지나간 위치에 DoT 존 생성/업데이트
    void DropFireTrail();
    void UpdateFireTrail(float deltaTime);

    // 불꽃 자국 존
    struct FireZone
    {
        DirectX::XMFLOAT3 center;
        float lifetime;
        float tickTimer;
        int   trailVfxId = -1;  // 불꽃 자국 VFX 슬롯 ID
    };

    SkillData  m_SkillData;
    bool       m_bIsFinished = true;
    bool       m_bWaveActive = false;
    GameObject*           m_pCaster      = nullptr;
    FluidSkillVFXManager* m_pVFXManager  = nullptr;
    Scene*                m_pScene       = nullptr;
    DecalManager*         m_pDecalManager = nullptr;
    int        m_vfxId            = -1;
    int        m_channelAmbientId = -1;
    int        m_chargeVFXId      = -1;
    int        m_enhanceAuraId    = -1;

    float      m_damageMult        = 1.f;
    float      m_waveElapsed       = 0.f;
    float      m_trailDropTimer    = 0.f;

    std::unordered_set<EnemyComponent*> m_hitEnemies;  // 파도 본체에 이미 히트된 적
    std::vector<FireZone>               m_fireTrail;   // 활성 불꽃 자국 존
    std::vector<int>                    m_extraVFXIds; // 다중 원소 추가 VFX 슬롯

    // 파도 본체 히트 판정
    // WAVE_PARTICLE_SPEED: Q_WaveSlash sph.maxParticleSpeed(20)에 맞춰 실제 파티클 선두 속도 사용
    // waveSpeed(10)는 타이머 전용 — 파티클은 push force로 훨씬 빠르게 이동
    static constexpr float CHANNEL_RANGE        = 10.0f;  // 채널 미니 슬래시 사거리
    static constexpr float WAVE_DURATION        = 2.0f;   // waveMaxDist(20) / waveSpeed(10)
    static constexpr float WAVE_PARTICLE_SPEED  = 20.0f;  // 히트 슬랩 선두 속도 (m/s) ← 조정 가능
    static constexpr float WAVE_HIT_DEPTH       = 6.0f;   // 히트 슬랩 두께 (m)
    static constexpr float WAVE_HALF_W          = 5.0f;   // = Q_WaveSlash sph.waveHalfW
    static constexpr float WAVE_HALF_H          = 3.0f;   // 수직 판정 여유

    // 불꽃 자국
    static constexpr float TRAIL_DROP_INTERVAL = 0.15f;  // 존 생성 간격 (초)
    static constexpr float TRAIL_ZONE_RADIUS   = 4.0f;   // 존 반경 (m)
    static constexpr float TRAIL_LIFETIME      = 3.0f;   // 존 지속 시간 (초)
    static constexpr float TRAIL_TICK_INTERVAL = 0.5f;   // DoT 간격 (초)
    static constexpr float TRAIL_DMG_MULT      = 0.25f;  // 자국 데미지 배율 (파도 본체 대비)
};
