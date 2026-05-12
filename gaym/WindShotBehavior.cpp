#include "stdafx.h"
#include "WindShotBehavior.h"
#include "ProjectileManager.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "PlayerComponent.h"

WindShotBehavior::WindShotBehavior()
    : m_SkillData(WindSkillPresets::WindShot())
{
}

void WindShotBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bIsFinished = true;
    if (!m_pProjectileManager) return;

    XMFLOAT3 startPos = {};
    if (caster && caster->GetTransform())
    {
        startPos = caster->GetTransform()->GetPosition();
        startPos.y += 5.0f;
    }

    if (caster)
        if (auto* pPC = caster->GetComponent<PlayerComponent>())
            m_SkillData.element = pPC->GetElementType();

    RuneCombo combo;
    SkillStats stats;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            combo = pSC->GetRuneCombo(m_slot);
            stats = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
        }
    }

    XMFLOAT3 flatTarget = targetPosition;
    flatTarget.y = startPos.y;

    m_pProjectileManager->SpawnProjectile(
        startPos, flatTarget,
        m_SkillData.damage * damageMultiplier,
        SHOT_SPEED,
        0.35f,
        m_SkillData.radius * stats.radiusMult,
        m_SkillData.element,
        caster, true, SHOT_SCALE,
        combo, 0.f,
        m_SkillData.range * stats.rangeMult,
        true,           // 바람탄은 기본 관통
        stats.homing,
        stats.lifestealRatio, stats.execDamageBonus, stats.cdResetChance,
        m_slot, stats.elementSet, stats.subVFXIds
    );
    m_bIsFinished = true;
}
