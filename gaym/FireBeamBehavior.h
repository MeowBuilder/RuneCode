#pragma once

#include "ISkillBehavior.h"
#include "SkillData.h"
#include "VFXTypes.h"      // EffectDef
#include <vector>
#include <string>

class FluidSkillVFXManager;
class Scene;
// E 슬롯 - 화염 빔 (Channel 방식, 키 누르는 동안 지속)
class FireBeamBehavior : public ISkillBehavior
{
public:
    FireBeamBehavior();
    FireBeamBehavior(const SkillData& customData);
    virtual ~FireBeamBehavior() = default;

    void SetVFXManager(FluidSkillVFXManager* mgr)    { m_pVFXManager   = mgr; }
    void SetScene(Scene* pScene)                     { m_pScene        = pScene; }
    // ISkillBehavior 인터페이스
    virtual SkillCategory GetCategory() const override { return SkillCategory::Beam; }
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }
    // 메아리 룬 재발동: 채널 세션 없이 즉시 단발 피해
    virtual void OnEchoFire(GameObject* caster, const DirectX::XMFLOAT3& targetPos, float mult) override;
    // 설치 룬 발동: 설치 위치에 고정, 가장 가까운 적 방향으로 빔
    virtual void OnPlaceTrigger(GameObject* caster, const DirectX::XMFLOAT3& trapPos, float mult) override;

private:
    uint32_t GetRuneFlags(GameObject* caster) const;
    // pDirOverride: null이면 caster의 Look 방향 사용, 지정 시 해당 방향으로 판정
    void     HitEnemiesInBeam(float damage, const DirectX::XMFLOAT3* pDirOverride = nullptr);

    static EffectDef BuildCoreBeamDef();
    static EffectDef BuildSwirlDef();
    static EffectDef BuildBurstDef();

    SkillData m_SkillData;
    bool m_bIsFinished = true;
    bool m_bIsActive = false;
    FluidSkillVFXManager* m_pVFXManager  = nullptr;
    Scene*                m_pScene       = nullptr;
    int m_vfxCoreId  = -1;  // 코어 빔 (직선 흐름)
    int m_vfxSwirlId = -1;  // 나선 공전
    int m_vfxBurstId = -1;  // 시작점 방사 스파크
    std::vector<int> m_subVFXIds; // 룬 서브 파티클 슬롯 IDs
    GameObject* m_pCaster = nullptr;
    XMFLOAT3 m_lastTargetPos = { 0.f, 0.f, 0.f };

    // 히트 판정용
    float m_damageMult   = 1.f;
    float m_hitTimer     = 0.f;
    // echo 모드: 방향 고정 + 자동 종료 타이머
    DirectX::XMFLOAT3 m_overrideDir  = { 0.f, 0.f, 0.f };  // non-zero → GetLook() 대신 사용
    DirectX::XMFLOAT3 m_echoOrigin   = { 0.f, 0.f, 0.f };  // 발동 시점 위치 고정 (캐릭터 추적 X)
    float             m_echoTimeLeft = 0.f;                  // > 0 동안 echo 빔 실행 중
    static constexpr float HIT_INTERVAL  = 0.2f;
    static constexpr float BEAM_RADIUS   = 2.0f;
    static constexpr float BEAM_RANGE    = 20.0f;
    static constexpr float ECHO_DURATION = 1.5f;  // echo 빔 지속 시간 (초)
};
