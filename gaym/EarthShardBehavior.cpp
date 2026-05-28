#include "stdafx.h"
#include "EarthShardBehavior.h"
#include "ProjectileManager.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "PlayerComponent.h"

EarthShardBehavior::EarthShardBehavior()
    : m_SkillData(EarthSkillPresets::EarthShard())
{
}

void EarthShardBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_bChannelMode = true;
}

void EarthShardBehavior::OnChannelEnd(GameObject* caster)
{
    m_bChannelMode = false;
    if (m_pVFXManager && m_channelAmbientId >= 0)
    {
        m_pVFXManager->StopEffect(m_channelAmbientId);
        m_channelAmbientId = -1;
    }
}

void EarthShardBehavior::Reset()
{
    if (m_pVFXManager && m_channelAmbientId >= 0)
    {
        m_pVFXManager->StopEffect(m_channelAmbientId);
        m_channelAmbientId = -1;
    }
    m_bChannelMode = false;
    m_bIsFinished  = true;
}

void EarthShardBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
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
        SHARD_SPEED,
        0.7f,
        m_SkillData.radius * stats.radiusMult,
        m_SkillData.element,
        caster, true, SHARD_SCALE,
        combo, 0.f,
        m_SkillData.range * stats.rangeMult,
        stats.piercing, stats.homing,
        stats.lifestealRatio, stats.execDamageBonus, stats.cdResetChance,
        m_slot, stats.elementSet, stats.subVFXIds
    );
    m_bIsFinished = true;
}
