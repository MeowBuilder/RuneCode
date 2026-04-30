#include "stdafx.h"
#include "MeteorBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include <cmath>

MeteorBehavior::MeteorBehavior()
    : m_SkillData(FireSkillPresets::Meteor())
{
}

MeteorBehavior::MeteorBehavior(const SkillData& customData)
    : m_SkillData(customData)
{
}

void MeteorBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bIsFinished = false;

    if (!m_pVFXManager)
    {
        OutputDebugString(L"[Meteor] Warning: No VFXManager set!\n");
        m_bIsFinished = true;
        return;
    }

    // 1. 타겟 위치 결정 (targetPosition 사용, 마우스 클릭 위치)
    //    SkillComponent::CalculateTargetPosition()이 이미 계산해서 넘겨줌
    XMFLOAT3 targetPos = targetPosition;

    // targetPosition이 (0,0,0)이면 플레이어 전방으로 폴백
    if (fabsf(targetPos.x) < 0.001f && fabsf(targetPos.z) < 0.001f)
    {
        if (caster && caster->GetTransform())
        {
            XMFLOAT3 casterPos = caster->GetTransform()->GetPosition();
            XMVECTOR look = caster->GetTransform()->GetLook();
            look = XMVectorSetY(look, 0.f);
            look = XMVector3Normalize(look);

            XMVECTOR targetV = XMVectorAdd(
                XMLoadFloat3(&casterPos),
                XMVectorScale(look, METEOR_FORWARD_DIST)
            );
            XMStoreFloat3(&targetPos, targetV);
        }
    }

    // 2. 스폰 위치 = 타겟 상공
    XMFLOAT3 spawnPos = { targetPos.x, targetPos.y + METEOR_SPAWN_HEIGHT, targetPos.z };

    // 3. 방향 = 아래 (낙하)
    XMFLOAT3 direction = { 0.f, -1.f, 0.f };

    // 4. 룬 플래그 + 원소 스탯
    uint32_t runeFlags = GetRuneFlags(caster);
    SkillStats stats;
    if (caster) {
        auto* pSkillComp = caster->GetComponent<SkillComponent>();
        if (pSkillComp && m_slot != SkillSlot::Count)
            stats = pSkillComp->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    // 5. EffectRegistry에서 R_Meteor EffectDef + 룬 원소 색상 오버라이드
    auto applyElement = [](EffectDef& def, ElementType e, bool reduceCount) {
        FluidElementColor ec = FluidElementColors::Get(e);
        def.element = e;
        for (auto& l : def.layers) {
            l.element   = e;
            l.coreColor = ec.coreColor;
            l.edgeColor = ec.edgeColor;
            if (reduceCount) {
                bool isSPH = (l.type >= EmitterType::SPH_Attract &&
                              l.type <= EmitterType::SPH_Beam);
                if (isSPH)
                    l.sph.particleCount = max(100, (int)(l.sph.particleCount * 0.6f));
                else
                    l.particleCount = max(100, (int)(l.particleCount * 0.6f));
            }
        }
    };

    EffectDef def = EffectRegistry::Get().GetEffect("R_Meteor", runeFlags);
    if (!stats.elementSet.empty())
        applyElement(def, stats.elementSet[0], stats.elementSet.size() > 1);

    // 6. 이펙트 생성 (1차 원소)
    m_vfxId = m_pVFXManager->SpawnEffectDef(spawnPos, direction, def, /*isPlayer*/true);

    // 추가 원소 VFX (2차 이상)
    m_extraVFXIds.clear();
    for (size_t ei = 1; ei < stats.elementSet.size(); ++ei)
    {
        EffectDef extraDef = EffectRegistry::Get().GetEffect("R_Meteor", runeFlags);
        applyElement(extraDef, stats.elementSet[ei], /*reduceCount*/true);
        int eid = m_pVFXManager->SpawnEffectDef(spawnPos, direction, extraDef, /*isPlayer*/true);
        if (eid >= 0) m_extraVFXIds.push_back(eid);
    }

    // 히트 판정 상태 초기화
    m_targetPos   = targetPos;
    m_damageMult  = damageMultiplier > 0.f ? damageMultiplier : 1.f;
    m_elapsed     = 0.f;
    m_bExploded   = false;
    m_hitTimer    = 0.f;
    m_bIsFinished = (m_vfxId < 0); // VFX 성공하면 Update에서 완료 처리

    wchar_t buf[256];
    swprintf_s(buf, 256,
        L"[Meteor] Execute: vfxId=%d, target=(%.1f,%.1f,%.1f), spawn=(%.1f,%.1f,%.1f), runeFlags=0x%X\n",
        m_vfxId, targetPos.x, targetPos.y, targetPos.z,
        spawnPos.x, spawnPos.y, spawnPos.z, runeFlags);
    OutputDebugString(buf);
}

