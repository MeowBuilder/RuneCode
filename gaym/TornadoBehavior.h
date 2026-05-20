#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"
#include <vector>
#include <random>

class FluidSkillVFXManager;
class Scene;

// R 슬롯 (바람술사) — 대형 토네이도: 타겟 지점에 생성, 천천히 이동하며 지속 흡입 + 피해
class TornadoBehavior : public ISkillBehavior
{
public:
    TornadoBehavior();
    virtual ~TornadoBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    void DamageEnemiesNearby(float dt);
    void UpdateMovement(float dt);

    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    GameObject*           m_pCaster     = nullptr;
    int                   m_vfxId       = -1;

    bool  m_bActive    = false;
    float m_damageMult = 1.f;
    float m_elapsed    = 0.f;
    float m_tickTimer  = 0.f;
    float m_dirTimer   = 0.f;

    DirectX::XMFLOAT3 m_pos    = {};
    DirectX::XMFLOAT3 m_moveDir = { 1.f, 0.f, 0.f };

    std::mt19937 m_rng;

    static constexpr float DURATION      = 6.0f;
    static constexpr float DMG_RADIUS    = 4.0f;
    static constexpr float TICK_INTERVAL = 0.5f;
    static constexpr float DMG_PER_TICK  = 0.12f;
    static constexpr float MOVE_SPEED    = 2.5f;
    static constexpr float DIR_INTERVAL  = 1.8f;
};
