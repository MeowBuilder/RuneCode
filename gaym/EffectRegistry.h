#pragma once
#include "VFXTypes.h"
#include "FluidParticle.h"  // FluidCPDesc, FluidSkillVFXDef 등
#include "SkillTypes.h"     // SkillSlot, ElementType
#include <unordered_map>
#include <string>
#include <vector>

// ─── 룬 플래그 (RuneCombo → 비트마스크 변환) ──────────────────────────────
enum RuneFlag : uint32_t {
    RUNE_NONE    = 0,
    RUNE_INSTANT = 1 << 0,
    RUNE_CHARGE  = 1 << 1,
    RUNE_CHANNEL = 1 << 2,
    RUNE_PLACE   = 1 << 3,
    RUNE_ENHANCE = 1 << 4,
    RUNE_SPLIT   = 1 << 5,
};

inline uint32_t ToRuneFlags(const RuneCombo& combo) {
    uint32_t flags = 0;
    if (combo.hasInstant) flags |= RUNE_INSTANT;
    if (combo.hasCharge)  flags |= RUNE_CHARGE;
    if (combo.hasChannel) flags |= RUNE_CHANNEL;
    if (combo.hasPlace)   flags |= RUNE_PLACE;
    if (combo.hasEnhance) flags |= RUNE_ENHANCE;
    if (combo.hasSplit)   flags |= RUNE_SPLIT;
    return flags;
}

// ─── EffectRegistry: EffectDef 기반 이펙트 레지스트리 ─────────────────────
class EffectRegistry {
public:
    static EffectRegistry& Get();

    void Initialize();

    void Register(EffectDef def);
    void RegisterRuneMod(const std::string& name, uint32_t runeFlag, VFXModifier mod);

    // 룬 수식자 적용 후 반환
    EffectDef GetEffect(const std::string& name, uint32_t runeFlags = 0) const;
    bool      HasEffect(const std::string& name) const;

private:
    EffectRegistry() = default;

    EffectDef ApplyMods(EffectDef def,
                        const std::unordered_map<uint32_t, VFXModifier>& mods,
                        uint32_t runeFlags) const;

    std::unordered_map<std::string, EffectDef>    m_Effects;
    std::unordered_map<std::string,
        std::unordered_map<uint32_t, VFXModifier>> m_RuneMods;
};
