#pragma once

#include "ISkillBehavior.h"
#include "SkillData.h"
#include "VFXTypes.h"      // EffectDef
#include <vector>
#include <string>

class FluidSkillVFXManager;
class Scene;
class DecalManager;

// E 슬롯 - 화염 빔 (Channel 방식, 키 누르는 동안 지속)
class FireBeamBehavior : public ISkillBehavior
{
public:
    FireBeamBehavior();
    FireBeamBehavior(const SkillData& customData);
    virtual ~FireBeamBehavior() = default;

    void SetVFXManager(FluidSkillVFXManager* mgr)    { m_pVFXManager   = mgr; }
    void SetScene(Scene* pScene)                     { m_pScene        = pScene; }
    void SetDecalManager(DecalManager* m) override   { m_pDecalManager = m; }

    // ISkillBehavior 인터페이스
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    uint32_t GetRuneFlags(GameObject* caster) const;
    void     HitEnemiesInBeam(float damage);

    static EffectDef BuildCoreBeamDef();
    static EffectDef BuildSwirlDef();
    static EffectDef BuildBurstDef();

    SkillData m_SkillData;
    bool m_bIsFinished = true;
    bool m_bIsActive = false;
    FluidSkillVFXManager* m_pVFXManager  = nullptr;
    Scene*                m_pScene       = nullptr;
    DecalManager*         m_pDecalManager = nullptr;
    int m_vfxCoreId  = -1;  // 코어 빔 (직선 흐름)
    int m_vfxSwirlId = -1;  // 나선 공전
    int m_vfxBurstId = -1;  // 시작점 방사 스파크
    std::vector<int> m_subVFXIds; // 룬 서브 파티클 슬롯 IDs
    GameObject* m_pCaster = nullptr;
    XMFLOAT3 m_lastTargetPos = { 0.f, 0.f, 0.f };

    // 히트 판정용
    float m_damageMult   = 1.f;
    float m_hitTimer     = 0.f;
    static constexpr float HIT_INTERVAL  = 0.2f;  // 다단히트 간격 (초)
    static constexpr float BEAM_RADIUS   = 2.0f;  // 빔 판정 반경 (m)
    static constexpr float BEAM_RANGE    = 20.0f; // 빔 최대 사거리 (m)
};
