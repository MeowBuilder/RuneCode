#pragma once
#include <cstdint>
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <DirectXMath.h>
#include "SkillTypes.h"
#include "VFXTypes.h"

using namespace DirectX;

class GameObject;

// ─────────────────────────────────────────────────────────────────────────────
// Context passed to rune hook callbacks (onCast, onHit)
// ─────────────────────────────────────────────────────────────────────────────
struct SkillContext
{
    GameObject*   caster        = nullptr;
    XMFLOAT3      targetPos     = {};
    ElementType   element       = ElementType::None;
    float         baseDamage    = 0.f;
    float         damageDealt   = 0.f;   // populated on hit
    int           projectileIdx = 0;     // for split/multi projectiles
    SkillSlot     skillSlot     = SkillSlot::Count; // onHit 훅: 어느 슬롯에서 발사됐는지
    void*         scene         = nullptr;          // Scene* (void* to avoid circular include)
    void*         hitEnemy      = nullptr;          // EnemyComponent* (void*)
    XMFLOAT3      hitEnemyPos   = {};
    // 상태이상 룬에서 사용 — SkillStats에서 전달
    float         statusChanceMult   = 1.f;
    float         statusDurationMult = 1.f;
};

// ─────────────────────────────────────────────────────────────────────────────
// Accumulated stats computed from all equipped runes on one skill slot.
// Skills and SkillComponent read only this — they never inspect RuneDef directly.
// ─────────────────────────────────────────────────────────────────────────────
struct SkillStats
{
    // Stat multipliers (all start at 1.0)
    float damageMult            = 1.f;
    float cooldownMult          = 1.f;
    float rangeMult             = 1.f;
    float radiusMult            = 1.f;
    float castTimeMult          = 1.f;
    float durationMult          = 1.f;
    float channelDurationMult   = 1.f;
    float manaCostMult          = 1.f;
    float statusDurationMult    = 1.f;
    float statusChanceMult      = 1.f;
    float knockbackMult         = 1.f;

    // 최종 스킬 원소 오버라이드.
    // 값이 없으면 스킬이 원래 가진 원소를 사용한다.
    std::optional<ElementType> elementOverride = std::nullopt;
    std::vector<ElementType> elementSet;
    bool resonanceActive = false;

    // 룬 행동 옵션 누적값
    int   extraProjectiles = 0;
    bool  piercing = false;
    bool  homing = false;
    float lifestealRatio = 0.f;
    float execDamageBonus = 0.f;
    bool  doublecast = false;
    bool  echoOnCast = false;
    float cdResetChance = 0.f;
    float revengeBonus = 0.f;
    float overheatBonus = 0.f;
    int   orbitalCount = 0;
    int   spawnOnHitCount = 0;

    // 룬에서 사용하는 VFX 정보
    VFXModifier vfxMod;

    // 장착 룬들의 서브 VFX
    std::vector<std::string> subVFXIds;

    // 룬 훅들을 모아서 실행하는 구조라면 필요
    std::vector<std::function<void(SkillContext&)>> onCastHooks;
    std::vector<std::function<void(SkillContext&)>> onHitHooks;

    // 기존 코드와 UI에서 대표 발동 방식을 조회할 때만 사용한다.
// 실제 실행은 아래 복수 플래그를 사용한다.
    ActivationType activationType = ActivationType::Instant;

    // 발동 룬이 하나라도 장착됐는지
    bool hasActivationRune = false;

    // 복수 발동 방식
    bool activationInstant = false;
    bool activationCharge = false;
    bool activationChannel = false;
    bool activationPlace = false;
    bool activationEnhance = false;
    bool activationSplit = false;

    void EnableActivation(ActivationType type)
    {
        switch (type)
        {
        case ActivationType::Instant:
            activationInstant = true;
            break;

        case ActivationType::Charge:
            activationCharge = true;
            break;

        case ActivationType::Channel:
            activationChannel = true;
            break;

        case ActivationType::Place:
            activationPlace = true;
            break;

        case ActivationType::Enhance:
            activationEnhance = true;
            break;

        case ActivationType::Split:
            activationSplit = true;
            break;

        default:
            break;
        }
    }

    // 룬이 추가한 발동 방식
    void AddRuneActivation(ActivationType type)
    {
        hasActivationRune = true;
        EnableActivation(type);
    }

    // 발동 룬이 하나도 없을 때만 스킬 기본 발동 방식을 사용
    void ApplyDefaultActivation(ActivationType defaultType)
    {
        if (hasActivationRune)
            return;

        EnableActivation(defaultType);
    }

