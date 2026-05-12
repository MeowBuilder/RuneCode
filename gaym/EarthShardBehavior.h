#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"

class ProjectileManager;

// RC 슬롯 (대지술사) — 대지 파편: 느리지만 묵직한 바위 파편, 착탄 시 강한 단타 폭발
class EarthShardBehavior : public ISkillBehavior
{
public:
    EarthShardBehavior();
    virtual ~EarthShardBehavior() = default;

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

    static constexpr float SHARD_SPEED = 22.0f;
    static constexpr float SHARD_SCALE = 1.6f;
};
