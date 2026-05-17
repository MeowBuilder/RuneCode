#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"

class FluidSkillVFXManager;
class Scene;

// E 슬롯 (대지술사) — 대지의 갑옷: 즉시 보호막 부여 + 피해감소 2.5초
class EarthArmorBehavior : public ISkillBehavior
{
public:
    EarthArmorBehavior();
    virtual ~EarthArmorBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override { return m_bIsFinished; }
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    GameObject*           m_pCaster     = nullptr;

    bool  m_bIsFinished = true;
    float m_elapsed     = 0.f;
    int   m_auraVfxId   = -1;

    static constexpr float SHIELD_AMOUNT = 60.f;
    static constexpr float DR_RATIO      = 0.40f;
    static constexpr float DR_DURATION   = 2.5f;
};
