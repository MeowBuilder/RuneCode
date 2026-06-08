#include "stdafx.h"
#include "DarkLordSigilSlash.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "Room.h"
#include "Scene.h"
#include "VFXManager.h"

using namespace DirectX;

namespace
{
    // 검 본 worldMatrix → 검 끝 월드 좌표 + 검 진행 방향.
    //   ComboAttackBehavior 의 UpdateTrailEmission(:692~700) 과 동일 convention:
    //     grip = (w._41, w._42, w._43)
    //     +Y row = (w._21, w._22, w._23) 가 본의 local +Y (DarkKnight2 기준 grip→pommel)
    //     -Y 가 검 끝.
    //   bone row vector 길이 = boss scale × mesh local axis 길이 → 자동 매칭.
    void GetSwordTipFromBone(class TransformComponent* pSwordBone,
                              XMFLOAT3& outTip, XMFLOAT3& outDir)
    {
        const XMFLOAT4X4& w = pSwordBone->GetWorldMatrix();
        XMFLOAT3 grip = { w._41, w._42, w._43 };
        XMFLOAT3 dirY = { w._21, w._22, w._23 };
        outTip = { grip.x - dirY.x, grip.y - dirY.y, grip.z - dirY.z };
        // dir = 검 길이축 정규화 (효과의 "방향성" 으로 사용)
        float len = sqrtf(dirY.x*dirY.x + dirY.y*dirY.y + dirY.z*dirY.z);
        if (len > 1e-4f)
            outDir = { -dirY.x/len, -dirY.y/len, -dirY.z/len };
        else
            outDir = { 0.0f, 0.0f, 1.0f };
    }

    // 검 본 없을 때 fallback — 보스 정면 + 가슴 높이.
    void GetSwordTipFallback(EnemyComponent* pEnemy,
                              XMFLOAT3& outTip, XMFLOAT3& outDir)
    {
        outTip = { 0, 9, 0 }; outDir = { 0, 0, 1 };
        if (!pEnemy) return;
        GameObject* pOwner = pEnemy->GetOwner();
        if (!pOwner) return;
        TransformComponent* pBossT = pOwner->GetTransform();
        if (!pBossT) return;

        XMFLOAT3 bossPos = pBossT->GetPosition();
        float yawRad = XMConvertToRadians(pBossT->GetRotation().y);
        XMFLOAT3 fwd = { sinf(yawRad), 0.0f, cosf(yawRad) };
        outTip = { bossPos.x + fwd.x * 4.0f,
                   bossPos.y + 9.0f,
                   bossPos.z + fwd.z * 4.0f };
        outDir = fwd;
    }
}

DarkLordSigilSlash::DarkLordSigilSlash(const SlashVFXDesc& desc,
                                       const std::vector<ComboAttackBehavior::ComboHit>& hits)
    : ComboAttackBehavior(hits)
    , m_desc(desc)
{
    // 우리는 자체 EffectDef (Boss_CrescentSigil_Fire / _Heavy) 로 검기 본체를 그리므로
    // 부모 ComboAttackBehavior 의 정적 SwordTrailMesh crescent flash 는 중복 → 끈다.
    //   ribbon (검 본 추적) 은 그대로 유지 — 보조 코어 역할.
    SetDisableStaticCrescent(true);
}

DarkLordSigilSlash::~DarkLordSigilSlash()
{
    m_cue.End();
}

void DarkLordSigilSlash::Execute(EnemyComponent* pEnemy)
{
    // 부모 — windup 타이머 시작.
    ComboAttackBehavior::Execute(pEnemy);

    // 검 본은 부모가 lazy 캐싱하므로 (UpdateTrailMesh 가 처음 호출될 때) 명시적으로 사전 캐싱.
    EnsureSwordBoneCached(pEnemy);

    Scene* pScene = nullptr;
    if (pEnemy)
    {
        if (CRoom* pRoom = pEnemy->GetRoom())
            pScene = pRoom->GetScene();
    }

    XMFLOAT3 tip, dir;
    if (TransformComponent* pBone = GetSwordBone())
        GetSwordTipFromBone(pBone, tip, dir);
    else
        GetSwordTipFallback(pEnemy, tip, dir);

    m_cue.Begin(pScene, m_desc, tip, dir);

    m_ePrevPhase   = HitPhase::Windup;
    m_nPrevHit     = GetCurrentHitIndex();
    m_bInitialized = true;
}

