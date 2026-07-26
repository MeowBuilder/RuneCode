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

    stats.damageMult          *= scaledMult(damageMult);
    stats.cooldownMult        *= scaledMult(cooldownMult);
    stats.rangeMult           *= scaledMult(rangeMult);
    stats.radiusMult          *= scaledMult(radiusMult);
    stats.castTimeMult        *= scaledMult(castTimeMult);
    stats.durationMult        *= scaledMult(durationMult);
    stats.channelDurationMult *= scaledMult(channelDurationMult);
    stats.manaCostMult        *= scaledMult(manaCostMult);
    stats.statusDurationMult  *= scaledMult(statusDurationMult);
    stats.statusChanceMult    *= scaledMult(statusChanceMult);
    stats.knockbackMult       *= scaledMult(knockbackMult);

    if (activationOverride.has_value())
    {
        stats.AddRuneActivation(activationOverride.value());
    }

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
    stats.revengeBonus     += revengeBonus     * static_cast<float>(stackCount);
    stats.overheatBonus    += overheatBonus    * static_cast<float>(stackCount);
    stats.orbitalCount          += orbitalCount     * stackCount;
    stats.spawnOnHitCount       += spawnOnHitCount  * stackCount;

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
    Register({ .id="FIR_1", .name="화속", .category="속성",
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
    Register({ .id="FIR_2", .name="점화", .category="속성+",
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
    Register({ .id="FIR_3", .name="작열", .category="속성+",
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
    Register({ .id="FIR_4", .name="업화", .category="속성+",
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
    Register({ .id="WAT_1", .name="수속", .category="속성",
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
    Register({ .id="WAT_2", .name="냉기", .category="속성+",
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
    Register({ .id="WAT_3", .name="결빙", .category="속성+",
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
    Register({ .id="WAT_4", .name="빙하", .category="속성+",
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
    Register({ .id="WND_1", .name="풍속", .category="속성",
               .description="바람 속성 변환. 넉백 거리 증가",
               .grade=RuneGrade::Normal, .element=ElementType::Wind,
               .knockbackMult=1.20f, .subVFXId="sub_wind" });

    // WND_2 질풍 (Rare): 넉백 +50% + 냉기 슬로우
    Register({ .id="WND_2", .name="질풍", .category="속성+",
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
    Register({ .id="WND_3", .name="폭풍", .category="속성+",
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
    Register({ .id="WND_4", .name="뇌풍", .category="속성+",
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
    Register({ .id="ERT_1", .name="토속", .category="속성",
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
    Register({ .id="ERT_2", .name="암석", .category="속성+",
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
    Register({ .id="ERT_3", .name="지진", .category="속성+",
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
    Register({ .id="ERT_4", .name="붕괴", .category="속성+",
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

    // ─── 🔄 변환 룬 — 발동 방식 ──────────────────────────────────────────────────
    // 발동 방식 변환 룬은 EffectRegistry의 RUNE_CHARGE/CHANNEL/ENHANCE/PLACE 모드로
    // 기존 이펙트를 자동으로 변형한다 (더 크게, 더 많이, 빔 강화 등).

    // TRF_CHG 차징 (Rare): RUNE_CHARGE 플래그 → EffectRegistry에서 기존 VFX 변형
    Register({ .id="TRF_CHG", .name="차징", .category="발동",
               .description="키 홀드로 차지. 1단계 ×1.5, 2단계 ×2.5 피해·범위",
               .grade=RuneGrade::Rare,
               .activationOverride=ActivationType::Charge });

    // TRF_CHN 채널 (Rare): RUNE_CHANNEL 플래그 → 빔/스트림 이펙트 강화
    Register({ .id="TRF_CHN", .name="채널", .category="발동",
               .description="키 홀드 중 연속 발동. 틱당 35% 피해",
               .grade=RuneGrade::Rare,
               .activationOverride=ActivationType::Channel });

    // TRF_DEP 설치 (Rare): RUNE_PLACE — 설치물 이펙트 지속 증가
    Register({ .id = "TRF_DEP", .name = "설치", .category = "발동",
            .description = "지점에 함정을 1개 설치합니다. 같은 스킬 슬롯의 기존 함정은 교체되며, 적이 접근하면 1회 발동 후 사라집니다.",
            .grade = RuneGrade::Rare,
            .activationOverride = ActivationType::Place });

    // TRF_EMP 증강 (Rare): RUNE_ENHANCE → 오라 이펙트 활성화
    Register({ .id="TRF_EMP", .name="증강", .category="발동",
               .description="시전 시 버프 적용. 다음 공격 피해 ×1.8 (5초 이내)",
               .grade=RuneGrade::Rare,
               .activationOverride=ActivationType::Enhance });

    // ─── 🔄 변환 룬 — 형태 변환 ────────────────────────────────────────────────
    // 투사체 외형이 서브 VFX로 눈에 띄게 달라진다.
    // 추가로 vfxMod에 주 이펙트 수식값을 저장해 두었다 (후속 연결 예정).

    // TRF_MLT 다연발 (Normal): 파편 트레일로 분열 느낌 강조
    Register({ .id="TRF_MLT", .name="다연발", .category="투사체",
               .description="투사체 +2 부채꼴 발사. 투사체당 피해 -25%",
               .grade=RuneGrade::Normal,
               .damageMult=0.75f,
               .vfxMod={.particleCountMult=0.65f, .sizeScaleMult=0.80f, .speedMult=1.3f},
               .extraProjectiles=2,
               .subVFXId="sub_frag_trail" });

    // TRF_PRC 관통 (Rare): 좁고 긴 흰 트레일 → 예리한 관통 느낌
    Register({ .id="TRF_PRC", .name="관통", .category="투사체",
               .description="투사체가 적을 관통해 지나감",
               .grade=RuneGrade::Rare,
               .vfxMod={.particleCountMult=0.85f, .sizeScaleMult=1.30f, .speedMult=1.5f},
               .piercing=true,
               .subVFXId="sub_pierce_trail" });

    // TRF_HOM 유도 (Rare): 나선형 청백 아우라 → 추적 느낌
    Register({ .id="TRF_HOM", .name="유도", .category="투사체",
               .description="투사체가 가장 가까운 적을 자동 추적",
               .grade=RuneGrade::Rare,
               .vfxMod={.particleCountMult=1.15f, .strengthMult=1.4f},
               .homing=true,
               .subVFXId="sub_homing_spiral" });

    // TRF_CHA 연쇄 (Epic): 노란 전기 스파크 → 번개 연쇄 느낌
    Register({ .id="TRF_CHA", .name="연쇄", .category="연쇄",
               .description="적중 시 인근 적 최대 2명에게 연쇄 투사체 발사 (50% 피해)",
               .grade=RuneGrade::Epic,
               .vfxMod={.strengthMult=1.2f},
               .spawnOnHitCount=2,
               .subVFXId="sub_chain_arc" });

    // TRF_ORB 궤도 (Epic): 금빛 링 헤일로 → 공전 표시
    Register({ .id="TRF_ORB", .name="궤도", .category="투사체",
               .description =
    "기본 투사체와 함께 35% 피해의 궤도 동반 투사체 1개를 생성",
               .grade=RuneGrade::Epic,
               .vfxMod={.particleCountMult=1.2f},
               .orbitalCount=1,
               .subVFXId="sub_orbital_halo" });

    // TRF_ECH 반향 (Epic): 희미한 고스트 잔상 → 메아리 느낌
    Register({ .id = "TRF_ECH", .name = "반향", .category = "연쇄",
            .description = "스킬 시전 1회당 첫 유효 적중 시 인근 적 1명에게 소형 반향 투사체를 발사해 50% 피해를 줍니다. 채널은 전체 시전에서 1회 발동합니다.",
            .grade = RuneGrade::Epic,
            .vfxMod = {.particleCountMult = 0.85f},
            .spawnOnHitCount = 1,
            .subVFXId = "sub_echo_ghost" });

    // ─── ⚡ 증폭 룬 ─────────────────────────────────────────────────────────────
    // 서브 VFX를 추가해 각 강화 효과를 시각적으로 나타낸다.
    // 기존 메인 이펙트는 유지.

    // AMP_DMG1 강타 (Normal): 금빛 타격 스파크
    Register({ .id="AMP_DMG1", .name="강타", .category="피해",
               .description="스킬 피해 +15%",
               .grade=RuneGrade::Normal,
               .damageMult=1.15f,
               .subVFXId="sub_strike_spark" });

    // AMP_RAD1 범위 (Normal): 확장 링 펄스
    Register({ .id="AMP_RAD1", .name="범위", .category="범위",
               .description="스킬 반경 +20%",
               .grade=RuneGrade::Normal,
               .radiusMult=1.20f,
               .subVFXId="sub_range_pulse" });

    // AMP_SPD1 신속 (Normal): 속도 스트릭 트레일
    Register({ .id="AMP_SPD1", .name="신속", .category="속도",
               .description="시전 속도 +15%",
               .grade=RuneGrade::Normal,
               .castTimeMult=0.85f,
               .subVFXId="sub_speed_streak" });

    // AMP_DUR1 지속 (Normal): 온기 있는 발광 오라
    Register({ .id="AMP_DUR1", .name="지속", .category="지속",
               .description="스킬 지속시간 +25%",
               .grade=RuneGrade::Normal,
               .durationMult=1.25f,
               .subVFXId="sub_persist_glow" });

    // CH_DUR1 집중석 (Normal): 채널링 지속시간 연장
    Register({ .id="CH_DUR1", .name="집중석", .category="채널",
               .description="채널링 지속시간 +50% (기본 2초 → 3초)",
               .grade=RuneGrade::Normal,
               .channelDurationMult=1.50f,
               .subVFXId="sub_earth" });

    // CH_DUR2 집중석+ (Rare): 채널링 지속시간 대폭 연장
    Register({ .id="CH_DUR2", .name="집중석+", .category="채널",
               .description="채널링 지속시간 +100% (기본 2초 → 4초)",
               .grade=RuneGrade::Rare,
               .channelDurationMult=2.00f,
               .subVFXId="sub_earth" });

    // AMP_DMG2 파괴 (Rare): 주황/붉은 파편 폭발
    Register({ .id="AMP_DMG2", .name="파괴", .category="피해",
               .description="피해 +30%. 단, 쿨다운 +15%",
               .grade=RuneGrade::Rare,
               .damageMult=1.30f, .cooldownMult=1.15f,
               .subVFXId="sub_shatter_shard" });

    // AMP_RAD2 광역 (Rare): 넓은 충격파 링
    Register({ .id="AMP_RAD2", .name="광역", .category="범위",
               .description="반경 +35%. 단, 피해 -10%",
               .grade=RuneGrade::Rare,
               .damageMult=0.90f, .radiusMult=1.35f,
               .subVFXId="sub_blast_wave" });

    // AMP_CD1 냉각 (Rare): 청록 냉기 미스트
    Register({ .id="AMP_CD1", .name="냉각", .category="쿨다운",
               .description="스킬 쿨다운 -25%",
               .grade=RuneGrade::Rare,
               .cooldownMult=0.75f,
               .subVFXId="sub_cool_mist" });

    // AMP_MCO 절약 (Rare): 파란 마나 위스프
    Register({ .id="AMP_MCO", .name="절약", .category="마나",
               .description="마나 소모 -40%. 단, 피해 -15%",
               .grade=RuneGrade::Rare,
               .damageMult=0.85f, .manaCostMult=0.60f,
               .subVFXId="sub_mana_wisp" });

    // AMP_DMG3 과부하 (Epic): 보라/흰 전하 폭주 (Ring+Sphere 2레이어)
    Register({ .id="AMP_DMG3", .name="과부하", .category="피해",
               .description="피해 +50%. 단, 쿨다운 +30%·마나 +20%",
               .grade=RuneGrade::Epic,
               .damageMult=1.50f, .cooldownMult=1.30f, .manaCostMult=1.20f,
               .subVFXId="sub_overload_surge" });

    // AMP_RAD3 확장 (Epic): 금빛 대형 코로나 링
    Register({ .id="AMP_RAD3", .name="확장", .category="범위",
               .description="반경 +50%. 단, 피해 -20%",
               .grade=RuneGrade::Epic,
               .damageMult=0.80f, .radiusMult=1.50f,
               .subVFXId="sub_expand_corona" });

    // ─── ⚫ 심연 룬 (Legendary) ──────────────────────────────────────────────────

    // ABY_TIM 시간 역행: 적중 시 해당 스킬 쿨다운 1초 감소
    Register({ .id="ABY_TIM", .name="시간 역행", .category="심연",
               .description="스킬 적중 시 해당 스킬 쿨다운 1초 감소",
               .grade=RuneGrade::Legendary,
               .onHit=[](SkillContext& ctx){
                   if (!ctx.caster || ctx.skillSlot == SkillSlot::Count) return;
                   auto* pSkill = ctx.caster->GetComponent<SkillComponent>();
                   if (pSkill) pSkill->ReduceCooldown(ctx.skillSlot, 1.f);
               }});

    // ABY_EXC 처형자: HP 30% 이하 적에게 피해 +50%
    Register({ .id="ABY_EXC", .name="처형자", .category="심연",
               .description="HP 30% 이하 적에게 피해 +50%",
               .grade=RuneGrade::Legendary,
               .execDamageBonus=0.50f });

    // ABY_VMP 흡혈: 피해의 15%를 HP로 회복
    Register({ .id="ABY_VMP", .name="흡혈", .category="심연",
               .description="스킬 피해의 15%를 HP로 회복",
               .grade=RuneGrade::Legendary,
               .lifestealRatio=0.15f });

    // ABY_INF 무한: 10% 확률로 쿨다운 즉시 초기화
    Register({ .id="ABY_INF", .name="무한", .category="심연",
               .description="적중 시 10% 확률로 해당 스킬 쿨다운 즉시 초기화",
               .grade=RuneGrade::Legendary,
               .cdResetChance=0.10f });

    // ABY_SHD 보호막: 시전 시 기본 데미지 30% 만큼 쉴드 생성
    Register({ .id="ABY_SHD", .name="보호막", .category="심연",
               .description="스킬 시전 시 기본 데미지의 30% 만큼 쉴드 생성",
               .grade=RuneGrade::Legendary,
               .onCast=[](SkillContext& ctx){
                   if (!ctx.caster) return;
                   auto* pPlayer = ctx.caster->GetComponent<PlayerComponent>();
                   if (pPlayer) pPlayer->AddShield(ctx.baseDamage * 0.30f);
               }});

    // ABY_RVG 보복: 피격 후 10초 내 다음 스킬 데미지 +30%
    Register({ .id="ABY_RVG", .name="보복", .category="심연",
               .description="피격 시 10초 내 다음 스킬 데미지 +30%",
               .grade=RuneGrade::Legendary,
               .revengeBonus=0.30f });

    // ABY_OVL 과열: 동일 스킬 연속 3회 사용 시 다음 1회 +60%
    Register({ .id="ABY_OVL", .name="과열", .category="심연",
               .description="동일 스킬 연속 3회 사용 시 다음 1회 피해 +60%",
               .grade=RuneGrade::Legendary,
               .overheatBonus=0.60f });

    // ABY_RES 원소 공명: 다른 원소 룬 2종 이상 장착 시 피해 +50% (BuildSkillStats에서 처리)
    Register({ .id="ABY_RES", .name="원소 공명", .category="심연",
               .description="서로 다른 원소 룬 2종 이상 장착 시 피해 +50%",
               .grade=RuneGrade::Legendary });

    // ABY_ECO 메아리: 50% 확률로 2초 뒤 가장 가까운 적을 향해 50% 위력 재발동
    Register({ .id = "ABY_ECO", .name = "메아리", .category = "심연",
            .description = "스킬 시전 시 50% 확률로 2초 후 가장 가까운 적을 향해 50% 위력으로 재발동합니다. 설치형 스킬은 함정이 실제 발동할 때 판정합니다.",
            .grade = RuneGrade::Legendary,
            .echoOnCast = true });
}
