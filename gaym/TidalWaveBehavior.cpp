#include "stdafx.h"
#include "TidalWaveBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include <algorithm>

TidalWaveBehavior::TidalWaveBehavior()
    : m_SkillData(WaterSkillPresets::TidalWave())
    , m_rng(std::random_device{}())
{
}

void TidalWaveBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    if (!EffectRegistry::Get().HasEffect("sub_water")) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_channelAmbientId = m_pVFXManager->SpawnEffectDef(pos, up,
        EffectRegistry::Get().GetEffect("sub_water"), true);
}

void TidalWaveBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& target, float tickMult)
{
    if (!m_pScene || !caster || !caster->GetTransform()) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMFLOAT3 origin = caster->GetTransform()->GetPosition();
    origin.y = 0.f;
    XMVECTOR oV = XMLoadFloat3(&origin);
    XMVECTOR dV = XMVector3Normalize(XMVectorSetY(XMVectorSubtract(XMLoadFloat3(&target), oV), 0.f));
    if (XMVectorGetX(XMVector3LengthSq(dV)) < 0.001f)
        dV = XMVector3Normalize(XMVectorSetY(caster->GetTransform()->GetLook(), 0.f));
    XMFLOAT3 dir; XMStoreFloat3(&dir, dV);

    if (m_pVFXManager && EffectRegistry::Get().HasEffect("sub_water"))
    {
        XMFLOAT3 spawnPos = { origin.x, origin.y + 0.5f, origin.z };
        m_pVFXManager->SpawnEffectDef(spawnPos, dir,
            EffectRegistry::Get().GetEffect("sub_water"), false);
    }

    if (m_pVFXManager && m_channelAmbientId >= 0)
    {
        XMFLOAT3 cpos = caster->GetTransform()->GetPosition();
        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        m_pVFXManager->TrackEffect(m_channelAmbientId, cpos, up);
    }

    float damage = m_SkillData.damage * tickMult;
    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        auto* pT = obj->GetTransform();
        if (!pT) continue;
        XMFLOAT3 ePos = pT->GetPosition();
        XMVECTOR toE = XMVectorSetY(XMVectorSubtract(XMLoadFloat3(&ePos), oV), 0.f);
        float fwd = XMVectorGetX(XMVector3Dot(toE, dV));
        if (fwd < 0.f || fwd > CHANNEL_RANGE) continue;
        XMVECTOR latV = XMVectorSubtract(toE, XMVectorScale(dV, fwd));
        if (XMVectorGetX(XMVector3Length(latV)) > CHANNEL_HALF_W) continue;
        pEnemy->TakeDamage(damage, false);
    }
}

void TidalWaveBehavior::OnChannelEnd(GameObject* caster)
{
    if (m_pVFXManager && m_channelAmbientId >= 0)
    {
        m_pVFXManager->StopEffect(m_channelAmbientId);
        m_channelAmbientId = -1;
    }
}

void TidalWaveBehavior::OnChargeBegin(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_chargeVFXId = m_pVFXManager->SpawnEffectDef(pos, up, EffectRegistry::Get().GetEffect(fx), true);
}

void TidalWaveBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void TidalWaveBehavior::OnEnhanceActivate(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_enhanceAuraId = m_pVFXManager->SpawnEffectDef(pos, up, EffectRegistry::Get().GetEffect(fx), true);
}

void TidalWaveBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
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

void TidalWaveBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bActive = false;
    m_pCaster = caster;
    m_hitEnemies.clear();
    m_extraVFXIds.clear();
    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }

    if (!m_pVFXManager) { return; }

    XMFLOAT3 origin    = { 0.f, 0.f, 0.f };
    XMFLOAT3 direction = { 0.f, 0.f, 1.f };

    if (caster && caster->GetTransform())
    {
        origin = caster->GetTransform()->GetPosition();
        origin.y += 5.0f;

        XMVECTOR dV = XMVector3Normalize(XMVectorSetY(
            XMVectorSubtract(XMLoadFloat3(&targetPosition), XMLoadFloat3(&origin)), 0.f));
        if (XMVectorGetX(XMVector3LengthSq(dV)) < 0.001f && caster->GetTransform())
            dV = XMVector3Normalize(XMVectorSetY(caster->GetTransform()->GetLook(), 0.f));
        XMStoreFloat3(&direction, dV);
    }

    SkillStats stats;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count)
            stats = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    EffectDef def = EffectRegistry::Get().GetEffect("R_TidalWave");
    if (!stats.elementSet.empty())
        ApplyElementToEffectDef(def, stats.elementSet[0]);
    m_vfxId = m_pVFXManager->SpawnEffectDef(origin, direction, def, true);

    if (m_vfxId >= 0)
    {
        m_bActive    = true;
        m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
        m_elapsed    = 0.f;
        m_foamTimer  = 0.f;
        std::uniform_real_distribution<float> d(0.05f, 0.18f);
        m_nextFoamAt = d(m_rng);
    }
}

void TidalWaveBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;

    m_elapsed += deltaTime;
    HitEnemiesInWave(m_SkillData.damage * m_damageMult);

    m_foamTimer += deltaTime;
    if (m_foamTimer >= m_nextFoamAt)
    {
        m_foamTimer = 0.f;
        DropFoam();
        std::uniform_real_distribution<float> d(0.05f, 0.18f);
        m_nextFoamAt = d(m_rng);
    }

    if (m_elapsed >= WAVE_DURATION)
        m_bActive = false;
}

void TidalWaveBehavior::DropFoam()
{
    if (!m_pVFXManager || m_vfxId < 0) return;
    if (!EffectRegistry::Get().HasEffect("R_TidalWave_Foam")) return;

    XMFLOAT3 waveOrigin = m_pVFXManager->GetWaveOrigin(m_vfxId);
    XMFLOAT3 waveDir    = m_pVFXManager->GetWaveDir(m_vfxId);
    XMVECTOR oV = XMLoadFloat3(&waveOrigin);
    XMVECTOR dV = XMVector3Normalize(XMLoadFloat3(&waveDir));

    // 파도 right 벡터 (폭 방향)
    XMVECTOR worldUp = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    XMVECTOR rightV  = XMVector3Normalize(XMVector3Cross(worldUp, dV));

    // 랜덤 가로 위치 — 파도 폭(±WAVE_HALF_W * 0.82) 내 균등 분포
    std::uniform_real_distribution<float> lateralDist(-WAVE_HALF_W * 0.5f, WAVE_HALF_W * 0.5f);
    // 파도 선두 기준 약간 뒤쪽에서 올라오는 느낌 (-5 ~ 0)
    std::uniform_real_distribution<float> lagDist(-5.f, 0.f);

    float frontDist = m_elapsed * WAVE_SPEED + lagDist(m_rng);
    if (frontDist < 0.f) frontDist = 0.f;

    XMVECTOR posV = XMVectorAdd(
        XMVectorAdd(oV, XMVectorScale(dV, frontDist)),
        XMVectorScale(rightV, lateralDist(m_rng))
    );
    XMFLOAT3 foamPos;
    XMStoreFloat3(&foamPos, posV);
    foamPos.y = 0.f;

    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    EffectDef def = EffectRegistry::Get().GetEffect("R_TidalWave_Foam");
    m_pVFXManager->SpawnEffectDef(foamPos, up, def, true);
}

void TidalWaveBehavior::HitEnemiesInWave(float damage)
{
    if (!m_pScene || m_vfxId < 0) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMFLOAT3 waveOrigin = m_pVFXManager->GetWaveOrigin(m_vfxId);
    XMFLOAT3 waveDir    = m_pVFXManager->GetWaveDir(m_vfxId);
    XMVECTOR oV = XMLoadFloat3(&waveOrigin);
    XMVECTOR dV = XMVector3Normalize(XMLoadFloat3(&waveDir));

    float front = m_elapsed * WAVE_SPEED;
    float back  = (std::max)(0.f, front - WAVE_HIT_DEPTH);

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
        if (XMVectorGetX(XMVector3Length(latV)) > WAVE_HALF_W + eR) continue;

        float yTol = (std::max)(0.f, eScale.y * 0.6f);
        if (fabsf(ePos.y - waveOrigin.y) > WAVE_HALF_H + yTol) continue;

        pEnemy->TakeDamage(damage, true);  // 해일은 경직 유발
        m_hitEnemies.insert(pEnemy);

        if (m_pCaster) {
            auto* pSC = m_pCaster->GetComponent<SkillComponent>();
            if (pSC && m_slot != SkillSlot::Count) {
                SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
                if (!sts.onHitHooks.empty()) {
                    SkillContext ctx;
                    ctx.caster             = m_pCaster;
                    ctx.baseDamage         = damage;
                    ctx.damageDealt        = damage;
                    ctx.hitEnemy           = pEnemy;
                    ctx.hitEnemyPos        = ePos;
                    ctx.scene              = m_pScene;
                    ctx.statusChanceMult   = sts.statusChanceMult;
                    ctx.statusDurationMult = sts.statusDurationMult;
                    for (auto& hook : sts.onHitHooks) hook(ctx);
                }
            }
        }
    }
}

bool TidalWaveBehavior::IsFinished() const
{
    return !m_bActive;
}

void TidalWaveBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_vfxId            >= 0) m_pVFXManager->StopEffect(m_vfxId);
        if (m_channelAmbientId >= 0) m_pVFXManager->StopEffect(m_channelAmbientId);
        if (m_chargeVFXId      >= 0) m_pVFXManager->StopEffect(m_chargeVFXId);
        if (m_enhanceAuraId    >= 0) m_pVFXManager->StopEffect(m_enhanceAuraId);
        for (int id : m_extraVFXIds) if (id >= 0) m_pVFXManager->StopEffect(id);
    }
    m_bActive          = false;
    m_vfxId            = -1;
    m_channelAmbientId = -1;
    m_chargeVFXId      = -1;
    m_enhanceAuraId    = -1;
    m_extraVFXIds.clear();
    m_hitEnemies.clear();
}
