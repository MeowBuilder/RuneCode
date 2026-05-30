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

void WaterPuddleBehavior::OnChargeBegin(GameObject* caster)
{
}

void WaterPuddleBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void WaterPuddleBehavior::OnEnhanceActivate(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_enhanceAuraId = m_pVFXManager->SpawnEffectDef(pos, up, EffectRegistry::Get().GetEffect(fx), true);
}

void WaterPuddleBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    if (m_pVFXManager && m_enhanceAuraId >= 0) { m_pVFXManager->StopEffect(m_enhanceAuraId); m_enhanceAuraId = -1; }
    if (!m_pVFXManager) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    EffectDef def = EffectRegistry::Get().GetEffect(fx);
    for (auto& l : def.layers) l.particleCount *= 3;
    m_pVFXManager->SpawnEffectDef(targetPosition, up, def, false);
}

void WaterPuddleBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_pCaster    = caster;
    m_damageMult = 1.0f;

    if (!m_pVFXManager) return;
    if (!EffectRegistry::Get().HasEffect("Q_WaterFall")) return;
    XMFLOAT3 pos  = { targetPosition.x, targetPosition.y + 6.f, targetPosition.z };
    XMFLOAT3 down = { 0.f, -1.f, 0.f };
    m_channelRainId = m_pVFXManager->SpawnEffectDef(pos, down,
        EffectRegistry::Get().GetEffect("Q_WaterFall"), true);
}

void WaterPuddleBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float tickMult)
{
    // 채널 빗줄기 이동 추적
    if (m_pVFXManager && m_channelRainId >= 0)
    {
        XMFLOAT3 rainPos = { targetPosition.x, targetPosition.y + 6.f, targetPosition.z };
        XMFLOAT3 down    = { 0.f, -1.f, 0.f };
        m_pVFXManager->TrackEffect(m_channelRainId, rainPos, down);
    }

    if (!m_pVFXManager) return;

    SkillStats stats;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count)
            stats = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    XMFLOAT3 down = { 0.f, -1.f, 0.f };

    // 낙하 이펙트: 비반복(false) — 자동 종료
    if (EffectRegistry::Get().HasEffect("Q_WaterFall"))
    {
        XMFLOAT3 fallPos = { targetPosition.x, targetPosition.y + 5.5f, targetPosition.z };
        EffectDef fallDef = EffectRegistry::Get().GetEffect("Q_WaterFall");
        if (!stats.elementSet.empty()) ApplyElementToEffectDef(fallDef, stats.elementSet[0]);
        VFXModifier mod;
        mod.sizeScaleMult     = 0.5f;
        mod.particleCountMult = 0.45f;
        ApplyVFXModifier(fallDef, mod);
        m_pVFXManager->SpawnEffectDef(fallPos, down, fallDef, false);
    }

    // 웅덩이 이펙트: 반복(true) — 수명 직접 관리
    if (EffectRegistry::Get().HasEffect("Q_WaterPuddle"))
    {
        XMFLOAT3 puddlePos = { targetPosition.x, 2.5f, targetPosition.z };
        EffectDef puddleDef = EffectRegistry::Get().GetEffect("Q_WaterPuddle");
        if (!stats.elementSet.empty()) ApplyElementToEffectDef(puddleDef, stats.elementSet[0]);
        VFXModifier mod;
        mod.sizeScaleMult     = 0.5f;
        mod.particleCountMult = 0.45f;
        ApplyVFXModifier(puddleDef, mod);
        int id = m_pVFXManager->SpawnEffectDef(puddlePos, down, puddleDef, true);
        if (id >= 0)
        {
            ChannelPuddle cp;
            cp.center      = { targetPosition.x, 0.f, targetPosition.z };
            cp.puddleVfxId = id;
            cp.elapsed     = 0.f;
            m_channelPuddles.push_back(cp);
        }
    }
}

void WaterPuddleBehavior::OnChannelEnd(GameObject* caster)
{
    if (m_pVFXManager && m_channelRainId >= 0)
    {
        m_pVFXManager->StopEffect(m_channelRainId);
        m_channelRainId = -1;
    }

    if (!m_channelPuddles.empty())
        m_bPostChannelPuddles = true;
}

void WaterPuddleBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    Reset();
    m_pCaster = caster;

    if (!m_pVFXManager) return;

    bool bChannelMode = false;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count)
            bChannelMode = pSC->GetRuneCombo(m_slot).hasChannel;
    }

    // 채널 모드: OnChannelBegin/Tick이 웅덩이 생성 담당
    if (bChannelMode)
    {
        m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
        return;
    }

    SkillStats stats;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count)
            stats = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    // ① 낙하 이펙트
    {
        XMFLOAT3 fallOrigin = { targetPosition.x, targetPosition.y + 5.5f, targetPosition.z };
        XMFLOAT3 fallDir    = { 0.f, -1.f, 0.f };
        EffectDef fallDef   = EffectRegistry::Get().GetEffect("Q_WaterFall");
        if (!stats.elementSet.empty())
            ApplyElementToEffectDef(fallDef, stats.elementSet[0]);
        m_fallVfxId = m_pVFXManager->SpawnEffectDef(fallOrigin, fallDir, fallDef, true);
    }

    // ② 웅덩이 이펙트
    {
        XMFLOAT3 puddleOrigin = { targetPosition.x, 2.5f, targetPosition.z };
        XMFLOAT3 puddleDir    = { 0.f, -1.f, 0.f };
        EffectDef puddleDef   = EffectRegistry::Get().GetEffect("Q_WaterPuddle");
        if (!stats.elementSet.empty())
            ApplyElementToEffectDef(puddleDef, stats.elementSet[0]);
        m_vfxId = m_pVFXManager->SpawnEffectDef(puddleOrigin, puddleDir, puddleDef, true);
    }

    if (m_vfxId >= 0 || m_fallVfxId >= 0)
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
    // 일반 단일 웅덩이
    if (m_bActive)
    {
        m_elapsed += deltaTime;

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

    // 채널 누적 웅덩이 (채널 중 + 채널 종료 후)
    if (!m_channelPuddles.empty())
    {
        TickChannelPuddles(deltaTime);

        // 만료된 웅덩이 제거
        for (int i = (int)m_channelPuddles.size() - 1; i >= 0; --i)
            if (m_channelPuddles[i].puddleVfxId < 0)
                m_channelPuddles.erase(m_channelPuddles.begin() + i);

        if (m_bPostChannelPuddles && m_channelPuddles.empty())
        {
            RemoveSlowFromAll();
            m_bPostChannelPuddles = false;
        }
    }
}

void WaterPuddleBehavior::TickChannelPuddles(float deltaTime)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    // 각 웅덩이 수명 진행 + 만료 처리
    for (auto& cp : m_channelPuddles)
    {
        if (cp.puddleVfxId < 0) continue;
        cp.elapsed += deltaTime;
        if (cp.elapsed >= CHANNEL_PUDDLE_DURATION)
        {
            if (m_pVFXManager) m_pVFXManager->StopEffect(cp.puddleVfxId);
            cp.puddleVfxId = -1;
        }
    }

    m_puddleTickTimer += deltaTime;
    bool bDoTick = (m_puddleTickTimer >= TICK_INTERVAL);
    if (bDoTick) m_puddleTickTimer = 0.f;

    float dotDmg   = m_SkillData.damage * m_damageMult * DMG_PER_TICK;
    float chRadius = PUDDLE_RADIUS * CHANNEL_PUDDLE_RADIUS_MULT;

    std::unordered_set<EnemyComponent*> inAnyPuddle;

    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        auto* pT = obj->GetTransform();
        if (!pT) continue;
        XMFLOAT3 ep = pT->GetPosition();

        for (const auto& cp : m_channelPuddles)
        {
            if (cp.puddleVfxId < 0) continue;
            float dx = ep.x - cp.center.x, dz = ep.z - cp.center.z;
            if (dx * dx + dz * dz > chRadius * chRadius) continue;

            inAnyPuddle.insert(pEnemy);

            if (m_slowedEnemies.find(pEnemy) == m_slowedEnemies.end())
            {
                pEnemy->SetSpeedMultiplier(SLOW_FACTOR);
                m_slowedEnemies.insert(pEnemy);
            }

            if (bDoTick)
            {
                pEnemy->TakeDamage(dotDmg, false, HasExecRune(m_pCaster));
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
            break;  // 중복 피해/슬로우 방지
        }
    }

    // 범위를 벗어난 적 슬로우 해제
    for (auto it = m_slowedEnemies.begin(); it != m_slowedEnemies.end(); )
    {
        if (inAnyPuddle.find(*it) == inAnyPuddle.end())
        {
            if (!(*it)->IsDead())
                (*it)->SetSpeedMultiplier(1.0f);
            it = m_slowedEnemies.erase(it);
        }
        else
            ++it;
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

        if (m_slowedEnemies.find(pEnemy) == m_slowedEnemies.end())
        {
            pEnemy->SetSpeedMultiplier(SLOW_FACTOR);
            m_slowedEnemies.insert(pEnemy);
        }

        if (bDoTick)
        {
            pEnemy->TakeDamage(ApplyExecBonus(dotDmg, pEnemy, m_pCaster), false, HasExecRune(m_pCaster));
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
    return !m_bActive && !m_bPostChannelPuddles;
}

void WaterPuddleBehavior::Reset()
{
    RemoveSlowFromAll();
    if (m_pVFXManager)
    {
        if (m_vfxId         >= 0) m_pVFXManager->StopEffect(m_vfxId);
        if (m_fallVfxId     >= 0) m_pVFXManager->StopEffect(m_fallVfxId);
        if (m_channelRainId >= 0) m_pVFXManager->StopEffect(m_channelRainId);
        for (auto& cp : m_channelPuddles)
            if (cp.puddleVfxId >= 0) m_pVFXManager->StopEffect(cp.puddleVfxId);
    }
    m_bActive              = false;
    m_bPostChannelPuddles  = false;
    m_vfxId                = -1;
    m_fallVfxId            = -1;
    m_channelRainId        = -1;
    m_puddleTickTimer      = 0.f;
    m_channelPuddles.clear();
}
