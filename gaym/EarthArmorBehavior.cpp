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

void EarthArmorBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
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
    if (m_pVFXManager && m_auraVfxId >= 0)
    {
        m_pVFXManager->StopEffect(m_auraVfxId);
        m_auraVfxId = -1;
    }
    m_bIsFinished = true;
    m_elapsed     = 0.f;
    m_pCaster     = nullptr;
}
