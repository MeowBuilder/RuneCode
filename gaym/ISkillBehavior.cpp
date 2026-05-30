#include "stdafx.h"
#include "ISkillBehavior.h"
#include "GameObject.h"
#include "SkillComponent.h"

bool ISkillBehavior::HasExecRune(GameObject* caster) const
{
    if (!caster || m_slot == SkillSlot::Count) return false;
    auto* pSC = caster->GetComponent<SkillComponent>();
    return pSC && pSC->HasRuneEquipped(m_slot, "ABY_EXC");
}
