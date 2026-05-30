#include "stdafx.h"
#include "EarthArmorBehavior.h"
#include "EffectRegistry.h"
#include "FluidSkillVFXManager.h"
#include "Scene.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"
#include "SkillComponent.h"

EarthArmorBehavior::EarthArmorBehavior()
    : m_SkillData(EarthSkillPresets::EarthArmor())
{
}

// ─── 채널링 흐름 ──────────────────────────────────────────────────────────────
// OnChannelBegin → Execute(채널 시작, 즉시 리턴) → OnChannelTick×N → OnChannelEnd
// → Execute(포스트채널 버스트, HasPostChannelWork=true) → IsFinished=true

void EarthArmorBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_bChannelMode = true;
    m_bPostChannel = false;
    m_cachedElem   = ElementType::None;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty()) m_cachedElem = sts.elementSet[0];
        }
    }

    // 채널 시작 즉시: 무적 + 오라 VFX + 황금 외곽선
    auto* pPC = caster ? caster->GetComponent<PlayerComponent>() : nullptr;
    if (pPC) pPC->SetInvincible(CHANNEL_INVINCIBLE_BUFFER);

    if (m_pVFXManager && caster && caster->GetTransform())
    {
        XMFLOAT3 pos = caster->GetTransform()->GetPosition();
        XMFLOAT3 up  = { 0.f, 1.f, 0.f };

        if (EffectRegistry::Get().HasEffect("E_EarthArmor_Aura"))
        {
            EffectDef def = EffectRegistry::Get().GetEffect("E_EarthArmor_Aura");
            if (m_cachedElem != ElementType::None) ApplyElementToEffectDef(def, m_cachedElem);
            m_auraVfxId = m_pVFXManager->SpawnEffectDef(pos, up, def, true);
        }
        if (EffectRegistry::Get().HasEffect("E_EarthArmor_Shield"))
        {
            EffectDef def = EffectRegistry::Get().GetEffect("E_EarthArmor_Shield");
            if (m_cachedElem != ElementType::None) ApplyElementToEffectDef(def, m_cachedElem);
            m_shieldVfxId = m_pVFXManager->SpawnEffectDef(pos, up, def, true);
        }
    }
    if (caster) caster->SetStatusColorAll({ 0.85f, 0.65f, 0.15f, 1.f }, 0.55f);
}

void EarthArmorBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float tickMult)
{
    if (!caster || !caster->GetTransform()) return;

    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };

    // 무적 갱신 (0.2s 틱 간격 커버)
    auto* pPC = caster->GetComponent<PlayerComponent>();
    if (pPC) pPC->SetInvincible(CHANNEL_INVINCIBLE_BUFFER);

    // VFX 위치 추적
    if (m_pVFXManager && m_auraVfxId  >= 0) m_pVFXManager->TrackEffect(m_auraVfxId,  pos, up);
    if (m_pVFXManager && m_shieldVfxId >= 0) m_pVFXManager->TrackEffect(m_shieldVfxId, pos, up);

    // 주변 충격파 버스트 VFX
    if (m_pVFXManager && EffectRegistry::Get().HasEffect("E_EarthArmor_Burst"))
    {
        EffectDef def = EffectRegistry::Get().GetEffect("E_EarthArmor_Burst");
        if (m_cachedElem != ElementType::None) ApplyElementToEffectDef(def, m_cachedElem);
        for (auto& l : def.layers) l.particleCount = (std::max)(l.particleCount / 2, 4);
        m_pVFXManager->SpawnEffectDef(pos, up, def, true);
    }

    // 반경 내 적 AoE 데미지
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    float damage = 10.f * tickMult;
    float r2 = CHANNEL_BURST_RADIUS * CHANNEL_BURST_RADIUS;
    XMVECTOR pV = XMLoadFloat3(&pos);
    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        auto* pT = obj->GetTransform();
        if (!pT) continue;
        XMFLOAT3 ep = pT->GetPosition();
        XMVECTOR toE = XMVectorSetY(XMVectorSubtract(XMLoadFloat3(&ep), pV), 0.f);
        if (XMVectorGetX(XMVector3LengthSq(toE)) <= r2)
            pEnemy->TakeDamage(damage, false, HasExecRune(m_pCaster));
    }
}

