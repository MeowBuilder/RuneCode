#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"
#include <vector>

class FluidSkillVFXManager;
class Scene;

// E 슬롯 (물결술사) — 물 소용돌이: 지정 지점에 소용돌이 생성, 주변 적 흡입 + 지속 피해
class WaterVortexBehavior : public ISkillBehavior
{
public:
    WaterVortexBehavior();
    virtual ~WaterVortexBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual SkillCategory GetCategory() const override { return SkillCategory::Summon; }
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
    void PullAndDamageEnemies(float deltaTime);

    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    GameObject*           m_pCaster     = nullptr;
    int                   m_vfxId         = -1;
    int                   m_chargeVFXId   = -1;
    int                   m_enhanceAuraId = -1;
    std::vector<int>      m_extraVFXIds;

    bool  m_bActive    = false;
    float m_damageMult = 1.f;
    float m_elapsed    = 0.f;
    float m_tickTimer  = 0.f;
    DirectX::XMFLOAT3 m_center = {};

    static constexpr float DURATION      = 4.0f;
    static constexpr float PULL_RADIUS   = 9.0f;
    static constexpr float PULL_FORCE    = 10.0f;   // 단위/초, 중심 방향
    static constexpr float TICK_INTERVAL = 0.4f;
    static constexpr float DMG_PER_TICK  = 0.15f;   // 스킬 데미지 대비 비율
};
