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

    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

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
    int                   m_vfxId       = -1;
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
    static constexpr float POOL_RADIUS        = 5.0f;
    static constexpr float POOL_LIFETIME      = 4.0f;
    static constexpr float POOL_TICK_INTERVAL = 0.6f;
    static constexpr float POOL_DMG_MULT      = 0.2f;
};
