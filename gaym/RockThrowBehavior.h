#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"

class ProjectileManager;

// E 슬롯 (대지술사) — 바위 투척: 느리고 무거운 바위 투척, 착탄 시 넓은 범위 피해
class RockThrowBehavior : public ISkillBehavior
{
public:
    RockThrowBehavior();
    virtual ~RockThrowBehavior() = default;

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

    static constexpr float ROCK_SPEED = 16.0f;
    static constexpr float ROCK_SCALE = 2.2f;
};
