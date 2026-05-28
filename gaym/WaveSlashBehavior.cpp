#include "stdafx.h"
#include "WaveSlashBehavior.h"
#include "DecalManager.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "RuneDef.h"
#include "PlayerComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include <algorithm>

WaveSlashBehavior::WaveSlashBehavior()
    : m_SkillData(FireSkillPresets::FlameWave())
{
    m_SkillData.name     = "WaveSlash";
    m_SkillData.cooldown = 3.0f;
}

WaveSlashBehavior::WaveSlashBehavior(const SkillData& customData)
    : m_SkillData(customData)
{
}

void WaveSlashBehavior::OnChargeBegin(GameObject* caster)
{
}

void WaveSlashBehavior::OnChargeUpdate(GameObject* caster, float chargeRatio)
{
    if (!m_pVFXManager || m_chargeVFXId < 0 || !caster || !caster->GetTransform()) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    pos.y += chargeRatio * 2.f;
    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    m_pVFXManager->TrackEffect(m_chargeVFXId, pos, up);
}

void WaveSlashBehavior::OnEnhanceActivate(GameObject* caster)
{
    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    const char* fx = SubVFXName(m_SkillData.element);
    if (!EffectRegistry::Get().HasEffect(fx)) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_enhanceAuraId = m_pVFXManager->SpawnEffectDef(pos, up, EffectRegistry::Get().GetEffect(fx), true);
}

void WaveSlashBehavior::OnEnhanceConsumed(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
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

void WaveSlashBehavior::OnChannelBegin(GameObject* caster, const DirectX::XMFLOAT3& targetPosition)
{
    m_bChannelActive = true;
    m_hitHalfW       = CHANNEL_WAVE_HALF_W;

    m_cachedElem = ElementType::None;
    if (caster) {
        auto* pSC = caster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            if (!sts.elementSet.empty()) m_cachedElem = sts.elementSet[0];
        }
        if (m_cachedElem == ElementType::None)
            if (auto* pPC = caster->GetComponent<PlayerComponent>())
                m_cachedElem = pPC->GetElementType();
    }

    if (!m_pVFXManager || !caster || !caster->GetTransform()) return;
    if (!EffectRegistry::Get().HasEffect("sub_fire")) return;
    XMFLOAT3 pos = caster->GetTransform()->GetPosition();
    XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    m_channelAmbientId = m_pVFXManager->SpawnEffectDef(pos, up,
        EffectRegistry::Get().GetEffect("sub_fire"), true);
}

void WaveSlashBehavior::OnChannelTick(GameObject* caster, const DirectX::XMFLOAT3& target, float tickMult)
{
    if (!caster || !caster->GetTransform()) return;

    XMFLOAT3 origin = caster->GetTransform()->GetPosition();
    origin.y += 5.0f;
    XMVECTOR oV = XMLoadFloat3(&origin);
    XMVECTOR dV = XMVector3Normalize(XMVectorSetY(XMVectorSubtract(XMLoadFloat3(&target), oV), 0.f));
    if (XMVectorGetX(XMVector3LengthSq(dV)) < 0.001f)
        dV = XMVector3Normalize(XMVectorSetY(caster->GetTransform()->GetLook(), 0.f));
    XMFLOAT3 dir; XMStoreFloat3(&dir, dV);

    // 채널 파도: 이전 파도를 죽이지 않고 새 파도를 추가 스폰 (isPlayer=true 필수 — SSF 렌더)
    // 각 파도는 WAVE_DURATION(2s) 동안 자체 물리(SPH waveMode)로 앞으로 진행
    // OnChannelEnd/Reset에서 일괄 StopEffect
    if (m_pVFXManager && EffectRegistry::Get().HasEffect("Q_WaveSlash"))
    {
        uint32_t runeFlags = GetRuneFlags(caster);
        EffectDef def = EffectRegistry::Get().GetEffect("Q_WaveSlash", runeFlags);
        ElementType elem = (m_cachedElem != ElementType::None) ? m_cachedElem : ElementType::Fire;
        FluidElementColor ec = FluidElementColors::Get(elem);
        def.element = elem;
        for (auto& l : def.layers) { l.element = elem; l.coreColor = ec.coreColor; l.edgeColor = ec.edgeColor; }

        VFXModifier mod;
        mod.sizeScaleMult    = 0.4f;
        mod.particleCountMult = 0.45f;
        mod.strengthMult     = 0.6f;
        mod.speedMult        = 1.25f;
        ApplyVFXModifier(def, mod);
        int waveId = m_pVFXManager->SpawnEffectDef(origin, dir, def, true);
        if (waveId >= 0) m_channelWaveVfxIds.push_back(waveId);
    }

    // 앰비언트 VFX 위치 갱신
    if (m_pVFXManager && m_channelAmbientId >= 0)
    {
        XMFLOAT3 cpos = caster->GetTransform()->GetPosition();
        XMFLOAT3 up = { 0.f, 1.f, 0.f };
        m_pVFXManager->TrackEffect(m_channelAmbientId, cpos, up);
    }

    // 좁은 파도 피해 판정
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMFLOAT3 groundOrigin = { origin.x, 0.f, origin.z };
    XMVECTOR gV = XMLoadFloat3(&groundOrigin);
    float damage = m_SkillData.damage * tickMult;
    for (const auto& obj : pRoom->GetGameObjects())
    {
        if (!obj) continue;
        auto* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        auto* pT = obj->GetTransform();
        if (!pT) continue;
        XMFLOAT3 ePos = pT->GetPosition();
        XMVECTOR toE = XMVectorSetY(XMVectorSubtract(XMLoadFloat3(&ePos), gV), 0.f);
        float fwd = XMVectorGetX(XMVector3Dot(toE, dV));
        if (fwd < 0.f || fwd > CHANNEL_RANGE) continue;
        XMVECTOR latV = XMVectorSubtract(toE, XMVectorScale(dV, fwd));
        if (XMVectorGetX(XMVector3Length(latV)) > CHANNEL_WAVE_HALF_W) continue;
        pEnemy->TakeDamage(damage, false);
    }
}

