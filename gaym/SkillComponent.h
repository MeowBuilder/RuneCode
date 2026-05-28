#pragma once

#include "Component.h"
#include "SkillTypes.h"
#include "RuneDef.h"
#include "VFXTypes.h"
#include <memory>
#include <array>

class ISkillBehavior;
class InputSystem;
class CCamera;
class FluidSkillVFXManager;

// Number of rune slots per skill
constexpr int RUNES_PER_SKILL = 3;

// Component that manages skill slots and execution for a GameObject
class SkillComponent : public Component
{
public:
    SkillComponent(GameObject* pOwner);
    virtual ~SkillComponent();

    virtual void Update(float deltaTime) override;

    // Process skill input from the input system
    // Should be called in the player's update with access to input and camera
    void ProcessSkillInput(InputSystem* pInputSystem, CCamera* pCamera);

    // Equip a skill behavior to a specific slot
    void EquipSkill(SkillSlot slot, std::unique_ptr<ISkillBehavior> pBehavior);

    // Remove skill from slot
    void UnequipSkill(SkillSlot slot);

    // Get skill behavior from slot
    ISkillBehavior* GetSkill(SkillSlot slot) const;

    // Check if a skill is ready to use
    bool IsSkillReady(SkillSlot slot) const;

    // Get remaining cooldown for a skill
    float GetCooldownRemaining(SkillSlot slot) const;

    // Get cooldown progress (0.0 = just used, 1.0 = ready)
    float GetCooldownProgress(SkillSlot slot) const;

    // 모든 슬롯 쿨타임 즉시 초기화
    void ResetAllCooldowns();

    // 무한 룬: 쿨다운 즉시 초기화
    void ResetCooldown(SkillSlot slot);
    // 시간 역행 룬: 쿨다운 seconds 초 감소
    void ReduceCooldown(SkillSlot slot, float seconds);

    // 룬 cooldownMult 적용한 실제 쿨다운 반환
    float GetEffectiveCooldown(size_t slotIndex) const;

    // Rune/Activation type management (legacy - uses first equipped rune)
    void SetActivationType(ActivationType type);
    ActivationType GetActivationType() const { return m_CurrentActivationType; }

    // Per-skill rune slot management
    void         SetRuneSlot(SkillSlot skill, int runeIndex, const std::string& runeId, int stackCount = 1);
    EquippedRune GetRuneSlot(SkillSlot skill, int runeIndex) const;
    void         ClearRuneSlot(SkillSlot skill, int runeIndex);
    int          GetEquippedRuneCount(SkillSlot skill) const;

    // Build accumulated SkillStats from all runes equipped on a slot.
    // defaultType: the skill's own ActivationType used as fallback if no rune overrides it.
    SkillStats BuildSkillStats(SkillSlot skill,
                               ActivationType defaultType = ActivationType::Instant) const;

    // Get combined activation type for a skill (based on equipped runes)
    ActivationType GetSkillActivationType(SkillSlot skill) const;

    // Legacy: get rune combo flags (delegates to BuildSkillStats internally)
    RuneCombo GetRuneCombo(SkillSlot skill) const;

    // ─── 활성화 VFX 수식자 ───────────────────────────────────────────────────
    // 차지/채널/강화 소모 상태에 따른 VFX 스케일 수식자를 계산.
    // 스킬 Execute() 직전에 SkillComponent가 내부 저장; 행동 클래스에서 GetCurrentActivationVFXMod() 로 읽음.
    VFXModifier BuildActivationVFXMod(SkillSlot slot, float chargeRatio,
                                       bool isChannelTick, bool isEnhanceConsumed) const;

    // 가장 최근 Execute() 직전에 계산된 활성화 VFXModifier (행동 클래스가 VFX 스폰 시 참조)
    VFXModifier GetCurrentActivationVFXMod() const { return m_activationVFXMod; }
    float       GetCurrentChargeRatio()      const { return m_currentChargeRatio; }

    // VFX 매니저 연결 (Scene 초기화 시 호출)
    void SetVFXManager(FluidSkillVFXManager* mgr) { m_pVFXManager = mgr; }

    // Block rune input (e.g., during drop rune selection)
    void SetRuneInputBlocked(bool blocked) { m_bRuneInputBlocked = blocked; }
    bool IsRuneInputBlocked() const { return m_bRuneInputBlocked; }

    // Charge state (for Charge activation type)
    bool IsCharging() const { return m_bIsCharging; }
    float GetChargeProgress() const;

