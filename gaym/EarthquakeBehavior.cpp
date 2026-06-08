#include "stdafx.h"
#include "EarthquakeBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include <algorithm>

EarthquakeBehavior::EarthquakeBehavior()
    : m_SkillData(EarthSkillPresets::Earthquake())
{
}

void EarthquakeBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_bChannelMode = true;
    m_cachedElemSet.clear();
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            m_cachedElemSet = sts.elementSet;
        }
    }
}

void EarthquakeBehavior::OnChannelEnd(GameObject* caster)
{
    m_bChannelMode = false;
    if (m_pVFXManager && m_channelAmbientId >= 0)
    {
        m_pVFXManager->StopEffect(m_channelAmbientId);
        m_channelAmbientId = -1;
    }
}

void EarthquakeBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& target, float tickMult)
{
    // 채널 중 캐스터 위치에 충격파 링 1개 즉시 발동 — 틱마다 반경 내 적 전체 타격
    if (!m_pScene || !caster || !caster->GetTransform()) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMFLOAT3 epicenter = caster->GetTransform()->GetPosition();
    epicenter.y = 0.f;

    if (m_pVFXManager && EffectRegistry::Get().HasEffect("R_Earthquake_Ring"))
    {
        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        EffectDef def = EffectRegistry::Get().GetEffect("R_Earthquake_Ring");
        if (!m_cachedElemSet.empty())
            ApplyElementSetToEffectDef(def, m_cachedElemSet);
        m_pVFXManager->SpawnEffectDef(epicenter, up, def, true);
    }

    float damage   = m_SkillData.damage * tickMult;
    float radiusSq = (RING_MAX_RADIUS * 0.6f) * (RING_MAX_RADIUS * 0.6f);
    XMVECTOR eV = XMLoadFloat3(&epicenter);

    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        auto* pT = obj->GetTransform();
        if (!pT) continue;
        XMFLOAT3 ep = pT->GetPosition();
        XMVECTOR toE = XMVectorSetY(XMVectorSubtract(XMLoadFloat3(&ep), eV), 0.f);
        if (XMVectorGetX(XMVector3LengthSq(toE)) > radiusSq) continue;
        pEnemy->TakeDamage(ApplyExecBonus(damage, pEnemy, m_pCaster), false, HasExecRune(m_pCaster));
        NotifyHit(m_pCaster, ep);
    }
}

void EarthquakeBehavior::OnChargeBegin(GameObject* caster)
{
}

void EarthquakeBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void EarthquakeBehavior::OnEnhanceActivate(GameObject* caster)
{
}

void EarthquakeBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    if (m_pVFXManager && m_enhanceAuraId >= 0) { m_pVFXManager->StopEffect(m_enhanceAuraId); m_enhanceAuraId = -1; }
}

void EarthquakeBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bActive = false;
    m_pCaster = caster;
    m_rings.clear();
    m_hitEnemies.clear();
    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }

    if (!m_pVFXManager) { return; }

    m_epicenter = { 0.f, 0.f, 0.f };
    if (caster && caster->GetTransform())
        m_epicenter = caster->GetTransform()->GetPosition();
    // m_epicenter.y는 캐릭터 실제 높이 유지 — Y=0 강제 시 수중 보스 등에서 미스 발생

    // 변환 룬 원소 세트 캡처 (멀티 원소 레이어용)
    m_cachedElemSet.clear();
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            m_cachedElemSet = sts.elementSet;
        }
    }

    // 초기 버스트 VFX (중심 폭발)
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    EffectDef burstDef = EffectRegistry::Get().GetEffect("R_Earthquake_Burst");
    if (!m_cachedElemSet.empty())
        ApplyElementSetToEffectDef(burstDef, m_cachedElemSet);
    m_burstVfxId = m_pVFXManager->SpawnEffectDef(m_epicenter, up, burstDef, true);

    // 충격파 링 스케줄
    m_rings.resize(RING_COUNT);
    for (int i = 0; i < RING_COUNT; ++i)
    {
        m_rings[i].launchAt  = i * RING_INTERVAL;
        m_rings[i].radius    = 0.f;
        m_rings[i].launched  = false;
        m_rings[i].hit       = false;
        m_rings[i].vfxId     = -1;
    }

    m_bActive    = true;
    m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
    m_elapsed    = 0.f;
}

void EarthquakeBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;
    m_elapsed += deltaTime;

    for (int i = 0; i < (int)m_rings.size(); ++i)
        if (!m_rings[i].launched && m_elapsed >= m_rings[i].launchAt)
            LaunchWave(i);

    UpdateWaves(deltaTime);

    if (m_elapsed >= TOTAL_DURATION)
        m_bActive = false;
}

void EarthquakeBehavior::LaunchWave(int idx)
{
    auto& r = m_rings[idx];
    r.launched = true;
    r.radius   = 0.f;

    if (m_pVFXManager)
    {
        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        EffectDef def = EffectRegistry::Get().GetEffect("R_Earthquake_Ring");
        if (!m_cachedElemSet.empty())
            ApplyElementSetToEffectDef(def, m_cachedElemSet);
        r.vfxId = m_pVFXManager->SpawnEffectDef(m_epicenter, up, def, true);
    }
}

void EarthquakeBehavior::UpdateWaves(float dt)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    for (auto& r : m_rings)
    {
        if (!r.launched) continue;
        if (r.radius >= RING_MAX_RADIUS) continue;

        float prevRadius = r.radius;
        r.radius += RING_SPEED * dt;
        if (r.radius > RING_MAX_RADIUS) r.radius = RING_MAX_RADIUS;

        // 링 영역 안에 있는 적 타격 (이미 hit한 적은 건너뜀)
        float dmg = (m_SkillData.damage / RING_COUNT) * m_damageMult;

        for (const auto& obj : pRoom->GetGameObjects())
        {
            if (!obj) continue;
            auto* pEnemy = obj->GetComponent<EnemyComponent>();
            if (!pEnemy || pEnemy->IsDead()) continue;
            if (m_hitEnemies.count(pEnemy)) continue;

            auto* pT = obj->GetTransform();
            if (!pT) continue;

            XMFLOAT3 ep = pT->GetPosition();
            float dx = ep.x - m_epicenter.x, dz = ep.z - m_epicenter.z;
            float dist = sqrtf(dx * dx + dz * dz);

            // 링 두께 범위 안에 있을 때만 타격
            float inner = (std::max)(0.f, r.radius - RING_THICKNESS);
            if (dist < inner || dist > r.radius + RING_THICKNESS) continue;

            float yTol = (std::max)(0.f, pT->GetScale().y * 0.6f);
            if (fabsf(ep.y - m_epicenter.y) > RING_HEIGHT + yTol) continue;

            pEnemy->TakeDamage(ApplyExecBonus(dmg, pEnemy, m_pCaster), true, HasExecRune(m_pCaster));
            m_hitEnemies.insert(pEnemy);
            NotifyHit(m_pCaster, ep);

            if (m_pCaster) {
                auto* pSC = m_pCaster->GetComponent<SkillComponent>();
                if (pSC && m_slot != SkillSlot::Count) {
                    SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
                    pSC->ApplyOnHitRunes(m_slot, sts, dmg, dmg, pEnemy, ep, m_pScene);
                }
            }
        }
    }
}

bool EarthquakeBehavior::IsFinished() const { return !m_bActive; }

void EarthquakeBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_burstVfxId       >= 0) m_pVFXManager->StopEffect(m_burstVfxId);
        if (m_chargeVFXId      >= 0) m_pVFXManager->StopEffect(m_chargeVFXId);
        if (m_enhanceAuraId    >= 0) m_pVFXManager->StopEffect(m_enhanceAuraId);
        if (m_channelAmbientId >= 0) m_pVFXManager->StopEffect(m_channelAmbientId);
        for (auto& r : m_rings)
            if (r.launched && r.vfxId >= 0) m_pVFXManager->StopEffect(r.vfxId);
    }
    m_bActive          = false;
    m_bChannelMode     = false;
    m_burstVfxId       = -1;
    m_chargeVFXId      = -1;
    m_enhanceAuraId    = -1;
    m_channelAmbientId = -1;
    m_rings.clear();
    m_hitEnemies.clear();
}