void WaveSlashBehavior::OnChannelEnd(GameObject* caster)
{
    if (m_pVFXManager && m_channelAmbientId >= 0)
    {
        m_pVFXManager->StopEffect(m_channelAmbientId);
        m_channelAmbientId = -1;
    }
    // 진행 중인 파도는 StopEffect하지 않음 — WAVE_DURATION 동안 자연 소멸 대기
    if (!m_channelWaveVfxIds.empty())
    {
        m_bPostChannelWaves = true;
        m_channelPostTimer  = 0.f;
    }
    m_bChannelActive   = false;
    m_hitHalfW         = WAVE_HALF_W;
}

void WaveSlashBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    if (m_pVFXManager && m_chargeVFXId >= 0) { m_pVFXManager->StopEffect(m_chargeVFXId); m_chargeVFXId = -1; }

    // 채널 모드: Execute 스킵 — OnChannelTick 이 틱마다 개별 파도를 스폰함
    if (m_bChannelActive) return;

    m_bIsFinished = false;
    m_bWaveActive = false;
    m_pCaster     = caster;

    if (!m_pVFXManager)
    {
        OutputDebugString(L"[WaveSlash] Warning: No VFXManager set!\n");
        m_bIsFinished = true;
        return;
    }

    // 1. 플레이어 위치·방향
    XMFLOAT3 origin    = { 0.f, 0.f, 0.f };
    XMFLOAT3 direction = { 0.f, 0.f, 1.f };

    if (caster && caster->GetTransform())
    {
        origin = caster->GetTransform()->GetPosition();
        origin.y += 5.0f;

        XMVECTOR originV = XMLoadFloat3(&origin);
        XMVECTOR targetV = XMLoadFloat3(&targetPosition);
        XMVECTOR dirV    = XMVectorSubtract(targetV, originV);
        dirV = XMVectorSetY(dirV, 0.f);
        dirV = XMVector3Normalize(dirV);

        if (XMVectorGetX(XMVector3LengthSq(dirV)) < 0.001f)
        {
            dirV = caster->GetTransform()->GetLook();
            dirV = XMVectorSetY(dirV, 0.f);
            dirV = XMVector3Normalize(dirV);
        }
        XMStoreFloat3(&direction, dirV);
    }

    // 2. 룬 플래그 + 원소 스탯
    uint32_t runeFlags = GetRuneFlags(caster);
    SkillStats stats;
    if (caster) {
        auto* pSkillComp = caster->GetComponent<SkillComponent>();
        if (pSkillComp && m_slot != SkillSlot::Count)
            stats = pSkillComp->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    // EffectRegistry에서 Q_WaveSlash EffectDef 가져와 색상 오버라이드 후 스폰
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

    // 캐릭터 기본 원소; 룬 원소 세트가 있으면 룬 원소가 우선
    ElementType baseElem = ElementType::Fire;
    if (caster) {
        if (auto* pPC = caster->GetComponent<PlayerComponent>())
            baseElem = pPC->GetElementType();
    }
    ElementType primaryElem = stats.elementSet.empty() ? baseElem : stats.elementSet[0];

    EffectDef def = EffectRegistry::Get().GetEffect("Q_WaveSlash", runeFlags);
    applyElement(def, primaryElem, stats.elementSet.size() > 1);

    // 룬 vfxMod + 활성화 vfxMod 병합하여 파티클 + 파동 물리 스케일 적용
    {
        VFXModifier activationMod;
        if (auto* pSC = caster ? caster->GetComponent<SkillComponent>() : nullptr)
            activationMod = pSC->GetCurrentActivationVFXMod();
        VFXModifier finalMod = MergeVFXModifiers(stats.vfxMod, activationMod);
        ApplyVFXModifier(def, finalMod);

        // 채널 모드: 파도 폭 좁힘 (40%)
        if (m_bChannelActive)
        {
            VFXModifier channelMod;
            channelMod.sizeScaleMult    = 0.4f;
            channelMod.particleCountMult = 0.45f;
            channelMod.strengthMult     = 0.6f;
            channelMod.speedMult        = 1.2f;
            ApplyVFXModifier(def, channelMod);
            m_hitHalfW = CHANNEL_WAVE_HALF_W;
        }

        m_damageMult = damageMultiplier > 0.f ? damageMultiplier : 1.f;
    }

    // 3. VFX 스폰 (1차 원소)
    m_vfxId = m_pVFXManager->SpawnEffectDef(origin, direction, def, /*isPlayer*/true);

    // 추가 원소 VFX (2차 이상)
    m_extraVFXIds.clear();
    for (size_t ei = 1; ei < stats.elementSet.size(); ++ei)
    {
        EffectDef extraDef = EffectRegistry::Get().GetEffect("Q_WaveSlash", runeFlags);
        applyElement(extraDef, stats.elementSet[ei], /*reduceCount*/true);
        int eid = m_pVFXManager->SpawnEffectDef(origin, direction, extraDef, /*isPlayer*/true);
        if (eid >= 0) m_extraVFXIds.push_back(eid);
    }

    // 서브 파티클 VFX 스폰 (EffectRegistry sub_* 이펙트)
    for (const auto& subId : stats.subVFXIds)
    {
        if (!EffectRegistry::Get().HasEffect(subId)) continue;
        EffectDef subDef = EffectRegistry::Get().GetEffect(subId, runeFlags);
        int sid = m_pVFXManager->SpawnEffectDef(origin, direction, subDef, /*isPlayer*/true);
        if (sid >= 0) m_extraVFXIds.push_back(sid);
    }

    wchar_t buf[256];
    swprintf_s(buf, 256, L"[WaveSlash] Execute: vfxId=%d, runeFlags=0x%X, dmgMult=%.1f\n",
        m_vfxId, runeFlags, damageMultiplier);
    OutputDebugString(buf);

    if (m_vfxId >= 0)
    {
        m_bWaveActive    = true;
        // m_damageMult은 위 vfxMod 블록에서 이미 설정됨
        m_waveElapsed    = 0.f;
        m_trailDropTimer = 0.f;
        m_hitEnemies.clear();
        m_fireTrail.clear();

        // 파도 진행로 데칼 — 발사 지점 지면에 스코치 자국
        if (m_pDecalManager)
        {
            XMFLOAT3 groundOrigin = origin;
            groundOrigin.y -= 5.0f; // origin.y가 +5 오프셋 적용됐으므로 원위치
            float rotY = atan2f(direction.x, direction.z);
            m_pDecalManager->Spawn(DecalTexture::Scorch3, groundOrigin, 4.0f, rotY, 4.f);
        }
    }
    else
    {
        m_bIsFinished = true;
    }
}

