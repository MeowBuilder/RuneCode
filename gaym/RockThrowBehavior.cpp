#include "stdafx.h"
#include "RockThrowBehavior.h"
#include "ProjectileManager.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "PlayerComponent.h"

RockThrowBehavior::RockThrowBehavior()
    : m_SkillData(EarthSkillPresets::RockThrow())
{
}

void RockThrowBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bIsFinished = true;
    if (!m_pProjectileManager) return;

    XMFLOAT3 startPos = {};
    if (caster && caster->GetTransform())
    {
        startPos = caster->GetTransform()->GetPosition();
        startPos.y += 5.0f;
    }

    if (caster) {
        if (auto* pPC = caster->GetComponent<PlayerComponent>())
            m_SkillData.element = pPC->GetElementType();
    }

    RuneCombo combo;
    SkillStats stats;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            combo = pSC->GetRuneCombo(m_slot);
            stats = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
        }
    }

    float finalDamage = m_SkillData.damage * damageMultiplier;
    float explosionR  = m_SkillData.radius * stats.radiusMult;
    float maxDist     = m_SkillData.range  * stats.rangeMult;

    XMFLOAT3 flatTarget = targetPosition;
    flatTarget.y = startPos.y;

    m_pProjectileManager->SpawnProjectile(
        startPos, flatTarget,
        finalDamage, ROCK_SPEED,
        0.8f,           // 충돌 반경 (크게)
        explosionR,
        m_SkillData.element,
        caster,
        true,
        ROCK_SCALE,
        combo,
        0.f,            // chargeRatio
        maxDist,
        stats.piercing,
        stats.homing,
        stats.lifestealRatio,
        stats.execDamageBonus,
        stats.cdResetChance,
        m_slot,
        stats.elementSet,
        stats.subVFXIds
    );

    m_bIsFinished = true;
}
