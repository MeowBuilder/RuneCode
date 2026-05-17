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

    // Optional per-frame geometry render (override in behaviors that need a mesh)
    virtual void Render(ID3D12GraphicsCommandList* pCmdList) {}

    // VFX/Scene 연결 — 기본 구현은 아무것도 안 함 (필요한 서브클래스만 override)
    virtual void SetVFXManager(FluidSkillVFXManager*) {}
    virtual void SetScene(Scene*) {}
    virtual void SetDecalManager(class DecalManager*) {}

    // Slot assignment (set by SkillComponent::EquipSkill)
    void      SetSlot(SkillSlot s) { m_slot = s; }
    SkillSlot GetSlot() const      { return m_slot; }

protected:
    SkillSlot m_slot = SkillSlot::Count;
};
