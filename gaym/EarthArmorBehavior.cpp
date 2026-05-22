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

void EarthArmorBehavior::OnChargeBegin(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_chargeVFXId = m_pVFXManager->SpawnEffectDef(pos, up, EffectRegistry::Get().GetEffect(fx), true);
}

void EarthArmorBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void EarthArmorBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }
    m_bIsFinished = false;
    m_elapsed     = 0.f;
    m_pCaster     = caster;

    auto* pPC = caster ? caster->GetComponent<PlayerComponent>() : nullptr;
    if (!pPC) { m_bIsFinished = true; return; }

    pPC->AddShield(SHIELD_AMOUNT);
    pPC->SetDamageReduction(DR_RATIO, DR_DURATION);

    // 변환 룬 원소 확인
    ElementType cachedElem = ElementType::None;
    {
        auto* pSC = caster ? caster->GetComponent<SkillComponent>() : nullptr;
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty())
                cachedElem = sts.elementSet[0];
        }
    }

    if (m_pVFXManager && caster && caster->GetTransform())
    {
        XMFLOAT3 pos = caster->GetTransform()->GetPosition();
        XMFLOAT3 up  = { 0.f, 1.f, 0.f };

        // 링 폭발 (일회성 — duration으로 자동 소멸)
        if (EffectRegistry::Get().HasEffect("E_EarthArmor_Burst"))
        {
            EffectDef burstDef = EffectRegistry::Get().GetEffect("E_EarthArmor_Burst");
            if (cachedElem != ElementType::None)
                ApplyElementToEffectDef(burstDef, cachedElem);
            m_pVFXManager->SpawnEffectDef(pos, up, burstDef, true);
        }

        // 먼지 오라 (지속 — DR 만료 시 Update에서 중단)
        if (EffectRegistry::Get().HasEffect("E_EarthArmor_Aura"))
        {
            EffectDef auraDef = EffectRegistry::Get().GetEffect("E_EarthArmor_Aura");
            if (cachedElem != ElementType::None)
                ApplyElementToEffectDef(auraDef, cachedElem);
            m_auraVfxId = m_pVFXManager->SpawnEffectDef(pos, up, auraDef, true);
        }
    }
}

void EarthArmorBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float tickMult)
{
    // 채널 중 캐스터 주위에 암석 충격파 + 방어막 소량 회복 — 일반 스킬은 한 번만 발동
    if (!caster || !caster->GetTransform()) return;

    XMFLOAT3 pos = caster->GetTransform()->GetPosition();

    auto* pPC = caster->GetComponent<PlayerComponent>();
    if (pPC)
    {
        pPC->AddShield(CHANNEL_SHIELD_REGEN);
    }

    if (m_pVFXManager && EffectRegistry::Get().HasEffect("E_EarthArmor_Burst"))
    {
        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        EffectDef def = EffectRegistry::Get().GetEffect("E_EarthArmor_Burst");
        for (auto& l : def.layers) l.particleCount = (std::max)(l.particleCount / 2, 4);
        m_pVFXManager->SpawnEffectDef(pos, up, def, false);
    }

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
            pEnemy->TakeDamage(damage, false);
    }
}

void EarthArmorBehavior::OnEnhanceActivate(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    // 갑옷 강화 버프 대기 중: 황금빛 오라로 강화 준비 상태 표시
    if (EffectRegistry::Get().HasEffect("E_EarthArmor_Aura"))
    {
        XMFLOAT3 pos = caster->GetTransform()->GetPosition();
        XMFLOAT3 up  = { 0.f, 1.f, 0.f };
        EffectDef def = EffectRegistry::Get().GetEffect("E_EarthArmor_Aura");
        // 강화 대기 오라는 작게 펄스
        for (auto& l : def.layers) l.particleCount = (std::max)(l.particleCount / 2, 4);
        m_enhanceAuraId = m_pVFXManager->SpawnEffectDef(pos, up, def, true);
    }
}

void EarthArmorBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    // 강화 대기 오라 해제
    if (m_pVFXManager && m_enhanceAuraId >= 0)
    {
        m_pVFXManager->StopEffect(m_enhanceAuraId);
        m_enhanceAuraId = -1;
    }
    // 강화 소모 시 타겟 위치에 충격 버스트
    if (m_pVFXManager && EffectRegistry::Get().HasEffect("E_EarthArmor_Burst"))
    {
        XMFLOAT3 pos = { targetPosition.x, targetPosition.y, targetPosition.z };
        XMFLOAT3 up  = { 0.f, 1.f, 0.f };
        m_pVFXManager->SpawnEffectDef(pos, up,
            EffectRegistry::Get().GetEffect("E_EarthArmor_Burst"), true);
    }
}

void EarthArmorBehavior::Update(float deltaTime)
{
    if (m_bIsFinished) return;
    m_elapsed += deltaTime;

    // 아직 지속 중이면 오라를 플레이어 현재 위치로 추적
    if (m_pVFXManager && m_auraVfxId >= 0 && m_pCaster && m_pCaster->GetTransform())
    {
        XMFLOAT3 pos = m_pCaster->GetTransform()->GetPosition();
        XMFLOAT3 up  = { 0.f, 1.f, 0.f };
        m_pVFXManager->TrackEffect(m_auraVfxId, pos, up);
    }

    if (m_elapsed >= DR_DURATION)
    {
        if (m_pVFXManager && m_auraVfxId >= 0)
        {
            m_pVFXManager->StopEffect(m_auraVfxId);
            m_auraVfxId = -1;
        }
        m_bIsFinished = true;
    }
}

void EarthArmorBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_auraVfxId     >= 0) { m_pVFXManager->StopEffect(m_auraVfxId);     m_auraVfxId     = -1; }
        if (m_enhanceAuraId >= 0) { m_pVFXManager->StopEffect(m_enhanceAuraId); m_enhanceAuraId = -1; }
        if (m_chargeVFXId   >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId);   m_chargeVFXId   = -1; }
    }
    m_bIsFinished = true;
    m_elapsed     = 0.f;
    m_pCaster     = nullptr;
}