void WaveSlashBehavior::Update(float deltaTime)
{
    if (!m_pVFXManager) return;
    if (!m_bWaveActive && m_fireTrail.empty()) return;

    if (m_bWaveActive && m_vfxId >= 0)
    {
        m_waveElapsed += deltaTime;
        HitEnemiesInWave(m_SkillData.damage * m_damageMult);

        m_trailDropTimer += deltaTime;
        if (m_trailDropTimer >= TRAIL_DROP_INTERVAL)
        {
            m_trailDropTimer = 0.f;
            if (!m_bChannelActive)  // 채널 모드에서는 파이어 자국 없음
                DropFireTrail();
        }

        if (m_waveElapsed >= WAVE_DURATION)
            m_bWaveActive = false;
    }

    // 채널 종료 후 파도 자연 소멸 대기
    if (m_bPostChannelWaves)
    {
        m_channelPostTimer += deltaTime;
        if (m_channelPostTimer >= WAVE_DURATION)
        {
            m_bPostChannelWaves = false;
            m_channelWaveVfxIds.clear();  // VFX는 이미 waveMaxDist 도달로 자연 소멸
        }
    }

    // 파도가 끝난 뒤에도 trail이 남아있는 동안 DoT 계속 적용
    UpdateFireTrail(deltaTime);
}

