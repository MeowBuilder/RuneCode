#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"
#include <unordered_set>

class FluidSkillVFXManager;
class Scene;
class EnemyComponent;

// E 슬롯 (바람술사) — 돌풍 질주: 전방 넓은 콘 범위 즉발 폭풍, 적 넉백
class GaleRushBehavior : public ISkillBehavior
{
public:
    GaleRushBehavior();
    virtual ~GaleRushBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    void HitEnemiesInCone(float damage);

    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    int                   m_vfxId       = -1;
    int                   m_ringVfxId   = -1;

    bool  m_bActive    = false;
    float m_damageMult = 1.f;
    float m_elapsed    = 0.f;
    bool  m_bHit       = false;
    DirectX::XMFLOAT3 m_origin    = {};
    DirectX::XMFLOAT3 m_direction = {};

    // 전방 콘 판정: 거리 MAX_RANGE + 반각 CONE_HALF_ANGLE
    static constexpr float DURATION         = 0.5f;
    static constexpr float MAX_RANGE        = 15.0f;
    static constexpr float CONE_HALF_ANGLE  = 50.0f;  // 도
};
