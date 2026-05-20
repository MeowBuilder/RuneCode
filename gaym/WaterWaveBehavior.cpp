#include "stdafx.h"
#include "WaterWaveBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "RuneDef.h"
#include "PlayerComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include <algorithm>

WaterWaveBehavior::WaterWaveBehavior()
    : m_SkillData(WaterSkillPresets::WaterWave())
{
}

uint32_t WaterWaveBehavior::GetRuneFlags(GameObject* caster) const
{
    if (!caster) return 0;
    auto* pSC = caster->GetComponent<SkillComponent>();
    if (!pSC || m_slot == SkillSlot::Count) return 0;
    RuneCombo c = pSC->GetRuneCombo(m_slot);
    uint32_t f = 0;
    if (c.hasInstant) f |= RUNE_INSTANT;
    if (c.hasCharge)  f |= RUNE_CHARGE;
    if (c.hasChannel) f |= RUNE_CHANNEL;
    if (c.hasEnhance) f |= RUNE_ENHANCE;
    if (c.hasSplit)   f |= RUNE_SPLIT;
    return f;
}

void WaterWaveBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_pCaster     = caster;
    m_bWaveActive = false;
    m_hitEnemies.clear();
    m_waterPools.clear();

    if (!m_pVFXManager) { return; }

    XMFLOAT3 origin    = { 0.f, 0.f, 0.f };
    XMFLOAT3 direction = { 0.f, 0.f, 1.f };

    if (caster && caster->GetTransform())
    {
        origin = caster->GetTransform()->GetPosition();
        origin.y += 5.0f;

        XMVECTOR oV = XMLoadFloat3(&origin);
        XMVECTOR tV = XMLoadFloat3(&targetPosition);
        XMVECTOR dV = XMVector3Normalize(XMVectorSetY(XMVectorSubtract(tV, oV), 0.f));
        if (XMVectorGetX(XMVector3LengthSq(dV)) < 0.001f)
            dV = XMVector3Normalize(XMVectorSetY(caster->GetTransform()->GetLook(), 0.f));
        XMStoreFloat3(&direction, dV);
    }

    uint32_t runeFlags = GetRuneFlags(caster);
    SkillStats stats;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count)
            stats = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    EffectDef def = EffectRegistry::Get().GetEffect("Q_WaterWave", runeFlags);
    if (!stats.elementSet.empty())
        ApplyElementToEffectDef(def, stats.elementSet[0]);
    m_vfxId = m_pVFXManager->SpawnEffectDef(origin, direction, def, true);

    m_extraVFXIds.clear();
    for (const auto& sid : stats.subVFXIds) {
        if (!EffectRegistry::Get().HasEffect(sid)) continue;
        int eid = m_pVFXManager->SpawnEffectDef(origin, direction,
            EffectRegistry::Get().GetEffect(sid, runeFlags), true);
        if (eid >= 0) m_extraVFXIds.push_back(eid);
    }

    if (m_vfxId >= 0)
    {
        m_bWaveActive   = true;
        m_damageMult    = damageMultiplier > 0.f ? damageMultiplier : 1.f;
        m_waveElapsed   = 0.f;
        m_poolDropTimer = 0.f;
    }
}

void WaterWaveBehavior::Update(float deltaTime)
{
    if (!m_pVFXManager) return;
    if (!m_bWaveActive && m_waterPools.empty()) return;

    if (m_bWaveActive && m_vfxId >= 0)
    {
        m_waveElapsed += deltaTime;
        HitEnemiesInWave(m_SkillData.damage * m_damageMult);

        m_poolDropTimer += deltaTime;
        if (m_poolDropTimer >= POOL_DROP_INTERVAL)
        {
            m_poolDropTimer = 0.f;
            DropWaterPool();
        }

        if (m_waveElapsed >= WAVE_DURATION)
            m_bWaveActive = false;
    }

    UpdateWaterPools(deltaTime);
}