void DarkLordSigilSlash::Update(float dt, EnemyComponent* pEnemy)
{
    ComboAttackBehavior::Update(dt, pEnemy);

    if (!m_bInitialized) return;

    HitPhase curPhase = GetCurrentHitPhase();
    int      curHit   = GetCurrentHitIndex();

    bool bHitJustEntered   = (m_ePrevPhase == HitPhase::Windup &&
                              curPhase     == HitPhase::Hit);
    bool bRecovJustEntered = (m_ePrevPhase == HitPhase::Hit &&
                              curPhase     == HitPhase::Recovery);
    bool bNewHitStarted    = (curHit != m_nPrevHit && curPhase == HitPhase::Windup);

    // 검 본 → 끝 좌표/방향 계산 (매 프레임 — 검이 움직이므로)
    XMFLOAT3 tip, dir;
    if (TransformComponent* pBone = GetSwordBone())
        GetSwordTipFromBone(pBone, tip, dir);
    else
        GetSwordTipFallback(pEnemy, tip, dir);

    if (bNewHitStarted)
    {
        Scene* pScene = nullptr;
        if (pEnemy)
            if (CRoom* pRoom = pEnemy->GetRoom())
                pScene = pRoom->GetScene();
        m_cue.End();
        m_cue.Begin(pScene, m_desc, tip, dir);
        m_fImpactCountdown = -1.0f;
    }
    else if (bHitJustEntered)
    {
        // L2 Release — 휘두르기 시작 모멘트
        m_cue.Release();
        // Impact 는 raw dt 기준 짧은 딜레이 후 별도 발사 (whoosh → BOOM 분리)
        m_fImpactCountdown = kImpactDelay;

        // ★ L3 메인 검기 본체 EffectDef 직접 spawn — Presentation 분기 처리.
        //   Light         : emitter 본체 skip (ribbon 만)
        //   Standard      : 보스 정면 표준 거리, Quick crescent
        //   Massive       : _Heavy 자동 승격 + 추가 Ring 충격파 발사
        //   Projectile    : "Boss_CrescentBolt_{원소}" 발사형 EffectDef 사용
        //   CrossSigil    : 4방향 4원소 동시 발사 — 시그니처
        //   Phantom       : 보스 주변 3곳 분신 spawn
        //   FinalJudgment : 8방향(십자+대각) 폭발

        // CrossSigil/Phantom/FinalJudgment 는 다중 위치 spawn → 표준 spawn 분기 전에 처리.
        if (!m_desc.skipBladeEmitter && pEnemy && pEnemy->GetOwner())
        {
            if (m_desc.presentation == SlashPresentation::CrossSigil
                || m_desc.presentation == SlashPresentation::Phantom
                || m_desc.presentation == SlashPresentation::FinalJudgment)
            {
                if (const ComboHit* pHit = GetCurrentHit())
                {
                    if (CRoom* pRoom = pEnemy->GetRoom())
                    {
                        if (Scene* pScene = pRoom->GetScene())
                        {
                            if (auto* pVFX = pScene->GetVFXManager())
                            {
                                TransformComponent* pBossT = pEnemy->GetOwner()->GetTransform();
                                XMFLOAT3 bossPos = pBossT->GetPosition();
                                float baseYawRad = XMConvertToRadians(pBossT->GetRotation().y);
                                float forwardDist = pHit->fHitRange * 0.55f
                                                  * m_desc.bladeForwardMultiplier;

                                auto SpawnAtAngle = [&](float angleDeg, ElementType elem)
                                {
                                    float yawRad = baseYawRad + XMConvertToRadians(angleDeg);
                                    XMFLOAT3 fwd = { sinf(yawRad), 0.0f, cosf(yawRad) };
                                    XMFLOAT3 sp  = { bossPos.x + fwd.x * forwardDist,
                                                     tip.y,
                                                     bossPos.z + fwd.z * forwardDist };
                                    std::string name = "Boss_CrescentSigil_";
                                    switch (elem)
                                    {
                                    case ElementType::Fire:  name += "Fire";  break;
                                    case ElementType::Water: name += "Water"; break;
                                    case ElementType::Wind:  name += "Wind";  break;
                                    case ElementType::Earth: name += "Earth"; break;
                                    default:                 name += "Fire";  break;
                                    }
                                    name += "_Heavy";
                                    pVFX->Spawn(name, sp, fwd, 0u, false);

                                    // 발치 ring 충격파도 추가 — 임팩트 더 명확하게
                                    XMFLOAT3 ringPos = { sp.x, bossPos.y + 0.2f, sp.z };
                                    pVFX->Spawn(SlashCue::ImpactEffectName(elem),
                                                ringPos, fwd, 0u, false);
                                };

                                if (m_desc.presentation == SlashPresentation::CrossSigil)
                                {
                                    // 4방향 (N/E/S/W) × 4원소
                                    SpawnAtAngle(  0.f, ElementType::Fire);
                                    SpawnAtAngle( 90.f, ElementType::Water);
                                    SpawnAtAngle(180.f, ElementType::Wind);
                                    SpawnAtAngle(270.f, ElementType::Earth);
                                }
                                else if (m_desc.presentation == SlashPresentation::Phantom)
                                {
                                    // 4방향 분신 (정면 + 좌우 + 뒤). 모두 같은 원소.
                                    SpawnAtAngle(   0.f, m_desc.element);
                                    SpawnAtAngle( -75.f, m_desc.element);
                                    SpawnAtAngle(  75.f, m_desc.element);
                                    SpawnAtAngle( 180.f, m_desc.element);
                                }
                                else // FinalJudgment
                                {
                                    // 8방향 (십자 + 대각) × 4원소 순환
                                    SpawnAtAngle(  0.f, ElementType::Fire);
                                    SpawnAtAngle( 45.f, ElementType::Wind);
                                    SpawnAtAngle( 90.f, ElementType::Water);
                                    SpawnAtAngle(135.f, ElementType::Earth);
                                    SpawnAtAngle(180.f, ElementType::Fire);
                                    SpawnAtAngle(225.f, ElementType::Wind);
                                    SpawnAtAngle(270.f, ElementType::Water);
                                    SpawnAtAngle(315.f, ElementType::Earth);
                                }
                            }
                        }
                    }
                }
                m_ePrevPhase = curPhase;
                m_nPrevHit   = curHit;
                return;   // 다중 spawn 처리 끝 → 표준 단일 spawn 건너뜀
            }
        }

        if (!m_desc.skipBladeEmitter)
        {
            if (const ComboHit* pHit = GetCurrentHit())
            {
                std::string effectName = pHit->strVFXOnHit;

                // Projectile 톤: 발사형 Bolt EffectDef 로 강제 교체
                if (m_desc.presentation == SlashPresentation::Projectile && !effectName.empty())
                {
                    // 원소 추출 — "Boss_CrescentSigil_Fire" → "Fire"
                    size_t underscorePos = effectName.rfind('_');
                    // _Heavy suffix 제거하고 원소만 추출
                    std::string base = effectName;
                    if (base.size() >= 6 && base.compare(base.size()-6, 6, "_Heavy") == 0)
                        base = base.substr(0, base.size()-6);
                    size_t lastUnder = base.rfind('_');
                    std::string elemName = (lastUnder != std::string::npos)
                                           ? base.substr(lastUnder)
                                           : "_Fire";
                    effectName = "Boss_CrescentBolt" + elemName;
                }
                // Massive 톤: Quick → _Heavy 자동 승격
                else if (m_desc.preferHeavyBlade && !effectName.empty()
                         && effectName.find("_Heavy") == std::string::npos)
                {
                    effectName += "_Heavy";
                }

                if (!effectName.empty() && pEnemy)
                {
                    if (CRoom* pRoom = pEnemy->GetRoom())
                    {
                        if (Scene* pScene = pRoom->GetScene())
                        {
                            if (auto* pVFX = pScene->GetVFXManager())
                            {
                                XMFLOAT3 spawnPos, spawnDir;
                                if (m_desc.aimMode == BladeAimMode::BossForward
                                    && pEnemy->GetOwner())
                                {
                                    TransformComponent* pBossT = pEnemy->GetOwner()->GetTransform();
                                    XMFLOAT3 bossPos = pBossT->GetPosition();
                                    float yawRad = XMConvertToRadians(pBossT->GetRotation().y);
                                    XMFLOAT3 fwd = { sinf(yawRad), 0.0f, cosf(yawRad) };
                                    float forwardDist = pHit->fHitRange * 0.55f
                                                      * m_desc.bladeForwardMultiplier;
                                    spawnPos = { bossPos.x + fwd.x * forwardDist,
                                                 tip.y,
                                                 bossPos.z + fwd.z * forwardDist };
                                    spawnDir = fwd;
                                }
                                else
                                {
                                    float fwdMul = 1.5f * m_desc.bladeForwardMultiplier;
                                    spawnPos = { tip.x + dir.x * fwdMul,
                                                 tip.y,
                                                 tip.z + dir.z * fwdMul };
                                    spawnDir = dir;
                                }
                                pVFX->Spawn(effectName, spawnPos, spawnDir, 0u, false);

                                // Massive 화면 임팩트 — 추가 Ring 충격파를 보스 발치에 spawn.
                                //   기존 Impact 와 별개로 즉시 발생 → 화면이 진짜 흔들리는 시그널.
                                if (m_desc.presentation == SlashPresentation::Massive)
                                {
                                    TransformComponent* pBossT = pEnemy->GetOwner()
                                        ? pEnemy->GetOwner()->GetTransform() : nullptr;
                                    if (pBossT)
                                    {
                                        XMFLOAT3 bossPos = pBossT->GetPosition();
                                        XMFLOAT3 ringPos = { bossPos.x, bossPos.y + 0.3f, bossPos.z };
                                        // 같은 원소의 SigilImpact 를 보스 발치에서 즉시 발사
                                        // (Release/Impact 의 sigil impact 와는 별개 — 추가 임팩트)
                                        const char* impactName = SlashCue::ImpactEffectName(m_desc.element);
                                        pVFX->Spawn(impactName, ringPos, spawnDir, 0u, false);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    else if (curPhase == HitPhase::Windup)
    {
        // 차징 중 — 검 끝 anchoring.
        float rawDt = dt;
        if (pEnemy)
            if (CRoom* pRoom = pEnemy->GetRoom())
                if (Scene* pScene = pRoom->GetScene())
                    rawDt = pScene->GetRawDeltaTime();
        m_cue.Tick(rawDt, tip, dir);
    }

    // Impact 카운트다운 — raw dt (hit-stop 영향 X). Hit 단계 중 또는 Recovery 직후에도 발사.
    if (m_fImpactCountdown > 0.0f)
    {
        float rawDt = dt;
        if (pEnemy)
            if (CRoom* pRoom = pEnemy->GetRoom())
                if (Scene* pScene = pRoom->GetScene())
                    rawDt = pScene->GetRawDeltaTime();
        m_fImpactCountdown -= rawDt;
        if (m_fImpactCountdown <= 0.0f)
        {
            m_fImpactCountdown = -1.0f;

            // Impact 위치 — 검 끝 (sword tip) + 지면 (ground = 보스 발 높이)
            XMFLOAT3 groundPos = tip;
            if (pEnemy && pEnemy->GetOwner())
                if (TransformComponent* pBossT = pEnemy->GetOwner()->GetTransform())
                    groundPos.y = pBossT->GetPosition().y + 0.2f; // 살짝 띄움 (z-fight 방지)
            m_cue.Impact(tip, groundPos, dir);
        }
    }

    m_ePrevPhase = curPhase;
    m_nPrevHit   = curHit;
}

void DarkLordSigilSlash::Reset()
{
    ComboAttackBehavior::Reset();
    m_cue.End();
    m_ePrevPhase       = HitPhase::Windup;
    m_nPrevHit         = -1;
    m_bInitialized     = false;
    m_fImpactCountdown = -1.0f;
}
