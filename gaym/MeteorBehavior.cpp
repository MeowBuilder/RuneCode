#include "stdafx.h"
#include "MeteorBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "FluidParticle.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "PlayerComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include "DamageNumberManager.h"
#include <cmath>
#include <algorithm>
#include <random>

static constexpr float TWO_PI = 6.28318530718f;

MeteorBehavior::MeteorBehavior()
    : m_SkillData(FireSkillPresets::Meteor())
    , m_rng(std::random_device{}())
{
}

MeteorBehavior::MeteorBehavior(const SkillData& customData)
    : m_SkillData(customData)
    , m_rng(std::random_device{}())
{
}

void MeteorBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_bChannelMode   = true;
    m_bIsFinished    = false;
    m_pCaster        = caster;
    m_damageMult     = 1.f;
    m_elapsed        = 0.f;
    m_meteorsSpawned = 0;
    m_bFinalSpawned  = false;
    m_bFinalImpacted = false;
    m_smallMeteors.clear();

    m_elementType = ElementType::Fire;
    if (caster) {
        if (auto* pPC = caster->GetComponent<PlayerComponent>())
            m_elementType = pPC->GetElementType();
    }
    m_elementSet = { m_elementType };
}

void MeteorBehavior::SpawnSmallMeteorAt(const XMFLOAT3& targetPos)
{
    if (!m_pVFXManager) return;

    SmallMeteorData sm;
    sm.targetPos   = { targetPos.x, 0.f, targetPos.z };
    sm.spawnPos    = { targetPos.x, SMALL_SPAWN_HEIGHT, targetPos.z };
    sm.fallDuration = SMALL_SPAWN_HEIGHT / SMALL_FALL_SPEED;
    sm.elapsed     = 0.f;
    sm.impacted    = false;
    sm.damage      = m_SkillData.damage * m_damageMult * SMALL_DAMAGE_RATIO;
    sm.radius      = SMALL_EXPLODE_RADIUS;
    sm.stagger     = true;
    sm.isEcho      = false;

    XMFLOAT3 upDir = { 0.f, 1.f, 0.f };
    if (EffectRegistry::Get().HasEffect("R_MeteorSmallTrail"))
    {
        EffectDef trailDef = EffectRegistry::Get().GetEffect("R_MeteorSmallTrail");
        ApplyElementSetToEffectDef(trailDef, m_elementSet);
        if (!trailDef.layers.empty())
            sm.trailVfxId = m_pVFXManager->SpawnEffectLayer(
                sm.spawnPos, upDir, trailDef.name, trailDef.layers[0], true);
    }

    m_smallMeteors.push_back(sm);
}

void MeteorBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& target, float tickMult)
{
    m_pCaster = caster;
    SpawnSmallMeteorAt(target);
}

void MeteorBehavior::OnChargeBegin(GameObject* caster)
{
}

void MeteorBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void MeteorBehavior::OnEnhanceActivate(GameObject* caster)
{
}

void MeteorBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    if (m_pVFXManager && m_enhanceAuraId >= 0) { m_pVFXManager->StopEffect(m_enhanceAuraId); m_enhanceAuraId = -1; }
}

// ─── Execute: 메테오 샤워 시작 ─────────────────────────────────────────────────
void MeteorBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    // 채널 모드: Execute는 아무것도 하지 않음 (OnChannelTick이 낙하 메테오를 생성)
    if (m_bChannelMode) return;

    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }
    if (!m_pVFXManager)
    {
        OutputDebugString(L"[Meteor] Warning: No VFXManager set!\n");
        return;
    }

    // 타겟 위치 결정 (마우스 클릭 위치 또는 캐스터 전방 폴백)
    XMFLOAT3 targetPos = targetPosition;
    if (fabsf(targetPos.x) < 0.001f && fabsf(targetPos.z) < 0.001f)
    {
        if (caster && caster->GetTransform())
        {
            XMFLOAT3 casterPos = caster->GetTransform()->GetPosition();
            XMVECTOR look = caster->GetTransform()->GetLook();
            look = XMVectorSetY(look, 0.f);
            look = XMVector3Normalize(look);
            XMVECTOR targetV = XMVectorAdd(
                XMLoadFloat3(&casterPos),
                XMVectorScale(look, METEOR_FORWARD_DIST));
            XMStoreFloat3(&targetPos, targetV);
        }
    }

    m_pCaster = caster;

    // 캐스터 원소 캡처 (변환 룬 우선)
    m_elementType = ElementType::Fire;
    m_elementSet.clear();
    if (caster) {
        if (auto* pPC = caster->GetComponent<PlayerComponent>())
            m_elementType = pPC->GetElementType();
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty()) {
                m_elementType = sts.elementSet[0];
                m_elementSet  = sts.elementSet;
            }
        }
    }
    if (m_elementSet.empty()) m_elementSet = { m_elementType };

    // 상태 초기화
    m_targetPos      = targetPos;
    m_damageMult     = damageMultiplier > 0.f ? damageMultiplier : 1.f;
    m_elapsed        = 0.f;
    m_meteorsSpawned = 0;
    m_bFinalSpawned  = false;
    m_bFinalImpacted = false;
    m_finalElapsed   = 0.f;
    m_finalTrailId   = -1;
    m_finalOuterId   = -1;
    m_finalImpactId  = -1;
    m_finalGroundId  = -1;
    m_smallMeteors.clear();
    m_bIsFinished    = false;

    // 첫 번째 소형 메테오 즉시 스폰 (t=0)
    SpawnSmallMeteor();
    ++m_meteorsSpawned;

    OutputDebugStringA("[Meteor] Execute: meteor shower started\n");
}

