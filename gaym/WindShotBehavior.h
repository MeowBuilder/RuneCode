#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"

class ProjectileManager;

// RC 슬롯 (바람술사) — 바람 탄환: 빠른 속도로 날아가는 관통 바람 투사체
class WindShotBehavior : public ISkillBehavior
{
public:
    WindShotBehavior();
    virtual ~WindShotBehavior() = default;

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

    static constexpr float SHOT_SPEED = 50.0f;
    static constexpr float SHOT_SCALE = 0.8f;
};
