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

// 변환 룬 원소에 맞춰 EffectDef 전체 레이어의 색상을 재지정한다.
// SpawnEffectDef 호출 직전에 !stats.elementSet.empty() 조건 하에 사용.
inline void ApplyElementToEffectDef(EffectDef& def, ElementType e)
{
    FluidElementColor ec = FluidElementColors::Get(e);
    def.element = e;
    for (auto& l : def.layers) {
        l.element        = e;
        l.coreColor      = ec.coreColor;
        l.edgeColor      = ec.edgeColor;
        l.overrideColors = true;
    }
}

// 여러 원소를 한 EffectDef 안에 레이어로 합성한다 (멀티 원소 룬 장착 시).
//   set[0]   = 주 원소: 기존 레이어를 재색.
//   set[1..] = 보조 원소: 원본 레이어를 복제해 각 원소 색으로 추가.
// 다원소일 때는 레이어 수가 늘어나므로 파티클 수를 줄여 총량 폭주를 막는다.
// 단일/빈 세트는 ApplyElementToEffectDef 와 동일하게 동작.
inline void ApplyElementSetToEffectDef(EffectDef& def, const std::vector<ElementType>& set)
{
    if (set.empty()) return;
    if (set.size() == 1) { ApplyElementToEffectDef(def, set[0]); return; }

    auto reduceCount = [](EffectLayer& l) {
        const bool isSPH = (l.type >= EmitterType::SPH_Attract &&
                            l.type <= EmitterType::SPH_Beam);
        if (isSPH) {
            int c = (int)(l.sph.particleCount * 0.6f);
            l.sph.particleCount = c < 100 ? 100 : c;
        } else {
            int c = (int)(l.particleCount * 0.6f);
            l.particleCount = c < 40 ? 40 : c;
        }
    };

    // 복제 전 원본 레이어 스냅샷 (주 원소 재색 후 이 스냅샷을 보조 원소용으로 복제)
    const std::vector<EffectLayer> base = def.layers;

    // 주 원소: 기존 레이어 재색 + 카운트 축소
    {
        FluidElementColor ec = FluidElementColors::Get(set[0]);
        def.element = set[0];
        for (auto& l : def.layers) {
            l.element        = set[0];
            l.coreColor      = ec.coreColor;
            l.edgeColor      = ec.edgeColor;
            l.overrideColors = true;
            reduceCount(l);
        }
    }

    // 보조 원소: 원본 레이어 복제 후 각 색으로 추가
    for (size_t i = 1; i < set.size(); ++i) {
        FluidElementColor ec = FluidElementColors::Get(set[i]);
        for (EffectLayer l : base) {   // 값 복사
            l.element        = set[i];
            l.coreColor      = ec.coreColor;
            l.edgeColor      = ec.edgeColor;
            l.overrideColors = true;
            reduceCount(l);
            def.layers.push_back(l);
        }
    }
}
