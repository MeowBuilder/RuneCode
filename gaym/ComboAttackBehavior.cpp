#include "stdafx.h"
#include "ComboAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"
#include "AnimationComponent.h"
#include "MathUtils.h"
#include "Room.h"
#include "Scene.h"
#include "FluidSkillVFXManager.h"
#include "VFXManager.h"
#include "EffectRegistry.h"
#include "VFXTypes.h"
#include "Mesh.h"
#include "Dx12App.h"
#include <fstream>
#include <chrono>

namespace
{
    // 파일 로그 — OutputDebugString 안 보이는 환경에서도 명확히 확인. gaym.exe 옆에 생김.
    void VFXLog(const char* msg)
    {
        OutputDebugStringA(msg);
        std::ofstream f("vfx_debug.log", std::ios::app);
        if (f.is_open()) f << msg;
    }
    void VFXLogf(const char* fmt, ...)
    {
        char buf[512];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        VFXLog(buf);
    }
}

ComboAttackBehavior::ComboAttackBehavior(const std::vector<ComboHit>& hits)
    : m_vHits(hits)
{
}

ComboAttackBehavior::~ComboAttackBehavior()
{
    // Behavior 교체/소멸 시 잔여 slash piece 정리 — 안 그러면 GameObject leak + 안 사라지는 잔영
    CleanupAllSlashPieces();
    StopSwordGlow();
    StopRibbon();
}

void ComboAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();

    if (m_vHits.empty())
    {
        m_bFinished = true;
        return;
    }

    VFXLogf("[ComboVFX] Execute hits=%zu firstVFX=%s\n",
            m_vHits.size(),
            m_vHits[0].strVFXOnHit.empty() ? "(none)" : m_vHits[0].strVFXOnHit.c_str());

    if (pEnemy)
    {
        // Face target for first hit
        if (m_vHits[0].bTrackTarget)
        {
            pEnemy->FaceTarget();
        }

        AnimationComponent* pAnimComp = pEnemy->GetAnimationComponent();
        if (pAnimComp)
        {
            pAnimComp->CrossFade(m_vHits[0].strAnimation, 0.1f, false);
        }
    }

    m_eHitPhase = HitPhase::Windup;
}

void ComboAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    // Dynamic triangle strip ribbon mesh (검 본 history 따라 swing arc 형성)
    UpdateTrailMesh(dt, pEnemy);
    UpdateSlashPieces(dt);   // legacy — 잔여 piece 페이드 (이제 spawn 안 됨)
    UpdateSwordGlow(dt);

    if (m_bFinished || m_vHits.empty()) return;

    m_fTimer += dt;

    const ComboHit& currentHit = m_vHits[m_nCurrentHit];

    switch (m_eHitPhase)
    {
    case HitPhase::Windup:
        if (m_fTimer >= currentHit.fWindupTime)
        {
            m_eHitPhase = HitPhase::Hit;
            m_fTimer = 0.0f;
        }
        break;

    case HitPhase::Hit:
        if (!m_bHitDealt)
        {
            DealConeDamage(pEnemy, currentHit);
            SpawnHitVFX(pEnemy, currentHit);
            m_bHitDealt = true;
        }

        if (m_fTimer >= currentHit.fHitTime)
        {
            m_eHitPhase = HitPhase::Recovery;
            m_fTimer = 0.0f;
        }
        break;

    case HitPhase::Recovery:
        if (m_fTimer >= currentHit.fRecoveryTime)
        {
            // Move to next hit or finish
            m_nCurrentHit++;
            m_bHitDealt = false;
            m_fTimer = 0.0f;

            if (m_nCurrentHit >= (int)m_vHits.size())
            {
                m_bFinished = true;
            }
            else
            {
                m_eHitPhase = HitPhase::Windup;

                const ComboHit& nextHit = m_vHits[m_nCurrentHit];

                // Re-face target if specified
                if (nextHit.bTrackTarget && pEnemy)
                {
                    pEnemy->FaceTarget();
                }

                // Play next animation — 직전 타와 동일 클립이면 재시작하지 않음 (연속 애니 유지)
                if (pEnemy)
                {
                    const ComboHit& prevHit = m_vHits[m_nCurrentHit - 1];
                    bool bSameClip = (nextHit.strAnimation == prevHit.strAnimation);
                    AnimationComponent* pAnimComp = pEnemy->GetAnimationComponent();
                    if (pAnimComp && !bSameClip)
                    {
                        pAnimComp->CrossFade(nextHit.strAnimation, 0.08f, false);
                    }
                }
            }
        }
        break;
    }
}

