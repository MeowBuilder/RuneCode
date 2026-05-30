#pragma once

#include "ISkillBehavior.h"
#include "SkillData.h"
#include <vector>
#include <random>

class FluidSkillVFXManager;
class Scene;
struct SmallMeteorData
{
    XMFLOAT3 spawnPos     = {};
    XMFLOAT3 targetPos    = {};
    float    fallDuration = 1.4f;
    float    elapsed      = 0.f;
    int      trailVfxId   = -1;
    bool     impacted     = false;
};

// R 슬롯 — 메테오 샤워 (소형 메테오 다수 랜덤 낙하 → 최종 대형 메테오 전체 범위 피해)
class MeteorBehavior : public ISkillBehavior
{
public:
    MeteorBehavior();
    MeteorBehavior(const SkillData& customData);
    virtual ~MeteorBehavior() = default;

    virtual SkillCategory GetCategory() const override { return SkillCategory::AoE; }
    void SetVFXManager(FluidSkillVFXManager* mgr)    { m_pVFXManager    = mgr; }
    void SetScene(Scene* pScene)                     { m_pScene         = pScene; }
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void OnChargeBegin(GameObject* caster) override;
    virtual void OnChargeUpdate(GameObject* caster, float chargeRatio) override;
    virtual void OnEnhanceActivate(GameObject* caster) override;
    virtual void OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) override;
    virtual void OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) override;
    virtual void OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float tickMult) override;
    virtual void OnChannelComplete(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) override;
    virtual void OnChannelEnd(GameObject* caster) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual void Render(ID3D12GraphicsCommandList* pCmdList) override {}
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }
    virtual bool HasPostChannelWork() const override { return m_bPostChannel; }

private:
    void SpawnSmallMeteor();
    void SpawnSmallMeteorAt(const DirectX::XMFLOAT3& targetPos);
    void OnSmallImpact(SmallMeteorData& meteor);
    void SpawnFinalMeteor();
    void OnFinalImpact();
    void ApplyExplosionDamage(float damage, float radius, const XMFLOAT3& center, bool bTriggerStagger);

    SkillData             m_SkillData;
    bool                  m_bIsFinished = true;
    FluidSkillVFXManager* m_pVFXManager    = nullptr;
    Scene*                m_pScene         = nullptr;
    GameObject*           m_pCaster        = nullptr;
    ElementType           m_elementType = ElementType::Fire;

    bool m_bChannelMode  = false;  // 채널 룬 발동 중 (샤워 스킵, 틱마다 낙하 메테오)
    bool m_bPostChannel  = false;  // 채널 종료 후 낙하 중 메테오 마무리 처리 중

    int m_chargeVFXId   = -1;
    int m_enhanceAuraId = -1;

    // 최종 메테오 VFX IDs
    int m_finalTrailId  = -1;
    int m_finalOuterId  = -1;
    int m_finalImpactId = -1;
    int m_finalGroundId = -1;

    // 상태
    XMFLOAT3 m_targetPos      = {};
    float    m_damageMult     = 1.f;
    float    m_elapsed        = 0.f;
    int      m_meteorsSpawned = 0;
    bool     m_bFinalSpawned  = false;
    bool     m_bFinalImpacted = false;
    XMFLOAT3 m_finalSpawnPos  = {};
    float    m_finalElapsed   = 0.f;

    // 소형 메테오 목록
    std::vector<SmallMeteorData> m_smallMeteors;

    // 랜덤
    std::mt19937 m_rng;

    static constexpr int   SHOWER_COUNT         = 6;
    static constexpr float SHOWER_INTERVAL      = 0.5f;   // 소형 스폰 간격(초)
    static constexpr float SMALL_SPAWN_HEIGHT   = 40.f;
    static constexpr float SMALL_FALL_SPEED     = 28.f;
    static constexpr float SMALL_EXPLODE_RADIUS = 5.0f;
    static constexpr float SMALL_DAMAGE_RATIO   = 0.20f;  // 총 데미지 대비 각 소형 비율
    static constexpr float SMALL_SCATTER_RADIUS = 12.f;   // 타겟 주변 산포 반경
    static constexpr float FINAL_SPAWN_HEIGHT   = 60.f;
    static constexpr float FINAL_FALL_SPEED     = 22.f;
    static constexpr float FINAL_DELAY          = 0.5f;   // 마지막 소형 스폰 후 최종 대기
    static constexpr float EXPLODE_RADIUS       = 10.0f;
    static constexpr float METEOR_FORWARD_DIST  = 15.f;
};
