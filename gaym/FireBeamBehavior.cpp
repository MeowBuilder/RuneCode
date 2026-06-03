#include "stdafx.h"
#include "FireBeamBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "FluidParticle.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "PlayerComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include "Dx12App.h"

static void ApplyElementColors(EffectDef& def, ElementType e)
{
    FluidElementColor ec = FluidElementColors::Get(e);
    def.element = e;
    for (auto& l : def.layers) {
        l.element   = e;
        l.coreColor = ec.coreColor;
        l.edgeColor = ec.edgeColor;
    }
}

// 코어 빔: swirlSpeed=0, 좁은 spreadRadius → 빔 축을 따라 빽빽하게 흐르는 직선
EffectDef FireBeamBehavior::BuildCoreBeamDef()
{
    EffectDef def;
    def.name    = "E_FireBeam_Core";
    def.element = ElementType::Fire;

    EffectLayer layer;
    layer.type       = EmitterType::SPH_Beam;
    layer.element    = ElementType::Fire;
    layer.coreColor  = { 1.0f, 0.92f, 0.55f, 1.0f };
    layer.edgeColor  = { 1.0f, 0.55f, 0.10f, 0.80f };

    SPHEmitterParams& s = layer.sph;
    s.particleCount = 300;
    s.spawnRadius   = 0.1f;
    s.particleSize  = 0.35f;

    VFXPhase p;
    p.startTime             = 0.f;
    p.duration              = 99.f;
    p.motionMode            = ParticleMotionMode::Beam;
    p.beamDesc.speedMin     = 10.f;
    p.beamDesc.speedMax     = 16.f;
    p.beamDesc.spreadRadius = 0.15f;
    p.beamDesc.swirlSpeed   = 0.f;
    p.beamDesc.swirlExpand  = false;
    p.beamDesc.enableFlow   = true;
    s.phases.push_back(p);

    def.layers.push_back(std::move(layer));
    return def;
}

// 나선: swirlExpand=true → 시작점에서 퍼지며 공전, swirlFadeEnd=10m에서 소멸
EffectDef FireBeamBehavior::BuildSwirlDef()
{
    EffectDef def;
    def.name    = "E_FireBeam_Swirl";
    def.element = ElementType::Fire;

    EffectLayer layer;
    layer.type       = EmitterType::SPH_Beam;
    layer.element    = ElementType::Fire;
    layer.coreColor  = { 0.95f, 0.10f, 0.02f, 0.85f };
    layer.edgeColor  = { 0.55f, 0.04f, 0.00f, 0.50f };

    SPHEmitterParams& s = layer.sph;
    s.particleCount = 80;
    s.spawnRadius   = 0.1f;
    s.particleSize  = 0.18f;

    VFXPhase p;
    p.startTime              = 0.f;
    p.duration               = 99.f;
    p.motionMode             = ParticleMotionMode::Beam;
    p.beamDesc.speedMin      = 7.f;
    p.beamDesc.speedMax      = 12.f;
    p.beamDesc.spreadRadius  = 2.0f;
    p.beamDesc.swirlSpeed    = 10.f;
    p.beamDesc.swirlExpand   = true;
    p.beamDesc.swirlFadeEnd  = 10.f;
    p.beamDesc.swirlFadeInOut = true;
    p.beamDesc.enableFlow    = true;
    s.phases.push_back(p);

    def.layers.push_back(std::move(layer));
    return def;
}

// 방사 스파크
EffectDef FireBeamBehavior::BuildBurstDef()
{
    EffectDef def;
    def.name    = "E_FireBeam_Burst";
    def.element = ElementType::Fire;

    EffectLayer layer;
    layer.type       = EmitterType::SPH_Beam;
    layer.element    = ElementType::Fire;
    layer.coreColor  = { 1.00f, 0.32f, 0.02f, 1.00f };
    layer.edgeColor  = { 0.80f, 0.10f, 0.01f, 0.80f };

    SPHEmitterParams& s = layer.sph;
    s.particleCount = 120;
    s.spawnRadius   = 0.1f;
    s.particleSize  = 0.12f;

    VFXPhase p;
    p.startTime              = 0.f;
    p.duration               = 99.f;
    p.motionMode             = ParticleMotionMode::Beam;
    p.beamDesc.speedMin      = 14.f;
    p.beamDesc.speedMax      = 20.f;
    p.beamDesc.spreadRadius  = 3.5f;
    p.beamDesc.swirlSpeed    = 0.f;
    p.beamDesc.swirlExpand   = true;
    p.beamDesc.swirlFadeEnd  = 1.5f;
    p.beamDesc.enableFlow    = true;
    s.phases.push_back(p);

    def.layers.push_back(std::move(layer));
    return def;
}