bool ComboAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void ComboAttackBehavior::Reset()
{
    m_eHitPhase = HitPhase::Windup;
    m_nCurrentHit = 0;
    m_fTimer = 0.0f;
    m_bHitDealt = false;
    m_bFinished = false;
    m_bEmittingTrail = false;
    m_fTrailRemain = 0.0f;
    CleanupAllSlashPieces();
    StopSwordGlow();
    StopRibbon();   // 누락 시 trail mesh 가 fade 시작 못 하고 영구 잔존
}

void ComboAttackBehavior::DealConeDamage(EnemyComponent* pEnemy, const ComboHit& hit)
{
    if (!pEnemy) return;

    GameObject* pOwner = pEnemy->GetOwner();
    GameObject* pTarget = pEnemy->GetTarget();
    if (!pOwner || !pTarget) return;

    // 전방 사각형 판정 모드 — 설정되면 cone 체크 대신 사각형
    if (hit.fRectWidthHalf > 0.0f && hit.fRectLength > 0.0f)
    {
        if (!pEnemy->IsTargetInForwardRect(hit.fRectWidthHalf, hit.fRectLength))
            return;
        PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>();
        if (pPlayer) pPlayer->TakeDamage(hit.fDamage);
        return;
    }

    // Check distance
    float distance = pEnemy->GetDistanceToTarget();
    if (distance > hit.fHitRange) return;

    // Check angle (cone attack)
    TransformComponent* pMyTransform = pOwner->GetTransform();
    TransformComponent* pTargetTransform = pTarget->GetTransform();
    if (!pMyTransform || !pTargetTransform) return;

    XMFLOAT3 myPos = pMyTransform->GetPosition();
    XMFLOAT3 targetPos = pTargetTransform->GetPosition();
    XMFLOAT3 myRot = pMyTransform->GetRotation();

    // Direction to target
    float dx = targetPos.x - myPos.x;
    float dz = targetPos.z - myPos.z;
    float len = sqrtf(dx * dx + dz * dz);
    if (len > 0.0f)
    {
        dx /= len;
        dz /= len;
    }

    // Boss facing direction
    float facingRad = XMConvertToRadians(myRot.y);
    float facingX = sinf(facingRad);
    float facingZ = cosf(facingRad);

    // Dot product to check angle
    float dot = dx * facingX + dz * facingZ;
    float halfConeRad = XMConvertToRadians(hit.fConeAngle * 0.5f);
    float cosHalfCone = cosf(halfConeRad);

    if (dot < cosHalfCone) return;

    // Deal damage
    PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>();
    if (pPlayer)
        pPlayer->TakeDamage(hit.fDamage);
}

// 타격 순간 검 끝 또는 보스 본체에 원소 색 글로우를 짧게 터뜨림.
//   1차: 검 본(가능하면) 의 world 위치에 스폰 → "원소가 깃든 검" 연출
//   2차: status_*/sub_* 짧은 컬러 펄스 → "딱 빛난다" 느낌, 입자 발산 최소화
//   bone fallback: 보스 forward * forwardOffset + Y * yOffset
namespace
{
    // EffectDef 의 모든 레이어를 scale 배로 부풀림 (SPH + 경량 이미터 + sphere 모두)
    void ScaleEffectDef(EffectDef& def, float scale)
    {
        if (scale <= 1.0f) return;
        float sqrtScale = sqrtf(scale);
        for (auto& l : def.layers)
        {
            // SPH 레이어
            SPHEmitterParams& s = l.sph;
            if (s.particleCount > 0)
            {
                s.particleCount       = static_cast<int>(s.particleCount * scale);
                s.spawnRadius        *= sqrtScale;
                s.cardinalSpawnRadius *= sqrtScale;
                if (s.particleSize > 0.f) s.particleSize *= sqrtScale;
                if (!s.cpDescs.empty())
                {
                    s.cpDescs[0].sphereRadius *= sqrtScale;
                }
            }
            // 경량 이미터 레이어
            if (l.particleCount > 0)
            {
                l.particleCount = static_cast<int>(l.particleCount * scale);
                l.sizeScale    *= sqrtScale;
            }
            // sphere 발산 (status_* 계열)
            if (l.type == EmitterType::Sphere)
            {
                l.sphere.radius *= sqrtScale;
            }
        }
    }

