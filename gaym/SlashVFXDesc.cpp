#include "stdafx.h"
#include "SlashVFXDesc.h"

SlashVFXDesc SlashVFXDesc::Preset(ElementType e, SlashPowerLevel lvl)
{
    SlashVFXDesc d;
    d.element    = e;
    d.powerLevel = lvl;
    // 검기 본체는 항상 보스 정면으로 spawn — 모든 톤. (피드백: 검 끝 방향은 회전 X
    // 정면 베기 시 옆으로 새서 어색함. ribbon 이 이미 검 sweep 따라가므로 emitter 는
    // 가독성 우선 = 보스 정면.)  필요 시 desc.aimMode 수동 override 가능.
    d.aimMode    = BladeAimMode::BossForward;

    switch (lvl)
    {
    case SlashPowerLevel::Small:
        d.chargeDuration        = 0.25f;
        d.hitStopSeconds        = 0.02f;
        d.whiteFlashAlpha       = 0.15f;
        d.whiteFlashFade        = 0.06f;
        d.ribbonLifetime        = 0.30f;
        d.diskLifetime          = 0.15f;
        d.useDiskSlash          = false;
        d.useDistortion         = false;
        d.residueLifetime       = 1.2f;
        d.residueParticleBudget = 25;
        d.useGroundResidue      = false;
        d.cameraShakeIntensity  = 0.6f;
        d.cameraShakeDuration   = 0.12f;
        d.cameraZoomBoost       = 0.0f;
        d.usePostPulse          = false;
        d.impactHitStopSeconds  = 0.012f;
        d.impactShakeIntensity  = 0.4f;
        d.impactShakeDuration   = 0.07f;
        d.impactFlashAlpha      = 0.08f;
        d.impactFlashFade       = 0.05f;
        break;

    case SlashPowerLevel::Medium:
        d.chargeDuration        = 0.50f;
        d.hitStopSeconds        = 0.05f;   // 0.04 → 0.05 (Release 모멘트 강화)
        d.whiteFlashAlpha       = 0.40f;   // 0.30 → 0.40
        d.whiteFlashFade        = 0.08f;   // 0.10 → 0.08 (ease-out 과 함께)
        d.ribbonLifetime        = 0.40f;
        d.diskLifetime          = 0.22f;
        d.useDiskSlash          = true;
        d.useDistortion         = false;
        d.residueLifetime       = 2.0f;
        d.residueParticleBudget = 40;
        d.useGroundResidue      = true;
        d.cameraShakeIntensity  = 1.6f;    // 1.2 → 1.6
        d.cameraShakeDuration   = 0.18f;   // 0.20 → 0.18 (짧고 강하게)
        d.cameraZoomBoost       = 2.5f;    // 1.5 → 2.5
        d.usePostPulse          = false;
        d.impactHitStopSeconds  = 0.02f;
        d.impactShakeIntensity  = 0.8f;
        d.impactShakeDuration   = 0.10f;
        d.impactFlashAlpha      = 0.18f;
        d.impactFlashFade       = 0.06f;
        break;

    case SlashPowerLevel::Signature:
        d.chargeDuration        = 1.10f;
        d.hitStopSeconds        = 0.07f;
        d.whiteFlashAlpha       = 0.55f;
        d.whiteFlashFade        = 0.15f;
        d.ribbonLifetime        = 0.55f;
        d.diskLifetime          = 0.30f;
        d.useDiskSlash          = true;
        d.useDistortion         = true;
        d.residueLifetime       = 3.0f;
        d.residueParticleBudget = 100;
        d.useGroundResidue      = true;
        d.cameraShakeIntensity  = 2.0f;
        d.cameraShakeDuration   = 0.35f;
        d.cameraZoomBoost       = 3.5f;
        d.usePostPulse          = true;
        d.impactHitStopSeconds  = 0.035f;
        d.impactShakeIntensity  = 1.3f;
        d.impactShakeDuration   = 0.14f;
        d.impactFlashAlpha      = 0.30f;
        d.impactFlashFade       = 0.08f;
        break;

    case SlashPowerLevel::Ultimate:
        d.chargeDuration        = 1.80f;
        d.hitStopSeconds        = 0.12f;
        d.whiteFlashAlpha       = 0.85f;
        d.whiteFlashFade        = 0.25f;
        d.ribbonLifetime        = 0.80f;
        d.diskLifetime          = 0.45f;
        d.useDiskSlash          = true;
        d.useDistortion         = true;
        d.residueLifetime       = 4.5f;
        d.residueParticleBudget = 160;
        d.useGroundResidue      = true;
        d.cameraShakeIntensity  = 3.0f;
        d.cameraShakeDuration   = 0.55f;
        d.cameraZoomBoost       = 5.5f;
        d.usePostPulse          = true;
        d.impactHitStopSeconds  = 0.06f;
        d.impactShakeIntensity  = 2.0f;
        d.impactShakeDuration   = 0.22f;
        d.impactFlashAlpha      = 0.50f;
        d.impactFlashFade       = 0.12f;
        break;
    }
    return d;
}