FireBeamBehavior::FireBeamBehavior()
{
    m_SkillData.name = "FireBeam";
    m_SkillData.element = ElementType::Fire;
    m_SkillData.activationType = ActivationType::Channel;
    m_SkillData.damage = 15.0f;
    m_SkillData.cooldown = 4.0f;
    m_SkillData.castTime = 0.0f;
    m_SkillData.range = 20.0f;
    m_SkillData.radius = 1.0f;
    m_SkillData.manaCost = 30.0f;
}

FireBeamBehavior::FireBeamBehavior(const SkillData& customData)
    : m_SkillData(customData)
{
}

void FireBeamBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    if (!m_pVFXManager)
    {
        OutputDebugString(L"[FireBeam] Warning: No VFXManager set!\n");
        m_bIsFinished = true;
        return;
    }

    m_pCaster = caster;
    m_lastTargetPos = targetPosition;

    if (!m_bIsActive)
    {
        m_bIsFinished = false;
        m_bIsActive   = true;
        m_damageMult  = damageMultiplier > 0.f ? damageMultiplier : 1.f;
        m_hitTimer    = 0.f;

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
            XMStoreFloat3(&origin, XMVectorAdd(XMLoadFloat3(&origin),
                XMVectorScale(dirV, 1.3f)));
        }

        ElementType elemType = ElementType::Fire;
        if (caster) {
            if (auto* pPC = caster->GetComponent<PlayerComponent>())
                elemType = pPC->GetElementType();
        }

        // 변환 룬 원소가 있으면 우선 적용
        SkillStats stats;
        if (caster) {
            auto* pSkillComp = caster->GetComponent<SkillComponent>();
            if (pSkillComp && m_slot != SkillSlot::Count)
                stats = pSkillComp->BuildSkillStats(m_slot, m_SkillData.activationType);
        }
        if (!stats.elementSet.empty())
            elemType = stats.elementSet[0];

        EffectDef coreDef  = BuildCoreBeamDef();
        EffectDef swirlDef = BuildSwirlDef();
        EffectDef burstDef = BuildBurstDef();
        ApplyElementColors(coreDef,  elemType);
        ApplyElementColors(swirlDef, elemType);
        ApplyElementColors(burstDef, elemType);

        // 룬 vfxMod + 활성화 vfxMod 병합하여 빔 두께/밀도 스케일 적용
        {
            VFXModifier activationMod;
            if (auto* pSC = caster ? caster->GetComponent<SkillComponent>() : nullptr)
                activationMod = pSC->GetCurrentActivationVFXMod();
            VFXModifier finalMod = MergeVFXModifiers(stats.vfxMod, activationMod);
            ApplyVFXModifier(coreDef,  finalMod);
            ApplyVFXModifier(swirlDef, finalMod);
            // burstDef는 진입 스파크 — 사이즈만 적용
            VFXModifier burstMod;
            burstMod.sizeScaleMult = finalMod.sizeScaleMult;
            ApplyVFXModifier(burstDef, burstMod);
        }

        m_vfxCoreId  = m_pVFXManager->SpawnEffectDef(origin, direction, coreDef,  true);
        m_vfxSwirlId = m_pVFXManager->SpawnEffectDef(origin, direction, swirlDef, true);
        m_vfxBurstId = m_pVFXManager->SpawnEffectDef(origin, direction, burstDef, true);

        // 서브 파티클 VFX 스폰 (EffectRegistry sub_* 이펙트)
        m_subVFXIds.clear();
        for (const auto& subId : stats.subVFXIds)
        {
            if (!EffectRegistry::Get().HasEffect(subId)) continue;
            EffectDef subDef = EffectRegistry::Get().GetEffect(subId);
            int sid = m_pVFXManager->SpawnEffectDef(origin, direction, subDef, true);
            if (sid >= 0) m_subVFXIds.push_back(sid);
        }

        wchar_t buf[96];
        swprintf_s(buf, 96, L"[FireBeam] Started core=%d swirl=%d burst=%d\n",
            m_vfxCoreId, m_vfxSwirlId, m_vfxBurstId);
        OutputDebugString(buf);
    }
    else
    {
        if (caster && caster->GetTransform())
        {
            XMFLOAT3 origin = caster->GetTransform()->GetPosition();
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

            XMFLOAT3 direction;
            XMStoreFloat3(&direction, dirV);
            if (m_vfxCoreId  >= 0) m_pVFXManager->TrackEffect(m_vfxCoreId,  origin, direction);
            if (m_vfxSwirlId >= 0) m_pVFXManager->TrackEffect(m_vfxSwirlId, origin, direction);
            if (m_vfxBurstId >= 0) m_pVFXManager->TrackEffect(m_vfxBurstId, origin, direction);
            for (int sid : m_subVFXIds)
                if (sid >= 0) m_pVFXManager->TrackEffect(sid, origin, direction);
        }
    }
}

