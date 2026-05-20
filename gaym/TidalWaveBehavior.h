#pragma once
#include "ISkillBehavior.h"
#include "SkillData.h"
#include <vector>
#include <unordered_set>
#include <random>

class FluidSkillVFXManager;
class Scene;
class EnemyComponent;

// R 슬롯 (물결술사) — 해일: 방 전체를 가로지르는 거대 해일, 관통 + 전체 적 타격
class TidalWaveBehavior : public ISkillBehavior
{
public:
    TidalWaveBehavior();
    virtual ~TidalWaveBehavior() = default;

    virtual void SetVFXManager(FluidSkillVFXManager* mgr) override { m_pVFXManager = mgr; }
    virtual void SetScene(Scene* pScene)                  override { m_pScene = pScene; }

    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    void HitEnemiesInWave(float damage);
    void DropFoam();

    SkillData             m_SkillData;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*                m_pScene      = nullptr;
    GameObject*           m_pCaster     = nullptr;
    int                   m_vfxId       = -1;
    std::vector<int>      m_extraVFXIds;

    bool  m_bActive     = false;
    float m_damageMult  = 1.f;
    float m_elapsed     = 0.f;
    float m_foamTimer   = 0.f;
    float m_nextFoamAt  = 0.1f;  // 다음 거품 스폰까지 남은 시간 (랜덤 갱신)

    std::mt19937 m_rng;
    std::unordered_set<EnemyComponent*> m_hitEnemies;

    static constexpr float WAVE_DURATION  = 4.4f;
    static constexpr float WAVE_SPEED     = 20.0f;
    static constexpr float WAVE_HIT_DEPTH = 12.0f;
    static constexpr float WAVE_HALF_W    = 16.0f;
    static constexpr float WAVE_HALF_H    = 5.0f;
};
