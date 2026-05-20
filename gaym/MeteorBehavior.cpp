#include "stdafx.h"
#include "MeteorBehavior.h"
#include "DecalManager.h"
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
#include <cmath>
#include <algorithm>
#include <random>

static void ApplyElementColors(EffectDef& def, ElementType e)
{
    FluidElementColor ec = FluidElementColors::Get(e);
    def.element = e;
    for (auto& l : def.layers) {
        l.element   = e;
        l.coreColor = ec.coreColor;
        l.edgeColor = ec.edgeColor;
    }
}

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

// ─── Execute: 메테오 샤워 시작 ─────────────────────────────────────────────────
void MeteorBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
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
    if (caster) {
        if (auto* pPC = caster->GetComponent<PlayerComponent>())
            m_elementType = pPC->GetElementType();
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty())
                m_elementType = sts.elementSet[0];
        }
    }

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

    XMFLOAT3 upDir = { 0.f, 1.f, 0.f };
    EffectDef trailDef = EffectRegistry::Get().GetEffect("R_MeteorSmallTrail");
    ApplyElementColors(trailDef, m_elementType);
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

    // 소형 메테오 추가 스폰
    while (m_meteorsSpawned < SHOWER_COUNT)
    {
        float spawnTime = m_meteorsSpawned * SHOWER_INTERVAL;
        if (m_elapsed < spawnTime) break;
        SpawnSmallMeteor();
        ++m_meteorsSpawned;
    }

    // 소형 메테오 낙하 추적 + 착지 판정
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

    // 최종 메테오 스폰 타이밍 — 마지막 소형 스폰 후 FINAL_DELAY 경과 시
    if (!m_bFinalSpawned)
    {
        float lastSmallTime = (SHOWER_COUNT - 1) * SHOWER_INTERVAL;
        if (m_elapsed >= lastSmallTime + FINAL_DELAY)
            SpawnFinalMeteor();
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
        EffectDef impDef = EffectRegistry::Get().GetEffect("R_MeteorSmallImpact");
        ApplyElementColors(impDef, m_elementType);
        m_pVFXManager->SpawnEffectDef(sm.targetPos, up, impDef, true);
    }

    if (m_pDecalManager)
        m_pDecalManager->Spawn(DecalTexture::Scorch2, sm.targetPos, 3.0f, 0.f, 5.f);

    float smallDmg = m_SkillData.damage * m_damageMult * SMALL_DAMAGE_RATIO;
    ApplyExplosionDamage(smallDmg, SMALL_EXPLODE_RADIUS, sm.targetPos, true);
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
    ApplyElementColors(trailDef, m_elementType);
    if (!trailDef.layers.empty())
        m_finalTrailId = m_pVFXManager->SpawnEffectLayer(
            m_finalSpawnPos, upDir, trailDef.name, trailDef.layers[0], true);

    EffectDef outerDef = EffectRegistry::Get().GetEffect("R_MeteorTrailOuter");
    ApplyElementColors(outerDef, m_elementType);
    if (!outerDef.layers.empty())
        m_finalOuterId = m_pVFXManager->SpawnEffectLayer(
            m_finalSpawnPos, upDir, outerDef.name, outerDef.layers[0], true);

    OutputDebugStringA("[Meteor] Final meteor launched!\n");
}

// ─── 최종 메테오 착지 ────────────────────────────────────────────────────────
void MeteorBehavior::OnFinalImpact()
{
    m_bFinalImpacted = true;
    m_bIsFinished    = true;

    if (m_pVFXManager)
    {
        if (m_finalTrailId >= 0) { m_pVFXManager->StopEffect(m_finalTrailId); m_finalTrailId = -1; }
        if (m_finalOuterId >= 0) { m_pVFXManager->StopEffect(m_finalOuterId); m_finalOuterId = -1; }

        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        EffectDef impDef  = EffectRegistry::Get().GetEffect("R_MeteorImpact");
        ApplyElementColors(impDef, m_elementType);
        m_finalImpactId = m_pVFXManager->SpawnEffectDef(m_targetPos, up, impDef, true);
        EffectDef fireDef = EffectRegistry::Get().GetEffect("R_MeteorGroundFire");
        ApplyElementColors(fireDef, m_elementType);
        m_finalGroundId = m_pVFXManager->SpawnEffectDef(m_targetPos, up, fireDef, true);
    }

    if (m_pDecalManager)
    {
        m_pDecalManager->Spawn(DecalTexture::Scorch1, m_targetPos, 8.0f, 0.f, 8.f);
        m_pDecalManager->Spawn(DecalTexture::Magic2,  m_targetPos, 10.f, 0.f, 6.f);
    }

    // 최종 — 범위 내 모든 적 피해 (보스 포함, 경직 없음)
    float finalDmg = m_SkillData.damage * m_damageMult;
    ApplyExplosionDamage(finalDmg, m_SkillData.range, m_targetPos, false);

    OutputDebugStringA("[Meteor] FINAL IMPACT! Full range damage applied.\n");
}

bool MeteorBehavior::IsFinished() const { return m_bIsFinished; }

void MeteorBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_finalTrailId >= 0) m_pVFXManager->StopEffect(m_finalTrailId);
        if (m_finalOuterId >= 0) m_pVFXManager->StopEffect(m_finalOuterId);
        for (auto& sm : m_smallMeteors)
            if (sm.trailVfxId >= 0) m_pVFXManager->StopEffect(sm.trailVfxId);
    }

    m_bIsFinished    = true;
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
            pEnemy->TakeDamage(actualDmg, bTriggerStagger);

            if (m_pCaster) {
                auto* pSC = m_pCaster->GetComponent<SkillComponent>();
                if (pSC && m_slot != SkillSlot::Count) {
                    SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
                    if (!sts.onHitHooks.empty()) {
                        SkillContext ctx;
                        ctx.caster             = m_pCaster;
                        ctx.baseDamage         = damage;
                        ctx.damageDealt        = actualDmg;
                        ctx.hitEnemy           = pEnemy;
                        ctx.hitEnemyPos        = ePos;
                        ctx.scene              = m_pScene;
                        ctx.statusChanceMult   = sts.statusChanceMult;
                        ctx.statusDurationMult = sts.statusDurationMult;
                        for (auto& hook : sts.onHitHooks) hook(ctx);
                    }
                }
            }
        }
    }
}
