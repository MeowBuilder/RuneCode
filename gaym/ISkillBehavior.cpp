#include "stdafx.h"
#include "ISkillBehavior.h"
#include "GameObject.h"
#include "SkillComponent.h"
#include "EnemyComponent.h"

bool ISkillBehavior::HasExecRune(GameObject* caster) const
{
    if (!caster || m_slot == SkillSlot::Count) return false;
    auto* pSC = caster->GetComponent<SkillComponent>();
    return pSC && pSC->HasRuneEquipped(m_slot, "ABY_EXC");
}

float ISkillBehavior::ApplyExecBonus(float dmg, EnemyComponent* pEnemy, GameObject* caster) const
{
    if (!pEnemy || !caster || m_slot == SkillSlot::Count) return dmg;
    auto* pSC = caster->GetComponent<SkillComponent>();
    if (!pSC || !pSC->HasRuneEquipped(m_slot, "ABY_EXC")) return dmg;
    if (pEnemy->GetHpRatio() >= 0.3f) return dmg;
    SkillStats sts = pSC->BuildSkillStats(m_slot, ActivationType::Instant);
    return dmg * (1.f + sts.execDamageBonus);
}

void ISkillBehavior::NotifyHit(GameObject* caster, const DirectX::XMFLOAT3& hitPos) const
{
    if (!caster || m_slot == SkillSlot::Count) return;
    auto* pSC = caster->GetComponent<SkillComponent>();
    if (pSC) pSC->TryTriggerInfiniteRune(m_slot, hitPos);
}
