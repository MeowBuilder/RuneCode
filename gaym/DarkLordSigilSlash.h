#pragma once
#include "ComboAttackBehavior.h"
#include "SlashVFXDesc.h"
#include "SlashCue.h"

// ─── DarkLordSigilSlash ──────────────────────────────────────────────────────
//
// 다크로드의 기본 근접 검기 패턴. ComboAttackBehavior 의 모든 짤패 메커니즘
// (windup→hit→recovery, ribbon, crescent flash, 사거리/콘 판정, 임팩트 VFX) 을
// 그대로 활용하면서, 그 위에 검기 5-레이어 컷씬 (L1 차징·L2 발도·L5 임팩트) 을
// SlashCue 로 얹는다.
//
// 사용 예 (EnemySpawner / BossPhaseConfig 안에서):
//
//   SlashVFXDesc desc = SlashVFXDesc::Preset(ElementType::Fire, SlashPowerLevel::Medium);
//   std::vector<ComboAttackBehavior::ComboHit> hits;
//   ComboAttackBehavior::ComboHit h;
//   h.fDamage = 70.f; h.fHitRange = 10.f; h.fConeAngle = 110.f;
//   h.strAnimation = "attack2"; h.strVFXOnHit = "status_burn";
//   h.strVFXImpact = "sub_strike_spark";
//   h.eShape = ComboAttackBehavior::SwordEnergyShape::Wide;
//   hits.push_back(h);
//   return std::make_unique<DarkLordSigilSlash>(desc, std::move(hits));
class DarkLordSigilSlash : public ComboAttackBehavior
{
public:
    DarkLordSigilSlash(const SlashVFXDesc& desc,
                       const std::vector<ComboAttackBehavior::ComboHit>& hits);
    ~DarkLordSigilSlash() override;

    void Execute(EnemyComponent* pEnemy) override;
    void Update(float dt, EnemyComponent* pEnemy) override;
    void Reset() override;

private:
    SlashVFXDesc m_desc;
    SlashCue     m_cue;

    // 직전 프레임 phase — 전환 감지용 (Windup→Hit / Hit→Recovery)
    HitPhase m_ePrevPhase = HitPhase::Windup;
    int      m_nPrevHit   = -1;
    bool     m_bInitialized = false;

    // Hit entry 후 Impact 발사까지 짧은 딜레이 — "whoosh ... BOOM" 분리감.
    //   <=0 인 동안 Impact 미발사. Hit entry 에서 m_fImpactDelay 로 초기화.
    float    m_fImpactCountdown = -1.0f;
    static constexpr float kImpactDelay = 0.04f;  // 발도 직후 ~40ms 뒤 진짜 임팩트
};