// ─── 소형 메테오 스폰 ─────────────────────────────────────────────────────────
void MeteorBehavior::SpawnSmallMeteor()
{
    if (!m_pVFXManager) return;

    std::uniform_real_distribution<float> angleDist(0.f, TWO_PI);
    std::uniform_real_distribution<float> radiusDist(0.f, SMALL_SCATTER_RADIUS);

    SmallMeteorData sm;
    float angle  = angleDist(m_rng);
    float radius = radiusDist(m_rng);
    sm.targetPos.x = m_targetPos.x + radius * cosf(angle);
    sm.targetPos.y = m_targetPos.y;
    sm.targetPos.z = m_targetPos.z + radius * sinf(angle);
    sm.spawnPos    = { sm.targetPos.x, sm.targetPos.y + SMALL_SPAWN_HEIGHT, sm.targetPos.z };
    sm.fallDuration = SMALL_SPAWN_HEIGHT / SMALL_FALL_SPEED;
    sm.elapsed     = 0.f;
    sm.impacted    = false;
    sm.damage      = m_SkillData.damage * m_damageMult * SMALL_DAMAGE_RATIO;
    sm.radius      = SMALL_EXPLODE_RADIUS;
    sm.stagger     = true;
    sm.isEcho      = false;

    XMFLOAT3 upDir = { 0.f, 1.f, 0.f };
    EffectDef trailDef = EffectRegistry::Get().GetEffect("R_MeteorSmallTrail");
    ApplyElementSetToEffectDef(trailDef, m_elementSet);
    if (!trailDef.layers.empty())
        sm.trailVfxId = m_pVFXManager->SpawnEffectLayer(
            sm.spawnPos, upDir, trailDef.name, trailDef.layers[0], true);

    m_smallMeteors.push_back(sm);
}

