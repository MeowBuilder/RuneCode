#pragma once

#include <DirectXMath.h>
#include "SkillTypes.h"

class GameObject;
struct SkillData;
class FluidSkillVFXManager;
class Scene;

// Strategy pattern interface for skill execution
class ISkillBehavior
{
public:
    virtual ~ISkillBehavior() = default;

    // Execute the skill - called when skill is activated
    // caster: The GameObject using the skill
    // targetPosition: World position where skill is aimed
    // damageMultiplier: Multiplier for damage (default 1.0, can be modified by charge/enhance)
    //   Special values: -1.0 = placement mode, 0.0 = VFX only (no damage)
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) = 0;

    // Update the skill each frame while active
    // Returns true if skill is still active, false if finished
    virtual void Update(float deltaTime) = 0;

    // Check if the skill execution is complete
    virtual bool IsFinished() const = 0;

    // Reset the skill state for reuse
    virtual void Reset() = 0;

    // Get the skill data
    virtual const SkillData& GetSkillData() const = 0;

    // Broad category of this skill — used by SkillComponent for activation VFX scaling
    virtual SkillCategory GetCategory() const { return SkillCategory::Projectile; }

    // ─── 발동 방식 생명주기 훅 ──────────────────────────────────────────────────
    // 각 행동 클래스는 필요한 훅만 override. 미구현 시 기본 no-op.

    // [차징] 키를 누르는 순간 (차지 시작)
    virtual void OnChargeBegin(GameObject* caster) {}
    // [차징] 매 프레임 (chargeRatio: 0→1). 시각적 프리뷰 VFX 업데이트에 사용
    virtual void OnChargeUpdate(GameObject* caster, float chargeRatio) {}

    // [채널] 채널 진입 순간
    virtual void OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) {}
    // [채널] 매 0.2초 틱 — 비-투사체 스킬이 Execute 대신 이걸 받음 (이미 구현됨)
    virtual void OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float tickMult) {}
    // [채널] 채널 종료 (키 해제 or 최대 지속 만료)
    virtual void OnChannelEnd(GameObject* caster) {}
    // [채널] 키를 끝까지 눌러 채널이 완전히 완료됐을 때만 호출 (중단 시엔 호출 안 됨)
    virtual void OnChannelComplete(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) {}

    // [증강] 강화 버프가 플레이어에게 걸리는 순간
    virtual void OnEnhanceActivate(GameObject* caster) {}
    // [증강] 강화 버프를 소모해 스킬이 발동되는 순간
    virtual void OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition) {}

    // 채널 종료 후 Update()를 계속 실행해야 하면 true 반환 (예: 낙하 중 메테오)
    virtual bool HasPostChannelWork() const { return false; }

    // Optional per-frame geometry render (override in behaviors that need a mesh)
    virtual void Render(ID3D12GraphicsCommandList* pCmdList) {}

    // VFX/Scene 연결 — 기본 구현은 아무것도 안 함 (필요한 서브클래스만 override)
    virtual void SetVFXManager(FluidSkillVFXManager*) {}
    virtual void SetScene(Scene*) {}

    // Slot assignment (set by SkillComponent::EquipSkill)
    void      SetSlot(SkillSlot s) { m_slot = s; }
    SkillSlot GetSlot() const      { return m_slot; }

protected:
    SkillSlot m_slot = SkillSlot::Count;

    // 이 스킬 슬롯에 처형자 룬(ABY_EXC)이 장착되어 있으면 true.
    // TakeDamage 세 번째 파라미터(bExecRune)에 직접 사용 가능.
    bool HasExecRune(GameObject* caster) const;

    // 원소 타입에 맞는 서브 VFX 이름 반환 (차지/강화 공용)
    static const char* SubVFXName(ElementType e)
    {
        switch (e)
        {
            case ElementType::Fire:  return "sub_fire";
            case ElementType::Water: return "sub_water";
            case ElementType::Wind:  return "sub_wind";
            case ElementType::Earth: return "sub_earth";
            default:                 return "sub_fire";
        }
    }
};
