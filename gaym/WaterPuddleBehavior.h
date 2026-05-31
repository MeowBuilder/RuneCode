#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"
#include <vector>
#include <unordered_set>

class FluidSkillVFXManager;
class Scene;
class EnemyComponent;

// Q 슬롯 (물결술사) — 물 웅덩이 장판: 범위 슬로우 + 지속 피해
class WaterPuddleBehavior : public ISkillBehavior
{
public:
    WaterPuddleBehavior();
    virtual ~WaterPuddleBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual SkillCategory GetCategory() const override { return SkillCategory::AoE; }
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
    virtual void OnEchoFire(GameObject* caster, const DirectX::XMFLOAT3& targetPos, float mult) override;
    virtual bool HasPostChannelWork() const override { return m_bActive || m_bPostChannelPuddles; }

private:
    struct ChannelPuddle {
        DirectX::XMFLOAT3 center      = {};
        int               puddleVfxId = -1;
        float             elapsed     = 0.f;
    };

    void TickPuddle(float deltaTime);
    void TickChannelPuddles(float deltaTime);
    void RemoveSlowFromAll();

    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    GameObject*           m_pCaster     = nullptr;
    int                   m_vfxId          = -1;
    int                   m_fallVfxId      = -1;
    int                   m_channelRainId  = -1;
    int                   m_chargeVFXId    = -1;
    int                   m_enhanceAuraId  = -1;

    bool                  m_bActive              = false;
    bool                  m_bPostChannelPuddles  = false;
    float                 m_elapsed              = 0.f;
    float                 m_tickTimer            = 0.f;
    float                 m_puddleTickTimer      = 0.f;
    float                 m_damageMult           = 1.f;
    DirectX::XMFLOAT3     m_center               = {};

    std::vector<ChannelPuddle>            m_channelPuddles;
    std::unordered_set<EnemyComponent*>   m_slowedEnemies;

    static constexpr float DURATION                  = 6.0f;
    static constexpr float FALL_DURATION             = 1.5f;
    static constexpr float PUDDLE_RADIUS             = 7.0f;
    static constexpr float TICK_INTERVAL             = 0.5f;
    static constexpr float DMG_PER_TICK              = 0.12f;
    static constexpr float SLOW_FACTOR               = 0.4f;
    static constexpr float CHANNEL_PUDDLE_DURATION   = 6.0f;
    static constexpr float CHANNEL_PUDDLE_RADIUS_MULT = 0.6f;
};