// ─── Update ──────────────────────────────────────────────────────────────────
void MeteorBehavior::Update(float deltaTime)
{
    if (m_bIsFinished) return;

    m_elapsed += deltaTime;
    XMFLOAT3 upDir = { 0.f, 1.f, 0.f };

    // 소형 메테오 추가 스폰 (일반 샤워 모드만 — 채널/후처리 모드에서는 스킵)
    if (!m_bChannelMode && !m_bPostChannel)
    {
        while (m_meteorsSpawned < SHOWER_COUNT)
        {
            float spawnTime = m_meteorsSpawned * SHOWER_INTERVAL;
            if (m_elapsed < spawnTime) break;
            SpawnSmallMeteor();
            ++m_meteorsSpawned;
        }
    }

    // 소형 메테오 낙하 추적 + 착지 판정 (채널/일반 공용)
    for (auto& sm : m_smallMeteors)
    {
        if (sm.impacted) continue;

        sm.elapsed += deltaTime;

        if (sm.trailVfxId >= 0)
        {
            float curY = sm.spawnPos.y - SMALL_FALL_SPEED * sm.elapsed;
            XMFLOAT3 curPos = { sm.targetPos.x, curY, sm.targetPos.z };
            m_pVFXManager->TrackEffect(sm.trailVfxId, curPos, upDir);
        }

        if (sm.elapsed >= sm.fallDuration)
            OnSmallImpact(sm);
    }

    // 최종 메테오 스폰 — 일반 샤워 모드만 (채널은 OnChannelComplete 에서 처리)
    if (!m_bChannelMode && !m_bPostChannel && !m_bFinalSpawned)
    {
        float lastSmallTime = (SHOWER_COUNT - 1) * SHOWER_INTERVAL;
        if (m_elapsed >= lastSmallTime + FINAL_DELAY)
            SpawnFinalMeteor();
    }

    // 후처리 모드: 소형 + 대형 메테오가 모두 착지하면 완료
    if (m_bPostChannel)
    {
        bool allSmallDone = true;
        for (const auto& sm : m_smallMeteors)
            if (!sm.impacted) { allSmallDone = false; break; }
        bool finalDone = !m_bFinalSpawned || m_bFinalImpacted;
        if (allSmallDone && finalDone)
        {
            m_bPostChannel = false;
            m_bIsFinished  = true;
        }
    }

    // 최종 메테오 낙하 추적 + 착지 판정
    if (m_bFinalSpawned && !m_bFinalImpacted)
    {
        m_finalElapsed += deltaTime;

        float curY = m_finalSpawnPos.y - FINAL_FALL_SPEED * m_finalElapsed;
        XMFLOAT3 curPos = { m_targetPos.x, curY, m_targetPos.z };
        if (m_finalTrailId >= 0) m_pVFXManager->TrackEffect(m_finalTrailId, curPos, upDir);
        if (m_finalOuterId >= 0) m_pVFXManager->TrackEffect(m_finalOuterId, curPos, upDir);

        float finalFallDuration = FINAL_SPAWN_HEIGHT / FINAL_FALL_SPEED;
        if (m_finalElapsed >= finalFallDuration)
            OnFinalImpact();
    }

    // 일반 샤워 완료 판정 — 최종 메테오 착지 후에도 메아리 메테오가 남아 있으면 대기
    // (OnFinalImpact 에서 바로 끝내면 메아리 메테오가 추적 안 돼 공중에서 사라짐)
    if (!m_bChannelMode && !m_bPostChannel && m_bFinalSpawned && m_bFinalImpacted)
    {
        bool allSmallDone = true;
        for (const auto& sm : m_smallMeteors)
            if (!sm.impacted) { allSmallDone = false; break; }
        if (allSmallDone)
            m_bIsFinished = true;
    }
}

// ─── 소형 메테오 착지 ────────────────────────────────────────────────────────
void MeteorBehavior::OnSmallImpact(SmallMeteorData& sm)
{
    sm.impacted = true;

    if (m_pVFXManager)
    {
        if (sm.trailVfxId >= 0)
        {
            m_pVFXManager->StopEffect(sm.trailVfxId);
            sm.trailVfxId = -1;
        }
        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        if (sm.isEcho)
        {
            // 메아리 재발동: 최종 메테오급 대형 폭발 VFX
            EffectDef impDef = EffectRegistry::Get().GetEffect("R_MeteorImpact");
            ApplyElementSetToEffectDef(impDef, m_elementSet);
            m_pVFXManager->SpawnEffectDef(sm.targetPos, up, impDef, true);
            EffectDef fireDef = EffectRegistry::Get().GetEffect("R_MeteorGroundFire");
            ApplyElementSetToEffectDef(fireDef, m_elementSet);
            m_pVFXManager->SpawnEffectDef(sm.targetPos, up, fireDef, true);
        }
        else
        {
            EffectDef impDef = EffectRegistry::Get().GetEffect("R_MeteorSmallImpact");
            ApplyElementSetToEffectDef(impDef, m_elementSet);
            m_pVFXManager->SpawnEffectDef(sm.targetPos, up, impDef, true);
        }
    }

    // 스폰 시점에 캡처한 피해/반경 사용 (공유 m_damageMult 비의존)
    ApplyExplosionDamage(sm.damage, sm.radius, sm.targetPos, sm.stagger);
}

// ─── 최종 대형 메테오 스폰 ────────────────────────────────────────────────────
void MeteorBehavior::SpawnFinalMeteor()
{
    if (!m_pVFXManager) return;

    m_finalSpawnPos = { m_targetPos.x, m_targetPos.y + FINAL_SPAWN_HEIGHT, m_targetPos.z };
    m_finalElapsed  = 0.f;
    m_bFinalSpawned = true;

    XMFLOAT3 upDir  = { 0.f, 1.f, 0.f };

    EffectDef trailDef = EffectRegistry::Get().GetEffect("R_MeteorTrail");
    ApplyElementSetToEffectDef(trailDef, m_elementSet);
    if (!trailDef.layers.empty())
        m_finalTrailId = m_pVFXManager->SpawnEffectLayer(
            m_finalSpawnPos, upDir, trailDef.name, trailDef.layers[0], true);

    EffectDef outerDef = EffectRegistry::Get().GetEffect("R_MeteorTrailOuter");
    ApplyElementSetToEffectDef(outerDef, m_elementSet);
    if (!outerDef.layers.empty())
        m_finalOuterId = m_pVFXManager->SpawnEffectLayer(
            m_finalSpawnPos, upDir, outerDef.name, outerDef.layers[0], true);

    OutputDebugStringA("[Meteor] Final meteor launched!\n");
}

