#include "stdafx.h"
#include "GaleRushBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "PlayerComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"

GaleRushBehavior::GaleRushBehavior()
    : m_SkillData(WindSkillPresets::GaleRush())
{
}

void GaleRushBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bActive  = false;
    m_pCaster  = nullptr;
    m_hitEnemies.clear();
    m_trailVfxIds.clear();
    m_trailTimer = TRAIL_INTERVAL;  // 첫 프레임 즉시 분사

    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;

    XMFLOAT3 casterPos = caster->GetTransform()->GetPosition();

    // 방향 계산
    m_direction = { 0.f, 0.f, 1.f };
    {
        XMFLOAT3 flatOrigin = casterPos;
        XMVECTOR dV = XMVector3Normalize(XMVectorSetY(
            XMVectorSubtract(XMLoadFloat3(&targetPosition), XMLoadFloat3(&flatOrigin)), 0.f));
        if (XMVectorGetX(XMVector3LengthSq(dV)) < 0.001f)
            dV = XMVector3Normalize(XMVectorSetY(caster->GetTransform()->GetLook(), 0.f));
        XMStoreFloat3(&m_direction, dV);
    }

    // 출발 구체 폭발 — 몸통 높이(y+2)에서 사방으로 터짐
    {
        XMFLOAT3 burstPos = casterPos;
        burstPos.y += 2.0f;
        EffectDef burstDef = EffectRegistry::Get().GetEffect("E_GaleRush_Burst");
        m_vfxId = m_pVFXManager->SpawnEffectDef(burstPos, m_direction, burstDef, true);
    }

    // 출발 지면 충격파 링
    {
        XMFLOAT3 ringPos = casterPos;
        ringPos.y = 0.f;
        EffectDef ringDef = EffectRegistry::Get().GetEffect("E_GaleRush_Ring");
        m_ringVfxId = m_pVFXManager->SpawnEffectDef(ringPos, m_direction, ringDef, true);
    }

    if (m_vfxId >= 0 || m_ringVfxId >= 0)
    {
        m_pCaster    = caster;
        m_bActive    = true;
        m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
        m_elapsed    = 0.f;
        m_origin     = casterPos;

        // 플레이어를 전방으로 돌진시킴
        auto* pPC = caster->GetComponent<PlayerComponent>();
        if (pPC) pPC->StartSkillDash(m_direction, DASH_SPEED, DURATION);
    }
}

void GaleRushBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;
    m_elapsed += deltaTime;

    // 대쉬 이동 중 주변 적 타격
    HitEnemiesNearCaster(m_SkillData.damage * m_damageMult);

    // 후방 배기 분사 — 플레이어 현재 위치에서 진행 방향 반대로 분출
    m_trailTimer += deltaTime;
    if (m_trailTimer >= TRAIL_INTERVAL && m_pCaster && m_pVFXManager)
    {
        m_trailTimer = 0.f;

        auto* pT = m_pCaster->GetTransform();
        if (pT)
        {
            XMFLOAT3 pos = pT->GetPosition();
            pos.y += 2.0f;  // 허리 높이에서 분사

            XMFLOAT3 backDir = { -m_direction.x, 0.f, -m_direction.z };

            EffectDef trailDef = EffectRegistry::Get().GetEffect("E_GaleRush_Trail");
            int id = m_pVFXManager->SpawnEffectDef(pos, backDir, trailDef, true);
            if (id >= 0) m_trailVfxIds.push_back(id);
        }
    }

    if (m_elapsed >= DURATION) m_bActive = false;
}

void GaleRushBehavior::HitEnemiesNearCaster(float damage)
{
    if (!m_pScene || !m_pCaster) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    auto* pCasterTransform = m_pCaster->GetTransform();
    if (!pCasterTransform) return;

    XMFLOAT3 casterPos = pCasterTransform->GetPosition();
    XMVECTOR cPosV = XMVectorSetY(XMLoadFloat3(&casterPos), 0.f);

    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        if (m_hitEnemies.count(pEnemy)) continue;

        auto* pT = obj->GetTransform();
        if (!pT) continue;

        XMFLOAT3 ePos = pT->GetPosition();
        XMVECTOR toE = XMVectorSubtract(XMVectorSetY(XMLoadFloat3(&ePos), 0.f), cPosV);
        float dist = XMVectorGetX(XMVector3Length(toE));
        if (dist > HIT_RADIUS) continue;

        pEnemy->TakeDamage(damage, true);
        m_hitEnemies.insert(pEnemy);
    }
}

bool GaleRushBehavior::IsFinished() const { return !m_bActive; }

void GaleRushBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_vfxId     >= 0) m_pVFXManager->StopEffect(m_vfxId);
        if (m_ringVfxId >= 0) m_pVFXManager->StopEffect(m_ringVfxId);
        for (int id : m_trailVfxIds)
            if (id >= 0) m_pVFXManager->StopEffect(id);
    }
    m_bActive   = false;
    m_vfxId     = -1;
    m_ringVfxId = -1;
    m_pCaster   = nullptr;
    m_hitEnemies.clear();
    m_trailVfxIds.clear();
    m_trailTimer = 0.f;
}
