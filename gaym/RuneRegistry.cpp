#include "stdafx.h"
#include "RuneRegistry.h"
#include "EnemyComponent.h"
#include "PlayerComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "TransformComponent.h"

// ─────────────────────────────────────────────────────────────────────────────
// RuneDef::ApplyTo  — accumulate one rune into SkillStats
// Stack formula: effective = 1 + (baseMult - 1) * stackCount
// ─────────────────────────────────────────────────────────────────────────────
void RuneDef::ApplyTo(SkillStats& stats, int stackCount) const
{
    if (stackCount <= 0) stackCount = 1;

    auto scaledMult = [&](float base) {
        return 1.f + (base - 1.f) * static_cast<float>(stackCount);
    };

    stats.damageMult         *= scaledMult(damageMult);
    stats.cooldownMult       *= scaledMult(cooldownMult);
    stats.rangeMult          *= scaledMult(rangeMult);
    stats.radiusMult         *= scaledMult(radiusMult);
    stats.castTimeMult       *= scaledMult(castTimeMult);
    stats.durationMult       *= scaledMult(durationMult);
    stats.manaCostMult       *= scaledMult(manaCostMult);
    stats.statusDurationMult *= scaledMult(statusDurationMult);
    stats.statusChanceMult   *= scaledMult(statusChanceMult);
    stats.knockbackMult      *= scaledMult(knockbackMult);

    if (activationOverride.has_value())
        stats.activationType = activationOverride.value();

    // VFX mods accumulate multiplicatively
    stats.vfxMod.particleCountMult *= vfxMod.particleCountMult;
    stats.vfxMod.strengthMult      *= vfxMod.strengthMult;
    stats.vfxMod.sizeScaleMult     *= vfxMod.sizeScaleMult;
    stats.vfxMod.speedMult         *= vfxMod.speedMult;

    // Element override — 원소가 지정된 룬은 스킬 속성을 해당 원소로 전환
    if (element != ElementType::None)
        stats.elementOverride = element;

    // Behavioral flags
    stats.extraProjectiles += extraProjectiles * stackCount;
    if (piercing)       stats.piercing       = true;
    if (homing)         stats.homing         = true;
    if (doublecast)     stats.doublecast     = true;
    if (echoOnCast)     stats.echoOnCast     = true;
    stats.lifestealRatio   += lifestealRatio   * static_cast<float>(stackCount);
    stats.execDamageBonus  += execDamageBonus  * static_cast<float>(stackCount);
    stats.cdResetChance    += cdResetChance    * static_cast<float>(stackCount);
    stats.orbitalCount          += orbitalCount     * stackCount;
    stats.spawnOnHitCount       += spawnOnHitCount  * stackCount;
    if (randomElementOnCast)     stats.randomElementOnCast = true;

    // 서브 파티클 VFX (중복 방지)
    if (!subVFXId.empty())
    {
        bool already = false;
        for (const auto& id : stats.subVFXIds)
            if (id == subVFXId) { already = true; break; }
        if (!already) stats.subVFXIds.push_back(subVFXId);
    }

    // Hooks
    if (onCast) stats.onCastHooks.push_back(onCast);
    if (onHit)  stats.onHitHooks.push_back(onHit);
}

