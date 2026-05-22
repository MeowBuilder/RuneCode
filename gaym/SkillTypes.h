#pragma once

#include <cstdint>

// Skill slot enum - maps to input keys
enum class SkillSlot : uint8_t
{
    Q = 0,          // Q key skill
    E,              // E key skill
    R,              // R key skill (ultimate)
    RightClick,     // Right mouse button skill
    Count
};

// Element type for skills
enum class ElementType : uint8_t
{
    None = 0,
    Fire,       // 불
    Water,      // 물
    Wind,       // 바람
    Earth       // 대지
};

// How the skill is activated (determined by Activation Rune)
enum class ActivationType : uint8_t
{
    None = 0,       // Empty slot
    Instant,        // 1: Immediate effect on key press
    Charge,         // 2: Charge up then release (more damage)
    Channel,        // 3: Channeled over time (continuous effect)
    Place,          // 4: Place trap/turret at location
    Enhance,        // 5: Self-buff, enhance next attack
    Split,          // 6: Split projectile into multiple
    Count
};

// Rune types for skill modification
enum class RuneType : uint8_t
{
    // Activation Runes (changes how skill is cast)
    ActivationInstant = 0,
    ActivationCharge,
    ActivationChannel,
    ActivationPlace,
    ActivationEnhance,
    ActivationSplit,

    // Element Runes (changes element) - future
    // Effect Runes (adds effects) - future

    None = 255
};

// Combo flags from multiple runes equipped on a single skill
struct RuneCombo {
    bool hasInstant = false;
    bool hasCharge = false;
    bool hasChannel = false;
    bool hasPlace = false;
    bool hasEnhance = false;
    bool hasSplit = false;
    int count = 0;  // Total equipped rune count
};

// Broad category of skill behavior — used for activation VFX scaling logic
enum class SkillCategory : uint8_t
{
    Projectile, // 투사체 기반 (Fireball, WaterOrb, EarthShard…)
    Wave,       // 파동 전진형 (WaveSlash, TidalWave)
    Beam,       // 지속 빔 (FireBeam)
    AoE,        // 즉발 범위 (Meteor, Earthquake, StoneSpikeS)
    Dash,       // 돌진 이동 (GaleRush)
    Summon,     // 소환/지속 오브젝트 (Tornado, WaterVortex)
};

// Current state of a skill
enum class SkillState : uint8_t
{
    Ready,          // Available to use
    Casting,        // Currently being cast/executed
    Cooldown,       // On cooldown, cannot use
    Disabled        // Temporarily unavailable
};
