#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"
#include <unordered_set>

class FluidSkillVFXManager;
class Scene;
class EnemyComponent;

// Q 슬롯 (바람술사) — 진공 커터: 좁고 빠른 바람날이 직선 관통, 경로 모든 적 타격
class WindCutterBehavior : public ISkillBehavior
{
public:
    WindCutterBehavior();
    virtual ~WindCutterBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual SkillCategory GetCategory() const override { return SkillCategory::Wave; }
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    uint32_t GetRuneFlags(GameObject* caster) const;
    void HitEnemiesInCutter(float damage);

    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    int                   m_vfxId       = -1;

    bool  m_bActive    = false;
    float m_damageMult = 1.f;
    float m_elapsed    = 0.f;
    DirectX::XMFLOAT3 m_origin    = {};
    DirectX::XMFLOAT3 m_direction = {};

    std::unordered_set<EnemyComponent*> m_hitEnemies;

    // 빠르고 좁은 관통기 — 모든 적 동시 히트
    static constexpr float DURATION   = 0.7f;
    static constexpr float SPEED      = 35.0f;
    static constexpr float HALF_W     = 2.0f;
    static constexpr float HALF_H     = 2.5f;
    static constexpr float HIT_DEPTH  = 9.0f;
};
