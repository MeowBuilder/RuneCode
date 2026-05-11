#include "stdafx.h"
#include "EarthquakeBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include <algorithm>

EarthquakeBehavior::EarthquakeBehavior()
    : m_SkillData(EarthSkillPresets::Earthquake())
{
}

void EarthquakeBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bActive = false;
    m_rings.clear();
    m_hitEnemies.clear();

    if (!m_pVFXManager) { return; }

    m_epicenter = { 0.f, 0.f, 0.f };
    if (caster && caster->GetTransform())
        m_epicenter = caster->GetTransform()->GetPosition();
    m_epicenter.y = 0.f;

    // 초기 버스트 VFX (중심 폭발)
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    EffectDef burstDef = EffectRegistry::Get().GetEffect("R_Earthquake_Burst");
    m_burstVfxId = m_pVFXManager->SpawnEffectDef(m_epicenter, up, burstDef, true);

    // 충격파 링 스케줄
    m_rings.resize(RING_COUNT);
    for (int i = 0; i < RING_COUNT; ++i)
    {
        m_rings[i].launchAt  = i * RING_INTERVAL;
        m_rings[i].radius    = 0.f;
        m_rings[i].launched  = false;
        m_rings[i].hit       = false;
        m_rings[i].vfxId     = -1;
    }

    m_bActive    = true;
    m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
    m_elapsed    = 0.f;
}

void EarthquakeBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;
    m_elapsed += deltaTime;

    for (int i = 0; i < (int)m_rings.size(); ++i)
        if (!m_rings[i].launched && m_elapsed >= m_rings[i].launchAt)
            LaunchWave(i);

    UpdateWaves(deltaTime);

    if (m_elapsed >= TOTAL_DURATION)
        m_bActive = false;
}

void EarthquakeBehavior::LaunchWave(int idx)
{
    auto& r = m_rings[idx];
    r.launched = true;
    r.radius   = 0.f;

    if (m_pVFXManager)
    {
        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        EffectDef def = EffectRegistry::Get().GetEffect("R_Earthquake_Ring");
        r.vfxId = m_pVFXManager->SpawnEffectDef(m_epicenter, up, def, true);
    }
}

void EarthquakeBehavior::UpdateWaves(float dt)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    for (auto& r : m_rings)
    {
        if (!r.launched) continue;
        if (r.radius >= RING_MAX_RADIUS) continue;

        float prevRadius = r.radius;
        r.radius += RING_SPEED * dt;
        if (r.radius > RING_MAX_RADIUS) r.radius = RING_MAX_RADIUS;

        // 링 영역 안에 있는 적 타격 (이미 hit한 적은 건너뜀)
        float dmg = (m_SkillData.damage / RING_COUNT) * m_damageMult;

        for (const auto& obj : pRoom->GetGameObjects())
        {
            if (!obj) continue;
            auto* pEnemy = obj->GetComponent<EnemyComponent>();
            if (!pEnemy || pEnemy->IsDead()) continue;
            if (m_hitEnemies.count(pEnemy)) continue;

            auto* pT = obj->GetTransform();
            if (!pT) continue;

            XMFLOAT3 ep = pT->GetPosition();
            float dx = ep.x - m_epicenter.x, dz = ep.z - m_epicenter.z;
            float dist = sqrtf(dx * dx + dz * dz);

            // 링 두께 범위 안에 있을 때만 타격
            float inner = (std::max)(0.f, r.radius - RING_THICKNESS);
            if (dist < inner || dist > r.radius + RING_THICKNESS) continue;

            float yTol = (std::max)(0.f, pT->GetScale().y * 0.6f);
            if (fabsf(ep.y - m_epicenter.y) > RING_HEIGHT + yTol) continue;

            pEnemy->TakeDamage(dmg, true);
            m_hitEnemies.insert(pEnemy);
        }
    }
}

bool EarthquakeBehavior::IsFinished() const { return !m_bActive; }

void EarthquakeBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_burstVfxId >= 0) m_pVFXManager->StopEffect(m_burstVfxId);
        for (auto& r : m_rings)
            if (r.launched && r.vfxId >= 0) m_pVFXManager->StopEffect(r.vfxId);
    }
    m_bActive    = false;
    m_burstVfxId = -1;
    m_rings.clear();
    m_hitEnemies.clear();
}
