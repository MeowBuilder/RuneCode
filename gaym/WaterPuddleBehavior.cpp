#include "stdafx.h"
#include "WaterPuddleBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"

WaterPuddleBehavior::WaterPuddleBehavior()
    : m_SkillData(WaterSkillPresets::WaterPuddle())
{
}

void WaterPuddleBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    Reset();
    m_pCaster = caster;

    if (!m_pVFXManager) { return; }

    SkillStats stats;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count)
            stats = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    // ① 낙하 이펙트 — 타겟 위 5.5 유닛에서 아래 방향으로 스폰
    {
        XMFLOAT3 fallOrigin = { targetPosition.x, targetPosition.y + 5.5f, targetPosition.z };
        XMFLOAT3 fallDir    = { 0.f, -1.f, 0.f };
        EffectDef fallDef   = EffectRegistry::Get().GetEffect("Q_WaterFall");
        if (!stats.elementSet.empty())
            ApplyElementToEffectDef(fallDef, stats.elementSet[0]);
        m_fallVfxId = m_pVFXManager->SpawnEffectDef(fallOrigin, fallDir, fallDef, true);
    }

    // ② 웅덩이 이펙트 — origin=(x,2.5,z) + dir=(0,-1,0) → ConfinementBox center=(x,0,z)
    {
        XMFLOAT3 puddleOrigin = { targetPosition.x, 2.5f, targetPosition.z };
        XMFLOAT3 puddleDir    = { 0.f, -1.f, 0.f };
        EffectDef puddleDef   = EffectRegistry::Get().GetEffect("Q_WaterPuddle");
        if (!stats.elementSet.empty())
            ApplyElementToEffectDef(puddleDef, stats.elementSet[0]);
        m_vfxId = m_pVFXManager->SpawnEffectDef(puddleOrigin, puddleDir, puddleDef, true);
    }

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

void WaterPuddleBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;

    m_elapsed += deltaTime;

    // 낙하 이펙트 — FALL_DURATION 후 종료
    if (m_fallVfxId >= 0 && m_elapsed >= FALL_DURATION)
    {
        m_pVFXManager->StopEffect(m_fallVfxId);
        m_fallVfxId = -1;
    }

    TickPuddle(deltaTime);

    if (m_elapsed >= DURATION)
    {
        RemoveSlowFromAll();
        if (m_pVFXManager && m_vfxId >= 0)
            m_pVFXManager->StopEffect(m_vfxId);
        m_bActive = false;
        m_vfxId   = -1;
    }
}

void WaterPuddleBehavior::TickPuddle(float deltaTime)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    m_tickTimer += deltaTime;
    bool bDoTick = (m_tickTimer >= TICK_INTERVAL);
    if (bDoTick) m_tickTimer = 0.f;

    float dotDmg = m_SkillData.damage * m_damageMult * DMG_PER_TICK;

    std::unordered_set<EnemyComponent*> inRange;

    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        auto* pT = obj->GetTransform();
        if (!pT) continue;

        XMFLOAT3 ep = pT->GetPosition();
        float dx = ep.x - m_center.x, dz = ep.z - m_center.z;
        if (dx * dx + dz * dz > PUDDLE_RADIUS * PUDDLE_RADIUS) continue;

        inRange.insert(pEnemy);

        // 슬로우 진입
        if (m_slowedEnemies.find(pEnemy) == m_slowedEnemies.end())
        {
            pEnemy->SetSpeedMultiplier(SLOW_FACTOR);
            m_slowedEnemies.insert(pEnemy);
        }

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

    // 범위를 벗어난 적의 슬로우 해제
    for (auto it = m_slowedEnemies.begin(); it != m_slowedEnemies.end(); )
    {
        if (inRange.find(*it) == inRange.end())
        {
            if (!(*it)->IsDead())
                (*it)->SetSpeedMultiplier(1.0f);
            it = m_slowedEnemies.erase(it);
        }
        else
            ++it;
    }
}

void WaterPuddleBehavior::RemoveSlowFromAll()
{
    for (auto* pEnemy : m_slowedEnemies)
        if (!pEnemy->IsDead())
            pEnemy->SetSpeedMultiplier(1.0f);
    m_slowedEnemies.clear();
}

bool WaterPuddleBehavior::IsFinished() const
{
    return !m_bActive;
}

void WaterPuddleBehavior::Reset()
{
    RemoveSlowFromAll();
    if (m_pVFXManager)
    {
        if (m_vfxId     >= 0) m_pVFXManager->StopEffect(m_vfxId);
        if (m_fallVfxId >= 0) m_pVFXManager->StopEffect(m_fallVfxId);
    }
    m_bActive   = false;
    m_vfxId     = -1;
    m_fallVfxId = -1;
}
