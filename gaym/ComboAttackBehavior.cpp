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
    StopCrescent();
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
    UpdateCrescent(dt);      // 2단계: static crescent flash fade

    if (m_bFinished || m_vHits.empty()) return;

    m_fTimer += dt;

    const ComboHit& currentHit = m_vHits[m_nCurrentHit];

    switch (m_eHitPhase)
    {
    case HitPhase::Windup:
        // Windup 후반(75%) prearm — sword glow / 검 본 trail 활성 (실제 검 궤적 ribbon 시작).
        if (!m_bTrailStarted && currentHit.fWindupTime > 0.0f &&
            m_fTimer >= currentHit.fWindupTime * 0.75f)
        {
            SpawnHitVFX(pEnemy, currentHit);
        }
        // 검기는 미리 안 띄움 — 텔레그래프는 indicator 만, crescent 는 휘두르는 순간 정확히.
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
            // 검기는 strVFXOnHit 가 명시된 hit 만 (= 보스 opt-in). goblin/일반 적은 amber 디폴트 호 안 뜸.
            if (!m_bCrescentSpawned && !currentHit.strVFXOnHit.empty())
            {
                SpawnCrescentFlash(pEnemy, currentHit);
                m_bCrescentSpawned = true;
                TriggerCrescentBurst();
            }
            SpawnImpactVFX(pEnemy, currentHit);   // 원소별 impact particle (불꽃/물방울/먼지/속도선)
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
                m_bTrailStarted = false;   // 다음 hit 의 windup 후반에 다시 prearm
                m_bCrescentSpawned = false;   // 다음 hit 의 crescent prearm 허용

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
    m_bTrailStarted = false;
    m_bCrescentSpawned = false;
    m_fTrailRemain = 0.0f;
    CleanupAllSlashPieces();
    StopSwordGlow();
    StopRibbon();   // 누락 시 trail mesh 가 fade 시작 못 하고 영구 잔존
    StopCrescent(); // 2단계: 정지 호 잔여 정리
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

// 원소별 검기 베이스 텍스처 — namu 3×3 톱니 시트 대신 Kenney 깔끔 호 + Free Slash 노이즈 호.
//   ribbon / crescent 양쪽이 같은 매핑을 공유 → 원소 색 + 텍스처 결이 일관.
//   셰이더는 full-UV (sub-UV X) 로 단일 호를 샘플.
static const char* PickSlashTextureForElement(int elemId)
{
    switch (elemId)
    {
    case 1: return "Assets/Textures/VFX/slash_02.png";          // fire — 깔끔 둥근 호, hot core overlay 가 카오스 담당
    case 2: return "Assets/Textures/VFX/free_slash_noise.png";  // water — 파란 노이즈 결이 호 자체에 박혀 있음
    case 3: return "Assets/Textures/VFX/slash_04.png";          // earth — 넓은 평평 호, chunky stepped V 와 매칭
    case 4: return "Assets/Textures/VFX/slash_03.png";          // wind — 수직 슬림 호, multi-strand 3 가닥과 결 일치
    case 0:
    default: return "Assets/Textures/VFX/slash_02.png";         // default — 범용 깔끔 호
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
        // DarkLord (DarkKnight2_skin3) 실제 본명 — 손 본보다 검 본을 우선 매치해야 검기가 검 위치에서 시작.
        //   Bone_Sword 는 검 grip 본. SM_DarkKnight2_Sword 는 검 mesh 본 (grip 와 같은 위치).
        "Bone_Sword", "SM_DarkKnight2_Sword",
        // 일반 후보 (다른 보스 호환)
        "Sword", "Weapon", "WeaponBone", "weapon",
        "RightHand", "right_hand", "hand_r", "hand_R", "RHand", "R_Hand", "Right_Hand",
        "Bip01_R_Hand", "Bip01_RHand", "Bip01 R Hand",
        "mixamorig:RightHand", "mixamorig_RightHand",
        "Hand_R"
    };
    m_pCachedSwordBone = pAnim->FindBoneAny(candidates);

    if (m_pCachedSwordBone)
    {
        // 어느 후보가 실제 매치됐는지 확인 — Bone_Sword 가 아니면 다른 본 fallback 중.
        std::string matchedName = pAnim->FindBoneAnyName(candidates);
        VFXLogf("[ComboVFX] sword bone FOUND — matched=\"%s\"\n", matchedName.c_str());
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
    // 원소별 (core, edge, elementId) — AAA 스타일 vivid 팔레트 (Undead Lord 레퍼런스).
    //   elementId : 셰이더가 원소별 stylization (fire yellow core / wind multi-strand 등) 분기에 사용.
    //   0=default, 1=fire, 2=water, 3=earth, 4=wind.
    struct ColorPair { XMFLOAT4 core; XMFLOAT4 edge; int elementId; };
    ColorPair PickColorPairForEffect(const std::string& effectName)
    {
        // 불 — vivid 주황빨강 core / 핏빛 edge. 셰이더가 hot yellow core overlay 추가.
        if (effectName.find("burn")  != std::string::npos ||
            effectName.find("fire")  != std::string::npos ||
            effectName.find("Meteor")!= std::string::npos)
            return { XMFLOAT4(1.10f, 0.40f, 0.10f, 1.0f), XMFLOAT4(0.55f, 0.08f, 0.00f, 1.0f), 1 };

        // 물 — vivid 코발트/시안 core / 깊은 군청 edge. 밝은 cyan highlight.
        if (effectName.find("chill") != std::string::npos ||
            effectName.find("water") != std::string::npos ||
            effectName.find("Wave")  != std::string::npos ||
            effectName.find("Vortex")!= std::string::npos)
            return { XMFLOAT4(0.18f, 0.60f, 1.10f, 1.0f), XMFLOAT4(0.05f, 0.25f, 0.55f, 1.0f), 2 };

        // 바람 — vivid 민트/에메랄드 core / 짙은 녹청 edge. 셰이더가 multi-strand 3 parallel.
        if (effectName.find("freeze")!= std::string::npos ||
            effectName.find("wind")  != std::string::npos ||
            effectName.find("Wind")  != std::string::npos ||
            effectName.find("Gale")  != std::string::npos)
            return { XMFLOAT4(0.25f, 0.95f, 0.65f, 1.0f), XMFLOAT4(0.05f, 0.50f, 0.30f, 1.0f), 4 };

        // 땅 — vivid 황금/앰버 core / 흙갈색 edge. 셰이더가 chunky edge stepped.
        if (effectName.find("fracture")  != std::string::npos ||
            effectName.find("earth")     != std::string::npos ||
            effectName.find("Stone")     != std::string::npos ||
            effectName.find("EarthArmor")!= std::string::npos)
            return { XMFLOAT4(1.05f, 0.55f, 0.10f, 1.0f), XMFLOAT4(0.45f, 0.15f, 0.02f, 1.0f), 3 };

        return { XMFLOAT4(0.80f, 0.55f, 0.25f, 1.0f), XMFLOAT4(0.30f, 0.12f, 0.02f, 1.0f), 0 };
    }
}

// 인디케이터 override — cone 폭에 따라 type 분기.
//   cone ≤ 180° (베기) : ForwardBox (보스 전방 직사각 — 검 휘두름과 시각 매칭).
//   cone > 180° (회전/와이드) : Circle (보스 발치 원 — 회전 모션과 매칭).
//   색은 preset 기본 빨강 유지 (페이즈마다 색 바뀌는 게 거슬려서 tint 제거).
int ComboAttackBehavior::GetIndicatorTypeOverride() const
{
    if (m_vHits.empty()) return -1;
    if (m_vHits[0].fConeAngle > 180.0f)
    {
        return 1;   // IndicatorType::Circle
    }
    return 4;       // IndicatorType::ForwardBox (preset 일치하지만 명시 — preset 다른 보스 대비)
}

float ComboAttackBehavior::GetIndicatorRadius() const
{
    if (m_vHits.empty()) return 0.0f;
    if (m_vHits[0].fConeAngle > 180.0f)
    {
        return m_vHits[0].fHitRange;   // Circle 반경 = hit range
    }
    // ForwardBox half-width = range * sin(cone/2) — 호 폭 근사.
    float halfConeRad = XMConvertToRadians(m_vHits[0].fConeAngle * 0.5f);
    return m_vHits[0].fHitRange * sinf(halfConeRad);
}

float ComboAttackBehavior::GetIndicatorLength() const
{
    if (m_vHits.empty()) return 0.0f;
    return m_vHits[0].fHitRange;   // ForwardBox length = hit range
}

DirectX::XMFLOAT3 ComboAttackBehavior::GetIndicatorTint() const
{
    // 페이즈마다 색 변하는 게 거슬린다는 피드백 — preset 기본 빨강 유지.
    return DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);
}

void ComboAttackBehavior::SpawnHitVFX(EnemyComponent* pEnemy, const ComboHit& hit)
{
    // 이미 windup 후반에 prearm 으로 시작했으면 hit 순간 재시작 skip — trail 끊김 방지.
    if (m_bTrailStarted) return;
    if (hit.strVFXOnHit.empty() || !pEnemy) return;

    CRoom* pRoom = pEnemy->GetRoom();
    if (!pRoom) return;
    Scene* pScene = pRoom->GetScene();
    if (!pScene) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;
    TransformComponent* pBossT = pOwner->GetTransform();
    if (!pBossT) return;
    EnemySpawner* pSpawner = pScene->GetEnemySpawner();
    if (!pSpawner) return;

    // 검 본 존재 확인 — trail 모드는 본 위치 추적이 핵심
    FindSwordBone(pEnemy);

    // 원소 색은 가장 먼저 — Crescent / Ribbon / SwordGlow 모두 이 값을 참조.
    {
        ColorPair cp = PickColorPairForEffect(hit.strVFXOnHit);
        m_xmf4TrailColor     = cp.core;
        m_xmf4TrailEdgeColor = cp.edge;
        m_nCurrentElementId  = cp.elementId;
    }
    m_fTrailPieceScale = hit.fVFXScale;
    m_fCurrentConeAngle = hit.fConeAngle;
    // Trail life — windup 후반 시작 ~ recovery 80% 까지 emit. 검 본 실제 궤적 따라가는 메인 검기.
    float baseEmit = (hit.fHitTime > 0.12f) ? hit.fHitTime : 0.12f;
    m_fTrailRemain = baseEmit + hit.fRecoveryTime * 0.55f + 0.15f;
    if (m_fTrailRemain < 0.40f) m_fTrailRemain = 0.40f;
    m_fTrailEmitAccum = 0.0f;
    m_bHasPrevSwordPos = false;
    m_bYawInitialized  = false;
    CleanupAllSlashPieces();
    StopRibbon();   // 이전 hit 잔여 ribbon 즉시 정리

    // Dynamic ribbon 활성화 — 검 본이 잡혀 있을 때만. 메인 검기 = 실제 검 궤적.
    //   m_pTrailRibbon GameObject + SwordTrailMesh 새로 생성, m_bEmittingTrail = true.
    //   UpdateTrailMesh 가 매 frame 검 본 worldMatrix 따라 history push + vertex 갱신.
    if (m_pCachedSwordBone)
    {
        ID3D12Device* pDev = Dx12App::GetInstance()->GetDevice();
        if (pDev)
        {
            m_pTrailMesh = new SwordTrailMesh(pDev, 64);
            m_pTrailMesh->AddRef();
            XMFLOAT4 emi(m_xmf4TrailColor.x, m_xmf4TrailColor.y, m_xmf4TrailColor.z, 2.0f);
            // Ribbon = 원소별 단일 호 텍스처 (Kenney slash_02/03/04 + Free Slash noise).
            //   셰이더가 full-UV 로 슬래시 silhouette 샘플 → arcWindow soft 가장자리.
            m_pTrailRibbon = pSpawner->SpawnSlashMesh(
                pRoom, m_pTrailMesh,
                XMFLOAT3(0.0f, 0.0f, 0.0f),
                XMFLOAT3(0.0f, 0.0f, 0.0f),
                XMFLOAT3(1.0f, 1.0f, 1.0f),
                emi,
                PickSlashTextureForElement(m_nCurrentElementId));

            m_pRibbonScene = pScene;
            m_vSwordHistory.clear();
            m_bEmittingTrail = (m_pTrailRibbon != nullptr);
        }
    }

    // 검 자체 emissive 발광 — 검 본의 owner GameObject material.m_cEmissive 를 동적 boost.
    if (m_pCachedSwordBone)
    {
        m_pSwordObj = m_pCachedSwordBone->GetOwner();
        if (m_pSwordObj)
        {
            m_xmf4SwordGlowColor = m_xmf4TrailColor;
            // trail 과 같은 수명으로 통일 — 검 발광이 trail 보다 일찍 꺼지면 끝이 어색해짐.
            m_fSwordGlowMax    = m_fTrailRemain + 0.10f;
            m_fSwordGlowRemain = m_fSwordGlowMax;
            m_bSwordGlowing    = true;
            // 3단계 anticipation flash — windup 길이에 비례. 짧은 swing 에서 dominant 안 되게.
            //   windup < 0.40s: 0.025s (간결한 펀치). 길면 0.05s (drama).
            m_fSwordGlowFlashWhiteMax = (hit.fWindupTime < 0.40f) ? 0.025f : 0.05f;
            m_fSwordGlowFlashWhite    = m_fSwordGlowFlashWhiteMax;
        }
    }

    m_bTrailStarted = true;
}

// 원소별 impact particle — sub_blast_wave / sub_strike_spark / sub_cool_mist / sub_speed_streak.
//   검 끝 (m_pCachedSwordBone 의 -Y axis tip) 위치에서 발사. 본 없으면 보스 forward + chest.
void ComboAttackBehavior::SpawnImpactVFX(EnemyComponent* pEnemy, const ComboHit& hit)
{
    if (!pEnemy || hit.strVFXImpact.empty()) return;
    if (!EffectRegistry::Get().HasEffect(hit.strVFXImpact)) return;

    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    Scene* pScene = pApp->GetScene();
    if (!pScene) return;
    VFXManager* pVFX = pScene->GetVFXManager();
    if (!pVFX) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;
    TransformComponent* pBossT = pOwner->GetTransform();
    if (!pBossT) return;

    XMFLOAT3 pos;
    XMFLOAT3 dir;
    if (m_pCachedSwordBone)
    {
        const XMFLOAT4X4& w = m_pCachedSwordBone->GetWorldMatrix();
        XMFLOAT3 grip = { w._41, w._42, w._43 };
        XMFLOAT3 dirY = { w._21, w._22, w._23 };
        // -Y = tip (UpdateTrailMesh 와 같은 convention).
        pos = { grip.x - dirY.x, grip.y - dirY.y, grip.z - dirY.z };
    }
    else
    {
        XMFLOAT3 bossPos = pBossT->GetPosition();
        float yawRad = XMConvertToRadians(pBossT->GetRotation().y);
        pos = { bossPos.x + sinf(yawRad) * hit.fHitRange * 0.6f,
                bossPos.y + 5.0f,
                bossPos.z + cosf(yawRad) * hit.fHitRange * 0.6f };
    }
    // 보스 정면 방향 = effect 발산 방향
    float yawRad = XMConvertToRadians(pBossT->GetRotation().y);
    dir = { sinf(yawRad), 0.0f, cosf(yawRad) };

    pVFX->Spawn(hit.strVFXImpact, pos, dir, 0u, false);
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

    // 3단계 anticipation flash — m_fSwordGlowFlashWhiteMax 를 분모로 사용 (per-hit 값).
    float whiteAmt = 0.0f;
    if (m_fSwordGlowFlashWhite > 0.0f && m_fSwordGlowFlashWhiteMax > 0.0f)
    {
        m_fSwordGlowFlashWhite -= dt;
        whiteAmt = m_fSwordGlowFlashWhite / m_fSwordGlowFlashWhiteMax;
        if (whiteAmt < 0.0f) whiteAmt = 0.0f;
        if (whiteAmt > 1.0f) whiteAmt = 1.0f;
    }
    // Lerp(element 색, 흰색, whiteAmt) — whiteAmt 1.0 이면 완전 흰빛, 0 이면 element 색.
    float rOut = m_xmf4SwordGlowColor.x + (1.0f - m_xmf4SwordGlowColor.x) * whiteAmt;
    float gOut = m_xmf4SwordGlowColor.y + (1.0f - m_xmf4SwordGlowColor.y) * whiteAmt;
    float bOut = m_xmf4SwordGlowColor.z + (1.0f - m_xmf4SwordGlowColor.z) * whiteAmt;

    MATERIAL mat = m_pSwordObj->GetMaterial();
    // 검 자체 emissive — ribbon HDR 5 톤다운에 맞춰 검도 1.4 (이전 2.2). 어두운 RPG 분위기.
    //   anticipation flash 시 +0.4 boost — 차징 순간만 잠깐 환하게.
    float kSwordEmBoost = 1.4f + 0.4f * whiteAmt;
    mat.m_cEmissive.x = rOut * intensity * kSwordEmBoost;
    mat.m_cEmissive.y = gOut * intensity * kSwordEmBoost;
    mat.m_cEmissive.z = bOut * intensity * kSwordEmBoost;
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
        // 검 본 (Bone_Sword = grip) 위치가 아니라 **검 끝** 위치를 trail head 로 사용.
        //   검 본 worldMatrix 의 +Y axis 가 검 길이 방향 (DarkKnight2 모델 기준 추정).
        //   안 맞으면 +Z 또는 -Y 시도 — 시각 확인 후 조정.
        //   결과: trail 이 검을 덮는 게 아니라 검 끝에서 시작해 swing 자취를 호로 그림.
        const XMFLOAT4X4& w = m_pCachedSwordBone->GetWorldMatrix();
        XMFLOAT3 grip = { w._41, w._42, w._43 };

        // 본 worldMatrix 의 +Y row 벡터 = 본의 local +Y axis × world scale (mesh 길이 자동 반영).
        //   DarkLord 의 경우 row 벡터 길이 ≈ 10 (= 보스 scale × mesh local 1.0).
        //   정규화 후 fixed 길이 곱하는 방식보다 직접 row 합산이 mesh 길이/scale 자동 매칭.
        // 시각 진단 결과 — 본의 local +Y 는 grip→검 끝 의 반대 방향. -Y 가 검 끝.
        XMFLOAT3 dirY = { w._21, w._22, w._23 };
        XMFLOAT3 cur = { grip.x - dirY.x, grip.y - dirY.y, grip.z - dirY.z };

        if (m_bHasPrevSwordPos)
        {
            XMFLOAT3 prev = m_xmf3PrevSwordPos;
            // sub-step 3→6 : 빠른 swing 의 호 누적 정확도 ↑ (frame 사이 곡선 보간 더 dense).
            const int nSub = 6;
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

        // 회전 공격(cone > 180°)은 검 본이 거의 360° 도는데, 90점 누적되면 trail 이 자기 자신 감싸며
        //   perpendicular 가 교차 → 끊긴 듯 보임. 회전 시 28점으로 캡 → 호 self-intersect 차단.
        //   28점 ≈ 0.08s @ 60fps × 6 sub-step → 빠른 spin 의 ~70° 만 표시 (자연스러운 잔상).
        const size_t kMaxPoints = (m_fCurrentConeAngle > 180.0f) ? 28u : 90u;
        while (m_vSwordHistory.size() > kMaxPoints)
            m_vSwordHistory.erase(m_vSwordHistory.begin());

        // 끝 0.18s 동안 호 페이드아웃 — halfWidth 감쇠 + tail(앞쪽 오래된 점) 점진적 erase.
        //   결과: 검이 멈춰도 호가 정체되지 않고 head 쪽으로 빨려들어가며 사라짐.
        const float kFadeWindow = 0.18f;
        float widthScale = 1.0f;
        if (m_fTrailRemain < kFadeWindow)
        {
            float t = m_fTrailRemain / kFadeWindow;     // 1 → 0
            widthScale = t * t;                          // 가속 페이드 (끝부분 빠르게 ↓)
            // history 앞쪽도 깎아 호 자체를 짧게 — tail 부터 사라지는 시각 효과
            size_t target = (size_t)(m_vSwordHistory.size() * t);
            if (target < 2) target = 2;
            while (m_vSwordHistory.size() > target)
                m_vSwordHistory.erase(m_vSwordHistory.begin());
        }

        // Dynamic ribbon 은 실제 검 궤적을 따라가는 본체. 큰 임팩트는 crescent burst 가 담당.
        //   0.30 → 0.60 — 이전엔 시각이 너무 얇아 검기 자체가 잘 안 보임. 두 배로 키우되
        //   부채꼴 덩어리 느낌은 텍스처 silhouette 가 잡아주므로 안전.
        const float halfWidth = m_fTrailPieceScale * 0.60f * widthScale;
        m_pTrailMesh->UpdateTrail(m_vSwordHistory, halfWidth);
    }
    else
    {
        // hit phase 끝 → 즉시 cleanup (잔영 안 남김)
        StopRibbon();
        return;
    }

    // material: emissive.w = 2.0 ribbon sentinel, ambient.w = elementId (정수).
    //   diffuse.r = 1 (revealProgress) — ribbon 은 reveal mask 통과해야 visible. crescent 와 셰이더 공유.
    //   diffuse.g = 0 (dissolveProgress 미사용), diffuse.b = 0 (burst 미사용), diffuse.a = fadeT.
    MATERIAL mat;
    mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.0f, 0.0f, m_fRibbonFadeT);
    mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    mat.m_cEmissive = XMFLOAT4(m_xmf4TrailColor.x,
                                m_xmf4TrailColor.y,
                                m_xmf4TrailColor.z, 2.0f);
    mat.m_cAmbient  = XMFLOAT4(m_xmf4TrailEdgeColor.x,
                                m_xmf4TrailEdgeColor.y,
                                m_xmf4TrailEdgeColor.z, (float)m_nCurrentElementId);
    m_pTrailRibbon->SetMaterial(mat);
}

