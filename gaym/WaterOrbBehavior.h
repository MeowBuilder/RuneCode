#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"

class ProjectileManager;

// RC 슬롯 (물결술사) — 물 구체: 느리게 날아가다 착탄 시 넓은 범위 물 폭발
class WaterOrbBehavior : public ISkillBehavior
{
public:
    WaterOrbBehavior();
    virtual ~WaterOrbBehavior() = default;

    void SetProjectileManager(ProjectileManager* mgr) { m_pProjectileManager = mgr; }

    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override {}
    virtual bool IsFinished() const override { return m_bIsFinished; }
    virtual void Reset() override { m_bIsFinished = true; }
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    SkillData          m_SkillData;
    bool               m_bIsFinished       = true;
    ProjectileManager* m_pProjectileManager = nullptr;

    static constexpr float ORB_SPEED  = 20.0f;
    static constexpr float ORB_SCALE  = 1.4f;
};
