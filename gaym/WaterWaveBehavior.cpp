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

// ─── 발동 방식 훅 ─────────────────────────────────────────────────────────────

void WaterWaveBehavior::OnChargeBegin(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    if (!EffectRegistry::Get().HasEffect("Q_WaterFall")) return;

    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 down = { 0.f, -1.f, 0.f };
    // 캐스터 위에서 물방울이 소규모로 모이기 시작하는 프리뷰
    EffectDef def = EffectRegistry::Get().GetEffect("Q_WaterFall");
    // 차지 프리뷰는 작게 — 파티클 수 축소
    for (auto& l : def.layers)
        l.particleCount = (std::max)(l.particleCount / 3, 5);
    m_chargeVFXId = m_pVFXManager->SpawnEffectDef(pos, down, def, true);
}

void WaterWaveBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    // 캐스터를 따라다니도록 VFX 위치 추적
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += 2.f + chargeRatio * 3.f;  // 차지할수록 높이 올라감
    XMFLOAT3 down = { 0.f, -1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, down);
}

void WaterWaveBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    if (!m_pVFXManager) return;
    if (!EffectRegistry::Get().HasEffect("Q_WaterFall")) return;

    XMFLOAT3 pos = { targetPosition.x, targetPosition.y + 8.f, targetPosition.z };
    XMFLOAT3 down = { 0.f, -1.f, 0.f };
    // 타겟 위에서 비처럼 물방울이 계속 떨어지는 채널 주변 VFX
    m_channelAmbientId = m_pVFXManager->SpawnEffectDef(pos, down,
        EffectRegistry::Get().GetEffect("Q_WaterFall"), true);
}

void WaterWaveBehavior::OnChannelEnd(GameObject* caster)
{
    if (m_pVFXManager && m_channelAmbientId >= 0)
    {
        m_pVFXManager->StopEffect(m_channelAmbientId);
        m_channelAmbientId = -1;
    }
}

void WaterWaveBehavior::OnEnhanceActivate(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    if (!EffectRegistry::Get().HasEffect("sub_water")) return;

    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    // 강화 버프 활성화 시 수속성 오라 생성
    m_enhanceAuraId = m_pVFXManager->SpawnEffectDef(pos, up,
        EffectRegistry::Get().GetEffect("sub_water"), true);
}

void WaterWaveBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    // 강화 오라 해제
    if (m_pVFXManager && m_enhanceAuraId >= 0)
    {
        m_pVFXManager->StopEffect(m_enhanceAuraId);
        m_enhanceAuraId = -1;
    }
    // 소모 지점에 물기둥 폭발 VFX
    if (m_pVFXManager && EffectRegistry::Get().HasEffect("Q_WaterFall"))
    {
        XMFLOAT3 pos = { targetPosition.x, targetPosition.y + 4.f, targetPosition.z };
        XMFLOAT3 down = { 0.f, -1.f, 0.f };
        EffectDef def = EffectRegistry::Get().GetEffect("Q_WaterFall");
        // 강화 소모 폭발은 크게
        for (auto& l : def.layers)
        {
            l.particleCount = l.particleCount * 2;
            l.sizeScale *= 1.5f;
        }
        m_pVFXManager->SpawnEffectDef(pos, down, def, true);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void WaterWaveBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    // 차지 프리뷰 VFX 종료 (차지 발사 시)
    if (m_pVFXManager && m_chargeVFXId >= 0)
    {
        m_pVFXManager->StopEffect(m_chargeVFXId);
        m_chargeVFXId = -1;
    }

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

void WaterWaveBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& target, float tickMult)
{
    // 틱마다 커서 방향으로 미니 웨이브 발사 — 방향을 바꿀 수 있어 일반 웨이브와 확연히 다름
    if (!m_pScene || !caster || !caster->GetTransform()) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMFLOAT3 origin = caster->GetTransform()->GetPosition();
    origin.y = 0.f;

    XMVECTOR oV = XMLoadFloat3(&origin);
    XMVECTOR dV = XMVector3Normalize(XMVectorSetY(XMVectorSubtract(XMLoadFloat3(&target), oV), 0.f));
    if (XMVectorGetX(XMVector3LengthSq(dV)) < 0.001f)
        dV = XMVector3Normalize(XMVectorSetY(caster->GetTransform()->GetLook(), 0.f));

    XMFLOAT3 direction; XMStoreFloat3(&direction, dV);

    // 미니 웨이브 VFX — sub_water를 전방으로 방출
    if (m_pVFXManager && EffectRegistry::Get().HasEffect("sub_water"))
    {
        XMFLOAT3 spawnPos = { origin.x, origin.y + 0.3f, origin.z };
        m_pVFXManager->SpawnEffectDef(spawnPos, direction,
            EffectRegistry::Get().GetEffect("sub_water", 0), false);
    }

    // 전방 직사각형 피해 — 틱마다 hit set 리셋으로 연속 타격 허용
    float damage = m_SkillData.damage * tickMult;
    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        auto* pT = obj->GetTransform();
        if (!pT) continue;

        XMFLOAT3 ePos = pT->GetPosition();
        XMVECTOR toE  = XMVectorSetY(XMVectorSubtract(XMLoadFloat3(&ePos), oV), 0.f);
        float fwd = XMVectorGetX(XMVector3Dot(toE, dV));
        if (fwd < 0.f || fwd > MINI_WAVE_RANGE) continue;

        XMVECTOR latV = XMVectorSubtract(toE, XMVectorScale(dV, fwd));
        if (XMVectorGetX(XMVector3Length(latV)) > WAVE_HALF_W) continue;

        pEnemy->TakeDamage(ApplyExecBonus(damage, pEnemy, m_pCaster), false, HasExecRune(m_pCaster));

        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count)
        {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.onHitHooks.empty())
            {
                SkillContext ctx;
                ctx.caster             = caster;
                ctx.baseDamage         = damage;
                ctx.damageDealt        = damage;
                ctx.hitEnemy           = pEnemy;
                ctx.hitEnemyPos        = ePos;
                ctx.statusChanceMult   = sts.statusChanceMult;
                ctx.statusDurationMult = sts.statusDurationMult;
                for (auto& hook : sts.onHitHooks) hook(ctx);
            }
        }
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
    waveOrigin.y = 0.f;  // VFX spawns at y+5 for visuals; damage hitbox uses ground level
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

        pEnemy->TakeDamage(ApplyExecBonus(damage, pEnemy, m_pCaster), false, HasExecRune(m_pCaster));
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
                pEnemy->TakeDamage(ApplyExecBonus(dotDmg, pEnemy, m_pCaster), false, HasExecRune(m_pCaster));
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
        if (m_vfxId           >= 0) m_pVFXManager->StopEffect(m_vfxId);
        if (m_chargeVFXId     >= 0) m_pVFXManager->StopEffect(m_chargeVFXId);
        if (m_channelAmbientId >= 0) m_pVFXManager->StopEffect(m_channelAmbientId);
        if (m_enhanceAuraId   >= 0) m_pVFXManager->StopEffect(m_enhanceAuraId);
        for (int id : m_extraVFXIds) if (id >= 0) m_pVFXManager->StopEffect(id);
    }
    m_bWaveActive      = false;
    m_vfxId            = -1;
    m_chargeVFXId      = -1;
    m_channelAmbientId = -1;
    m_enhanceAuraId    = -1;
    m_extraVFXIds.clear();
    m_hitEnemies.clear();
    m_waterPools.clear();
}
