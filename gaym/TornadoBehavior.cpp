#include "stdafx.h"
#include "TornadoBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"

TornadoBehavior::TornadoBehavior()
    : m_SkillData(WindSkillPresets::Tornado())
    , m_rng(std::random_device{}())
{
}

void TornadoBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_bChannelMode   = true;
    m_dirTimer       = 0.f;
    // 현재 토네이도 위치를 lerp 출발점으로, 커서를 최초 목표로 설정
    m_channelTargetPos = { targetPosition.x, 0.f, targetPosition.z };
}

void TornadoBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& target, float tickMult)
{
    // 매 채널 틱마다 lerp 목표를 갱신 — m_pos는 UpdateMovement에서 부드럽게 추적
    if (!m_bActive) return;
    m_channelTargetPos = { target.x, 0.f, target.z };
}

void TornadoBehavior::OnChannelEnd(GameObject* caster)
{
    m_bChannelMode = false;
    m_dirTimer     = DIR_INTERVAL;  // 채널 종료 후 즉시 새 방향 선택
}

void TornadoBehavior::OnChargeBegin(GameObject* caster)
{
}

void TornadoBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void TornadoBehavior::OnEnhanceActivate(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_enhanceAuraId = m_pVFXManager->SpawnEffectDef(pos, up, EffectRegistry::Get().GetEffect(fx), true);
}

void TornadoBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
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

void TornadoBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bActive = false;
    m_pCaster = caster;
    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }

    if (!m_pVFXManager) { return; }

    XMFLOAT3 origin    = targetPosition;
    origin.y           = 0.f;  // 토네이도는 지면에서 상승
    XMFLOAT3 direction = { 0.f, 1.f, 0.f };

    SkillStats stats;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count)
            stats = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    EffectDef def = EffectRegistry::Get().GetEffect("R_TornadoPlayer");
    if (!stats.elementSet.empty())
        ApplyElementToEffectDef(def, stats.elementSet[0]);
    m_vfxId = m_pVFXManager->SpawnEffectDef(origin, direction, def, true);

    if (m_vfxId >= 0)
    {
        m_bActive    = true;
        m_pos        = targetPosition;
        m_pos.y      = 0.f;
        m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
        m_elapsed    = 0.f;
        m_tickTimer  = 0.f;
        m_dirTimer   = 0.f;

        // 초기 이동 방향: 랜덤
        std::uniform_real_distribution<float> angleDist(0.f, XM_2PI);
        float a = angleDist(m_rng);
        m_moveDir = { cosf(a), 0.f, sinf(a) };
    }
}

void TornadoBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;

    m_elapsed += deltaTime;
    UpdateMovement(deltaTime);
    DamageEnemiesNearby(deltaTime);

    if (m_elapsed >= DURATION)
    {
        if (m_pVFXManager && m_vfxId >= 0)
            m_pVFXManager->StopEffect(m_vfxId);
        m_bActive = false;
        m_vfxId   = -1;
    }
}

void TornadoBehavior::UpdateMovement(float dt)
{
    if (m_bChannelMode)
    {
        // 채널 중: 커서 위치(m_channelTargetPos)를 향해 매 프레임 lerp
        // TrackEffect를 0.2s 간격이 아닌 매 프레임 호출 → attractor 점프 없이 부드럽게 이동
        XMVECTOR cur = XMVectorSet(m_pos.x, 0.f, m_pos.z, 0.f);
        XMVECTOR tgt = XMVectorSet(m_channelTargetPos.x, 0.f, m_channelTargetPos.z, 0.f);
        XMVECTOR delta = XMVectorSubtract(tgt, cur);
        float dist = XMVectorGetX(XMVector3Length(delta));
        if (dist > 0.001f)
        {
            float step = (std::min)(dist, CHANNEL_LERP_SPEED * dt);
            XMVECTOR newPos = XMVectorAdd(cur, XMVectorScale(XMVector3Normalize(delta), step));
            XMStoreFloat3(&m_pos, newPos);
            m_pos.y = 0.f;
        }
        if (m_pVFXManager && m_vfxId >= 0)
        {
            XMFLOAT3 vfxPos = { m_pos.x, 0.f, m_pos.z };
            XMFLOAT3 up = { 0.f, 1.f, 0.f };
            m_pVFXManager->TrackEffect(m_vfxId, vfxPos, up);
        }
        return;
    }

    m_dirTimer += dt;
    if (m_dirTimer >= DIR_INTERVAL)
    {
        m_dirTimer = 0.f;
        std::uniform_real_distribution<float> a(-0.8f, 0.8f);
        float angle = a(m_rng);
        float cs = cosf(angle), sn = sinf(angle);
        float nx = m_moveDir.x * cs - m_moveDir.z * sn;
        float nz = m_moveDir.x * sn + m_moveDir.z * cs;
        float len = sqrtf(nx * nx + nz * nz);
        if (len > 0.f) { m_moveDir.x = nx / len; m_moveDir.z = nz / len; }
    }

    m_pos.x += m_moveDir.x * MOVE_SPEED * dt;
    m_pos.z += m_moveDir.z * MOVE_SPEED * dt;

    if (m_pVFXManager && m_vfxId >= 0)
    {
        XMFLOAT3 vfxPos = { m_pos.x, 0.f, m_pos.z };
        XMFLOAT3 up     = { 0.f, 1.f, 0.f };
        m_pVFXManager->TrackEffect(m_vfxId, vfxPos, up);
    }
}

void TornadoBehavior::DamageEnemiesNearby(float dt)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    m_tickTimer += dt;
    if (m_tickTimer < TICK_INTERVAL) return;
    m_tickTimer = 0.f;

    float dmg = m_SkillData.damage * m_damageMult * DMG_PER_TICK;

    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        auto* pT = obj->GetTransform();
        if (!pT) continue;

        XMFLOAT3 ep = pT->GetPosition();
        float dx = ep.x - m_pos.x, dz = ep.z - m_pos.z;
        float dist = sqrtf(dx * dx + dz * dz);
        if (dist > DMG_RADIUS) continue;

        pEnemy->TakeDamage(ApplyExecBonus(dmg, pEnemy, m_pCaster), false, HasExecRune(m_pCaster));

        if (m_pCaster) {
            auto* pSC = m_pCaster->GetComponent<SkillComponent>();
            if (pSC && m_slot != SkillSlot::Count) {
                SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
                if (!sts.onHitHooks.empty()) {
                    SkillContext ctx;
                    ctx.caster             = m_pCaster;
                    ctx.baseDamage         = dmg;
                    ctx.damageDealt        = dmg;
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

bool TornadoBehavior::IsFinished() const { return !m_bActive; }

void TornadoBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_vfxId         >= 0) m_pVFXManager->StopEffect(m_vfxId);
        if (m_chargeVFXId   >= 0) m_pVFXManager->StopEffect(m_chargeVFXId);
        if (m_enhanceAuraId >= 0) m_pVFXManager->StopEffect(m_enhanceAuraId);
    }
    m_bActive       = false;
    m_bChannelMode  = false;
    m_vfxId         = -1;
    m_chargeVFXId   = -1;
    m_enhanceAuraId = -1;
}
