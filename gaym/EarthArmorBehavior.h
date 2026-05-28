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

    virtual SkillCategory GetCategory() const override { return SkillCategory::AoE; }
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void OnChargeBegin(GameObject* caster) override;
    virtual void OnChargeUpdate(GameObject* caster, float chargeRatio) override;
    virtual void OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) override;
    virtual void OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float tickMult) override;
    virtual void OnChannelEnd(GameObject* caster) override;
    virtual bool HasPostChannelWork() const override { return true; }
    virtual void OnEnhanceActivate(GameObject* caster) override;
    virtual void OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override { return m_bIsFinished; }
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    GameObject*           m_pCaster     = nullptr;

    bool        m_bIsFinished      = true;
    bool        m_bChannelMode     = false;
    bool        m_bPostChannel     = false;  // OnChannelEnd → Execute 버스트 전달용
    float       m_elapsed          = 0.f;
    ElementType m_cachedElem       = ElementType::None;
    int         m_auraVfxId        = -1;
    int         m_shieldVfxId      = -1;
    int         m_enhanceAuraId    = -1;
    int         m_chargeVFXId      = -1;
    int         m_channelAmbientId = -1;

    static constexpr float INVINCIBLE_DURATION       = 2.5f;
    static constexpr float CHANNEL_INVINCIBLE_BUFFER = 0.5f; // 틱 간격(0.2s) 대비 여유분
    static constexpr float CHANNEL_BURST_RADIUS      = 6.0f;
};
