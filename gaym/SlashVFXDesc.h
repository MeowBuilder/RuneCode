#pragma once
#include <cstdint>
#include "SkillTypes.h"  // ElementType

// ─── Dark Lord 검기(Sword Aura) 5-레이어 컷씬 시스템 공통 정의 ───────────────
//
// 모든 검기 패턴은 SlashVFXDesc 1개로 강도(SlashPowerLevel)와 레이어 활성화를
// 설정한 뒤 SlashCue(Day2~)가 BeginCharge / Release / SpawnBladeBody /
// SpawnResidue / SpawnImpact 5단계를 호출. powerLevel만 정하면 합리적 기본값이
// Preset()으로 채워진다.

enum class SlashPowerLevel : uint8_t
{
    Small,      // 기본 베기. L3 ribbon 위주 + 작은 임팩트.
    Medium,     // 중형 검기. L1 차징 약하게 + L3 ribbon+disk + 약한 잔류.
    Signature,  // 시그니처 (Cross Sigil 등). 전 레이어 적용.
    Ultimate,   // 궁극 (Final Judgment). 전 레이어 + 후처리 펄스 + 컷인.
};

// 검기 본체(Crescent emitter) 의 spawn 위치/방향 기준.
//   SwordTip      : 검 끝 위치 + 검 끝 방향 — 빠른 sweep, 검의 자세를 그대로 따라감.
//   BossForward   : 보스 정면 방향 + 검 끝 근처 위치 — 묵직 일격용 가독성 우선.
enum class BladeAimMode : uint8_t
{
    SwordTip,
    BossForward,
};

// 검기 연출 스타일 — 같은 애니메이션 + 같은 원소로도 다른 인상.
//   Light         : 검 궤적(ribbon) 위주. emitter 본체는 skip. 약한 카메라/플래시.
//   Standard      : 기본 — 검기 본체 + ribbon + 표준 카메라/플래시.
//   Massive       : 거대 emitter + 강한 화면 연출. "광역 묵직 일격".
//   Projectile    : 검기가 멀리·길게·빠르게 — 발사된 호.
//   CrossSigil    : 4방향(N/E/S/W) 에 4원소 동시 발사 — 시그니처 (P1→P2 전환).
//   Phantom       : 보스 주변 3~5곳에 분신처럼 시간차 spawn — P3/P4 시그니처.
//   FinalJudgment : Windup 동안 십자 라인 사전 시각화 → Hit 에 4원소 폭발 — P4 마무리.
//   TwinCleave    : 정면 ±separation° 두 방향 동시 cone — 회피 라인 압박 (P2 Wind 톤).
enum class SlashPresentation : uint8_t
{
    Light,
    Standard,
    Massive,
    Projectile,
    CrossSigil,
    Phantom,
    FinalJudgment,
    TwinCleave,
};

struct SlashVFXDesc
{
    // 원소·강도 (필수)
    ElementType       element      = ElementType::Fire;
    SlashPowerLevel   powerLevel   = SlashPowerLevel::Small;
    BladeAimMode      aimMode      = BladeAimMode::BossForward;
    SlashPresentation presentation = SlashPresentation::Standard;

    // ── Presentation modifier (Light/Massive/Projectile 시 자동 조정) ──
    bool  skipBladeEmitter       = false;   // Light: emitter spawn 안 함 (ribbon만)
    float bladeForwardMultiplier = 1.0f;    // Projectile: spawn 거리 배수
    float bladeScaleMultiplier   = 1.0f;    // Massive: emitter 규모 (현재 후처리 X — VFXManager 호출 후 size override 미지원이라 일단 보스 forward Y 만 조정)
    bool  preferHeavyBlade       = false;   // Massive: HitTpl 의 Quick → Heavy 자동 승격

    // Projectile burst — Projectile presentation 일 때만 의미.
    //   burstCount > 1 이면 한 hit 에서 N발 연속 발사. burstInterval(초) 간격, ±burstSpread(도) 좌우 spread.
    int   projectileBurstCount    = 1;
    float projectileBurstInterval = 0.12f;
    float projectileBurstSpreadDeg = 5.0f;

    // TwinCleave — 정면 ±twinSeparation° 두 방향 동시 cone 데미지 + VFX.
    float twinSeparationDeg      = 35.0f;

    // L1 Charge — 검 본에 원소 오라가 응축되는 시간 (0=비활성)
    float chargeDuration       = 0.6f;

    // L2 Release — 정적→동적 전환의 찰나
    float hitStopSeconds       = 0.04f;   // 0=비활성. 보통 0.02~0.08 (1~5프레임)
    float whiteFlashAlpha      = 0.40f;   // 0=비활성. peak alpha (0~1)
    float whiteFlashFade       = 0.10f;   // alpha→0 페이드 시간(초)

    // L3 Blade Body — 검기 본체 수명
    float ribbonLifetime       = 0.35f;   // SwordTrailMesh 잔존 시간(초)
    float diskLifetime         = 0.20f;   // Crescent disk-slash 잔존 시간(초)
    bool  useDiskSlash         = true;
    bool  useDistortion        = false;   // SSF 굴절 — Signature/Ultimate만 권장

    // L4 Residue — 잔향 / 환경
    float residueLifetime      = 2.0f;    // 지면 데칼 + 잔재 입자 lingering 시간
    int   residueParticleBudget = 30;     // Small 20~40, Medium 25~50,
                                          // Signature 80~120, Ultimate 150+
    bool  useGroundResidue     = true;

    // L5 Impact — 충격
    //   * Release(발도) ≠ Impact(실제 타격). 두 모멘트를 분리해서 2단 임팩트 구조.
    //     Release : 휘두르기 시작 — 큰 hit-stop / 큰 셰이크 / white flash
    //     Impact  : 타격 순간    — micro hit-stop / 짧고 강한 셰이크 / impact VFX
    float cameraShakeIntensity = 1.0f;    // Release 셰이크 강도
    float cameraShakeDuration  = 0.20f;
    float cameraZoomBoost      = 0.0f;    // 음수 = pull in, 양수 = push back
    bool  usePostPulse         = false;   // (Day6+ 후처리 펄스, 후순위)

    // Impact 전용 (실제 타격 모멘트)
    float impactHitStopSeconds = 0.02f;   // micro hit-stop — Release 보다 짧게
    float impactShakeIntensity = 0.6f;    // Release 보다 약하지만 더 날카롭게
    float impactShakeDuration  = 0.10f;
    float impactFlashAlpha     = 0.15f;   // 작은 색띠 펄스 (원소색)
    float impactFlashFade      = 0.06f;

    // 합리적 기본값 채우기 — 패턴 작성자는 Preset 후 필요한 필드만 override.
    static SlashVFXDesc Preset(ElementType e, SlashPowerLevel lvl);

    // Presentation 스타일 적용 — power level 기본값을 override 한다.
    //   호출 예: auto d = SlashVFXDesc::Preset(Fire, Medium); d.ApplyPresentation(Light);
    void ApplyPresentation(SlashPresentation s);
};