void WaveSlashBehavior::HitEnemiesInWave(float damage)
{
    if (!m_pScene || m_vfxId < 0 || !m_pVFXManager) return;

    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    // 실제 파티클 선두 위치: VFX 스폰 원점 + elapsed × WAVE_PARTICLE_SPEED
    // (waveDist 타이머는 waveSpeed=10 m/s — 파티클보다 2배 느림)
    XMFLOAT3 waveOrigin = m_pVFXManager->GetWaveOrigin(m_vfxId);
    XMFLOAT3 waveDir    = m_pVFXManager->GetWaveDir(m_vfxId);
    XMVECTOR originV    = XMLoadFloat3(&waveOrigin);
    XMVECTOR dirV       = XMVector3Normalize(XMLoadFloat3(&waveDir));

    float hitFront = m_waveElapsed * WAVE_PARTICLE_SPEED;
    float hitBack  = (std::max)(0.f, hitFront - WAVE_HIT_DEPTH);

    const auto& gameObjects = pRoom->GetGameObjects();
    for (const auto& obj : gameObjects)
    {
        if (!obj) continue;
        EnemyComponent* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;
        if (m_hitEnemies.count(pEnemy)) continue;

        TransformComponent* pTransform = obj->GetTransform();
        if (!pTransform) continue;

        XMFLOAT3 ePos    = pTransform->GetPosition();
        XMVECTOR toEnemy = XMVectorSubtract(XMLoadFloat3(&ePos), originV);

        // 적 크기 반영 (FireBeam/Meteor 와 동일한 스타일) — 뚱뚱한 보스도 잘 맞게
        XMFLOAT3 eScale = pTransform->GetScale();
        float eRadius = max(0.f, max(eScale.x, eScale.z) * 0.9f);

        // 전진 방향 거리: 슬랩 범위 체크 (적 반경만큼 관대하게)
        float fwdProj = XMVectorGetX(XMVector3Dot(toEnemy, dirV));
        if (fwdProj < hitBack - eRadius || fwdProj > hitFront + eRadius) continue;

        // 수평 측면 거리 (적 반경만큼 관대하게)
        XMVECTOR lateralV = XMVectorSubtract(toEnemy, XMVectorScale(dirV, fwdProj));
        lateralV = XMVectorSetY(lateralV, 0.f);
        if (XMVectorGetX(XMVector3Length(lateralV)) > m_hitHalfW + eRadius) continue;

        // 수직 범위 (적 키만큼 관대하게)
        float yTolerance = max(0.f, eScale.y * 0.6f);
        if (fabsf(ePos.y - waveOrigin.y) > WAVE_HALF_H + yTolerance) continue;

        pEnemy->TakeDamage(damage, false);
        m_hitEnemies.insert(pEnemy);

        if (m_pCaster)
        {
            auto* pSC = m_pCaster->GetComponent<SkillComponent>();
            if (pSC && m_slot != SkillSlot::Count)
            {
                SkillStats sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
                if (!sts.onHitHooks.empty())
                {
                    SkillContext ctx;
                    ctx.caster             = m_pCaster;
                    ctx.baseDamage         = damage;
                    ctx.damageDealt        = damage;
                    ctx.hitEnemy           = pEnemy;
                    ctx.hitEnemyPos        = pTransform->GetPosition();
                    ctx.statusChanceMult   = sts.statusChanceMult;
                    ctx.statusDurationMult = sts.statusDurationMult;
                    for (auto& hook : sts.onHitHooks) hook(ctx);
                }
            }
        }
    }
}