    // Channel state
    bool IsChanneling() const { return m_bIsChanneling; }
    float GetChannelProgress() const { return (m_fChannelDuration > 0.0f) ? (m_fChannelTime / m_fChannelDuration) : 0.0f; }

    // Enhance state
    bool IsEnhanced() const { return m_bIsEnhanced; }
    float GetEnhanceTimeRemaining() const { return m_fEnhanceTimer; }

    // Check if any skill is currently active (casting)
    bool IsSkillActive() const { return m_ActiveSkillSlot != SkillSlot::Count; }

private:
    // Try to use a skill in the given slot
    bool TryUseSkill(SkillSlot slot, const DirectX::XMFLOAT3& targetPosition);

    // Calculate target position from mouse cursor (ray-plane intersection)
    DirectX::XMFLOAT3 CalculateTargetPosition(InputSystem* pInputSystem, CCamera* pCamera) const;

    // Check if the input key for a skill slot is pressed
    bool IsSkillKeyPressed(SkillSlot slot, InputSystem* pInputSystem) const;

private:
    // Skill slots
    std::array<std::unique_ptr<ISkillBehavior>, static_cast<size_t>(SkillSlot::Count)> m_Skills;

    // Cooldown timers for each slot
    std::array<float, static_cast<size_t>(SkillSlot::Count)> m_CooldownTimers;

    // Skill states
    std::array<SkillState, static_cast<size_t>(SkillSlot::Count)> m_SkillStates;

    // Currently active skill (if any)
    SkillSlot m_ActiveSkillSlot = SkillSlot::Count;  // Count means no active skill

    // Current activation type (set by rune) - legacy, for backwards compatibility
    ActivationType m_CurrentActivationType = ActivationType::Instant;

    // Per-skill rune slots: [skill][runeSlot] = EquippedRune
    // Empty runeId = empty slot
    std::array<std::array<EquippedRune, RUNES_PER_SKILL>, static_cast<size_t>(SkillSlot::Count)> m_SkillRunes;

    // Charge system
    bool m_bIsCharging = false;
    float m_fChargeTime = 0.0f;
    float m_fMaxChargeTime = 1.5f;  // Time to full charge
    SkillSlot m_ChargingSlot = SkillSlot::Count;
    DirectX::XMFLOAT3 m_ChargeTargetPosition;

    // Channel system
    bool m_bIsChanneling = false;
    float m_fChannelTime = 0.0f;
    float m_fChannelDuration = 2.0f;  // Total channel duration
    float m_fChannelTickRate = 0.2f;  // Time between ticks
    float m_fChannelTickAccum = 0.0f;
    DirectX::XMFLOAT3 m_ChannelTargetPosition;  // Stored target for channeling
    bool m_bChannelTickFiredThisFrame = false;  // Set true on tick, consumed by network sync then reset

    // Enhance system
    bool m_bIsEnhanced = false;
    float m_fEnhanceTimer = 0.0f;
    float m_fEnhanceDuration = 5.0f;  // Enhancement lasts 5 seconds
    float m_fEnhanceMultiplier = 2.0f;  // Damage multiplier

    // Process rune input (1-5 keys)
    void ProcessRuneInput(InputSystem* pInputSystem);

    // Rune input blocking
    bool m_bRuneInputBlocked = false;

    // 활성화 VFX 컨텍스트 (Execute 직전에 세팅, 행동 클래스가 참조)
    VFXModifier m_activationVFXMod;
    float       m_currentChargeRatio    = 0.f;
    bool        m_bCurrentIsChannelTick = false;
    bool        m_bCurrentEnhanceUsed  = false;

    // Execute skill based on current activation type
    void ExecuteWithActivationType(SkillSlot slot, const DirectX::XMFLOAT3& targetPosition);

    // Execute or split into multiple projectiles if Split rune is equipped
    void ExecuteOrSplit(size_t index, const DirectX::XMFLOAT3& target, float mult);

    // 차지 결집 VFX 스폰/업데이트 (step 0=초기, 1~3=성장 단계)
    void SpawnChargeGatherVFX(int step);

    // 차지 VFX (슬롯별 독립)
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    std::array<int, static_cast<size_t>(SkillSlot::Count)> m_chargeGatherVFXIds;
    std::array<int, static_cast<size_t>(SkillSlot::Count)> m_chargeScaleSteps;
};
