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

void TornadoBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bActive = false;
    m_pCaster = caster;

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

    // VFX 위치 갱신
    if (m_pVFXManager && m_vfxId >= 0)
    {
        XMFLOAT3 vfxPos = m_pos;
        vfxPos.y = 0.f;
        XMFLOAT3 up = { 0.f, 1.f, 0.f };
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

        pEnemy->TakeDamage(dmg, false);

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
    if (m_pVFXManager && m_vfxId >= 0)
        m_pVFXManager->StopEffect(m_vfxId);
    m_bActive = false;
    m_vfxId   = -1;
}
