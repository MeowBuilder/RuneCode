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

void GaleRushBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_bChannelMode = true;
    m_pCaster      = caster;
    m_hitEnemies.clear();
    m_trailTimer = 0.f;
    // Execute가 채널 모드에서 early-return하므로 여기서 변환 룬 원소를 캐시
    m_cachedElem = ElementType::None;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty()) m_cachedElem = sts.elementSet[0];
        }
    }
}

void GaleRushBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& target, float tickMult)
{
    // 채널 메커니즘: 매 틱 커서 방향으로 단거리 돌진 + 주변 적 타격
    if (!m_pScene || !caster || !caster->GetTransform()) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    // 커서 방향 계산 (VFX보다 먼저 — 대쉬는 VFX 의존 없이 발동해야 함)
    XMFLOAT3 casterPos = caster->GetTransform()->GetPosition();
    XMVECTOR oV = XMVectorSetY(XMLoadFloat3(&casterPos), 0.f);
    XMVECTOR dV = XMVector3Normalize(XMVectorSetY(
        XMVectorSubtract(XMLoadFloat3(&target), oV), 0.f));
    if (XMVectorGetX(XMVector3LengthSq(dV)) < 0.001f)
        dV = XMVector3Normalize(XMVectorSetY(caster->GetTransform()->GetLook(), 0.f));
    XMStoreFloat3(&m_direction, dV);

    // 매 틱 커서 방향으로 단거리 돌진 — 기존 대쉬와 동일한 느낌, 거리만 줄임
    auto* pPC = caster->GetComponent<PlayerComponent>();
    if (pPC) pPC->StartSkillDash(m_direction, DASH_SPEED, 0.18f);

    // 틱마다 히트셋 초기화 (매 틱 독립 피해)
    m_hitEnemies.clear();
    float damage = m_SkillData.damage * tickMult;
    HitEnemiesNearCaster(damage);

    // VFX — m_pVFXManager가 있을 때만
    if (m_pVFXManager)
    {
        XMFLOAT3 up = { 0.f, 1.f, 0.f };

        // 시전자 위치 돌진 폭발 (기존 대쉬 VFX 재활용)
        if (EffectRegistry::Get().HasEffect("E_GaleRush_Burst"))
        {
            XMFLOAT3 burstPos = casterPos;
            burstPos.y += 2.0f;
            EffectDef burstDef = EffectRegistry::Get().GetEffect("E_GaleRush_Burst");
            if (m_cachedElem != ElementType::None)
                ApplyElementToEffectDef(burstDef, m_cachedElem);
            m_pVFXManager->SpawnEffectDef(burstPos, m_direction, burstDef, false);
        }

        // 배기 트레일 (진행 반대 방향)
        if (EffectRegistry::Get().HasEffect("E_GaleRush_Trail"))
        {
            XMFLOAT3 trailPos = casterPos;
            trailPos.y += 2.0f;
            XMFLOAT3 backDir = { -m_direction.x, 0.f, -m_direction.z };
            EffectDef trailDef = EffectRegistry::Get().GetEffect("E_GaleRush_Trail");
            if (m_cachedElem != ElementType::None)
                ApplyElementToEffectDef(trailDef, m_cachedElem);
            int id = m_pVFXManager->SpawnEffectDef(trailPos, backDir, trailDef, false);
            if (id >= 0) m_trailVfxIds.push_back(id);
        }

        // 칼날 VFX: 돌진 방향 ±20° 좌우로 Q_WindCutter 크레센트 소형 2장
        if (EffectRegistry::Get().HasEffect("Q_WindCutter"))
        {
            auto spawnBlade = [&](float angleDeg)
            {
                float rad = angleDeg * (XM_PI / 180.f);
                float cs = cosf(rad), sn = sinf(rad);
                XMFLOAT3 bladeDir = {
                    m_direction.x * cs - m_direction.z * sn,
                    0.f,
                    m_direction.x * sn + m_direction.z * cs
                };
                XMFLOAT3 bladePos = casterPos;
                bladePos.y += 3.0f;

                EffectDef bladeDef = EffectRegistry::Get().GetEffect("Q_WindCutter");
                for (auto& l : bladeDef.layers)
                {
                    l.particleCount       = (std::max)(60, l.particleCount / 4);
                    l.sizeScale          *= 0.45f;
                    l.crescent.radius    *= 0.45f;
                    l.crescent.thickness *= 0.45f;
                }
                m_pVFXManager->SpawnEffectDef(bladePos, bladeDir, bladeDef, true);
            };

            spawnBlade(-20.f);
            spawnBlade( 20.f);
        }
    }
}

