#include "stdafx.h"
#include "WindCutterBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include <algorithm>

WindCutterBehavior::WindCutterBehavior()
    : m_SkillData(WindSkillPresets::WindCutter())
{
}

uint32_t WindCutterBehavior::GetRuneFlags(GameObject* caster) const
{
    if (!caster) return 0;
    auto* pSC = caster->GetComponent<SkillComponent>();
    if (!pSC || m_slot == SkillSlot::Count) return 0;
    RuneCombo c = pSC->GetRuneCombo(m_slot);
    uint32_t f = 0;
    if (c.hasInstant) f |= RUNE_INSTANT;
    if (c.hasCharge)  f |= RUNE_CHARGE;
    if (c.hasEnhance) f |= RUNE_ENHANCE;
    return f;
}

void WindCutterBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_bChannelMode = true;
    m_cachedElem   = ElementType::None;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty()) m_cachedElem = sts.elementSet[0];
        }
    }
}

void WindCutterBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& target, float tickMult)
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

    // 채널 틱마다 미니 커터 투사체 발사: 파티클 수·크기·반경 절반 수준으로 축소
    if (m_pVFXManager && EffectRegistry::Get().HasEffect("Q_WindCutter"))
    {
        XMFLOAT3 spawnPos = caster->GetTransform()->GetPosition();
        spawnPos.y += 5.0f;
        EffectDef miniDef = EffectRegistry::Get().GetEffect("Q_WindCutter");
        for (auto& l : miniDef.layers)
        {
            l.particleCount          = (std::max)(80, l.particleCount / 2);
            l.sizeScale             *= 0.55f;
            l.crescent.radius       *= 0.55f;
            l.crescent.thickness    *= 0.55f;
            // 히트판정 CHANNEL_RANGE(15) 에 맞게 파티클 비행 거리 조정
            // 원본 speed=62, avg_lifetime=0.65s → 40u 비행; 목표 15u → scale = 15/(62*0.65) ≈ 0.37
            l.crescent.normalSpeedMin *= 0.37f;
            l.crescent.normalSpeedMax *= 0.37f;
        }
        if (m_cachedElem != ElementType::None)
            ApplyElementToEffectDef(miniDef, m_cachedElem);
        m_pVFXManager->SpawnEffectDef(spawnPos, dir, miniDef, true);
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
        if (XMVectorGetX(XMVector3Length(latV)) > HALF_W) continue;
        pEnemy->TakeDamage(ApplyExecBonus(damage, pEnemy, caster), false, HasExecRune(caster));
    }
}

void WindCutterBehavior::OnChannelEnd(GameObject* caster)
{
    m_bChannelMode = false;
    if (m_pVFXManager && m_channelAmbientId >= 0)
    {
        m_pVFXManager->StopEffect(m_channelAmbientId);
        m_channelAmbientId = -1;
    }
}

void WindCutterBehavior::OnChargeBegin(GameObject* caster)
{
}

void WindCutterBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void WindCutterBehavior::OnEnhanceActivate(GameObject* caster)
{
}

void WindCutterBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    if (m_pVFXManager && m_enhanceAuraId >= 0) { m_pVFXManager->StopEffect(m_enhanceAuraId); m_enhanceAuraId = -1; }
}

void WindCutterBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    // 채널 룬: 진입 시 Execute가 호출되지만 큰 커터는 발사하지 않음
    // OnChannelTick에서 매 틱 미니 커터를 발사하는 것으로 대체
    if (m_bChannelMode)
    {
        if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }
        return;
    }

    m_bActive = false;
    m_hitEnemies.clear();
    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }

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

    m_pCaster = caster;
    uint32_t runeFlags = GetRuneFlags(caster);
    EffectDef def = EffectRegistry::Get().GetEffect("Q_WindCutter", runeFlags);
    m_vfxId = m_pVFXManager->SpawnEffectDef(m_origin, m_direction, def, true);

    if (m_vfxId >= 0)
    {
        m_bActive    = true;
        m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
        m_elapsed    = 0.f;
    }
}

void WindCutterBehavior::Update(float deltaTime)
{
    if (!m_bActive) return;
    m_elapsed += deltaTime;
    HitEnemiesInCutter(m_SkillData.damage * m_damageMult);
    if (m_elapsed >= DURATION) m_bActive = false;
}

void WindCutterBehavior::HitEnemiesInCutter(float damage)
{
    if (!m_pScene || m_vfxId < 0) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMVECTOR oV = XMLoadFloat3(&m_origin);
    XMVECTOR dV = XMVector3Normalize(XMLoadFloat3(&m_direction));

    // 커터는 짧은 시간 내에 전체 경로를 즉시 체크
    float front = m_elapsed * SPEED;
    float back  = (std::max)(0.f, front - HIT_DEPTH);

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
        if (XMVectorGetX(XMVector3Length(latV)) > HALF_W + eR) continue;

        float yTol = (std::max)(0.f, eScale.y * 0.6f);
        if (fabsf(ePos.y - m_origin.y) > HALF_H + yTol) continue;

        // 관통 — 이미 히트한 적도 매 프레임 체크하지 않으므로 set에 넣어 중복 방지
        pEnemy->TakeDamage(ApplyExecBonus(damage, pEnemy, m_pCaster), false, HasExecRune(m_pCaster));
        m_hitEnemies.insert(pEnemy);
    }
}

bool WindCutterBehavior::IsFinished() const { return !m_bActive; }

void WindCutterBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_vfxId            >= 0) m_pVFXManager->StopEffect(m_vfxId);
        if (m_channelAmbientId >= 0) m_pVFXManager->StopEffect(m_channelAmbientId);
        if (m_chargeVFXId      >= 0) m_pVFXManager->StopEffect(m_chargeVFXId);
        if (m_enhanceAuraId    >= 0) m_pVFXManager->StopEffect(m_enhanceAuraId);
    }
    m_bActive          = false;
    m_bChannelMode     = false;
    m_vfxId            = -1;
    m_channelAmbientId = -1;
    m_chargeVFXId      = -1;
    m_enhanceAuraId    = -1;
    m_hitEnemies.clear();
}