// ─── 최종 메테오 착지 ────────────────────────────────────────────────────────
void MeteorBehavior::OnFinalImpact()
{
    m_bFinalImpacted = true;
    // 완료 판정은 Update() 말미의 통합 체크에서 처리 (메아리 메테오가 남아 있을 수 있음)

    if (m_pVFXManager)
    {
        if (m_finalTrailId >= 0) { m_pVFXManager->StopEffect(m_finalTrailId); m_finalTrailId = -1; }
        if (m_finalOuterId >= 0) { m_pVFXManager->StopEffect(m_finalOuterId); m_finalOuterId = -1; }

        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        EffectDef impDef  = EffectRegistry::Get().GetEffect("R_MeteorImpact");
        ApplyElementSetToEffectDef(impDef, m_elementSet);
        m_finalImpactId = m_pVFXManager->SpawnEffectDef(m_targetPos, up, impDef, true);
        EffectDef fireDef = EffectRegistry::Get().GetEffect("R_MeteorGroundFire");
        ApplyElementSetToEffectDef(fireDef, m_elementSet);
        m_finalGroundId = m_pVFXManager->SpawnEffectDef(m_targetPos, up, fireDef, true);
    }


    // 최종 — 범위 내 모든 적 피해 (보스 포함, 경직 없음)
    float finalDmg = m_SkillData.damage * m_damageMult;
    ApplyExplosionDamage(finalDmg, m_SkillData.range, m_targetPos, false);

    OutputDebugStringA("[Meteor] FINAL IMPACT! Full range damage applied.\n");
}

bool MeteorBehavior::IsFinished() const { return m_bIsFinished; }

// ─── 채널 완료: 하늘에서 낙하하는 대형 메테오 ────────────────────────────────
void MeteorBehavior::OnChannelComplete(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    if (!m_bChannelMode) return;
    m_pCaster   = caster;
    m_targetPos = targetPosition;
    SpawnFinalMeteor();  // 실제 낙하 애니메이션 → OnFinalImpact 에서 폭발
    OutputDebugStringA("[Meteor] Channel complete: final meteor launched from sky!\n");
}

// ─── 채널 종료 (중단 or 완료 후 정리) ───────────────────────────────────────
void MeteorBehavior::OnChannelEnd(GameObject* caster)
{
    if (!m_bChannelMode) return;
    m_bChannelMode = false;

    // 낙하 중 소형 메테오 or 대형 메테오가 있으면 후처리 모드로 전환
    bool anyFalling = false;
    for (const auto& sm : m_smallMeteors)
        if (!sm.impacted) { anyFalling = true; break; }

    bool finalPending = m_bFinalSpawned && !m_bFinalImpacted;

    if (anyFalling || finalPending)
    {
        m_bPostChannel = true;
        // m_bIsFinished는 false 유지 → SkillComponent가 Update() 계속 호출
    }
    else
    {
        m_bIsFinished = true;
    }
}

// ─── 메아리 룬(ABY_ECO) 재발동 ────────────────────────────────────────────────
// 첫 시전의 진행 중 상태(m_smallMeteors / 최종 메테오)를 절대 건드리지 않고,
// 타겟 위에 독립적인 대형 메테오 1개를 떨군다. 피해/반경은 메테오에 직접 캡처되어
// 공유 멤버(m_damageMult 등)와 충돌하지 않는다.
void MeteorBehavior::SpawnEchoMeteorAt(const XMFLOAT3& targetPos, float mult)
{
    if (!m_pVFXManager) return;

    SmallMeteorData sm;
    sm.targetPos    = { targetPos.x, 0.f, targetPos.z };
    sm.spawnPos     = { targetPos.x, SMALL_SPAWN_HEIGHT, targetPos.z };
    sm.fallDuration = SMALL_SPAWN_HEIGHT / SMALL_FALL_SPEED;
    sm.elapsed      = 0.f;
    sm.impacted     = false;
    // 메아리는 이미 0.5배 mult가 적용돼 들어온다 → 최종 메테오급 전체 범위 단발
    sm.damage       = m_SkillData.damage * mult;
    sm.radius       = m_SkillData.range;
    sm.stagger      = false;
    sm.isEcho       = true;

    XMFLOAT3 upDir = { 0.f, 1.f, 0.f };
    EffectDef trailDef = EffectRegistry::Get().GetEffect("R_MeteorTrail");
    ApplyElementSetToEffectDef(trailDef, m_elementSet);
    if (!trailDef.layers.empty())
        sm.trailVfxId = m_pVFXManager->SpawnEffectLayer(
            sm.spawnPos, upDir, trailDef.name, trailDef.layers[0], true);

    m_smallMeteors.push_back(sm);
}