    // 기존 코드나 UI에서 사용하는 대표 타입.
    // 실제 조합 실행에는 이 값을 사용하지 않는다.
    ActivationType ResolvePrimaryActivation() const
    {
        // 실행 파이프라인 순서에 맞춘다.
        if (activationCharge)
            return ActivationType::Charge;

        if (activationChannel)
            return ActivationType::Channel;

        if (activationPlace)
            return ActivationType::Place;

        if (activationEnhance)
            return ActivationType::Enhance;

        if (activationSplit)
            return ActivationType::Split;

        if (activationInstant)
            return ActivationType::Instant;

        return ActivationType::Instant;
    }

    bool IsCharge() const
    {
        return activationCharge;
    }

    bool IsChannel() const
    {
        return activationChannel;
    }

    bool IsPlace() const
    {
        return activationPlace;
    }

    bool IsEnhance() const
    {
        return activationEnhance;
    }

    bool IsSplit() const
    {
        return activationSplit || extraProjectiles > 0;
    }

    RuneCombo ToRuneCombo() const
    {
        RuneCombo combo{};

        combo.hasInstant = activationInstant;
        combo.hasCharge = activationCharge;
        combo.hasChannel = activationChannel;
        combo.hasPlace = activationPlace;
        combo.hasEnhance = activationEnhance;
        combo.hasSplit = activationSplit || extraProjectiles > 0;
        combo.count = 0;

        return combo;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Rune grades
// ─────────────────────────────────────────────────────────────────────────────
enum class RuneGrade : uint8_t
{
    Normal,
    Rare,
    Epic,
    Unique,
    Legendary
};

// ─────────────────────────────────────────────────────────────────────────────
// Static definition of one rune type (registered in RuneRegistry)
// ─────────────────────────────────────────────────────────────────────────────
struct RuneDef
{
    std::string id;
    std::string name;
    std::string category;     // "속성 변경", "속성 강화" 등
    std::string description;  // 선택창 표시 설명 (onHit 효과 등 자동 생성 불가 내용)
    RuneGrade   grade   = RuneGrade::Normal;
    ElementType element = ElementType::None;  // None = universal

    // Stat multipliers (base values for 1 stack)
    // Stack formula: effective = 1 + (baseMult - 1) * stackCount
    float damageMult          = 1.f;
    float cooldownMult        = 1.f;
    float rangeMult           = 1.f;
    float radiusMult          = 1.f;
    float castTimeMult        = 1.f;
    float durationMult        = 1.f;
    float channelDurationMult = 1.f;
    float manaCostMult        = 1.f;
    float statusDurationMult  = 1.f;
    float statusChanceMult    = 1.f;
    float knockbackMult       = 1.f;

    // 이 룬이 추가하는 발동 방식.
    // 여러 발동 룬은 SkillStats에 누적되어 조합된다.
    std::optional<ActivationType> activationOverride;

    // VFX modification applied when this rune is equipped
    VFXModifier vfxMod;

    // Behavioral flags (additive / boolean)
    int   extraProjectiles = 0;
    bool  piercing         = false;
    bool  homing           = false;
    float lifestealRatio   = 0.f;
    float execDamageBonus  = 0.f;
    bool  doublecast       = false;
    bool  echoOnCast       = false;
    float cdResetChance    = 0.f;
    float revengeBonus     = 0.f;
    float overheatBonus    = 0.f;
    int   orbitalCount          = 0;    // 선회/성좌: 궤도 파티클 다단히트 수
    int   spawnOnHitCount       = 0;    // 반향/폭발반향: 적중 시 추가 투사체 수

    // Sub-particle VFX: EffectRegistry에 등록된 서브 파티클 def ID (빈 문자열 = 없음)
    std::string subVFXId;

    // Complex behavior hooks (nullptr for simple runes)
    std::function<void(SkillContext&)> onCast;
    std::function<void(SkillContext&)> onHit;

    // Accumulate this rune's contribution into stats
    void ApplyTo(SkillStats& stats, int stackCount = 1) const;

    // Per-stack bonus for each grade (see RuneList.md)
    static float GetStackBonus(RuneGrade grade);
};

// ─────────────────────────────────────────────────────────────────────────────
// An equipped rune instance (stored per slot in SkillComponent)
// ─────────────────────────────────────────────────────────────────────────────
struct EquippedRune
{
    std::string runeId;      // "" = empty slot
    int         stackCount = 1;

    bool IsEmpty() const { return runeId.empty(); }
};