void WaterWaveBehavior::HitEnemiesInWave(float damage)
{
    if (!m_pScene || m_vfxId < 0) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMFLOAT3 waveOrigin = m_pVFXManager->GetWaveOrigin(m_vfxId);
    XMFLOAT3 waveDir    = m_pVFXManager->GetWaveDir(m_vfxId);
    XMVECTOR oV  = XMLoadFloat3(&waveOrigin);
    XMVECTOR dV  = XMVector3Normalize(XMLoadFloat3(&waveDir));

    float front = m_waveElapsed * WAVE_SPEED;
    float back  = (std::max)(0.f, front - WAVE_HIT_DEPTH);

    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        if (m_hitEnemies.count(pEnemy)) continue;

        auto* pT = obj->GetTransform();
        if (!pT) continue;

        XMFLOAT3 ePos  = pT->GetPosition();
        XMFLOAT3 eScale = pT->GetScale();
        float eR = (std::max)(0.f, (std::max)(eScale.x, eScale.z) * 0.9f);

        XMVECTOR toE = XMVectorSubtract(XMLoadFloat3(&ePos), oV);
        float fwd = XMVectorGetX(XMVector3Dot(toE, dV));
        if (fwd < back - eR || fwd > front + eR) continue;

        XMVECTOR latV = XMVectorSetY(XMVectorSubtract(toE, XMVectorScale(dV, fwd)), 0.f);
        if (XMVectorGetX(XMVector3Length(latV)) > WAVE_HALF_W + eR) continue;

        float yTol = (std::max)(0.f, eScale.y * 0.6f);
        if (fabsf(ePos.y - waveOrigin.y) > WAVE_HALF_H + yTol) continue;

        pEnemy->TakeDamage(damage, false);
        m_hitEnemies.insert(pEnemy);

        if (m_pCaster)
        {
            auto* pSC = m_pCaster->GetComponent<SkillComponent>();
            if (pSC && m_slot != SkillSlot::Count)
            {
                SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
                if (!sts.onHitHooks.empty())
                {
                    SkillContext ctx;
                    ctx.caster             = m_pCaster;
                    ctx.baseDamage         = damage;
                    ctx.damageDealt        = damage;
                    ctx.hitEnemy           = pEnemy;
                    ctx.hitEnemyPos        = pT->GetPosition();
                    ctx.statusChanceMult   = sts.statusChanceMult;
                    ctx.statusDurationMult = sts.statusDurationMult;
                    for (auto& hook : sts.onHitHooks) hook(ctx);
                }
            }
        }
    }
}

void WaterWaveBehavior::DropWaterPool()
{
    if (m_vfxId < 0) return;

    XMFLOAT3 front = m_pVFXManager->GetWaveFrontPos(m_vfxId);
    front.y = 0.f;

    WaterPool pool;
    pool.center    = front;
    pool.lifetime  = POOL_LIFETIME;
    pool.tickTimer = 0.f;
    pool.vfxId     = -1; // 물 웅덩이 VFX는 별도 구현 여지
    m_waterPools.push_back(pool);
}

void WaterWaveBehavior::UpdateWaterPools(float deltaTime)
{
    if (!m_pScene || m_waterPools.empty()) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    float dotDmg = m_SkillData.damage * m_damageMult * POOL_DMG_MULT;

    for (auto& pool : m_waterPools)
    {
        pool.lifetime  -= deltaTime;
        pool.tickTimer += deltaTime;
        if (pool.tickTimer < POOL_TICK_INTERVAL) continue;
        pool.tickTimer = 0.f;

        for (const auto& obj : pRoom->GetGameObjects())
        {
            if (!obj) continue;
            auto* pEnemy = obj->GetComponent<EnemyComponent>();
            if (!pEnemy || pEnemy->IsDead()) continue;
            auto* pT = obj->GetTransform();
            if (!pT) continue;

            XMFLOAT3 ep = pT->GetPosition();
            float dx = ep.x - pool.center.x, dz = ep.z - pool.center.z;
            if (dx * dx + dz * dz <= POOL_RADIUS * POOL_RADIUS)
                pEnemy->TakeDamage(dotDmg, false);
        }
    }

    m_waterPools.erase(
        std::remove_if(m_waterPools.begin(), m_waterPools.end(),
            [](const WaterPool& p) { return p.lifetime <= 0.f; }),
        m_waterPools.end());
}

bool WaterWaveBehavior::IsFinished() const
{
    return !m_bWaveActive && m_waterPools.empty();
}

void WaterWaveBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_vfxId >= 0) m_pVFXManager->StopEffect(m_vfxId);
        for (int id : m_extraVFXIds) if (id >= 0) m_pVFXManager->StopEffect(id);
    }
    m_bWaveActive = false;
    m_vfxId       = -1;
    m_extraVFXIds.clear();
    m_hitEnemies.clear();
    m_waterPools.clear();
}
