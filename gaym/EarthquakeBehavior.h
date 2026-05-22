#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"
#include "SkillTypes.h"
#include <vector>
#include <unordered_set>

class FluidSkillVFXManager;
class Scene;
class EnemyComponent;

// R 슬롯 (대지술사) — 지진: 플레이어 중심 전방위 충격파 다중 링, 범위 내 전체 적 타격
class EarthquakeBehavior : public ISkillBehavior
{
public:
    EarthquakeBehavior();
    virtual ~EarthquakeBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual SkillCategory GetCategory() const override { return SkillCategory::AoE; }
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void OnChargeBegin(GameObject* caster) override;
    virtual void OnChargeUpdate(GameObject* caster, float chargeRatio) override;
    virtual void OnEnhanceActivate(GameObject* caster) override;
    virtual void OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) override;
    virtual void OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float tickMult) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    void LaunchWave(int idx);
    void UpdateWaves(float dt);

    struct ShockRing
    {
        float  launchAt;
        float  radius   = 0.f;
        bool   launched = false;
        bool   hit      = false;
        int    vfxId    = -1;
    };

    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    int                   m_burstVfxId  = -1;

    int   m_chargeVFXId   = -1;
    int   m_enhanceAuraId = -1;
    bool  m_bActive    = false;
    float m_damageMult = 1.f;
    float m_elapsed    = 0.f;
    DirectX::XMFLOAT3 m_epicenter = {};
    ElementType m_cachedElem = ElementType::None;
    GameObject* m_pCaster    = nullptr;

    std::vector<ShockRing>              m_rings;
    std::unordered_set<EnemyComponent*> m_hitEnemies;

    static constexpr int   RING_COUNT      = 3;
    static constexpr float RING_INTERVAL   = 0.35f;
    static constexpr float RING_SPEED      = 12.0f;   // 확장 속도 (units/s)
    static constexpr float RING_MAX_RADIUS = 20.0f;
    static constexpr float RING_THICKNESS  = 4.0f;
    static constexpr float RING_HEIGHT     = 4.5f;
    static constexpr float TOTAL_DURATION  = RING_COUNT * RING_INTERVAL + RING_MAX_RADIUS / RING_SPEED + 0.3f;
};