void WaveSlashBehavior::DropFireTrail()
{
    if (m_vfxId < 0 || !m_pVFXManager) return;

    // GetWaveFrontPos: waveSpeed(10 m/s) 기준 박스 진행 위치 — 실제 파티클 선두(~20 m/s)보다 느림
    // → 파도가 지나간 자리에 자국이 깔리는 효과
    XMFLOAT3 waveOrigin = m_pVFXManager->GetWaveOrigin(m_vfxId);
    XMFLOAT3 waveDir    = m_pVFXManager->GetWaveDir(m_vfxId);
    XMFLOAT3 waveFront  = m_pVFXManager->GetWaveFrontPos(m_vfxId);

    XMFLOAT3 frontPos;
    frontPos = waveFront;
    frontPos.y = 0.f;

    XMVECTOR fwdV    = XMVector3Normalize(XMLoadFloat3(&waveDir));
    XMVECTOR worldUp = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    XMVECTOR rightV  = XMVector3Normalize(XMVector3Cross(worldUp, fwdV));
    XMFLOAT3 waveRight;
    XMStoreFloat3(&waveRight, rightV);

    FireZone zone;
    zone.center     = frontPos;
    zone.lifetime   = TRAIL_LIFETIME;
    zone.tickTimer  = 0.f;
    zone.trailVfxId = m_pVFXManager->SpawnFireTrailEffect(
        frontPos, waveRight, WAVE_HALF_W, TRAIL_LIFETIME);
    m_fireTrail.push_back(zone);
}

void WaveSlashBehavior::UpdateFireTrail(float deltaTime)
{
    if (!m_pScene || m_fireTrail.empty()) return;

    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    const auto& gameObjects = pRoom->GetGameObjects();
    float dotDamage = m_SkillData.damage * m_damageMult * TRAIL_DMG_MULT;

    for (auto& zone : m_fireTrail)
    {
        zone.lifetime  -= deltaTime;
        zone.tickTimer += deltaTime;

        if (zone.tickTimer < TRAIL_TICK_INTERVAL) continue;
        zone.tickTimer = 0.f;

        // 존 안의 적에게 DoT (경직 없음)
        for (const auto& obj : gameObjects)
        {
            if (!obj) continue;
            EnemyComponent* pEnemy = obj->GetComponent<EnemyComponent>();
            if (!pEnemy || pEnemy->IsDead()) continue;

            TransformComponent* pT = obj->GetTransform();
            if (!pT) continue;

            XMFLOAT3 ePos = pT->GetPosition();
            float dx = ePos.x - zone.center.x;
            float dz = ePos.z - zone.center.z;
            if (dx * dx + dz * dz <= TRAIL_ZONE_RADIUS * TRAIL_ZONE_RADIUS)
            {
                pEnemy->TakeDamage(dotDamage, false);
            }
        }
    }

    // 만료된 존: StopEffect 없이 trailVfxId만 해제 — VFX 시퀀스의 fade-out 페이즈로 자연 소멸
    for (auto& zone : m_fireTrail)
    {
        if (zone.lifetime <= 0.f)
            zone.trailVfxId = -1;
    }
    m_fireTrail.erase(
        std::remove_if(m_fireTrail.begin(), m_fireTrail.end(),
            [](const FireZone& z) { return z.lifetime <= 0.f; }),
        m_fireTrail.end());
}

bool WaveSlashBehavior::IsFinished() const
{
    return !m_bWaveActive && m_fireTrail.empty() && !m_bPostChannelWaves;
}

void WaveSlashBehavior::Reset()
{
    if (m_pVFXManager)
    {
        for (auto& zone : m_fireTrail)
            if (zone.trailVfxId >= 0) m_pVFXManager->StopEffect(zone.trailVfxId);
        for (int eid : m_extraVFXIds)
            if (eid >= 0) m_pVFXManager->StopEffect(eid);
    }
    if (m_pVFXManager)
    {
        if (m_channelAmbientId  >= 0) m_pVFXManager->StopEffect(m_channelAmbientId);
        for (int id : m_channelWaveVfxIds)  // 아직 소멸 전 파도 강제 정리
            if (id >= 0) m_pVFXManager->StopEffect(id);
        if (m_chargeVFXId       >= 0) m_pVFXManager->StopEffect(m_chargeVFXId);
        if (m_enhanceAuraId     >= 0) m_pVFXManager->StopEffect(m_enhanceAuraId);
    }
    m_bIsFinished       = true;
    m_bWaveActive       = false;
    m_bChannelActive    = false;
    m_cachedElem        = ElementType::None;
    m_bPostChannelWaves = false;
    m_channelPostTimer  = 0.f;
    m_hitHalfW          = WAVE_HALF_W;
    m_vfxId             = -1;
    m_channelAmbientId  = -1;
    m_channelWaveVfxIds.clear();
    m_chargeVFXId       = -1;
    m_enhanceAuraId    = -1;
    m_extraVFXIds.clear();
    m_hitEnemies.clear();
    m_fireTrail.clear();
}

uint32_t WaveSlashBehavior::GetRuneFlags(GameObject* caster) const
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