void EarthArmorBehavior::OnChannelEnd(GameObject* caster)
{
    m_bChannelMode = false;
    m_bPostChannel = true;  // Execute()에서 마무리 버스트만 처리하도록

    // 무적 즉시 해제
    auto* pPC = caster ? caster->GetComponent<PlayerComponent>() : nullptr;
    if (pPC) pPC->ClearInvincible();

    // VFX + 외곽선 해제
    if (m_pVFXManager)
    {
        if (m_auraVfxId   >= 0) { m_pVFXManager->StopEffect(m_auraVfxId);   m_auraVfxId   = -1; }
        if (m_shieldVfxId >= 0) { m_pVFXManager->StopEffect(m_shieldVfxId); m_shieldVfxId = -1; }
        if (m_channelAmbientId >= 0) { m_pVFXManager->StopEffect(m_channelAmbientId); m_channelAmbientId = -1; }
    }
    if (caster) caster->SetStatusColorAll({ 0.f, 0.f, 0.f, 0.f }, 0.f);
}

// ─── 일반 발동 / 포스트채널 버스트 ───────────────────────────────────────────
void EarthArmorBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }

    // 채널 시작 시 Execute — OnChannelBegin이 이미 처리했으므로 아무것도 안 함
    if (m_bChannelMode)
        return;

    // 채널 종료 후 포스트채널 Execute — 마무리 버스트 VFX만 터뜨리고 즉시 종료
    if (m_bPostChannel)
    {
        m_bPostChannel = false;
        if (m_pVFXManager && caster && caster->GetTransform()
            && EffectRegistry::Get().HasEffect("E_EarthArmor_Burst"))
        {
            XMFLOAT3 pos = caster->GetTransform()->GetPosition();
            XMFLOAT3 up  = { 0.f, 1.f, 0.f };
            EffectDef def = EffectRegistry::Get().GetEffect("E_EarthArmor_Burst");
            if (m_cachedElem != ElementType::None) ApplyElementToEffectDef(def, m_cachedElem);
            m_pVFXManager->SpawnEffectDef(pos, up, def, true);
        }
        m_bIsFinished = true;
        return;
    }

    // ── 일반(비채널) 발동: 2.5초 무적 ──────────────────────────────────────
    m_bIsFinished = false;
    m_elapsed     = 0.f;
    m_pCaster     = caster;

    auto* pPC = caster ? caster->GetComponent<PlayerComponent>() : nullptr;
    if (!pPC) { m_bIsFinished = true; return; }

    pPC->SetInvincible(INVINCIBLE_DURATION);

    if (m_cachedElem == ElementType::None) {
        auto* pSC = caster ? caster->GetComponent<SkillComponent>() : nullptr;
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty()) m_cachedElem = sts.elementSet[0];
        }
    }
    ElementType cachedElem = m_cachedElem;

    if (m_pVFXManager && caster && caster->GetTransform())
    {
        XMFLOAT3 pos = caster->GetTransform()->GetPosition();
        XMFLOAT3 up  = { 0.f, 1.f, 0.f };

        if (EffectRegistry::Get().HasEffect("E_EarthArmor_Burst"))
        {
            EffectDef def = EffectRegistry::Get().GetEffect("E_EarthArmor_Burst");
            if (cachedElem != ElementType::None) ApplyElementToEffectDef(def, cachedElem);
            m_pVFXManager->SpawnEffectDef(pos, up, def, true);
        }
        if (EffectRegistry::Get().HasEffect("E_EarthArmor_Aura"))
        {
            EffectDef def = EffectRegistry::Get().GetEffect("E_EarthArmor_Aura");
            if (cachedElem != ElementType::None) ApplyElementToEffectDef(def, cachedElem);
            m_auraVfxId = m_pVFXManager->SpawnEffectDef(pos, up, def, true);
        }
        if (EffectRegistry::Get().HasEffect("E_EarthArmor_Shield"))
        {
            EffectDef def = EffectRegistry::Get().GetEffect("E_EarthArmor_Shield");
            if (cachedElem != ElementType::None) ApplyElementToEffectDef(def, cachedElem);
            m_shieldVfxId = m_pVFXManager->SpawnEffectDef(pos, up, def, true);
        }
    }
    if (caster) caster->SetStatusColorAll({ 0.85f, 0.65f, 0.15f, 1.f }, 0.55f);
}

// ─── 차지 ────────────────────────────────────────────────────────────────────
void EarthArmorBehavior::OnChargeBegin(GameObject* caster)
{
}

void EarthArmorBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

