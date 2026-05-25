#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"
#include "SkillTypes.h"
#include <unordered_set>

class FluidSkillVFXManager;
class Scene;
class EnemyComponent;

// E 슬롯 (바람술사) — 돌풍 질주: 전방으로 돌진하며 주변 적에게 데미지 + 넉백
class GaleRushBehavior : public ISkillBehavior
{
public:
    GaleRushBehavior();
    virtual ~GaleRushBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual SkillCategory GetCategory() const override { return SkillCategory::Dash; }
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
    void HitEnemiesNearCaster(float damage);

    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    GameObject*           m_pCaster     = nullptr;
    int                   m_vfxId         = -1;
    int                   m_ringVfxId     = -1;
    int                   m_chargeVFXId   = -1;
    int                   m_enhanceAuraId = -1;

    bool  m_bActive    = false;
    float m_damageMult = 1.f;
    float m_elapsed    = 0.f;
    DirectX::XMFLOAT3 m_origin    = {};
    DirectX::XMFLOAT3 m_direction = {};

    std::unordered_set<EnemyComponent*> m_hitEnemies;  // 이번 대쉬에서 이미 맞은 적
    std::vector<int> m_trailVfxIds;                    // 트레일 VFX ID 목록 (Reset 시 정리)
    float m_trailTimer = 0.f;                          // 다음 트레일 스폰까지 남은 시간
    ElementType m_cachedElem = ElementType::None;      // Execute 시 결정된 원소 (Update trail에도 적용)
    int  m_channelGatherVfxId = -1;                    // 채널 중 시전자 gathering 이펙트
    bool m_bChannelMode       = false;

    // 돌진 대쉬 파라미터
    static constexpr float DURATION        = 0.45f;
    static constexpr float DASH_SPEED      = 55.f;
    static constexpr float HIT_RADIUS      = 5.5f;
    static constexpr float TRAIL_INTERVAL  = 0.05f;   // 배기 분사 스폰 주기 (밀집)
};
