#pragma once

#include "Component.h"
#include "SkillTypes.h"
#include "RuneDef.h"
#include "VFXTypes.h"
#include <memory>
#include <array>
#include <set>
#include <vector>

class ISkillBehavior;
class InputSystem;
class CCamera;
class FluidSkillVFXManager;
class EnemyComponent;

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

    // 오프라인: 무한 룬(ABY_INF) RNG 롤 → 성공 시 ResetCooldown + VFX
    void TryTriggerInfiniteRune(SkillSlot slot, const DirectX::XMFLOAT3& hitPos);
    // 시간 역행 룬: 쿨다운 seconds 초 감소.
    //   playClockVFX=false 면 시계 VFX 를 띄우지 않음 (멀티에서 NetworkManager 가 직접 스폰).
    void ReduceCooldown(SkillSlot slot, float seconds, bool playClockVFX = true);

    // 비투사체(AoE/빔/채널) 스킬의 적중 룬 일괄 처리: onHit 훅(시간역행 등) + 흡혈(ABY_VMP).
    // skillSlot 을 ctx 에 채워 슬롯 의존 훅이 동작하도록 한다. 투사체는 ProjectileManager 가
    // 동일 역할을 수행. 멀티에서는 BuildSkillStats 가 훅/흡혈을 미리 비워 서버 권위와 충돌 없음.
    void ApplyOnHitRunes(SkillSlot slot, const SkillStats& stats,
                         float baseDamage, float dealtDamage,
                         void* hitEnemy, const DirectX::XMFLOAT3& hitEnemyPos,
                         void* scene);

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
    // BuildSkillStats 없이 특정 룬 장착 여부만 빠르게 조회
    bool         HasRuneEquipped(SkillSlot skill, const char* runeId) const;

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

    // echo(메아리) 로 발동된 슬롯 — Update() 호출하되 쿨다운 타이머는 초기화하지 않음
    std::set<size_t> m_echoRunningSlots;

    // place(설치) 로 발동된 슬롯 — 동일 패턴
    std::set<size_t> m_placeRunningSlots;

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

    // 메아리 트리거 VFX (큐에 등록될 때 발 아래 회전 마법진 연출, 슬롯 인덱스 반환)
    // targetPos: 스킬 타겟 위치 (가장 가까운 적 탐색 기준)
    // pOutTarget: 찾은 적 포인터 반환 (DeferredEcho 추적용)
    int SpawnEchoTriggerVFX(ElementType element, const DirectX::XMFLOAT3& targetPos, EnemyComponent** pOutTarget = nullptr);

    // 차지 VFX (슬롯별 독립)
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    std::array<int, static_cast<size_t>(SkillSlot::Count)> m_chargeGatherVFXIds;
    std::array<int, static_cast<size_t>(SkillSlot::Count)> m_chargeScaleSteps;

    // 과열 추적 (ABY_OVL: 동일 스킬 연속 3회 → 다음 1회 +60%)
    std::array<int,  static_cast<size_t>(SkillSlot::Count)> m_overheatConsecutive{};
    std::array<bool, static_cast<size_t>(SkillSlot::Count)> m_overheatReady{};

    // 무한 룬(ABY_INF) Casting 중 reset 요청 보류 — Casting 종료 후 GetEffectiveCooldown 으로 덮어써지지 않도록 Update 끝에서 소비
    std::array<bool, static_cast<size_t>(SkillSlot::Count)> m_pendingCooldownReset{};

    // 시간역행 룬(ABY_TIM) Casting 중 쿨다운 감소 누적 — 시전 중엔 쿨다운이 0이라 감소가 버려지므로
    // 누적했다가 Casting 종료(쿨다운 세팅) 직후 Update 에서 실제 타이머에 적용
    std::array<float, static_cast<size_t>(SkillSlot::Count)> m_pendingCooldownReduce{};

    // 채널 중단 마킹 — IsFinished() 후 쿨타임 세팅 시 50% 페널티 적용 여부 추적
    std::array<bool, static_cast<size_t>(SkillSlot::Count)> m_bChannelInterrupted{};

    // 무한 룬 VFX 추적 (플레이어 따라 이동)
    int   m_infRuneVFXSlot  = -1;
    float m_infRuneVFXTimer = 0.f;

    // 시간역행 룬(ABY_TIM) 시계 VFX 추적 (플레이어 따라 이동, 새 발동 시 교체)
    int   m_timeRewindVFXSlot  = -1;
    float m_timeRewindVFXTimer = 0.f;

    // 과열 룬(ABY_OVL) 스택 불꽃 오라 — 플레이어 머리 위 추적, 스택 수만큼 표시
    std::vector<int> m_overheatStackVFX;
    float            m_overheatVFXTimer = 0.f;
    // 발동(4회째) 폭발 VFX — 플레이어 위치 추적
    int   m_overheatBurstVFXSlot  = -1;
    float m_overheatBurstVFXTimer = 0.f;
    // 스택 불꽃/발동 폭발 VFX 스폰 헬퍼
    void SpawnOverheatStackVFX(int stackCount);
    void SpawnOverheatBurstVFX();

    // 원소 공명(ABY_RES) 오라 VFX — 확장하는 다원소 링 + 중앙 펄스 + 머리 위 소용돌이.
    //   슬롯/각도를 추적해 Update() 에서 링을 바깥으로 펼치고 위로 띄운다.
    static constexpr int kResonanceRingMax = 12;
    struct ResonanceRing { int slot = -1; float angle = 0.f; };
    std::array<ResonanceRing, kResonanceRingMax> m_resonanceRing{};
    int   m_resonanceRingCount = 0;
    int   m_resonanceCoreSlot  = -1;   // 중앙 flare 펄스
    float m_resonanceTimer     = 0.f;  // 남은 수명
    float m_resonanceLife      = 0.f;  // 총 수명
    // 서로 다른 원소 색으로 링을 구성하는 공명 오라 스폰
    void SpawnResonanceAura(const std::vector<ElementType>& elements);

    // 활성화 룬(차지/증강/설치) 상태 전이를 다른 클라에 알리는 헬퍼. NetworkManager::IsConnected() 일 때만 전송.
    //   actionType: PLAYER_ACTION_CHARGE_BEGIN/END, ENHANCE_BEGIN/END, PLACE_SPAWN/FIRE
    //   slot: skillSlot (4 = SkillSlot::Count 는 슬롯 불명 sentinel)
    //   extraY: ENHANCE_BEGIN duration 등 보조값
    void NotifyActionNet(uint32_t actionType, SkillSlot slot, float extraY = 0.f);
    void NotifyActionNetAt(uint32_t actionType, SkillSlot slot, const DirectX::XMFLOAT3& pos, float extraY = 0.f);

    // 스킬 시전을 서버에 전송 (SendSkill + SendPlayerAttack) — ExecuteWithActivationType 평시 경로와
    //   charge release 경로 양쪽에서 호출되도록 추출.
    //   chargeRatio: 차징 비율 (0~1), 평시 경로에선 0
    //   sendSkill:   설치+차징은 false 로 SendSkill 생략하고 PlayerAttack 만 전송
    void SendSkillNet(SkillSlot slot, const DirectX::XMFLOAT3& targetPosition,
                      float chargeRatio = 0.0f, bool sendSkill = true);

    // 메아리 지연 큐 (ABY_ECO: 2초 후 가장 가까운 적을 향해 50% 재발동)
    struct DeferredEcho { size_t index; float mult; float timer; int decalSlot = -1; EnemyComponent* pTarget = nullptr; };
    std::vector<DeferredEcho> m_echoQueue;

    // R 스킬 지연 발동 (마법진 reveal 완료 후 실제 스킬 발사)
    struct DelayedCast {
        SkillSlot         slot;
        size_t            skillIndex;
        DirectX::XMFLOAT3 target;
        float             timeRemain;
    };
    std::vector<DelayedCast> m_delayedCasts;
    bool m_bRSkillExecuting = false; // 지연 후 재진입 시 마법진 재생성 방지

    // 설치 룬 함정 큐 (TRF_DEP: 바닥 표식 → 트리거 시 스킬 발동)
    struct PlacedTrap {
        size_t    skillIndex;
        float     damageMultiplier;
        DirectX::XMFLOAT3 worldPos;
        int       spriteSlot    = -1;
        float     activateRadius = 3.0f;
        bool      playerTrigger = false;  // true: 플레이어가 밟으면 (EarthArmor)
        bool      windGate      = false;  // true: E키 재입력으로 텔레포트 (GaleRush)
        bool      triggered     = false;  // 발동 플래그 (spriteSlot 덮어쓰지 않음)
    };
    std::vector<PlacedTrap> m_placeQueue;

    void SpawnPlaceTrap(size_t skillIndex, const DirectX::XMFLOAT3& pos, float mult, const RuneCombo& combo);
    void FirePlacedTrap(PlacedTrap& trap, const DirectX::XMFLOAT3& currentTargetPos);
};
