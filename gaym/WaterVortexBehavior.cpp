#include "stdafx.h"
#include "WaterVortexBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"

WaterVortexBehavior::WaterVortexBehavior()
    : m_SkillData(WaterSkillPresets::WaterVortex())
{
}

void WaterVortexBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bActive = false;
    m_pCaster = caster;
    m_extraVFXIds.clear();

    if (!m_pVFXManager) { return; }

    XMFLOAT3 origin    = targetPosition;
    origin.y           = 3.0f;  // OrbitalCP 중심을 지면 위로 — y=0이면 위성 궤도가 지면에 파묻힘
    XMFLOAT3 direction = { 0.f, 1.f, 0.f };  // 수직 — 소용돌이는 Y축 기준

    SkillStats stats;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count)
            stats = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    EffectDef def = EffectRegistry::Get().GetEffect("E_WaterVortex");
    if (!stats.elementSet.empty())
        ApplyElementToEffectDef(def, stats.elementSet[0]);
    m_vfxId = m_pVFXManager->SpawnEffectDef(origin, direction, def, true);

    if (m_vfxId >= 0)
    {
        m_bActive    = true;
        m_center     = targetPosition;
        m_center.y   = 0.f;
        m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
        m_elapsed    = 0.f;
        m_tickTimer  = 0.f;
    }
}

void WaterVortexBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;

    m_elapsed += deltaTime;
    PullAndDamageEnemies(deltaTime);

    if (m_elapsed >= DURATION)
    {
        if (m_pVFXManager && m_vfxId >= 0)
            m_pVFXManager->StopEffect(m_vfxId);
        m_bActive = false;
        m_vfxId   = -1;
    }
}

void WaterVortexBehavior::PullAndDamageEnemies(float deltaTime)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    m_tickTimer += deltaTime;
    bool bDoTick = (m_tickTimer >= TICK_INTERVAL);
    if (bDoTick) m_tickTimer = 0.f;

    float dotDmg = m_SkillData.damage * m_damageMult * DMG_PER_TICK;
    XMVECTOR centerV = XMVectorSet(m_center.x, 0.f, m_center.z, 0.f);

    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        auto* pT = obj->GetTransform();
        if (!pT) continue;

        XMFLOAT3 ep = pT->GetPosition();
        float dx = ep.x - m_center.x, dz = ep.z - m_center.z;
        float dist = sqrtf(dx * dx + dz * dz);
        if (dist > PULL_RADIUS) continue;

        // 중심 방향으로 끌어당김 (AI의 이동을 부분적으로 상쇄)
        if (dist > 0.5f)
        {
            float pull = PULL_FORCE * deltaTime;
            float nx = -dx / dist, nz = -dz / dist;
            ep.x += nx * pull;
            ep.z += nz * pull;
            pT->SetPosition(ep.x, ep.y, ep.z);
        }

        // 주기 피해 + onHit 훅
        if (bDoTick)
        {
            pEnemy->TakeDamage(dotDmg, false);
            if (m_pCaster) {
                auto* pSC = m_pCaster->GetComponent<SkillComponent>();
                if (pSC && m_slot != SkillSlot::Count) {
                    SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
                    if (!sts.onHitHooks.empty()) {
                        SkillContext ctx;
                        ctx.caster             = m_pCaster;
                        ctx.baseDamage         = dotDmg;
                        ctx.damageDealt        = dotDmg;
                        ctx.hitEnemy           = pEnemy;
                        ctx.hitEnemyPos        = ep;
                        ctx.scene              = m_pScene;
                        ctx.statusChanceMult   = sts.statusChanceMult;
                        ctx.statusDurationMult = sts.statusDurationMult;
                        for (auto& hook : sts.onHitHooks) hook(ctx);
                    }
                }
            }
        }
    }
}

bool WaterVortexBehavior::IsFinished() const
{
    return !m_bActive;
}

void WaterVortexBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_vfxId >= 0) m_pVFXManager->StopEffect(m_vfxId);
        for (int id : m_extraVFXIds) if (id >= 0) m_pVFXManager->StopEffect(id);
    }
    m_bActive = false;
    m_vfxId   = -1;
    m_extraVFXIds.clear();
}