void MeteorBehavior::Update(float deltaTime)
{
    if (m_bIsFinished || m_vfxId < 0) return;

    m_elapsed += deltaTime;

    // ── 낙하 단계 (0 ~ FALL_DURATION) ──────────────────────────────
    if (m_elapsed < FALL_DURATION) return;

    // ── 폭발 단계: 1회 히트 후 즉시 종료 ─────────────────────────
    if (!m_bExploded) {
        m_bExploded   = true;
        m_bIsFinished = true;
        // 단일 폭발 AoE: 경직 허용
        ApplyExplosionDamage(m_SkillData.damage * m_damageMult, EXPLODE_RADIUS, true);
        OutputDebugStringA("[Meteor] Explosion hit! (single)\n");
    }
}

void MeteorBehavior::ApplyExplosionDamage(float damage, float radius, bool bTriggerStagger)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMVECTOR centerV = XMLoadFloat3(&m_targetPos);

    const auto& gameObjects = pRoom->GetGameObjects();
    for (const auto& obj : gameObjects)
    {
        if (!obj) continue;
        EnemyComponent* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;

        TransformComponent* pTransform = obj->GetTransform();
        if (!pTransform) continue;

        XMFLOAT3 ePos = pTransform->GetPosition();
        XMFLOAT3 eScale = pTransform->GetScale();
        float eRadius = max(1.5f, max(eScale.x, max(eScale.y, eScale.z)) * 1.5f);

        float dist = XMVectorGetX(XMVector3Length(
            XMVectorSubtract(XMLoadFloat3(&ePos), centerV)));

        if (dist < radius + eRadius) {
            // 거리 기반 감쇠: 중심 100%, 가장자리 50%
            float falloff = 1.f - (dist / (radius + eRadius)) * 0.5f;
            falloff = max(0.5f, falloff);
            pEnemy->TakeDamage(damage * falloff, bTriggerStagger);
        }
    }
}

bool MeteorBehavior::IsFinished() const
{
    return m_bIsFinished;
}

void MeteorBehavior::Reset()
{
    if (m_pVFXManager)
        for (int eid : m_extraVFXIds)
            if (eid >= 0) m_pVFXManager->StopEffect(eid);
    m_bIsFinished = true;
    m_bExploded   = false;
    m_elapsed     = 0.f;
    m_hitTimer    = 0.f;
    m_vfxId       = -1;
    m_extraVFXIds.clear();
}

uint32_t MeteorBehavior::GetRuneFlags(GameObject* caster) const
{
    uint32_t flags = 0;
    if (!caster) return flags;

    auto* pSkillComp = caster->GetComponent<SkillComponent>();
    if (!pSkillComp || m_slot == SkillSlot::Count) return flags;

    RuneCombo combo = pSkillComp->GetRuneCombo(m_slot);
    if (combo.hasInstant) flags |= RUNE_INSTANT;
    if (combo.hasCharge)  flags |= RUNE_CHARGE;
    if (combo.hasChannel) flags |= RUNE_CHANNEL;
    if (combo.hasPlace)   flags |= RUNE_PLACE;
    if (combo.hasEnhance) flags |= RUNE_ENHANCE;
    if (combo.hasSplit)   flags |= RUNE_SPLIT;
    return flags;
}