    // status_* 같은 "지속 오라" 이펙트를 "딱 빛나는" 정지 글로우 펄스로 변환.
    //   기존: shell 에서 입자가 사방으로 흩날림.
    //   변환 후: 입자가 거의 정지 + 매우 짧음 + 밀집 → 한 점에서 색이 번쩍 빛난 후 사라짐.
    void MakeBriefPulse(EffectDef& def, float pulseDuration = 0.18f, float emitBoost = 4.0f)
    {
        for (auto& l : def.layers)
        {
            // duration 짧게 (무한 오라 → 한 번 펄스)
            if (l.duration < 0.f) l.duration = pulseDuration;
            else if (l.duration > pulseDuration) l.duration = pulseDuration;

            // 짧은 시간 안에 빵 → 입자 밀도 ↑
            l.emitRate    *= emitBoost;

            // 입자 수명 매우 짧게 → 잔상 X, "딱" 사라짐
            l.lifetimeMin *= 0.30f;
            l.lifetimeMax *= 0.30f;

            // 입자 속도 거의 0 → 흩날리지 않고 그 자리에서 빛만 남
            l.speedMin    *= 0.15f;
            l.speedMax    *= 0.15f;

            // 입자 크기 ↑ → 적은 수로도 환한 글로우
            l.sizeScale   *= 1.4f;

            // sphere 반경 줄여서 한 점에 집중 (지금은 shell 외곽 spawn)
            if (l.type == EmitterType::Sphere)
            {
                l.sphere.radius *= 0.45f;
                l.sphere.shellFraction = 0.0f;   // shell 대신 내부 spawn — 더 밀집
                l.sphere.rotationSpeed = 0.0f;   // 회전 X
            }
        }
    }
}

// 흔히 쓰이는 칼 든 손 본 이름 후보 — DarkLord 모델 본명 정확히는 모르니 광역 매칭.
//   첫 매치를 사용. 없으면 nullptr → fallback.
TransformComponent* ComboAttackBehavior::FindSwordBone(EnemyComponent* pEnemy)
{
    if (m_bBoneLookupDone) return m_pCachedSwordBone;
    m_bBoneLookupDone = true;

    VFXLog("[ComboVFX] FindSwordBone called\n");

    if (!pEnemy)
    {
        VFXLog("[ComboVFX] FindSwordBone: pEnemy null\n");
        return nullptr;
    }
    AnimationComponent* pAnim = pEnemy->GetAnimationComponent();
    if (!pAnim)
    {
        VFXLog("[ComboVFX] FindSwordBone: AnimationComponent null\n");
        return nullptr;
    }

    static const std::vector<std::string> candidates = {
        "Sword", "Weapon", "WeaponBone", "weapon",
        "RightHand", "right_hand", "hand_r", "hand_R", "RHand", "R_Hand", "Right_Hand",
        "Bip01_R_Hand", "Bip01_RHand", "Bip01 R Hand",
        "mixamorig:RightHand", "mixamorig_RightHand",
        "Hand_R"
    };
    m_pCachedSwordBone = pAnim->FindBoneAny(candidates);

    if (m_pCachedSwordBone)
    {
        VFXLog("[ComboVFX] sword bone FOUND — VFX 가 검 끝 추종\n");
    }
    else
    {
        VFXLog("[ComboVFX] sword bone NOT found — 본 이름 dump:\n");
        auto names = pAnim->GetBoneNames();
        VFXLogf("[ComboVFX] bone cache size=%zu\n", names.size());
        for (const auto& n : names)
        {
            VFXLogf("[ComboVFX]   %s\n", n.c_str());
        }
    }
    return m_pCachedSwordBone;
}