// ─── 강화 룬 ─────────────────────────────────────────────────────────────────
void EarthArmorBehavior::OnEnhanceActivate(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    if (EffectRegistry::Get().HasEffect("E_EarthArmor_Aura"))
    {
        XMFLOAT3 pos = caster->GetTransform()->GetPosition();
        XMFLOAT3 up  = { 0.f, 1.f, 0.f };
        EffectDef def = EffectRegistry::Get().GetEffect("E_EarthArmor_Aura");
        for (auto& l : def.layers) l.particleCount = (std::max)(l.particleCount / 2, 4);
        m_enhanceAuraId = m_pVFXManager->SpawnEffectDef(pos, up, def, true);
    }
}

void EarthArmorBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    if (m_pVFXManager && m_enhanceAuraId >= 0)
    {
        m_pVFXManager->StopEffect(m_enhanceAuraId);
        m_enhanceAuraId = -1;
    }
    if (m_pVFXManager && EffectRegistry::Get().HasEffect("E_EarthArmor_Burst"))
    {
        XMFLOAT3 pos = { targetPosition.x, targetPosition.y, targetPosition.z };
        XMFLOAT3 up  = { 0.f, 1.f, 0.f };
        m_pVFXManager->SpawnEffectDef(pos, up,
            EffectRegistry::Get().GetEffect("E_EarthArmor_Burst"), true);
    }
}

// ─── Update (일반 발동 전용 — 채널 모드는 OnChannelTick이 처리) ──────────────
void EarthArmorBehavior::Update(float deltaTime)
{
    if (m_bIsFinished) return;
    m_elapsed += deltaTime;

    if (m_pCaster && m_pCaster->GetTransform())
    {
        XMFLOAT3 pos = m_pCaster->GetTransform()->GetPosition();
        XMFLOAT3 up  = { 0.f, 1.f, 0.f };

        if (m_pVFXManager && m_auraVfxId >= 0)
            m_pVFXManager->TrackEffect(m_auraVfxId, pos, up);

        if (m_pVFXManager && m_shieldVfxId >= 0)
        {
            auto* pPC = m_pCaster->GetComponent<PlayerComponent>();
            if (pPC && pPC->IsInvincible())
            {
                m_pVFXManager->TrackEffect(m_shieldVfxId, pos, up);
            }
            else
            {
                m_pVFXManager->StopEffect(m_shieldVfxId);
                m_shieldVfxId = -1;
                m_pCaster->SetStatusColorAll({ 0.f, 0.f, 0.f, 0.f }, 0.f);
            }
        }
    }

    if (m_elapsed >= INVINCIBLE_DURATION)
    {
        if (m_pVFXManager && m_auraVfxId >= 0) { m_pVFXManager->StopEffect(m_auraVfxId); m_auraVfxId = -1; }
        if (m_pVFXManager && m_shieldVfxId >= 0) { m_pVFXManager->StopEffect(m_shieldVfxId); m_shieldVfxId = -1; }
        if (m_pCaster) m_pCaster->SetStatusColorAll({ 0.f, 0.f, 0.f, 0.f }, 0.f);
        m_bIsFinished = true;
    }
}

void EarthArmorBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_auraVfxId        >= 0) { m_pVFXManager->StopEffect(m_auraVfxId);        m_auraVfxId        = -1; }
        if (m_shieldVfxId      >= 0) { m_pVFXManager->StopEffect(m_shieldVfxId);      m_shieldVfxId      = -1; }
        if (m_enhanceAuraId    >= 0) { m_pVFXManager->StopEffect(m_enhanceAuraId);    m_enhanceAuraId    = -1; }
        if (m_chargeVFXId      >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId);      m_chargeVFXId      = -1; }
        if (m_channelAmbientId >= 0) { m_pVFXManager->StopEffect(m_channelAmbientId); m_channelAmbientId = -1; }
    }
    if (m_pCaster) m_pCaster->SetStatusColorAll({ 0.f, 0.f, 0.f, 0.f }, 0.f);
    if (m_pCaster) { auto* pPC = m_pCaster->GetComponent<PlayerComponent>(); if (pPC) pPC->ClearInvincible(); }
    m_bIsFinished  = true;
    m_bChannelMode = false;
    m_bPostChannel = false;
    m_elapsed      = 0.f;
    m_cachedElem   = ElementType::None;
    m_shieldVfxId  = -1;
    m_pCaster      = nullptr;
}
