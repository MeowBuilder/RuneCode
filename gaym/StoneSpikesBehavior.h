#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"
#include "SkillTypes.h"
#include <vector>

class FluidSkillVFXManager;
class Scene;

// Q 슬롯 (대지술사) — 바위 기둥: 플레이어 → 타겟 방향으로 순차 솟아오르는 석회 기둥들
class StoneSpikesBehavior : public ISkillBehavior
{
public:
    StoneSpikesBehavior();
    virtual ~StoneSpikesBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual SkillCategory GetCategory() const override { return SkillCategory::AoE; }
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    void TriggerSpike(int idx);
    void HitEnemiesAtSpike(const DirectX::XMFLOAT3& center, float damage);

    struct SpikeEntry
    {
        DirectX::XMFLOAT3 pos;
        float triggerAt;
        bool  triggered = false;
        int   vfxId     = -1;
    };

    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;

    bool  m_bActive    = false;
    float m_damageMult = 1.f;
    float m_elapsed    = 0.f;
    ElementType m_cachedElem = ElementType::None;
    GameObject* m_pCaster    = nullptr;

    std::vector<SpikeEntry> m_spikes;

    static constexpr int   SPIKE_COUNT    = 4;
    static constexpr float SPIKE_INTERVAL = 0.28f;  // 기둥 간 시간차
    static constexpr float SPIKE_SPACING  = 5.0f;   // 기둥 간 거리
    static constexpr float HIT_RADIUS     = 3.5f;
    static constexpr float HIT_HEIGHT     = 5.0f;
    static constexpr float TOTAL_DURATION = SPIKE_COUNT * SPIKE_INTERVAL + 0.5f;
};