// 원소 effect 이름에서 발광색 결정 — status_burn/chill/freeze/fracture 식별 후 매핑.
//   톤 다운된 채도 (형광 회피) — 코어가 너무 밝지 않게 ~1.6 (HDR 부드러운 발광).
namespace
{
    // 원소별 (core, edge) 두 색 — 검기 가운데 밝은 코어 + 양 옆 saturated edge.
    //   core 는 거의 white-tinted (광원같은 강도), edge 는 원소 채도 강한 색.
    // 원소별 (core, edge) — 명확한 color contrast. core 거의 white-tinted, edge 매우 saturated.
    struct ColorPair { XMFLOAT4 core; XMFLOAT4 edge; };
    ColorPair PickColorPairForEffect(const std::string& effectName)
    {
        // 불: 흰-노랑 core / 진한 빨강 edge — 강한 contrast
        if (effectName.find("burn")  != std::string::npos ||
            effectName.find("fire")  != std::string::npos ||
            effectName.find("Meteor")!= std::string::npos)
            return { XMFLOAT4(2.00f, 1.70f, 0.60f, 1.0f), XMFLOAT4(2.20f, 0.20f, 0.00f, 1.0f) };

        // 물: 흰-시안 core / 짙은 파랑 edge
        if (effectName.find("chill") != std::string::npos ||
            effectName.find("water") != std::string::npos ||
            effectName.find("Wave")  != std::string::npos ||
            effectName.find("Vortex")!= std::string::npos)
            return { XMFLOAT4(0.80f, 1.60f, 2.00f, 1.0f), XMFLOAT4(0.00f, 0.30f, 1.80f, 1.0f) };

        // 바람: 흰-초록 core / 짙은 청록 edge
        if (effectName.find("freeze")!= std::string::npos ||
            effectName.find("wind")  != std::string::npos ||
            effectName.find("Wind")  != std::string::npos ||
            effectName.find("Gale")  != std::string::npos)
            return { XMFLOAT4(1.00f, 1.80f, 1.20f, 1.0f), XMFLOAT4(0.10f, 1.20f, 0.40f, 1.0f) };

        // 땅: 흰-앰버 core / 짙은 갈색 edge
        if (effectName.find("fracture")  != std::string::npos ||
            effectName.find("earth")     != std::string::npos ||
            effectName.find("Stone")     != std::string::npos ||
            effectName.find("EarthArmor")!= std::string::npos)
            return { XMFLOAT4(2.00f, 1.50f, 0.70f, 1.0f), XMFLOAT4(1.60f, 0.40f, 0.00f, 1.0f) };

        return { XMFLOAT4(1.50f, 1.30f, 1.00f, 1.0f), XMFLOAT4(1.00f, 0.50f, 0.20f, 1.0f) };
    }
}

void ComboAttackBehavior::SpawnHitVFX(EnemyComponent* pEnemy, const ComboHit& hit)
{
    if (hit.strVFXOnHit.empty() || !pEnemy) return;

    CRoom* pRoom = pEnemy->GetRoom();
    if (!pRoom) return;
    Scene* pScene = pRoom->GetScene();
    if (!pScene) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;
    TransformComponent* pBossT = pOwner->GetTransform();
    if (!pBossT) return;

    // 검 본 존재 확인 — trail 모드는 본 위치 추적이 핵심
    FindSwordBone(pEnemy);

    // Trail emission 시작 — Hit phase + recovery 일부까지 길게 emit.
    //   실제 swing 끝나도 검 본 잔여 모션 동안 piece 계속 emit → 호 누적.
    {
        ColorPair cp = PickColorPairForEffect(hit.strVFXOnHit);
        m_xmf4TrailColor     = cp.core;
        m_xmf4TrailEdgeColor = cp.edge;
    }
    m_fTrailPieceScale = hit.fVFXScale;
    float baseEmit = (hit.fHitTime > 0.10f) ? hit.fHitTime : 0.10f;
    m_fTrailRemain = baseEmit + hit.fRecoveryTime * 0.3f + 0.05f;
    m_fTrailEmitAccum = 0.0f;
    m_bEmittingTrail = true;
    m_bHasPrevSwordPos = false;
    m_bYawInitialized  = false;
    CleanupAllSlashPieces();
    StopRibbon();   // 이전 hit 잔여 ribbon 즉시 정리

    // Dynamic triangle strip ribbon mesh 생성 — 한 hit 당 1 mesh + 1 GameObject.
    if (m_pCachedSwordBone)
    {
        const XMFLOAT4X4& w = m_pCachedSwordBone->GetWorldMatrix();
        m_xmf3TrailStartPos = { w._41, w._42, w._43 };
        m_vSwordHistory.clear();
        m_vSwordHistory.push_back(m_xmf3TrailStartPos);

        ID3D12Device* pDevice = Dx12App::GetInstance()->GetDevice();
        m_pTrailMesh = new SwordTrailMesh(pDevice, 40);
        m_pTrailMesh->AddRef();   // self-ref: Behavior 가 살아있는 동안 mesh 유지

        EnemySpawner* pSpawner = pScene->GetEnemySpawner();
        if (pSpawner)
        {
            // identity transform — mesh 가 이미 world space vertex 를 갖고 있음
            m_pTrailRibbon = pSpawner->SpawnSlashMesh(pRoom, m_pTrailMesh,
                                                      XMFLOAT3(0.0f, 0.0f, 0.0f),
                                                      XMFLOAT3(0.0f, 0.0f, 0.0f),
                                                      XMFLOAT3(1.0f, 1.0f, 1.0f),
                                                      m_xmf4TrailColor);
            m_pRibbonScene = pScene;
            m_fRibbonFadeT = 1.0f;
            m_bRibbonFading = false;
        }
    }

    // 검 자체 emissive 발광 — 검 본의 owner GameObject material.m_cEmissive 를 동적 boost.
    if (m_pCachedSwordBone)
    {
        m_pSwordObj = m_pCachedSwordBone->GetOwner();
        if (m_pSwordObj)
        {
            m_xmf4SwordGlowColor = m_xmf4TrailColor;
            m_fSwordGlowMax    = baseEmit + 0.20f;
            m_fSwordGlowRemain = m_fSwordGlowMax;
            m_bSwordGlowing    = true;
        }
    }
}