void SlashVFXDesc::ApplyPresentation(SlashPresentation s)
{
    presentation = s;
    switch (s)
    {
    case SlashPresentation::Light:
        // 사용자 의도: emitter 본체 X, 검 끝 ribbon 만 남는 "채찍" 느낌.
        //   → ComboHit 의 fVFXScale (ribbon 두께), eShape Long (긴 호), fHitTime/Recovery
        //     길게 잡힌 helper 사용 시 진짜 채찍 톤. 카메라/플래시는 가볍게.
        skipBladeEmitter       = true;
        hitStopSeconds        *= 0.30f;
        whiteFlashAlpha       *= 0.20f;
        whiteFlashFade        *= 0.7f;
        cameraShakeIntensity  *= 0.30f;
        cameraShakeDuration   *= 0.7f;
        cameraZoomBoost       *= 0.10f;
        impactHitStopSeconds  *= 0.30f;
        impactShakeIntensity  *= 0.40f;
        impactFlashAlpha      *= 0.25f;
        break;

    case SlashPresentation::Massive:
        // 거대 emitter + 강한 화면 연출.
        preferHeavyBlade       = true;
        bladeForwardMultiplier = 1.15f;
        bladeScaleMultiplier   = 1.50f;
        hitStopSeconds        *= 1.60f;
        whiteFlashAlpha       *= 1.60f;
        whiteFlashFade        *= 1.30f;
        cameraShakeIntensity  *= 1.80f;
        cameraShakeDuration   *= 1.40f;
        cameraZoomBoost       *= 1.80f;
        impactHitStopSeconds  *= 1.70f;
        impactShakeIntensity  *= 1.80f;
        impactFlashAlpha      *= 1.80f;
        break;

    case SlashPresentation::Projectile:
        // 검기가 발사된 호 — 멀리·길게·빠르게.
        bladeForwardMultiplier = 2.40f;
        bladeScaleMultiplier   = 1.10f;
        cameraShakeIntensity  *= 0.85f;
        cameraShakeDuration   *= 0.90f;
        cameraZoomBoost       *= 1.10f;
        impactHitStopSeconds  *= 1.10f;
        break;

    case SlashPresentation::CrossSigil:
        // 4방향 4원소 동시 발사 — 시그니처.
        preferHeavyBlade       = true;
        bladeForwardMultiplier = 1.40f;
        bladeScaleMultiplier   = 1.30f;
        hitStopSeconds        *= 1.80f;
        whiteFlashAlpha       *= 1.70f;
        cameraShakeIntensity  *= 2.00f;
        cameraShakeDuration   *= 1.50f;
        cameraZoomBoost       *= 2.20f;
        impactHitStopSeconds  *= 1.70f;
        impactShakeIntensity  *= 1.90f;
        impactFlashAlpha      *= 1.70f;
        break;

    case SlashPresentation::Phantom:
        // 분신 검기 — 다중 위치 시간차 spawn.
        bladeForwardMultiplier = 1.20f;
        bladeScaleMultiplier   = 1.00f;
        hitStopSeconds        *= 1.30f;
        whiteFlashAlpha       *= 1.20f;
        cameraShakeIntensity  *= 1.40f;
        cameraShakeDuration   *= 1.20f;
        cameraZoomBoost       *= 1.30f;
        impactHitStopSeconds  *= 1.20f;
        impactShakeIntensity  *= 1.30f;
        break;

    case SlashPresentation::FinalJudgment:
        // 인과 역전 궁극 — 사전 라인 + 4원소 십자 폭발.
        preferHeavyBlade       = true;
        bladeForwardMultiplier = 1.50f;
        bladeScaleMultiplier   = 1.50f;
        hitStopSeconds        *= 2.20f;
        whiteFlashAlpha       *= 2.00f;
        cameraShakeIntensity  *= 2.50f;
        cameraShakeDuration   *= 1.80f;
        cameraZoomBoost       *= 2.50f;
        impactHitStopSeconds  *= 2.00f;
        impactShakeIntensity  *= 2.20f;
        impactFlashAlpha      *= 2.00f;
        break;

    case SlashPresentation::Standard:
    default:
        // no-op
        break;
    }

    // ── 결과값 글로벌 클램프 ──────────────────────────────────────────────
    //   Ultimate × Massive 등 곱연산 폭주로 화면이 못 봐줄 정도로 흔들리거나
    //   카메라가 너무 멀리 뒤로 빠지는 것을 방지. 톤은 강하게 유지하되 가시성 보장.
    if (whiteFlashAlpha       > 0.85f) whiteFlashAlpha       = 0.85f;
    if (impactFlashAlpha      > 0.55f) impactFlashAlpha      = 0.55f;
    if (cameraShakeIntensity  > 3.0f)  cameraShakeIntensity  = 3.0f;   // 5+ 폭주 차단
    if (cameraShakeDuration   > 0.55f) cameraShakeDuration   = 0.55f;
    if (cameraZoomBoost       > 5.0f)  cameraZoomBoost       = 5.0f;   // 9+ 폭주 차단
    if (impactShakeIntensity  > 2.5f)  impactShakeIntensity  = 2.5f;
    if (hitStopSeconds        > 0.13f) hitStopSeconds        = 0.13f;
    if (impactHitStopSeconds  > 0.08f) impactHitStopSeconds  = 0.08f;
}