void MeteorBehavior::OnEchoFire(GameObject* caster, const DirectX::XMFLOAT3& targetPos, float mult)
{
    if (!m_pVFXManager) return;
    if (caster) m_pCaster = caster;

    SpawnEchoMeteorAt(targetPos, mult);
    m_bIsFinished = false;  // Update()가 메아리 메테오를 끝까지 추적하도록
    OutputDebugStringA("[Meteor] Echo rune: independent meteor dropped!\n");
}

void MeteorBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_finalTrailId >= 0) m_pVFXManager->StopEffect(m_finalTrailId);
        if (m_finalOuterId >= 0) m_pVFXManager->StopEffect(m_finalOuterId);
        if (m_chargeVFXId  >= 0) m_pVFXManager->StopEffect(m_chargeVFXId);
        if (m_enhanceAuraId >= 0) m_pVFXManager->StopEffect(m_enhanceAuraId);
        for (auto& sm : m_smallMeteors)
            if (sm.trailVfxId >= 0) m_pVFXManager->StopEffect(sm.trailVfxId);
    }

    m_bIsFinished    = true;
    m_bChannelMode   = false;
    m_bPostChannel   = false;
    m_chargeVFXId    = -1;
    m_enhanceAuraId  = -1;
    m_elapsed        = 0.f;
    m_meteorsSpawned = 0;
    m_bFinalSpawned  = false;
    m_bFinalImpacted = false;
    m_finalElapsed   = 0.f;
    m_finalTrailId   = -1;
    m_finalOuterId   = -1;
    m_finalImpactId  = -1;
    m_finalGroundId  = -1;
    m_smallMeteors.clear();
}

// ─── AoE 데미지 ──────────────────────────────────────────────────────────────
void MeteorBehavior::ApplyExplosionDamage(float damage, float radius, const XMFLOAT3& center, bool bTriggerStagger)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMVECTOR centerV = XMLoadFloat3(&center);

    // SkillStats를 루프 밖에서 한 번만 빌드
    SkillStats sts;
    bool hasStats = false;
    if (m_pCaster) {
        auto* pSC = m_pCaster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            hasStats = true;
        }
    }

    const auto& gameObjects = pRoom->GetGameObjects();
    for (const auto& obj : gameObjects)
    {
        if (!obj) continue;
        EnemyComponent* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        TransformComponent* pTransform = obj->GetTransform();
        if (!pTransform) continue;

        XMFLOAT3 ePos   = pTransform->GetPosition();
        XMFLOAT3 eScale = pTransform->GetScale();
        float eRadius   = max(1.5f, max(eScale.x, max(eScale.y, eScale.z)) * 1.5f);

        float dist = XMVectorGetX(XMVector3Length(
            XMVectorSubtract(XMLoadFloat3(&ePos), centerV)));

        if (dist < radius + eRadius)
        {
            float falloff = 1.f - (dist / (radius + eRadius)) * 0.5f;
            falloff = max(0.5f, falloff);
            float actualDmg = damage * falloff;

            // 처형자: HP 30% 이하 추가 피해
            bool bExec = hasStats && sts.execDamageBonus > 0.f;
            if (bExec && pEnemy->GetHpRatio() < 0.3f)
                actualDmg *= (1.f + sts.execDamageBonus);

            pEnemy->TakeDamage(actualDmg, bTriggerStagger, bExec);
            NotifyHit(m_pCaster, ePos);

            if (hasStats) {
                if (auto* pSC = m_pCaster->GetComponent<SkillComponent>())
                    pSC->ApplyOnHitRunes(m_slot, sts, damage, actualDmg, pEnemy, ePos, m_pScene);
            }
        }
    }
}