void ComboAttackBehavior::UpdateSwordGlow(float dt)
{
    if (!m_bSwordGlowing || !m_pSwordObj) return;
    m_fSwordGlowRemain -= dt;
    if (m_fSwordGlowRemain <= 0.0f)
    {
        StopSwordGlow();
        return;
    }
    float t = m_fSwordGlowRemain / m_fSwordGlowMax;
    float intensity = (t > 0.7f) ? 1.0f : (t / 0.7f);

    MATERIAL mat = m_pSwordObj->GetMaterial();
    // 검 자체 emissive 도 강하게 (HDR 3.5x) — trail 과 어우러져 포스 ↑
    const float kSwordEmBoost = 3.5f;
    mat.m_cEmissive.x = m_xmf4SwordGlowColor.x * intensity * kSwordEmBoost;
    mat.m_cEmissive.y = m_xmf4SwordGlowColor.y * intensity * kSwordEmBoost;
    mat.m_cEmissive.z = m_xmf4SwordGlowColor.z * intensity * kSwordEmBoost;
    mat.m_cEmissive.w = 1.0f;
    m_pSwordObj->SetMaterial(mat);
}

void ComboAttackBehavior::StopSwordGlow()
{
    if (m_pSwordObj)
    {
        MATERIAL mat = m_pSwordObj->GetMaterial();
        mat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        m_pSwordObj->SetMaterial(mat);
    }
    m_bSwordGlowing = false;
    m_fSwordGlowRemain = 0.0f;
}

