#include "stdafx.h"
#include "WindCutterBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include <algorithm>

WindCutterBehavior::WindCutterBehavior()
    : m_SkillData(WindSkillPresets::WindCutter())
{
}

uint32_t WindCutterBehavior::GetRuneFlags(GameObject* caster) const
{
    if (!caster) return 0;
    auto* pSC = caster->GetComponent<SkillComponent>();
    if (!pSC || m_slot == SkillSlot::Count) return 0;
    RuneCombo c = pSC->GetRuneCombo(m_slot);
    uint32_t f = 0;
    if (c.hasInstant) f |= RUNE_INSTANT;
    if (c.hasCharge)  f |= RUNE_CHARGE;
    if (c.hasEnhance) f |= RUNE_ENHANCE;
    return f;
}

void WindCutterBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bActive = false;
    m_hitEnemies.clear();

    if (!m_pVFXManager) { return; }

    m_origin    = { 0.f, 0.f, 0.f };
    m_direction = { 0.f, 0.f, 1.f };

    if (caster && caster->GetTransform())
    {
        m_origin = caster->GetTransform()->GetPosition();
        m_origin.y += 5.0f;

        XMVECTOR dV = XMVector3Normalize(XMVectorSetY(
            XMVectorSubtract(XMLoadFloat3(&targetPosition), XMLoadFloat3(&m_origin)), 0.f));
        if (XMVectorGetX(XMVector3LengthSq(dV)) < 0.001f)
            dV = XMVector3Normalize(XMVectorSetY(caster->GetTransform()->GetLook(), 0.f));
        XMStoreFloat3(&m_direction, dV);
    }

    uint32_t runeFlags = GetRuneFlags(caster);
    EffectDef def = EffectRegistry::Get().GetEffect("Q_WindCutter", runeFlags);
    m_vfxId = m_pVFXManager->SpawnEffectDef(m_origin, m_direction, def, true);

    if (m_vfxId >= 0)
    {
        m_bActive    = true;
        m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
        m_elapsed    = 0.f;
    }
}

void WindCutterBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;
    m_elapsed += deltaTime;
    HitEnemiesInCutter(m_SkillData.damage * m_damageMult);
    if (m_elapsed >= DURATION) m_bActive = false;
}

void WindCutterBehavior::HitEnemiesInCutter(float damage)
{
    if (!m_pScene || m_vfxId < 0) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMVECTOR oV = XMLoadFloat3(&m_origin);
    XMVECTOR dV = XMVector3Normalize(XMLoadFloat3(&m_direction));

    // 커터는 짧은 시간 내에 전체 경로를 즉시 체크
    float front = m_elapsed * SPEED;
    float back  = (std::max)(0.f, front - HIT_DEPTH);

    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        if (m_hitEnemies.count(pEnemy)) continue;

        auto* pT = obj->GetTransform();
        if (!pT) continue;

        XMFLOAT3 ePos   = pT->GetPosition();
        XMFLOAT3 eScale = pT->GetScale();
        float eR = (std::max)(0.f, (std::max)(eScale.x, eScale.z) * 0.9f);

        XMVECTOR toE = XMVectorSubtract(XMLoadFloat3(&ePos), oV);
        float fwd = XMVectorGetX(XMVector3Dot(toE, dV));
        if (fwd < back - eR || fwd > front + eR) continue;

        XMVECTOR latV = XMVectorSetY(XMVectorSubtract(toE, XMVectorScale(dV, fwd)), 0.f);
        if (XMVectorGetX(XMVector3Length(latV)) > HALF_W + eR) continue;

        float yTol = (std::max)(0.f, eScale.y * 0.6f);
        if (fabsf(ePos.y - m_origin.y) > HALF_H + yTol) continue;

        // 관통 — 이미 히트한 적도 매 프레임 체크하지 않으므로 set에 넣어 중복 방지
        pEnemy->TakeDamage(damage, false);
        m_hitEnemies.insert(pEnemy);
    }
}

bool WindCutterBehavior::IsFinished() const { return !m_bActive; }

void WindCutterBehavior::Reset()
{
    if (m_pVFXManager && m_vfxId >= 0)
        m_pVFXManager->StopEffect(m_vfxId);
    m_bActive = false;
    m_vfxId   = -1;
    m_hitEnemies.clear();
}
