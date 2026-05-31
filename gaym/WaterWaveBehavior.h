#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"
#include <vector>
#include <unordered_set>

class FluidSkillVFXManager;
class Scene;
class EnemyComponent;

// Q 슬롯 (물결술사) — 물결 슬래시: 넓은 물 파도 전진 + 물 웅덩이 DoT
class WaterWaveBehavior : public ISkillBehavior
{
public:
    WaterWaveBehavior();
    virtual ~WaterWaveBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual SkillCategory GetCategory() const override { return SkillCategory::Wave; }
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;

    // 발동 방식 훅
    virtual void OnChargeBegin(GameObject* caster) override;
    virtual void OnChargeUpdate(GameObject* caster, float chargeRatio) override;
    virtual void OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) override;
    virtual void OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float tickMult) override;
    virtual void OnChannelEnd(GameObject* caster) override;
    virtual void OnEnhanceActivate(GameObject* caster) override;
    virtual void OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) override;

    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }
    virtual void OnEchoFire(GameObject* caster, const DirectX::XMFLOAT3& targetPos, float mult) override;

private:
    uint32_t GetRuneFlags(GameObject* caster) const;
    void HitEnemiesInWave(float damage);
    void DropWaterPool();
    void UpdateWaterPools(float deltaTime);

    struct WaterPool
    {
        DirectX::XMFLOAT3 center;
        float lifetime;
        float tickTimer;
        int   vfxId = -1;
    };

    SkillData             m_SkillData;
    GameObject*           m_pCaster     = nullptr;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    int                   m_vfxId           = -1;
    int                   m_chargeVFXId     = -1;  // 차지 프리뷰 VFX
    int                   m_channelAmbientId = -1; // 채널 주변 강우 VFX
    int                   m_enhanceAuraId   = -1;  // 강화 오라 VFX
    std::vector<int>      m_extraVFXIds;

    bool  m_bWaveActive   = false;
    float m_damageMult    = 1.f;
    float m_waveElapsed   = 0.f;
    float m_poolDropTimer = 0.f;

    std::unordered_set<EnemyComponent*> m_hitEnemies;
    std::vector<WaterPool>              m_waterPools;

    static constexpr float WAVE_DURATION      = 2.25f;  // = VFX waveMaxDist(18)/waveSpeed(8)
    static constexpr float WAVE_SPEED         = 18.0f;
    static constexpr float WAVE_HIT_DEPTH     = 7.0f;
    static constexpr float WAVE_HALF_W        = 7.0f;
    static constexpr float WAVE_HALF_H        = 3.0f;
    static constexpr float POOL_DROP_INTERVAL = 0.2f;
    static constexpr float MINI_WAVE_RANGE    = 10.0f;  // 채널 미니 웨이브 사거리
    static constexpr float DROPLET_RADIUS     = 4.0f;
    static constexpr float POOL_RADIUS        = 5.0f;
    static constexpr float POOL_LIFETIME      = 4.0f;
    static constexpr float POOL_TICK_INTERVAL = 0.6f;
    static constexpr float POOL_DMG_MULT      = 0.2f;
};