void ComboAttackBehavior::StopRibbon()
{
    // Scene::MarkForDeletion 은 deferred (다음 frame 처리). 그 사이 mesh 는 마지막 vertex
    //   데이터로 계속 render → "끝났는데 안 사라지는" 잔존이 한 frame 이상 보임.
    //   UpdateTrail({}, 0) 로 m_nDrawVertices=0 set → Render() 즉시 skip → 그 frame 부터 invisible.
    if (m_pTrailMesh)
    {
        m_pTrailMesh->UpdateTrail({}, 0.0f);   // <2 point → 즉시 invisible
    }
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

// ── Shape 별 호 1개 spawn 헬퍼 ───────────────────────────────────────────────
//   bossPos / 보스 yaw / chestY 기반으로 arc 점들 계산 → SwordTrailMesh 생성 → SpawnSlashMesh.
//   bVertical = true 면 위→아래 vertical plane (overhead chop) 으로 점들 분포.
static void BuildArcPoints(const ArcShapeCfg& cfg, const XMFLOAT3& bossPos, float chestY,
                           float yawDeg, float hitRange, int nArcPoints,
                           std::vector<XMFLOAT3>& outArc)
{
    float effYaw = yawDeg + cfg.yawOffsetDeg;
    float yawRad = XMConvertToRadians(effYaw);
    float cy = cosf(yawRad);
    float sy = sinf(yawRad);

    float coneRad = XMConvertToRadians(cfg.coneDeg);
    float halfCone = coneRad * 0.5f;
    float radius = hitRange * cfg.radiusMul;
    float yArc = chestY + cfg.yOffset;

    outArc.clear();
    outArc.reserve(nArcPoints);
    for (int i = 0; i < nArcPoints; ++i)
    {
        float t = (float)i / (float)(nArcPoints - 1);
        float angle = -halfCone + t * coneRad;
        float wx, wy, wz;
        if (cfg.bVertical)
        {
            // 수직 plane: angle 0 = 정면, 양수 = 위, 음수 = 아래.
            float ly = sinf(angle) * radius;
            float lz = cosf(angle) * radius;
            wx = lz * sy;
            wz = lz * cy;
            wy = yArc + ly;
        }
        else
        {
            // 수평 plane: 기존 동작.
            float lx = sinf(angle) * radius;
            float lz = cosf(angle) * radius;
            wx = lx * cy + lz * sy;
            wz = -lx * sy + lz * cy;
            wy = yArc;
        }
        outArc.push_back(XMFLOAT3(bossPos.x + wx, wy, bossPos.z + wz));
    }
}

// 호 한 개 spawn — outObj/outMesh 에 결과 저장. 실패 시 둘 다 nullptr.
void ComboAttackBehavior::SpawnSingleCrescent(EnemyComponent* pEnemy, const ComboHit& hit,
                                              const ArcShapeCfg& cfg, float chestY, float yawDeg,
                                              GameObject** outObj, SwordTrailMesh** outMesh)
{
    *outObj = nullptr;
    *outMesh = nullptr;

    CRoom* pRoom = pEnemy->GetRoom();           if (!pRoom) return;
    Scene* pScene = pRoom->GetScene();          if (!pScene) return;
    GameObject* pOwner = pEnemy->GetOwner();    if (!pOwner) return;
    TransformComponent* pBossT = pOwner->GetTransform(); if (!pBossT) return;
    EnemySpawner* pSpawner = pScene->GetEnemySpawner();  if (!pSpawner) return;

    XMFLOAT3 bossPos = pBossT->GetPosition();

    const int kArcPoints = 30;
    std::vector<XMFLOAT3> arc;
    BuildArcPoints(cfg, bossPos, chestY, yawDeg, hit.fHitRange, kArcPoints, arc);

    ID3D12Device* pDevice = Dx12App::GetInstance()->GetDevice();
    SwordTrailMesh* pMesh = new SwordTrailMesh(pDevice, kArcPoints);
    pMesh->AddRef();

    float halfWidth = m_fTrailPieceScale * cfg.halfWidthMul;
    pMesh->UpdateTrail(arc, halfWidth);

    // Crescent — 원소별 단일 호 텍스처 (Kenney slash_02/03/04 + Free Slash noise).
    //   셰이더가 full-UV 로 슬래시 silhouette 샘플 → arcWindow + 원소 stylization overlay.
    GameObject* pObj = pSpawner->SpawnSlashMesh(pRoom, pMesh,
                                                XMFLOAT3(0.0f, 0.0f, 0.0f),
                                                XMFLOAT3(0.0f, 0.0f, 0.0f),
                                                XMFLOAT3(1.0f, 1.0f, 1.0f),
                                                m_xmf4CrescentColor,
                                                PickSlashTextureForElement(m_nCurrentElementId));
    if (pObj)
    {
        MATERIAL mat = pObj->GetMaterial();
        mat.m_cAmbient = XMFLOAT4(m_xmf4TrailEdgeColor.x,
                                    m_xmf4TrailEdgeColor.y,
                                    m_xmf4TrailEdgeColor.z, 1.0f);
        mat.m_cDiffuse.w = 1.0f;
        pObj->SetMaterial(mat);
    }

    *outObj = pObj;
    *outMesh = pMesh;
}

// ── Anchored arc + 셰이더 sweep mask (primary VFX) ────────────────────────────
//   Hit 진입 순간 boss 위치 + chest height + boss yaw 기준으로 shape 별 호 spawn.
void ComboAttackBehavior::SpawnCrescentFlash(EnemyComponent* pEnemy, const ComboHit& hit)
{
    if (!pEnemy) return;
    CRoom* pRoom = pEnemy->GetRoom();           if (!pRoom)   return;
    Scene* pScene = pRoom->GetScene();          if (!pScene)  return;
    GameObject* pOwner = pEnemy->GetOwner();    if (!pOwner)  return;
    TransformComponent* pBossT = pOwner->GetTransform(); if (!pBossT) return;

    StopCrescent();   // 직전 hit 잔여 정리

    // 색 세팅 — Crescent 가 SpawnHitVFX 보다 먼저 호출될 수도 있으므로 여기서 보장.
    //   안 그러면 이전 hit 의 m_xmf4TrailColor 가 leak 됨.
    {
        ColorPair cp = PickColorPairForEffect(hit.strVFXOnHit);
        m_xmf4TrailColor     = cp.core;
        m_xmf4TrailEdgeColor = cp.edge;
        m_nCurrentElementId  = cp.elementId;
    }

    XMFLOAT3 bossPos = pBossT->GetPosition();
    float yawDeg = pBossT->GetRotation().y;

    float chestY = bossPos.y + 6.0f;
    if (m_pCachedSwordBone)
    {
        const XMFLOAT4X4& w = m_pCachedSwordBone->GetWorldMatrix();
        chestY = w._42;
    }

    m_xmf4CrescentColor = XMFLOAT4(m_xmf4TrailColor.x,
                                    m_xmf4TrailColor.y,
                                    m_xmf4TrailColor.z, 3.0f);

    // 시각 cone 캡 (광역 회전).
    float visualConeDeg = hit.fConeAngle;
    if (visualConeDeg > 140.0f) visualConeDeg = 140.0f;

    // shape 별 primary/secondary config.
    ArcShapeCfg cfgA{}, cfgB{};
    bool bUseSecondary = false;
    // halfWidthMul 전 항목 ~1.8× — 이전엔 검기가 너무 얇아 잘 안 보임. radiusMul 도 살짝 키움.
    //   Cross 는 +45/-45 로 진짜 X자.
    switch (hit.eShape)
    {
    case SwordEnergyShape::Default:
        cfgA = { visualConeDeg, 1.35f, 1.05f, 0.0f, 0.0f, false };
        break;
    case SwordEnergyShape::Wide:
        cfgA = { visualConeDeg * 0.80f, 2.10f, 1.00f, 0.0f, 0.0f, false };
        break;
    case SwordEnergyShape::Slim:
        cfgA = { visualConeDeg * 0.50f, 0.65f, 1.10f, 0.0f, 0.0f, false };
        break;
    case SwordEnergyShape::Long:
        cfgA = { visualConeDeg * 0.55f, 0.80f, 1.55f, 0.0f, 0.0f, false };
        break;
    case SwordEnergyShape::Double:
        cfgA = { visualConeDeg * 0.85f, 1.00f, 1.05f, +1.4f, 0.0f, false };
        cfgB = { visualConeDeg * 0.85f, 1.00f, 1.05f, -1.4f, 0.0f, false };
        bUseSecondary = true;
        break;
    case SwordEnergyShape::Cross:
        cfgA = { visualConeDeg * 0.55f, 0.90f, 1.10f, 0.0f, +45.0f, false };
        cfgB = { visualConeDeg * 0.55f, 0.90f, 1.10f, 0.0f, -45.0f, false };
        bUseSecondary = true;
        break;
    case SwordEnergyShape::Overhead:
        cfgA = { 100.0f, 1.00f, 1.00f, 0.0f, 0.0f, true };   // 수직 plane
        break;
    }

    SpawnSingleCrescent(pEnemy, hit, cfgA, chestY, yawDeg, &m_pCrescentObj, &m_pCrescentMesh);
    if (bUseSecondary)
        SpawnSingleCrescent(pEnemy, hit, cfgB, chestY, yawDeg, &m_pCrescentObj2, &m_pCrescentMesh2);

    m_pCrescentScene = pScene;
    // Crescent envelope — Hit 순간 spawn 기준. "쾅 하고 생긴 뒤 찢어지며 사라지는" 짧고 강한 플래시.
    //   fadeIn   : 0.035s — 거의 즉시. burst 가 이 timing 에 흰빛 폭발 overlay.
    //   hold     : hit 전체 + 약간 — swing window cover.
    //   fadeOut  : 0.18s — dissolve.
    m_fCrescentTime       = 0.0f;
    m_fCrescentFadeInDur  = 0.035f;
    m_fCrescentHoldDur    = hit.fHitTime + 0.04f;
    if (m_fCrescentHoldDur < 0.10f) m_fCrescentHoldDur = 0.10f;
    m_fCrescentFadeOutDur = 0.18f;
}

void ComboAttackBehavior::TriggerCrescentBurst()
{
    // Hit phase 진입 순간 호출 — burst 타이머 reset. 호 spawn 안 됐어도 다음 frame 에서 spawn 후 자연 적용.
    m_fCrescentBurstRemain = m_fCrescentBurstDur;
}

void ComboAttackBehavior::UpdateCrescent(float dt)
{
    if (!m_pCrescentObj) return;
    m_fCrescentTime += dt;

    float total = m_fCrescentFadeInDur + m_fCrescentHoldDur + m_fCrescentFadeOutDur;
    if (m_fCrescentTime >= total)
    {
        StopCrescent();
        return;
    }

    // 노이즈 threshold 기반 reveal/dissolve — 균일 alpha 대신 픽셀마다 다른 timing.
    //   revealProgress (0→1 fadeIn) : 픽셀의 noise threshold 보다 크면 visible.
    //   dissolveProgress (0→1 fadeOut) : threshold 보다 크면 invisible.
    //   결과: 호가 흩어진 점들로 등장 → 모임 → 흩어져 사라짐 (유기적 dust 효과).
    float revealProgress, dissolveProgress;
    if (m_fCrescentTime < m_fCrescentFadeInDur)
    {
        revealProgress = m_fCrescentTime / m_fCrescentFadeInDur;
        dissolveProgress = 0.0f;
    }
    else if (m_fCrescentTime < m_fCrescentFadeInDur + m_fCrescentHoldDur)
    {
        revealProgress = 1.0f;
        dissolveProgress = 0.0f;
    }
    else
    {
        revealProgress = 1.0f;
        float dt2 = m_fCrescentTime - m_fCrescentFadeInDur - m_fCrescentHoldDur;
        dissolveProgress = dt2 / m_fCrescentFadeOutDur;
        if (dissolveProgress > 1.0f) dissolveProgress = 1.0f;
    }

    // Hit burst — Hit phase 진입 시 m_fCrescentBurstRemain = burstDur. 매 frame 감쇠.
    //   셰이더에 burstAmt ^ 1.7 로 전달 → 빠르게 0 로 떨어져 "번쩍" 한 frame 위주 효과.
    if (m_fCrescentBurstRemain > 0.0f)
    {
        m_fCrescentBurstRemain -= dt;
        if (m_fCrescentBurstRemain < 0.0f) m_fCrescentBurstRemain = 0.0f;
    }
    float burstAmt = (m_fCrescentBurstDur > 0.0f)
                     ? (m_fCrescentBurstRemain / m_fCrescentBurstDur) : 0.0f;
    burstAmt = powf(burstAmt, 1.7f);   // 빠른 감쇠

    MATERIAL mat;
    // ambient.w = elementId (정수 0~4) — 셰이더 stylization 분기에 사용 (robust integer encoding).
    mat.m_cAmbient  = XMFLOAT4(m_xmf4TrailEdgeColor.x,
                                m_xmf4TrailEdgeColor.y,
                                m_xmf4TrailEdgeColor.z, (float)m_nCurrentElementId);
    // diffuse.r = revealProgress, diffuse.g = dissolveProgress, diffuse.b = burstAmt.
    mat.m_cDiffuse  = XMFLOAT4(revealProgress, dissolveProgress, burstAmt, 1.0f);
    mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    // emissive.w = 3.0 crescent sentinel (별도 elementId 인코딩 X — ambient.w 가 담당).
    mat.m_cEmissive = XMFLOAT4(m_xmf4CrescentColor.x,
                                m_xmf4CrescentColor.y,
                                m_xmf4CrescentColor.z, 3.0f);
    m_pCrescentObj->SetMaterial(mat);
    // secondary 호도 같은 material 동기화 (Double/Cross shape).
    if (m_pCrescentObj2)
    {
        m_pCrescentObj2->SetMaterial(mat);
    }
}

void ComboAttackBehavior::StopCrescent()
{
    // primary
    if (m_pCrescentMesh)
    {
        m_pCrescentMesh->UpdateTrail({}, 0.0f);
    }
    if (m_pCrescentObj && m_pCrescentScene)
    {
        m_pCrescentScene->MarkForDeletion(m_pCrescentObj);
    }
    if (m_pCrescentMesh)
    {
        m_pCrescentMesh->Release();
        m_pCrescentMesh = nullptr;
    }
    m_pCrescentObj = nullptr;

    // secondary (Double/Cross shape)
    if (m_pCrescentMesh2)
    {
        m_pCrescentMesh2->UpdateTrail({}, 0.0f);
    }
    if (m_pCrescentObj2 && m_pCrescentScene)
    {
        m_pCrescentScene->MarkForDeletion(m_pCrescentObj2);
    }
    if (m_pCrescentMesh2)
    {
        m_pCrescentMesh2->Release();
        m_pCrescentMesh2 = nullptr;
    }
    m_pCrescentObj2 = nullptr;

    m_pCrescentScene = nullptr;
    m_fCrescentTime = 0.0f;
    m_fCrescentBurstRemain = 0.0f;
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
