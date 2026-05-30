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

void WaterVortexBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_pCaster = caster;
    m_bChannelActive = true;
    m_center         = { targetPosition.x, 0.f, targetPosition.z };
    m_bActive        = true;  // Update()의 흡인/피해 루프 활성화
    m_damageMult     = 1.0f;
    m_elapsed        = 0.f;
    m_tickTimer      = 0.f;
}

void WaterVortexBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& target, float tickMult)
{
    if (!m_pVFXManager) return;

    // 커서 위치 갱신 (Update의 PullAndDamageEnemies가 이 m_center를 사용)
    m_center = { target.x, 0.f, target.z };

    // 채널 틱마다 커서 위치에 미니 소용돌이 VFX 생성 (각각 독립 지속)
    if (!EffectRegistry::Get().HasEffect("E_WaterVortex")) return;

    XMFLOAT3 origin = { target.x, 3.0f, target.z };
    XMFLOAT3 up     = { 0.f, 1.f, 0.f };

    SkillStats stats;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count)
            stats = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    EffectDef def = EffectRegistry::Get().GetEffect("E_WaterVortex");
    if (!stats.elementSet.empty()) ApplyElementToEffectDef(def, stats.elementSet[0]);

    VFXModifier mod;
    mod.sizeScaleMult     = 0.5f;
    mod.particleCountMult = 0.4f;
    mod.strengthMult      = 0.7f;
    ApplyVFXModifier(def, mod);

    int id = m_pVFXManager->SpawnEffectDef(origin, up, def, true);
    if (id >= 0) m_channelVortexIds.push_back(id);
}

void WaterVortexBehavior::OnChannelEnd(GameObject* caster)
{
    m_bChannelActive = false;
    if (!m_channelVortexIds.empty())
    {
        m_bPostChannelVortexes = true;
        m_vortexPostTimer      = 0.f;
    }
}

void WaterVortexBehavior::OnChargeBegin(GameObject* caster)
{
}

void WaterVortexBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void WaterVortexBehavior::OnEnhanceActivate(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_enhanceAuraId = m_pVFXManager->SpawnEffectDef(pos, up, EffectRegistry::Get().GetEffect(fx), true);
}

void WaterVortexBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
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

void WaterVortexBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_pCaster = caster;
    m_extraVFXIds.clear();
    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }

    // 채널 모드: OnChannelBegin이 이미 m_bActive=true 설정 — Execute 스킵
    if (m_bChannelActive) return;

    m_bActive = false;
    if (m_pVFXManager && m_vfxId >= 0) { m_pVFXManager->StopEffect(m_vfxId); m_vfxId = -1; }

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
    if (m_bActive)
    {
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

    // 채널 소용돌이 후처리: CHANNEL_VORTEX_DURATION 경과 후 VFX 정리
    if (m_bPostChannelVortexes)
    {
        m_vortexPostTimer += deltaTime;
        if (m_vortexPostTimer >= CHANNEL_VORTEX_DURATION)
        {
            m_bPostChannelVortexes = false;
            if (m_pVFXManager)
                for (int id : m_channelVortexIds) if (id >= 0) m_pVFXManager->StopEffect(id);
            m_channelVortexIds.clear();
        }
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
    }
}

bool WaterVortexBehavior::IsFinished() const
{
    return !m_bActive && !m_bPostChannelVortexes;
}

void WaterVortexBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_vfxId         >= 0) m_pVFXManager->StopEffect(m_vfxId);
        if (m_chargeVFXId   >= 0) m_pVFXManager->StopEffect(m_chargeVFXId);
        if (m_enhanceAuraId >= 0) m_pVFXManager->StopEffect(m_enhanceAuraId);
        for (int id : m_extraVFXIds)       if (id >= 0) m_pVFXManager->StopEffect(id);
        for (int id : m_channelVortexIds)  if (id >= 0) m_pVFXManager->StopEffect(id);
    }
    m_bActive              = false;
    m_vfxId                = -1;
    m_chargeVFXId          = -1;
    m_enhanceAuraId        = -1;
    m_extraVFXIds.clear();
    m_channelVortexIds.clear();
    m_bChannelActive       = false;
    m_bPostChannelVortexes = false;
    m_vortexPostTimer      = 0.f;
}
