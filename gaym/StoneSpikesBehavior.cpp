#include "stdafx.h"
#include "StoneSpikesBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include "VFXSpriteManager.h"
#include "DamageNumberManager.h"

StoneSpikesBehavior::StoneSpikesBehavior()
    : m_SkillData(EarthSkillPresets::StoneSpikes())
{
}

void StoneSpikesBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_bChannelMode = true;
    m_pCaster = caster;  // 채널 모드에서도 caster 캐시 (처형자 룬 등 SkillStats 조회에 필요)
    // Execute에서 원소를 캐시하지 않으므로 여기서 미리 캐시
    m_cachedElem = ElementType::None;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty()) m_cachedElem = sts.elementSet[0];
        }
    }
}

void StoneSpikesBehavior::OnChannelEnd(GameObject* caster)
{
    m_bChannelMode = false;
    if (m_pVFXManager && m_channelAmbientId >= 0)
    {
        m_pVFXManager->StopEffect(m_channelAmbientId);
        m_channelAmbientId = -1;
    }
}

void StoneSpikesBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& target, float tickMult)
{
    // 채널 중 커서 위치에 기둥 1개 즉시 생성 — 방향을 바꿔가며 박을 수 있어 일반 스킬과 차별화
    if (!m_pVFXManager || !m_pScene) return;

    if (EffectRegistry::Get().HasEffect("Q_StoneSpike"))
    {
        XMFLOAT3 spawnPos = { target.x, 0.f, target.z };
        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        EffectDef def = EffectRegistry::Get().GetEffect("Q_StoneSpike");
        if (m_cachedElem != ElementType::None)
            ApplyElementToEffectDef(def, m_cachedElem);
        m_pVFXManager->SpawnEffectDef(spawnPos, up, def, true);
    }

    float damage = m_SkillData.damage * tickMult;
    HitEnemiesAtSpike({ target.x, 0.f, target.z }, damage);
}

void StoneSpikesBehavior::OnChargeBegin(GameObject* caster)
{
}

void StoneSpikesBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void StoneSpikesBehavior::OnEnhanceActivate(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_enhanceAuraId = m_pVFXManager->SpawnEffectDef(pos, up, EffectRegistry::Get().GetEffect(fx), true);
}

void StoneSpikesBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    if (m_pVFXManager && m_enhanceAuraId >= 0) { m_pVFXManager->StopEffect(m_enhanceAuraId); m_enhanceAuraId = -1; }
    if (!m_pVFXManager) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    EffectDef def = EffectRegistry::Get().GetEffect(fx);
    for (auto& l : def.layers) l.particleCount *= 3;
    m_pVFXManager->SpawnEffectDef(targetPosition, up, def, false);
}

void StoneSpikesBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    // 채널 모드: 직선 4기둥 스킵 — OnChannelTick에서 커서 위치 자유배치로 대체
    if (m_bChannelMode)
    {
        if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }
        return;
    }

    m_bActive = false;
    m_pCaster = caster;
    m_spikes.clear();
    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }

    if (!m_pVFXManager) { return; }

    XMFLOAT3 origin = { 0.f, 0.f, 0.f };
    if (caster && caster->GetTransform())
        origin = caster->GetTransform()->GetPosition();

    XMVECTOR oV = XMLoadFloat3(&origin);
    XMVECTOR tV = XMLoadFloat3(&targetPosition);
    XMVECTOR dV = XMVector3Normalize(XMVectorSetY(XMVectorSubtract(tV, oV), 0.f));
    if (XMVectorGetX(XMVector3LengthSq(dV)) < 0.001f && caster && caster->GetTransform())
        dV = XMVector3Normalize(XMVectorSetY(caster->GetTransform()->GetLook(), 0.f));

    XMFLOAT3 dir;
    XMStoreFloat3(&dir, dV);

    // 첫 번째 기둥은 캐릭터 앞 약간 거리부터
    for (int i = 0; i < SPIKE_COUNT; ++i)
    {
        SpikeEntry e;
        XMVECTOR posV = XMVectorAdd(oV, XMVectorScale(dV, SPIKE_SPACING * (float)(i + 1)));
        XMStoreFloat3(&e.pos, posV);
        e.pos.y      = origin.y;  // 플레이어 높이 기준 (수중 보스 등 고도 변화 대응)
        e.triggerAt  = i * SPIKE_INTERVAL;
        m_spikes.push_back(e);
    }

    // 변환 룬 원소 캡처
    m_cachedElem = ElementType::None;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty())
                m_cachedElem = sts.elementSet[0];
        }
    }

    m_bActive    = true;
    m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
    m_elapsed    = 0.f;
}

void StoneSpikesBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;
    m_elapsed += deltaTime;

    for (int i = 0; i < (int)m_spikes.size(); ++i)
    {
        auto& s = m_spikes[i];
        if (!s.triggered && m_elapsed >= s.triggerAt)
            TriggerSpike(i);
    }

    if (m_elapsed >= TOTAL_DURATION)
        m_bActive = false;
}

void StoneSpikesBehavior::TriggerSpike(int idx)
{
    auto& s = m_spikes[idx];
    s.triggered = true;

    // VFX 스폰
    if (m_pVFXManager)
    {
        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        EffectDef def = EffectRegistry::Get().GetEffect("Q_StoneSpike");
        if (m_cachedElem != ElementType::None)
            ApplyElementToEffectDef(def, m_cachedElem);
        s.vfxId = m_pVFXManager->SpawnEffectDef(s.pos, up, def, true);
    }

    HitEnemiesAtSpike(s.pos, m_SkillData.damage * m_damageMult);
}

void StoneSpikesBehavior::HitEnemiesAtSpike(const DirectX::XMFLOAT3& center, float damage)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    // stats를 루프 밖에서 한 번만 계산
    SkillStats sts;
    bool hasStats = false;
    if (m_pCaster) {
        auto* pSC = m_pCaster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            hasStats = true;
        }
    }
    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        auto* pT = obj->GetTransform();
        if (!pT) continue;

        XMFLOAT3 ep = pT->GetPosition();
        float dx = ep.x - center.x, dz = ep.z - center.z;
        float distSq = dx * dx + dz * dz;
        if (distSq > HIT_RADIUS * HIT_RADIUS) continue;
        if (fabsf(ep.y - center.y) > HIT_HEIGHT) continue;

        float dmg = damage;
        // 처형자: HP 30% 이하 시 추가 피해
        if (hasStats && sts.execDamageBonus > 0.f && pEnemy->GetHpRatio() < 0.3f)
            dmg *= (1.f + sts.execDamageBonus);

        bool bExec = hasStats && sts.execDamageBonus > 0.f;
        pEnemy->TakeDamage(dmg, true, bExec);

        // 처형자: 룬이 장착된 상태에서 적이 죽으면 skull 표시 (HP 조건 무관)

        if (hasStats && !sts.onHitHooks.empty()) {
            SkillContext ctx;
            ctx.caster             = m_pCaster;
            ctx.baseDamage         = damage;
            ctx.damageDealt        = dmg;
            ctx.hitEnemy           = pEnemy;
            ctx.hitEnemyPos        = ep;
            ctx.scene              = m_pScene;
            ctx.statusChanceMult   = sts.statusChanceMult;
            ctx.statusDurationMult = sts.statusDurationMult;
            for (auto& hook : sts.onHitHooks) hook(ctx);
        }
    }
}

bool StoneSpikesBehavior::IsFinished() const { return !m_bActive; }

void StoneSpikesBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_chargeVFXId      >= 0) m_pVFXManager->StopEffect(m_chargeVFXId);
        if (m_enhanceAuraId    >= 0) m_pVFXManager->StopEffect(m_enhanceAuraId);
        if (m_channelAmbientId >= 0) m_pVFXManager->StopEffect(m_channelAmbientId);
        for (auto& s : m_spikes)
            if (s.triggered && s.vfxId >= 0) m_pVFXManager->StopEffect(s.vfxId);
    }
    m_bActive          = false;
    m_bChannelMode     = false;
    m_chargeVFXId      = -1;
    m_enhanceAuraId    = -1;
    m_channelAmbientId = -1;
    m_spikes.clear();
}
