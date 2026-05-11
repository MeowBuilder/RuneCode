#include "stdafx.h"
#include "GaleRushBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"

GaleRushBehavior::GaleRushBehavior()
    : m_SkillData(WindSkillPresets::GaleRush())
{
}

void GaleRushBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bActive = false;
    m_bHit    = false;

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

    // 폭풍 콘 VFX — 시전자 가슴 높이에서 전방으로
    EffectDef coneDef = EffectRegistry::Get().GetEffect("E_GaleRush_Cone");
    m_vfxId = m_pVFXManager->SpawnEffectDef(m_origin, m_direction, coneDef, true);

    // 충격파 링 VFX — 지면(y=0) 기준으로 수평 확장
    XMFLOAT3 ringOrigin = m_origin;
    ringOrigin.y = 0.f;
    EffectDef ringDef = EffectRegistry::Get().GetEffect("E_GaleRush_Ring");
    m_ringVfxId = m_pVFXManager->SpawnEffectDef(ringOrigin, m_direction, ringDef, true);

    if (m_vfxId >= 0)
    {
        m_bActive    = true;
        m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
        m_elapsed    = 0.f;
        HitEnemiesInCone(m_SkillData.damage * m_damageMult);  // 즉발 판정
        m_bHit = true;
    }
}

void GaleRushBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;
    m_elapsed += deltaTime;
    if (m_elapsed >= DURATION) m_bActive = false;
}

void GaleRushBehavior::HitEnemiesInCone(float damage)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMVECTOR oV = XMLoadFloat3(&m_origin);
    XMVECTOR dV = XMVector3Normalize(XMLoadFloat3(&m_direction));
    float cosHalf = cosf(XMConvertToRadians(CONE_HALF_ANGLE));

    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;

        auto* pT = obj->GetTransform();
        if (!pT) continue;

        XMFLOAT3 ePos = pT->GetPosition();
        XMVECTOR toE  = XMVectorSubtract(XMLoadFloat3(&ePos), oV);
        XMVECTOR toE_flat = XMVectorSetY(toE, 0.f);

        float dist = XMVectorGetX(XMVector3Length(toE_flat));
        if (dist > MAX_RANGE) continue;
        if (dist < 0.01f) { pEnemy->TakeDamage(damage, true); continue; }

        XMVECTOR toE_norm = XMVectorScale(toE_flat, 1.f / dist);
        float dot = XMVectorGetX(XMVector3Dot(toE_norm, dV));
        if (dot < cosHalf) continue;

        pEnemy->TakeDamage(damage, true);
    }
}

bool GaleRushBehavior::IsFinished() const { return !m_bActive; }

void GaleRushBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_vfxId     >= 0) m_pVFXManager->StopEffect(m_vfxId);
        if (m_ringVfxId >= 0) m_pVFXManager->StopEffect(m_ringVfxId);
    }
    m_bActive   = false;
    m_vfxId     = -1;
    m_ringVfxId = -1;
    m_bHit      = false;
}