void FireBeamBehavior::Update(float deltaTime)
{
    if (!m_bIsActive || !m_pVFXManager || !m_pCaster) return;
    if (!m_pCaster->GetTransform()) return;

    // echo 모드: 타겟 추적 + 시간 만료 시 종료
    if (m_echoTimeLeft > 0.f)
    {
        m_echoTimeLeft -= deltaTime;
        if (m_echoTimeLeft <= 0.f)
        {
            Reset();
            return;
        }
        // echo origin 기준으로 가장 가까운 적 방향 갱신 (위치 고정, 방향만 회전)
        if (m_pScene && m_pCaster && m_pCaster->GetTransform())
        {
            EnemyComponent* pNearest = m_pScene->FindNearestEnemy(
                m_pCaster->GetTransform()->GetPosition());
            if (pNearest && pNearest->GetOwner() && pNearest->GetOwner()->GetTransform())
            {
                XMFLOAT3 ePos = pNearest->GetOwner()->GetTransform()->GetPosition();
                XMVECTOR dirV = XMVectorSetY(
                    XMLoadFloat3(&ePos) - XMLoadFloat3(&m_echoOrigin), 0.f);
                if (XMVectorGetX(XMVector3LengthSq(dirV)) > 0.001f)
                    XMStoreFloat3(&m_overrideDir, XMVector3Normalize(dirV));
            }
        }
    }

    // echo 모드: 발동 시점 위치 고정 / 일반 모드: 캐릭터 현재 위치
    XMFLOAT3 origin;
    if (m_echoTimeLeft > 0.f)
        origin = m_echoOrigin;
    else
    {
        origin = m_pCaster->GetTransform()->GetPosition();
        origin.y += 1.5f;
    }

    // echo 모드면 고정 방향, 일반 모드면 플레이어 시선
    XMVECTOR dirV;
    if (m_overrideDir.x != 0.f || m_overrideDir.z != 0.f)
        dirV = XMVector3Normalize(XMLoadFloat3(&m_overrideDir));
    else
    {
        dirV = m_pCaster->GetTransform()->GetLook();
        dirV = XMVectorSetY(dirV, 0.f);
        dirV = XMVector3Normalize(dirV);
    }

    XMFLOAT3 dir;
    XMStoreFloat3(&dir, dirV);

    XMFLOAT3 offsetOrigin;
    XMStoreFloat3(&offsetOrigin, XMVectorAdd(XMLoadFloat3(&origin),
        XMVectorScale(dirV, 1.3f)));

    if (m_vfxCoreId  >= 0) m_pVFXManager->TrackEffect(m_vfxCoreId,  offsetOrigin, dir);
    if (m_vfxSwirlId >= 0) m_pVFXManager->TrackEffect(m_vfxSwirlId, offsetOrigin, dir);
    if (m_vfxBurstId >= 0) m_pVFXManager->TrackEffect(m_vfxBurstId, offsetOrigin, dir);
    for (int sid : m_subVFXIds)
        if (sid >= 0) m_pVFXManager->TrackEffect(sid, offsetOrigin, dir);

    m_hitTimer += deltaTime;
    if (m_hitTimer >= HIT_INTERVAL) {
        m_hitTimer -= HIT_INTERVAL;
        const XMFLOAT3* pDir = (m_overrideDir.x != 0.f || m_overrideDir.z != 0.f) ? &m_overrideDir : nullptr;
        HitEnemiesInBeam(m_SkillData.damage * m_damageMult, pDir);
    }
}