void GaleRushBehavior::OnChannelEnd(GameObject* caster)
{
    m_bChannelMode = false;
}

void GaleRushBehavior::OnChargeBegin(GameObject* caster)
{
}

void GaleRushBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void GaleRushBehavior::OnEnhanceActivate(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_enhanceAuraId = m_pVFXManager->SpawnEffectDef(pos, up, EffectRegistry::Get().GetEffect(fx), true);
}

void GaleRushBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
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

void GaleRushBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    // 채널 룬: 진입 시 Execute가 호출되지만 대쉬는 스킵
    // OnChannelTick이 매 틱 단거리 돌진을 처리
    if (m_bChannelMode)
    {
        if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }
        return;
    }

    m_bActive  = false;
    m_pCaster  = nullptr;
    m_hitEnemies.clear();
    m_trailVfxIds.clear();
    m_trailTimer = TRAIL_INTERVAL;  // 첫 프레임 즉시 분사
    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }

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

    // 변환 룬 원소 캡처
    m_cachedElem = ElementType::None;
    {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty())
                m_cachedElem = sts.elementSet[0];
        }
    }

    // 출발 구체 폭발 — 몸통 높이(y+2)에서 사방으로 터짐
    {
        XMFLOAT3 burstPos = casterPos;
        burstPos.y += 2.0f;
        EffectDef burstDef = EffectRegistry::Get().GetEffect("E_GaleRush_Burst");
        if (m_cachedElem != ElementType::None)
            ApplyElementToEffectDef(burstDef, m_cachedElem);
        m_vfxId = m_pVFXManager->SpawnEffectDef(burstPos, m_direction, burstDef, true);
    }

    // 출발 지면 충격파 링
    {
        XMFLOAT3 ringPos = casterPos;
        ringPos.y = 0.f;
        EffectDef ringDef = EffectRegistry::Get().GetEffect("E_GaleRush_Ring");
        if (m_cachedElem != ElementType::None)
            ApplyElementToEffectDef(ringDef, m_cachedElem);
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
            if (m_cachedElem != ElementType::None)
                ApplyElementToEffectDef(trailDef, m_cachedElem);
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

        {
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

bool GaleRushBehavior::IsFinished() const { return !m_bActive; }

void GaleRushBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_vfxId              >= 0) m_pVFXManager->StopEffect(m_vfxId);
        if (m_ringVfxId          >= 0) m_pVFXManager->StopEffect(m_ringVfxId);
        if (m_chargeVFXId        >= 0) m_pVFXManager->StopEffect(m_chargeVFXId);
        if (m_enhanceAuraId      >= 0) m_pVFXManager->StopEffect(m_enhanceAuraId);
        if (m_channelGatherVfxId >= 0) m_pVFXManager->StopEffect(m_channelGatherVfxId);
        for (int id : m_trailVfxIds)
            if (id >= 0) m_pVFXManager->StopEffect(id);
    }
    m_bActive            = false;
    m_bChannelMode       = false;
    m_vfxId              = -1;
    m_ringVfxId          = -1;
    m_chargeVFXId        = -1;
    m_enhanceAuraId      = -1;
    m_channelGatherVfxId = -1;
    m_pCaster            = nullptr;
    m_hitEnemies.clear();
    m_trailVfxIds.clear();
    m_trailTimer = 0.f;
}