float RuneDef::GetStackBonus(RuneGrade grade)
{
    switch (grade)
    {
    case RuneGrade::Normal:    return 0.10f;
    case RuneGrade::Rare:      return 0.08f;
    case RuneGrade::Epic:      return 0.06f;
    case RuneGrade::Unique:    return 0.05f;
    case RuneGrade::Legendary: return 0.03f;
    default:                   return 0.f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RuneRegistry
// ─────────────────────────────────────────────────────────────────────────────
RuneRegistry& RuneRegistry::Get()
{
    static RuneRegistry instance;
    return instance;
}

const RuneDef* RuneRegistry::Find(const std::string& id) const
{
    auto it = m_defs.find(id);
    return (it != m_defs.end()) ? &it->second : nullptr;
}

std::vector<std::string> RuneRegistry::GetIdsByGrade(RuneGrade grade) const
{
    std::vector<std::string> result;
    for (const auto& [id, def] : m_defs)
        if (def.grade == grade) result.push_back(id);
    return result;
}

void RuneRegistry::Register(RuneDef def)
{
    m_defs.emplace(def.id, std::move(def));
}

// ═════════════════════════════════════════════════════════════════════════════
//  룬 정의 (docs/RuneDesign.md 기반)
//  추가/수정은 이 파일 한 곳에서만 작업
// ═════════════════════════════════════════════════════════════════════════════

static bool RollStatus(float basePct, float chanceMult)
{
    float chance = (std::min)(basePct * chanceMult, 1.f);
    return ((float)(rand() % 1000) / 1000.f) < chance;
}

RuneRegistry::RuneRegistry()
{
    // ─── 🔥 화속성 계열 ───────────────────────────────────────────────────────

    // FIR_1 화속 (Normal): 화상 1중첩, 25% 기본 확률
    Register({ .id="FIR_1", .name="화속", .category="속성 변경",
               .description="25% 확률로 화상 1중첩 (5초, 매초 틱 피해)",
               .grade=RuneGrade::Normal, .element=ElementType::Fire,
               .subVFXId="sub_fire",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.25f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyBurn(1, 5.f * ctx.statusDurationMult, ctx.baseDamage * 0.08f);
               }});

    // FIR_2 점화 (Rare): 확률+, 지속+
    Register({ .id="FIR_2", .name="점화", .category="속성 강화",
               .description="화상 확률 35%, 지속 6초로 강화",
               .grade=RuneGrade::Rare, .element=ElementType::Fire,
               .subVFXId="sub_fire",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.35f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyBurn(1, 6.f * ctx.statusDurationMult, ctx.baseDamage * 0.08f);
               }});

    // FIR_3 작열 (Epic): 화상 한도 5중첩, 틱 데미지 +20%
    Register({ .id="FIR_3", .name="작열", .category="속성 강화",
               .description="화상 최대 5중첩, 틱 피해 +20%, 확률 40%",
               .grade=RuneGrade::Epic, .element=ElementType::Fire,
               .subVFXId="sub_fire",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.40f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyBurn(1, 5.f * ctx.statusDurationMult, ctx.baseDamage * 0.096f, 5);
               }});

    // FIR_4 업화 (Unique): 화상 3중첩 이상 적 피격 시 즉시 추가 폭발 피해
    Register({ .id="FIR_4", .name="업화", .category="속성 강화",
               .description="화상 3중첩 이상 적중 시 중첩수×20% 즉발 폭발 피해",
               .grade=RuneGrade::Unique, .element=ElementType::Fire,
               .subVFXId="sub_fire",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   auto* pEnemy = static_cast<EnemyComponent*>(ctx.hitEnemy);
                   int burnStacks = pEnemy->GetBurnStacks();
                   if (burnStacks >= 3)
                       pEnemy->TakeDamage(ctx.baseDamage * 0.20f * burnStacks, false);
                   if (!RollStatus(0.40f, ctx.statusChanceMult)) return;
                   pEnemy->ApplyBurn(1, 5.f * ctx.statusDurationMult, ctx.baseDamage * 0.08f, 5);
               }});

    // ─── 💧 수속성 계열 ───────────────────────────────────────────────────────

    // WAT_1 수속 (Normal): 빙결 1중첩, 25%
    Register({ .id="WAT_1", .name="수속", .category="속성 변경",
               .description="25% 확률로 냉기 1중첩 (5초, 이동속도 -15%). 3중첩 시 완전 빙결",
               .grade=RuneGrade::Normal, .element=ElementType::Water,
               .subVFXId="sub_water",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.25f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyChill(1, 5.f * ctx.statusDurationMult);
               }});

    // WAT_2 냉기 (Rare): 확률+, 지속+
    Register({ .id="WAT_2", .name="냉기", .category="속성 강화",
               .description="냉기 확률 35%, 지속 6초로 강화",
               .grade=RuneGrade::Rare, .element=ElementType::Water,
               .subVFXId="sub_water",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.35f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyChill(1, 6.f * ctx.statusDurationMult);
               }});

    // WAT_3 결빙 (Epic): 2중첩으로도 완전 빙결
    Register({ .id="WAT_3", .name="결빙", .category="속성 강화",
               .description="냉기 2중첩으로 완전 빙결 발동 (기본 3중첩), 확률 40%",
               .grade=RuneGrade::Epic, .element=ElementType::Water,
               .subVFXId="sub_water",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.40f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyChill(1, 5.f * ctx.statusDurationMult, 2);
               }});

    // WAT_4 빙하 (Unique): 1중첩에서도 즉시 완전 빙결
    Register({ .id="WAT_4", .name="빙하", .category="속성 강화",
               .description="30% 확률로 즉시 완전 빙결 (냉기 1중첩만으로 발동)",
               .grade=RuneGrade::Unique, .element=ElementType::Water,
               .subVFXId="sub_water",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.30f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyChill(1, 5.f * ctx.statusDurationMult, 1);
               }});

    // ─── 🌀 풍속성 계열 ───────────────────────────────────────────────────────

    // WND_1 풍속 (Normal): 넉백 +20%
    Register({ .id="WND_1", .name="풍속", .category="속성 변경",
               .description="바람 속성 변환. 넉백 거리 증가",
               .grade=RuneGrade::Normal, .element=ElementType::Wind,
               .knockbackMult=1.20f, .subVFXId="sub_wind" });

    // WND_2 질풍 (Rare): 넉백 +50% + 냉기 슬로우
    Register({ .id="WND_2", .name="질풍", .category="속성 강화",
               .description="30% 확률로 냉기 1중첩 (4초, 이동속도 -15%)",
               .grade=RuneGrade::Rare, .element=ElementType::Wind,
               .knockbackMult=1.50f, .subVFXId="sub_wind",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.30f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyChill(1, 4.f * ctx.statusDurationMult);
               }});

    // WND_3 폭풍 (Epic): 넉백 +80% + 균열 1중첩
    Register({ .id="WND_3", .name="폭풍", .category="속성 강화",
               .description="25% 확률로 균열 1중첩 (5초, 방어력 -8%)",
               .grade=RuneGrade::Epic, .element=ElementType::Wind,
               .knockbackMult=1.80f, .subVFXId="sub_wind",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.25f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyFracture(1, 5.f * ctx.statusDurationMult);
               }});

    // WND_4 뇌풍 (Unique): 넉백 +120% + 균열 2중첩
    Register({ .id="WND_4", .name="뇌풍", .category="속성 강화",
               .description="35% 확률로 균열 2중첩 즉시 부여 (4초)",
               .grade=RuneGrade::Unique, .element=ElementType::Wind,
               .knockbackMult=2.20f, .subVFXId="sub_wind",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.35f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyFracture(2, 4.f * ctx.statusDurationMult);
               }});

    // ─── 🪨 토속성 계열 ───────────────────────────────────────────────────────

    // ERT_1 토속 (Normal): 균열 1중첩, 25%
    Register({ .id="ERT_1", .name="토속", .category="속성 변경",
               .description="25% 확률로 균열 1중첩 (6초, 방어력 -8%). 3중첩 시 경직",
               .grade=RuneGrade::Normal, .element=ElementType::Earth,
               .subVFXId="sub_earth",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.25f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyFracture(1, 6.f * ctx.statusDurationMult);
               }});

    // ERT_2 암석 (Rare): 균열 확률+, 2중첩 시 냉기도 추가
    Register({ .id="ERT_2", .name="암석", .category="속성 강화",
               .description="균열 확률 35%. 균열 2중첩 이상이면 냉기 1중첩도 부여",
               .grade=RuneGrade::Rare, .element=ElementType::Earth,
               .subVFXId="sub_earth",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.35f, ctx.statusChanceMult)) return;
                   auto* pEnemy = static_cast<EnemyComponent*>(ctx.hitEnemy);
                   pEnemy->ApplyFracture(1, 6.f * ctx.statusDurationMult);
                   if (pEnemy->GetFractureStacks() >= 2)
                       pEnemy->ApplyChill(1, 3.5f * ctx.statusDurationMult);
               }});

    // ERT_3 지진 (Epic): 2중첩으로도 경직
    Register({ .id="ERT_3", .name="지진", .category="속성 강화",
               .description="균열 2중첩으로 경직 발동 (기본 3중첩), 확률 40%",
               .grade=RuneGrade::Epic, .element=ElementType::Earth,
               .subVFXId="sub_earth",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   if (!RollStatus(0.40f, ctx.statusChanceMult)) return;
                   static_cast<EnemyComponent*>(ctx.hitEnemy)
                       ->ApplyFracture(1, 6.f * ctx.statusDurationMult, 2);
               }});

    // ERT_4 붕괴 (Unique): 경직/빙결 상태 적에게 +60% 추가 피해 + 즉시 경직
    Register({ .id="ERT_4", .name="붕괴", .category="속성 강화",
               .description="경직/빙결 상태 적에게 +60% 추가 피해. 35% 확률 균열 즉시 경직",
               .grade=RuneGrade::Unique, .element=ElementType::Earth,
               .subVFXId="sub_earth",
               .onHit=[](SkillContext& ctx){
                   if (!ctx.hitEnemy) return;
                   auto* pEnemy = static_cast<EnemyComponent*>(ctx.hitEnemy);
                   if (pEnemy->GetState() == EnemyState::Stagger || pEnemy->IsFrozen())
                       pEnemy->TakeDamage(ctx.damageDealt * 0.60f, false);
                   if (!RollStatus(0.35f, ctx.statusChanceMult)) return;
                   pEnemy->ApplyFracture(1, 6.f * ctx.statusDurationMult, 1);
               }});
}