// Dynamic triangle strip ribbon — 검 본 world position history 를 SwordTrailMesh 에 push.
//   mesh 내부에서 매 frame vertex buffer 갱신 → 한 mesh 가 swing arc 따라 곡선 형성.
void ComboAttackBehavior::UpdateTrailMesh(float dt, EnemyComponent* pEnemy)
{
    if (!m_pTrailRibbon || !m_pTrailMesh || !pEnemy || !m_pCachedSwordBone) return;

    // m_fTrailRemain 감소 — UpdateTrailEmission 가 호출 안 되니 여기서 직접 처리.
    if (m_bEmittingTrail)
    {
        m_fTrailRemain -= dt;
        if (m_fTrailRemain <= 0.0f)
        {
            m_bEmittingTrail = false;
            StopRibbon();
            return;
        }
    }

    if (m_bEmittingTrail)
    {
        // hit phase 중: 매 frame 검 본 world pos 를 sub-step 으로 dense push.
        //   swing hit phase 가 짧아 (0.10s) frame 당 1 point 면 history 가 6~9 point 만 쌓여 trail 짧음.
        //   prev → current 사이를 3 등분해 3 point 씩 push → trail mesh 가 검 길이 만큼 길게.
        const XMFLOAT4X4& w = m_pCachedSwordBone->GetWorldMatrix();
        XMFLOAT3 cur = { w._41, w._42, w._43 };

        if (m_bHasPrevSwordPos)
        {
            XMFLOAT3 prev = m_xmf3PrevSwordPos;
            const int nSub = 3;
            for (int i = 1; i <= nSub; ++i)
            {
                float t = (float)i / (float)nSub;
                XMFLOAT3 p = {
                    prev.x + (cur.x - prev.x) * t,
                    prev.y + (cur.y - prev.y) * t,
                    prev.z + (cur.z - prev.z) * t,
                };
                m_vSwordHistory.push_back(p);
            }
        }
        else
        {
            m_vSwordHistory.push_back(cur);
            m_bHasPrevSwordPos = true;
        }
        m_xmf3PrevSwordPos = cur;

        const size_t kMaxPoints = 40;
        while (m_vSwordHistory.size() > kMaxPoints)
            m_vSwordHistory.erase(m_vSwordHistory.begin());

        const float halfWidth = m_fTrailPieceScale * 1.40f;   // 두툼하게 — 포스 ↑
        m_pTrailMesh->UpdateTrail(m_vSwordHistory, halfWidth);
    }
    else
    {
        // hit phase 끝 → 즉시 cleanup (잔영 안 남김)
        StopRibbon();
        return;
    }

    // material: emissive = core, ambient = edge. shader 가 UV.u 양옆 gradient 로 lerp.
    //   bloom 으로 색이 blur 되지 않도록 HDR 강도 낮춤. 색 자체는 saturated.
    MATERIAL mat;
    mat.m_cDiffuse  = XMFLOAT4(0.0f, 0.0f, 0.0f, m_fRibbonFadeT);
    mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    const float kCoreBoost = 1.8f;   // HDR 적당히, bloom 강하지 않게
    const float kEdgeBoost = 1.4f;
    mat.m_cEmissive = XMFLOAT4(m_xmf4TrailColor.x * kCoreBoost,
                                m_xmf4TrailColor.y * kCoreBoost,
                                m_xmf4TrailColor.z * kCoreBoost, 1.0f);
    mat.m_cAmbient  = XMFLOAT4(m_xmf4TrailEdgeColor.x * kEdgeBoost,
                                m_xmf4TrailEdgeColor.y * kEdgeBoost,
                                m_xmf4TrailEdgeColor.z * kEdgeBoost, 1.0f);
    m_pTrailRibbon->SetMaterial(mat);
}

void ComboAttackBehavior::StopRibbon()
{
    if (m_pTrailRibbon && m_pRibbonScene)
    {
        m_pRibbonScene->MarkForDeletion(m_pTrailRibbon);
    }
    if (m_pTrailMesh)
    {
        m_pTrailMesh->Release();   // self-ref 해제. GameObject 의 ref 가 남아있으면 그쪽이 마지막에 delete.
        m_pTrailMesh = nullptr;
    }
    m_vSwordHistory.clear();
    m_pTrailRibbon = nullptr;
    m_pRibbonScene = nullptr;
    m_bRibbonFading = false;
    m_fRibbonFadeT = 1.0f;
}