void FireBeamBehavior::HitEnemiesInBeam(float damage, const XMFLOAT3* pDirOverride)
{
    if (!m_pScene || !m_pCaster || !m_pCaster->GetTransform()) return;

    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMFLOAT3 originF;
    if (m_echoTimeLeft > 0.f)
        originF = m_echoOrigin;
    else
    {
        originF = m_pCaster->GetTransform()->GetPosition();
        originF.y += 1.5f;
    }

    XMVECTOR originV = XMLoadFloat3(&originF);
    XMVECTOR dirV;
    if (pDirOverride)
        dirV = XMVector3Normalize(XMLoadFloat3(pDirOverride));
    else
    {
        dirV = m_pCaster->GetTransform()->GetLook();
        dirV = XMVectorSetY(dirV, 0.f);
        dirV = XMVector3Normalize(dirV);
    }

    SkillStats sts;
    bool hasStats = false;
    {
        auto* pSC = m_pCaster->GetComponent<SkillComponent>();
        if (pSC && m_slot != SkillSlot::Count) {
            sts = pSC->BuildSkillStats(m_slot, m_SkillData.activationType);
            hasStats = true;
        }
    }
    bool bExec = HasExecRune(m_pCaster);

    const auto& gameObjects = pRoom->GetGameObjects();
    for (const auto& obj : gameObjects)
    {
        if (!obj) continue;
        EnemyComponent* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;

        TransformComponent* pTransform = obj->GetTransform();
        if (!pTransform) continue;

        XMFLOAT3 ePos = pTransform->GetPosition();
        ePos.y += 1.0f;
        XMVECTOR toEnemyV = XMVectorSubtract(XMLoadFloat3(&ePos), originV);

        float fwdProj = XMVectorGetX(XMVector3Dot(toEnemyV, dirV));
        if (fwdProj < 0.f || fwdProj > BEAM_RANGE) continue;

        XMVECTOR lateralV = XMVectorSubtract(toEnemyV, XMVectorScale(dirV, fwdProj));
        float lateralDist = XMVectorGetX(XMVector3Length(lateralV));

        XMFLOAT3 eScale = pTransform->GetScale();
        float eRadius = max(1.5f, max(eScale.x, eScale.z) * 1.5f);

        if (lateralDist < BEAM_RADIUS + eRadius)
        {
            pEnemy->TakeDamage(ApplyExecBonus(damage, pEnemy, m_pCaster), false, bExec);
            NotifyHit(m_pCaster, ePos);

            if (hasStats && !sts.onHitHooks.empty())
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

void FireBeamBehavior::OnEchoFire(GameObject* caster, const XMFLOAT3& targetPos, float mult)
{
    if (!caster || !caster->GetTransform()) return;

    // 발동 시점 위치 고정 (echo 동안 캐릭터 이동 무관)
    XMFLOAT3 origin = caster->GetTransform()->GetPosition();
    origin.y += 1.5f;
    m_echoOrigin = origin;

    XMVECTOR originV = XMLoadFloat3(&origin);
    XMVECTOR dirV = XMVectorSetY(XMLoadFloat3(&targetPos) - originV, 0.f);
    if (XMVectorGetX(XMVector3LengthSq(dirV)) < 0.001f) return;
    XMStoreFloat3(&m_overrideDir, XMVector3Normalize(dirV));

    // 이미 빔이 켜져 있으면 강제 종료 후 재시작
    if (m_bIsActive) Reset();

    m_echoTimeLeft = ECHO_DURATION;
    Execute(caster, targetPos, mult);  // VFX 스폰 + m_bIsActive = true
}

void FireBeamBehavior::OnPlaceTrigger(GameObject* caster, const XMFLOAT3& trapPos, float mult)
{
    Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
    EnemyComponent* pNearest = pScene ? pScene->FindNearestEnemy(trapPos) : nullptr;

    XMFLOAT3 target = trapPos;
    if (pNearest && pNearest->GetOwner() && pNearest->GetOwner()->GetTransform())
        target = pNearest->GetOwner()->GetTransform()->GetPosition();

    OnEchoFire(caster, target, mult);
}

bool FireBeamBehavior::IsFinished() const
{
    return m_bIsFinished;
}

void FireBeamBehavior::Reset()
{
    if (m_bIsActive)
    {
        if (m_pVFXManager)
        {
            if (m_vfxCoreId  >= 0) m_pVFXManager->StopEffect(m_vfxCoreId);
            if (m_vfxSwirlId >= 0) m_pVFXManager->StopEffect(m_vfxSwirlId);
            if (m_vfxBurstId >= 0) m_pVFXManager->StopEffect(m_vfxBurstId);
            for (int sid : m_subVFXIds)
                if (sid >= 0) m_pVFXManager->StopEffect(sid);
            OutputDebugString(L"[FireBeam] Stopped\n");
        }
    }
    m_bIsFinished  = true;
    m_bIsActive    = false;
    m_vfxCoreId    = -1;
    m_vfxSwirlId   = -1;
    m_vfxBurstId   = -1;
    m_subVFXIds.clear();
    m_pCaster      = nullptr;
    m_overrideDir  = { 0.f, 0.f, 0.f };
    m_echoOrigin   = { 0.f, 0.f, 0.f };
    m_echoTimeLeft = 0.f;
}

uint32_t FireBeamBehavior::GetRuneFlags(GameObject* caster) const
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
