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

    // DarkLord 전용 — 검기 컷씬 자체가 텔레그래프이므로 평면 인디케이터는 끈다.
    //   ShowIndicators 가 None 을 보면 즉시 return → 빨간 박스 안 뜸.
    int GetIndicatorTypeOverride() const override { return 0; /* IndicatorType::None */ }

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

    // ── Projectile burst (검기 폭격) — desc.projectileBurstCount > 1 일 때만 활성 ──
    //   Hit entry 시 spawn 위치/방향을 lock 하고 interval 마다 1발씩 발사.
    //   index → offset 매핑: (idx - (N-1)/2) * spreadDeg (중앙 0 기준 좌우 spread).
    int               m_nBurstRemaining   = 0;
    int               m_nBurstShotIndex   = 0;     // 0-based; 0 = 첫 발 (중앙)
    float             m_fBurstTimer       = 0.0f;
    DirectX::XMFLOAT3 m_xmf3BurstStartPos = { 0,0,0 };
    DirectX::XMFLOAT3 m_xmf3BurstBaseDir  = { 0,0,1 };
    float             m_fBurstDamage      = 0.0f;
};