// Hit phase 동안 매 frame 호출 — 검 본 위치에 swing 방향으로 stretched dash 를 emit.
//   각 dash 는 검 swing 방향으로 길게, 짧은 lifetime 으로 페이드 → 호 형태로 보임.
void ComboAttackBehavior::UpdateTrailEmission(float dt, EnemyComponent* pEnemy)
{
    if (!m_bEmittingTrail || !pEnemy) return;

    m_fTrailRemain -= dt;
    if (m_fTrailRemain <= 0.0f)
    {
        m_bEmittingTrail = false;
        return;
    }

    if (!m_pCachedSwordBone) return;

    CRoom* pRoom = pEnemy->GetRoom();
    if (!pRoom) return;
    Scene* pScene = pRoom->GetScene();
    if (!pScene) return;
    EnemySpawner* pSpawner = pScene->GetEnemySpawner();
    if (!pSpawner || !pSpawner->m_pDiscMesh) return;

    const XMFLOAT4X4& w = m_pCachedSwordBone->GetWorldMatrix();
    XMFLOAT3 swordPos = { w._41, w._42, w._43 };

    if (!m_bHasPrevSwordPos)
    {
        m_xmf3PrevSwordPos = swordPos;
        m_bHasPrevSwordPos = true;
        return;
    }

    m_fTrailEmitAccum += dt;
    if (m_fTrailEmitAccum < m_fTrailEmitInterval) return;
    m_fTrailEmitAccum = 0.0f;

    XMFLOAT3 prev = m_xmf3PrevSwordPos;
    m_xmf3PrevSwordPos = swordPos;

    float dx = swordPos.x - prev.x;
    float dy = swordPos.y - prev.y;
    float dz = swordPos.z - prev.z;
    float fullLen = sqrtf(dx*dx + dy*dy + dz*dz);
    if (fullLen < 0.10f) return;
    if (fullLen > 5.0f) return;   // 비정상적 jump reject

    // DiscMesh ellipse + sub-step 4 — 양 끝 포함 prev → current 사이 4 위치에 spawn.
    float horizLen = sqrtf(dx*dx + dz*dz);
    float dashLen = horizLen * 13.0f;
    if (dashLen < 11.0f) dashLen = 11.0f;
    if (dashLen > 25.0f) dashLen = 25.0f;
    const float dashWid = m_fTrailPieceScale * 1.35f;

    // Yaw smoothing — 매 frame fresh atan2 는 검 본 jitter 로 piece 들이 spike 처럼 다른 각도로 튀어나옴.
    //   첫 emit 의 yaw 로 init 후, 이후 frame yaw 와 weighted lerp (shortest angular distance).
    float yawRaw = atan2f(dx, dz);
    if (!m_bYawInitialized)
    {
        m_fSmoothedYaw = yawRaw;
        m_bYawInitialized = true;
    }
    else
    {
        float diff = yawRaw - m_fSmoothedYaw;
        const float PI_F = 3.14159265f;
        while (diff >  PI_F) diff -= 2.0f * PI_F;
        while (diff < -PI_F) diff += 2.0f * PI_F;
        m_fSmoothedYaw += diff * 0.20f;  // 20% lerp — swing 곡선 따라가지만 spike 노이즈는 dampen
    }
    float yawDeg = XMConvertToDegrees(m_fSmoothedYaw);
    XMFLOAT3 rotDeg = { 0.0f, yawDeg, 0.0f };

    // Sub-step 4 — 양 끝 포함 prev → current 사이 4 위치에 ellipse spawn.
    //   piece 적게 + lifetime 짧게 → boundary 누적 최소화. 이 상태가 best 였음.
    const int nSubsteps = 4;
    for (int i = 0; i < nSubsteps; ++i)
    {
        float t = (float)i / (float)(nSubsteps - 1);
        XMFLOAT3 segCenter = {
            prev.x + dx * t,
            prev.y + dy * t,
            prev.z + dz * t,
        };

        SlashPiece p;
        p.pObj = pSpawner->SpawnSlashMesh(pRoom, pSpawner->m_pDiscMesh,
                                           segCenter, rotDeg,
                                           XMFLOAT3(dashWid, 1.0f, dashLen),
                                           m_xmf4TrailColor);
        p.fAge = 0.0f;
        p.fLifetime = 0.14f;
        p.startScale = { dashWid, 1.0f, dashLen };
        p.endScale   = { dashWid * 0.15f, 1.0f, dashLen * 1.10f };
        p.startEmissive = m_xmf4TrailColor;
        p.pScene = pScene;
        if (p.pObj) m_vSlashPieces.push_back(p);
    }
}

void ComboAttackBehavior::UpdateSlashPieces(float dt)
{
    for (auto it = m_vSlashPieces.begin(); it != m_vSlashPieces.end(); )
    {
        it->fAge += dt;
        float t = it->fAge / it->fLifetime;
        if (t >= 1.0f)
        {
            if (it->pObj && it->pScene) it->pScene->MarkForDeletion(it->pObj);
            it = m_vSlashPieces.erase(it);
            continue;
        }

        // scale lerp + emissive 페이드 (1.0 → 0.0)
        if (it->pObj)
        {
            float fadeOut = 1.0f - t;
            // emissive 는 처음 30% 살짝 더 밝게(임팩트 펀치) 후 빠르게 페이드
            float emMul = (t < 0.15f) ? (1.0f + (0.15f - t) * 3.0f) : (fadeOut * fadeOut);
            XMFLOAT4 em = {
                it->startEmissive.x * emMul,
                it->startEmissive.y * emMul,
                it->startEmissive.z * emMul,
                it->startEmissive.w * fadeOut
            };
            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(0.0f, 0.0f, 0.0f, em.w);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = em;
            it->pObj->SetMaterial(mat);

            XMFLOAT3 sc;
            sc.x = it->startScale.x + (it->endScale.x - it->startScale.x) * t;
            sc.y = it->startScale.y + (it->endScale.y - it->startScale.y) * t;
            sc.z = it->startScale.z + (it->endScale.z - it->startScale.z) * t;
            if (auto* pT = it->pObj->GetTransform()) pT->SetScale(sc);
        }
        ++it;
    }
}

void ComboAttackBehavior::CleanupAllSlashPieces()
{
    for (auto& p : m_vSlashPieces)
    {
        if (p.pObj && p.pScene) p.pScene->MarkForDeletion(p.pObj);
    }
    m_vSlashPieces.clear();
}

ComboAttackBehavior* ComboAttackBehavior::CreateLightCombo()
{
    std::vector<ComboHit> hits;

    // 리듬감 있는 3연타 — 탁 / 탁 / 탁-쾅 (총 ~1.8s, 반응 가능)
    ComboHit hit1;
    hit1.fDamage = 8.0f;
    hit1.fWindupTime = 0.25f;   // 텔레그래프 강화
    hit1.fHitTime = 0.1f;
    hit1.fRecoveryTime = 0.2f;  // hit 간 간격
    hit1.fHitRange = 5.0f;
    hit1.fConeAngle = 90.0f;
    hit1.strAnimation = "Basic Attack";
    hit1.bTrackTarget = true;
    hits.push_back(hit1);

    ComboHit hit2;
    hit2.fDamage = 8.0f;
    hit2.fWindupTime = 0.2f;
    hit2.fHitTime = 0.1f;
    hit2.fRecoveryTime = 0.2f;
    hit2.fHitRange = 5.0f;
    hit2.fConeAngle = 90.0f;
    hit2.strAnimation = "Claw Attack";
    hit2.bTrackTarget = false;
    hits.push_back(hit2);

    ComboHit hit3;
    hit3.fDamage = 12.0f;
    hit3.fWindupTime = 0.3f;    // 피니셔 크게 예비동작
    hit3.fHitTime = 0.15f;
    hit3.fRecoveryTime = 0.3f;
    hit3.fHitRange = 6.0f;
    hit3.fConeAngle = 120.0f;
    hit3.strAnimation = "Basic Attack";
    hit3.bTrackTarget = false;
    hits.push_back(hit3);

    return new ComboAttackBehavior(hits);
}

ComboAttackBehavior* ComboAttackBehavior::CreateHeavyCombo()
{
    std::vector<ComboHit> hits;

    ComboHit hit1;
    hit1.fDamage = 20.0f;
    hit1.fWindupTime = 0.4f;
    hit1.fHitTime = 0.2f;
    hit1.fRecoveryTime = 0.2f;
    hit1.fHitRange = 6.0f;
    hit1.fConeAngle = 120.0f;
    hit1.strAnimation = "Claw Attack";
    hit1.bTrackTarget = true;
    hits.push_back(hit1);

    ComboHit hit2;
    hit2.fDamage = 30.0f;
    hit2.fWindupTime = 0.5f;
    hit2.fHitTime = 0.25f;
    hit2.fRecoveryTime = 0.5f;
    hit2.fHitRange = 7.0f;
    hit2.fConeAngle = 150.0f;
    hit2.strAnimation = "Basic Attack";
    hit2.bTrackTarget = true;
    hits.push_back(hit2);

    return new ComboAttackBehavior(hits);
}

ComboAttackBehavior* ComboAttackBehavior::CreateFuryCombo()
{
    std::vector<ComboHit> hits;

    for (int i = 0; i < 5; i++)
    {
        ComboHit hit;
        hit.fDamage = 5.0f + (float)i;
        hit.fWindupTime = 0.08f;
        hit.fHitTime = 0.05f;
        hit.fRecoveryTime = 0.05f;
        hit.fHitRange = 4.5f;
        hit.fConeAngle = 70.0f;
        hit.strAnimation = (i % 2 == 0) ? "Basic Attack" : "Claw Attack";
        hit.bTrackTarget = (i == 0);
        hits.push_back(hit);
    }

    return new ComboAttackBehavior(hits);
}
