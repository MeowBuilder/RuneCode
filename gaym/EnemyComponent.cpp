#include "stdafx.h"
#include "EnemyComponent.h"
#include "DamageNumberManager.h"
#include "VFXSpriteManager.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "IAttackBehavior.h"
#include "AnimationComponent.h"
#include "PlayerComponent.h"
#include "VFXManager.h"
#include "Room.h"
#include "Scene.h"
#include "Dx12App.h"
#include "MathUtils.h"
#include "BossPhaseController.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include <algorithm>
#include <string>

extern void WriteNetworkLog(const std::string& msg);

EnemyComponent::EnemyComponent(GameObject* pOwner)
    : Component(pOwner)
{
}

EnemyComponent::~EnemyComponent()
{
    ClearOrbitingSwords();
}

// ── Orbiting Swords (DarkLord 봉인 검 — attack behavior 종료 후 검 자율 회전) ──
void EnemyComponent::AddOrbitingSword(const OrbitingSwordEntry& entry)
{
    m_vOrbitingSwords.push_back(entry);
}

void EnemyComponent::SetOrbitingSwordParams(float fRadius, float fSpeedDeg, float fYOffset,
                                            float fDamage, float fHitRadius, float fLifetime,
                                            ElementType element)
{
    m_fOrbitRadius       = fRadius;
    m_fOrbitSpeedDeg     = fSpeedDeg;
    m_fOrbitYOffset      = fYOffset;
    m_fOrbitDamage       = fDamage;
    m_fOrbitHitRadius    = fHitRadius;
    m_fOrbitRemainingSec = fLifetime;
    m_fOrbitAngleDeg     = 0.0f;
    m_fOrbitDmgCooldown  = 0.0f;
    m_eOrbitElement      = element;
}

void EnemyComponent::ClearOrbitingSwords()
{
    Scene* pScene = nullptr;
    if (m_pRoom) pScene = m_pRoom->GetScene();
    for (auto& sw : m_vOrbitingSwords)
    {
        if (pScene)
        {
            if (sw.vfxSlot >= 0)
                if (auto* pVFX = pScene->GetVFXManager())
                    pVFX->Stop(sw.vfxSlot);
            if (sw.pObj) pScene->MarkForDeletion(sw.pObj);
        }
    }
    m_vOrbitingSwords.clear();
    m_fOrbitRemainingSec = 0.0f;
}

void EnemyComponent::UpdateOrbitingSwords(float dt)
{
    if (m_vOrbitingSwords.empty()) return;

    m_fOrbitRemainingSec -= dt;
    if (m_fOrbitRemainingSec <= 0.0f)
    {
        ClearOrbitingSwords();
        return;
    }

    GameObject* pOwner = GetOwner();
    if (!pOwner) return;
    TransformComponent* pBossT = pOwner->GetTransform();
    if (!pBossT) return;
    XMFLOAT3 bossPos = pBossT->GetPosition();

    m_fOrbitAngleDeg += m_fOrbitSpeedDeg * dt;
    if (m_fOrbitAngleDeg > 360.0f) m_fOrbitAngleDeg -= 360.0f;

    Scene* pScene = m_pRoom ? m_pRoom->GetScene() : nullptr;
    VFXManager* pVFX = pScene ? pScene->GetVFXManager() : nullptr;

    // 검 transform + VFX 추적
    for (auto& sw : m_vOrbitingSwords)
    {
        if (!sw.pObj) continue;
        float angDeg = sw.baseAngleDeg + m_fOrbitAngleDeg;
        float yawRad = XMConvertToRadians(angDeg);
        XMFLOAT3 pos = {
            bossPos.x + sinf(yawRad) * m_fOrbitRadius,
            bossPos.y + m_fOrbitYOffset,
            bossPos.z + cosf(yawRad) * m_fOrbitRadius
        };
        auto* pT = sw.pObj->GetTransform();
        if (pT)
        {
            pT->SetPosition(pos);
            pT->SetRotation(0.0f, angDeg + 90.0f, 0.0f);
        }
        if (pVFX && sw.vfxSlot >= 0)
        {
            XMFLOAT3 dirOut = { sinf(yawRad), 0.0f, cosf(yawRad) };
            pVFX->Track(sw.vfxSlot, pos, dirOut);
        }
    }

    // 플레이어 충돌 데미지 — 쿨다운 간격으로 (다중 검 동시 hit 방지)
    if (m_fOrbitDmgCooldown > 0.0f) m_fOrbitDmgCooldown -= dt;
    if (m_fOrbitDmgCooldown <= 0.0f && pScene)
    {
        auto vPlayers = pScene->GetAllPlayers();
        for (GameObject* pPO : vPlayers)
        {
            if (!pPO) continue;
            auto* pPT = pPO->GetTransform();
            if (!pPT) continue;
            PlayerComponent* pPC = pPO->GetComponent<PlayerComponent>();
            if (!pPC) continue;
            XMFLOAT3 pp = pPT->GetPosition();
            bool bHit = false;
            for (auto& sw : m_vOrbitingSwords)
            {
                if (!sw.pObj) continue;
                XMFLOAT3 sp = sw.pObj->GetTransform()->GetPosition();
                float dx = pp.x - sp.x;
                float dz = pp.z - sp.z;
                if (sqrtf(dx*dx + dz*dz) <= m_fOrbitHitRadius)
                { bHit = true; break; }
            }
            if (bHit)
            {
                pPC->TakeDamage(m_fOrbitDamage);
                m_fOrbitDmgCooldown = 0.55f;
                break;
            }
        }
    }
}

void EnemyComponent::Update(float deltaTime)
{
    // Orbiting swords (independent of attack behavior — 보스가 다른 패턴 진행 중에도 검 유지)
    UpdateOrbitingSwords(deltaTime);

    // Decay hit flash every frame
    if (m_fHitFlashTimer > 0.f)
    {
        m_fHitFlashTimer -= deltaTime;
        float flash = (m_fHitFlashTimer > 0.f) ? (m_fHitFlashTimer / FLASH_DURATION) : 0.f;
        // 계층 전체 전파 — 자식 mesh 도 플래시 CB 반영 (SetHitFlash 는 root 만 설정)
        if (m_pOwner) m_pOwner->SetHitFlashAll(flash);
    }

    // AI 일시정지 (4스테이지 비행 보스 — Scene 이 transform 직접 제어)
    if (m_bAIPaused && m_eCurrentState != EnemyState::Attack)
        return;

    // 방어 분쇄 디버프 타이머
    if (m_fDefenseDebuffTimer > 0.f)
    {
        m_fDefenseDebuffTimer -= deltaTime;
        if (m_fDefenseDebuffTimer <= 0.f)
            m_fDefenseMult = 1.0f;
    }

    // 상태이상 업데이트
    UpdateStatusEffects(deltaTime);

    // Boss intro cutscene takes priority
    if (IsInIntro())
    {
        UpdateBossIntro(deltaTime);
        return;
    }

    // Boss phase transition takes priority
    if (m_pPhaseController && m_pPhaseController->IsInTransition())
    {
        m_pPhaseController->Update(deltaTime);
        return;  // 페이즈 전환 중에는 일반 AI 스킵
    }

    // Flying enemies maintain altitude, ground enemies use gravity
    if (m_bIsFlying)
    {
        auto* pTransform = m_pOwner ? m_pOwner->GetTransform() : nullptr;
        if (pTransform)
        {
            XMFLOAT3 pos = pTransform->GetPosition();
            // Smoothly maintain fly height
            float targetY = m_fFlyHeight;
            pos.y = pos.y + (targetY - pos.y) * 3.0f * deltaTime;
            pTransform->SetPosition(pos);
        }
    }
    else if (!m_bOnGround)
    {
        // Apply gravity for ground enemies
        auto* pTransform = m_pOwner ? m_pOwner->GetTransform() : nullptr;
        if (pTransform)
        {
            XMFLOAT3 pos = pTransform->GetPosition();
            m_fVelocityY -= GRAVITY * deltaTime;
            pos.y += m_fVelocityY * deltaTime;

            if (pos.y <= GROUND_Y)
            {
                pos.y = GROUND_Y;
                m_fVelocityY = 0.0f;
                m_bOnGround = true;
            }
            pTransform->SetPosition(pos);
        }
    }

    // Update cooldown timers
    if (m_fAttackCooldownTimer > 0.0f)
    {
        m_fAttackCooldownTimer -= deltaTime;
    }
    if (m_fSpecialCooldownTimer > 0.0f)
    {
        m_fSpecialCooldownTimer -= deltaTime;
    }
    if (m_fFlyingCooldownTimer > 0.0f)
    {
        m_fFlyingCooldownTimer -= deltaTime;
    }

    // Threat table update (distance-based threat decay/gain)
    auto* pTransform = m_pOwner ? m_pOwner->GetTransform() : nullptr;
    if (pTransform)
    {
        m_ThreatTable.Update(deltaTime, pTransform->GetPosition());
    }

    // Target reevaluation (every 0.5 seconds)
    m_fTargetReevaluationTimer += deltaTime;
    if (m_fTargetReevaluationTimer >= ThreatConstants::TARGET_REEVALUATION_INTERVAL)
    {
        m_fTargetReevaluationTimer = 0.0f;
        ReevaluateTarget();
    }

    // State machine
    switch (m_eCurrentState)
    {
    case EnemyState::Idle:
        UpdateIdle(deltaTime);
        break;
    case EnemyState::Chase:
        UpdateChase(deltaTime);
        break;
    case EnemyState::Attack:
        UpdateAttack(deltaTime);
        break;
    case EnemyState::Stagger:
        UpdateStagger(deltaTime);
        break;
    case EnemyState::Dead:
        UpdateDead(deltaTime);
        break;
    }
}

void EnemyComponent::ChangeState(EnemyState newState)
{
    if (m_eCurrentState == newState) return;
    if (m_eCurrentState == EnemyState::Dead) return; // Can't change state if dead

    EnemyState oldState = m_eCurrentState;
    m_eCurrentState = newState;

    // Hide indicators when leaving Attack state
    if (oldState == EnemyState::Attack)
    {
        HideIndicators();
        // 활성 behavior 의 Reset 호출 — 보스 시그니처(GroundRupture/GaleSlash/Shockwave/Tornado/RockFall)
        //   가 자체 indicator GameObject 들을 관리하므로, behavior.Reset → CleanupAll → MarkForDeletion.
        //   ComboAttackBehavior 도 Reset 에서 검기 ribbon/crescent/sword glow 모두 정리.
        //   누락 시: 보스가 stagger/dead/phase 전환으로 attack 중단되면 indicator 영구 잔존.
        IAttackBehavior* pActiveBehavior = m_bUsingFlyingAttack  ? m_pFlyingAttackBehavior.get()
                                         : m_bUsingSpecialAttack ? m_pSpecialAttackBehavior.get()
                                         :                         m_pAttackBehavior.get();
        if (pActiveBehavior) pActiveBehavior->Reset();
        // 공격 behavior 가 일시적으로 재생속도 override 했을 수 있으니 복원
        if (m_pAnimationComp && m_fBaseAnimPlaybackSpeed > 0.0f)
            m_pAnimationComp->SetPlaybackSpeed(m_fBaseAnimPlaybackSpeed);
    }

    // Animation transition
    if (m_pAnimationComp)
    {
        switch (newState)
        {
        case EnemyState::Idle:
            m_pAnimationComp->CrossFade(m_AnimConfig.m_strIdleClip, 0.2f, m_AnimConfig.m_bLoopIdle);
            break;
        case EnemyState::Chase:
            // 강제 재시작 — 직전이 non-loop 공격이었다면 m_bIsPlaying=false 로 얼어있을 수 있음
            m_pAnimationComp->CrossFade(m_AnimConfig.m_strChaseClip, 0.2f, m_AnimConfig.m_bLoopChase, true);
            break;
        case EnemyState::Attack:
        {
            // 행동별 전용 클립이 있으면 그걸 우선 사용
            IAttackBehavior* pActiveBehavior = m_bUsingFlyingAttack  ? m_pFlyingAttackBehavior.get()
                                             : m_bUsingSpecialAttack ? m_pSpecialAttackBehavior.get()
                                             :                         m_pAttackBehavior.get();
            const char* pClip = (pActiveBehavior && pActiveBehavior->GetAnimClipName()[0] != '\0')
                               ? pActiveBehavior->GetAnimClipName()
                               : m_AnimConfig.m_strAttackClip.c_str();
            // behavior 가 loop 여부 결정 (default true). preset 이 false 로 막아놓으면 최종 false
            bool bLoopAttack = pActiveBehavior ? pActiveBehavior->ShouldLoopAnim()
                                                : m_AnimConfig.m_bLoopAttack;
            if (!m_AnimConfig.m_bLoopAttack) bLoopAttack = false;
            m_pAnimationComp->CrossFade(pClip, 0.15f, bLoopAttack, true);
            break;
        }
        case EnemyState::Stagger:
            m_pAnimationComp->CrossFade(m_AnimConfig.m_strStaggerClip, 0.1f, m_AnimConfig.m_bLoopStagger);
            break;
        case EnemyState::Dead:
            m_pAnimationComp->CrossFade(m_AnimConfig.m_strDeathClip, 0.1f, m_AnimConfig.m_bLoopDeath);
            break;
        }
    }

    // State entry actions
    switch (newState)
    {
    case EnemyState::Stagger:
        m_fStaggerTimer = STAGGER_DURATION;
        break;
    case EnemyState::Dead:
        m_fDeadTimer = DEAD_LINGER_TIME;
        Die();
        break;
    case EnemyState::Attack:
        {
            // Execute the appropriate attack behavior (priority: flying > special > primary)
            IAttackBehavior* pBehavior = nullptr;
            if (m_bUsingFlyingAttack && m_pFlyingAttackBehavior)
            {
                pBehavior = m_pFlyingAttackBehavior.get();
            }
            else if (m_bUsingSpecialAttack && m_pSpecialAttackBehavior)
            {
                pBehavior = m_pSpecialAttackBehavior.get();
            }
            else
            {
                pBehavior = m_pAttackBehavior.get();
            }

            if (pBehavior)
            {
                pBehavior->Execute(this);
            }
            m_fIndicatorTimer = 0.0f;
            ShowIndicators();
        }
        break;
    }
}

void EnemyComponent::TakeDamage(float fDamage, bool bTriggerStagger, bool bExecRune)
{
    if (m_eCurrentState == EnemyState::Dead) return;

    // Invincible enemies ignore all damage (during special attacks like flying)
    if (m_bInvincible)
    {
        OutputDebugString(L"[Enemy] Damage blocked - Invincible!\n");
        return;
    }

    fDamage /= m_fDefenseMult;  // 방어 분쇄: defenseMult < 1이면 데미지 증가
    m_Stats.m_fCurrentHP -= fDamage;

    // Hit flash — 계층 전체 전파 (자식 mesh 까지) 해야 실제로 보임
    m_fHitFlashTimer = FLASH_DURATION;
    if (m_pOwner) m_pOwner->SetHitFlashAll(1.f);

    // Floating damage number
    if (m_pOwner && m_pOwner->GetTransform())
    {
        XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
        pos.y += 2.0f;
        DamageNumberManager::Get().AddNumber(pos, fDamage);
    }

    // Boss Phase System: HP 변화 알림
    if (m_pPhaseController)
    {
        m_pPhaseController->OnHealthChanged(m_Stats.m_fCurrentHP, m_Stats.m_fMaxHP);
    }

    if (m_Stats.m_fCurrentHP <= 0.0f)
    {
        m_Stats.m_fCurrentHP = 0.0f;
        ChangeState(EnemyState::Dead);

        // 킬 타격이 처형자 룬 스킬인 경우에만 skull 표시
        if (bExecRune && m_pOwner && m_pOwner->GetTransform())
        {
            XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
            VFXSpriteManager::Get().Spawn("skull", pos, 64.f, 1.0f,
                { 1.f, 0.f, 0.f, 1.f }, 0.f, VFXSpriteAnim::SkullPop);
        }
    }
    else
    {
        if (bTriggerStagger && !m_bIsBoss)
            ChangeState(EnemyState::Stagger);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 상태이상 시스템
// ─────────────────────────────────────────────────────────────────────────────

float EnemyComponent::GetChillSlowMult() const
{
    if (m_bFrozen) return 0.f;
    float mult = 1.f - m_Chill.stacks * CHILL_SLOW_PER_STACK;
    return std::max(mult, 0.1f);  // 최소 10% 이동속도 보장
}

void EnemyComponent::ApplyBurn(int stacks, float duration, float tickDmgBase, int maxStacks)
{
    if (m_eCurrentState == EnemyState::Dead) return;
    m_Burn.maxStacks   = maxStacks;
    m_Burn.stacks      = (std::min)(m_Burn.stacks + stacks, maxStacks);
    m_Burn.timer       = duration;
    m_Burn.tickDmgBase = tickDmgBase;
    if (m_Burn.tickTimer <= 0.f) m_Burn.tickTimer = BURN_TICK_INTERVAL;

    if (m_onStatusChanged && m_pOwner && m_pOwner->GetTransform())
        m_onStatusChanged(ElementType::Fire, m_Burn.stacks, m_pOwner->GetTransform()->GetPosition());
    SpawnStatusVFX("status_burn", m_vfxBurnId);
    RefreshStatusOutline();
}

void EnemyComponent::ApplyChill(int stacks, float duration, int maxStacks)
{
    if (m_eCurrentState == EnemyState::Dead || m_bFrozen) return;
    m_Chill.maxStacks = maxStacks;
    m_Chill.stacks    = (std::min)(m_Chill.stacks + stacks, maxStacks);
    m_Chill.timer     = duration;

    // 3중첩 달성 → 완전 빙결
    if (m_Chill.stacks >= 3)
    {
        m_bFrozen      = true;
        m_fFrozenTimer = FREEZE_DURATION;
        m_Chill.Clear();
        StopStatusVFX(m_vfxChillId);
        SpawnStatusVFX("status_freeze", m_vfxFreezeId);
    }
    else
    {
        SpawnStatusVFX("status_chill", m_vfxChillId);
    }

    if (m_onStatusChanged && m_pOwner && m_pOwner->GetTransform())
        m_onStatusChanged(ElementType::Water, m_bFrozen ? -1 : m_Chill.stacks,
                          m_pOwner->GetTransform()->GetPosition());
    RefreshStatusOutline();
}

void EnemyComponent::ApplyFracture(int stacks, float duration, int maxStacks)
{
    if (m_eCurrentState == EnemyState::Dead) return;
    m_Fracture.maxStacks = maxStacks;
    m_Fracture.stacks    = (std::min)(m_Fracture.stacks + stacks, maxStacks);
    m_Fracture.timer     = duration;

    // 중첩 수에 따라 방어력 디버프 갱신 (균열 8% per 중첩)
    float defMult = 1.f - m_Fracture.stacks * FRACTURE_DEF_PER_STACK;
    ApplyDefenseDebuff(std::max(defMult, 0.1f), duration);

    // 3중첩 달성 → 경직
    if (m_Fracture.stacks >= 3 &&
        m_eCurrentState != EnemyState::Stagger &&
        m_eCurrentState != EnemyState::Dead)
    {
        m_Fracture.Clear();
        m_fDefenseMult = 1.f;
        ChangeState(EnemyState::Stagger);
    }

    if (m_onStatusChanged && m_pOwner && m_pOwner->GetTransform())
        m_onStatusChanged(ElementType::Earth, m_Fracture.stacks, m_pOwner->GetTransform()->GetPosition());
    SpawnStatusVFX("status_fracture", m_vfxFractureId);
    RefreshStatusOutline();
}

void EnemyComponent::SpawnStatusVFX(const char* effectId, int& outId)
{
    if (!m_pStatusVFXMgr) {
        OutputDebugStringA("[StatusVFX] SKIP: m_pStatusVFXMgr is null\n");
        return;
    }
    if (outId >= 0) return;
    if (!EffectRegistry::Get().HasEffect(effectId)) {
        char buf[128]; sprintf_s(buf, "[StatusVFX] SKIP: effect not found: %s\n", effectId);
        OutputDebugStringA(buf);
        return;
    }

    XMFLOAT3 pos = {};
    if (m_pOwner && m_pOwner->GetTransform())
    {
        pos = m_pOwner->GetTransform()->GetPosition();
        // 보스 scale 비례 y offset — scale 1 = +1, scale 10 = +12. 머리 위로 띄워
        //   인디케이터 영역과 시각 분리 (status sphere 가 발치에 떠서 인디케이터로 오해되는 문제).
        float yScale = m_pOwner->GetTransform()->GetScale().y;
        pos.y += 1.0f + yScale * 1.1f;
    }

    EffectDef def = EffectRegistry::Get().GetEffect(effectId);
    outId = m_pStatusVFXMgr->SpawnEffectDef(pos, {0,1,0}, def, false);
}

void EnemyComponent::StopStatusVFX(int& id)
{
    if (!m_pStatusVFXMgr || id < 0) return;
    m_pStatusVFXMgr->StopEffect(id);
    id = -1;
}

void EnemyComponent::TrackStatusVFX()
{
    if (!m_pStatusVFXMgr || !m_pOwner || !m_pOwner->GetTransform()) return;

    XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
    // SpawnStatusVFX 와 동일 offset — 매 프레임 위치 트래킹.
    float yScale = m_pOwner->GetTransform()->GetScale().y;
    pos.y += 1.0f + yScale * 1.1f;
    XMFLOAT3 dir = { 0.f, 1.f, 0.f };

    if (m_vfxBurnId     >= 0) m_pStatusVFXMgr->TrackEffect(m_vfxBurnId,     pos, dir);
    if (m_vfxChillId    >= 0) m_pStatusVFXMgr->TrackEffect(m_vfxChillId,    pos, dir);
    if (m_vfxFreezeId   >= 0) m_pStatusVFXMgr->TrackEffect(m_vfxFreezeId,   pos, dir);
    if (m_vfxFractureId >= 0) m_pStatusVFXMgr->TrackEffect(m_vfxFractureId, pos, dir);
}

void EnemyComponent::SpawnMarkerVFX(const char* effectId, int& outId, float yOffset)
{
    if (outId >= 0) return;
    if (!effectId || !*effectId) return;
    if (!m_pStatusVFXMgr) {
        char buf[160]; sprintf_s(buf, "[Marker] SKIP (no VFXMgr): %s\n", effectId);
        OutputDebugStringA(buf);
        return;
    }
    if (!EffectRegistry::Get().HasEffect(effectId)) {
        char buf[160]; sprintf_s(buf, "[Marker] SKIP (effect not registered): %s\n", effectId);
        OutputDebugStringA(buf);
        return;
    }

    XMFLOAT3 pos = {};
    if (m_pOwner && m_pOwner->GetTransform())
    {
        pos = m_pOwner->GetTransform()->GetPosition();
        pos.y += yOffset;
    }
    EffectDef def = EffectRegistry::Get().GetEffect(effectId);
    outId = m_pStatusVFXMgr->SpawnEffectDef(pos, { 0, 1, 0 }, def, false);

    char buf[200];
    sprintf_s(buf, "[Marker] Spawned %s at (%.2f,%.2f,%.2f) id=%d\n",
              effectId, pos.x, pos.y, pos.z, outId);
    OutputDebugStringA(buf);
}

void EnemyComponent::TrackMarkerVFX()
{
    if (!m_pStatusVFXMgr || !m_pOwner || !m_pOwner->GetTransform()) return;
    if (m_vfxHeadMarkerId < 0 && m_vfxFootMarkerId < 0) return;

    XMFLOAT3 base = m_pOwner->GetTransform()->GetPosition();
    XMFLOAT3 dir  = { 0.f, 1.f, 0.f };

    if (m_vfxHeadMarkerId >= 0) {
        XMFLOAT3 head = { base.x, base.y + HEAD_MARKER_Y_OFFSET, base.z };
        m_pStatusVFXMgr->TrackEffect(m_vfxHeadMarkerId, head, dir);
    }
    if (m_vfxFootMarkerId >= 0) {
        XMFLOAT3 foot = { base.x, base.y + FOOT_MARKER_Y_OFFSET, base.z };
        m_pStatusVFXMgr->TrackEffect(m_vfxFootMarkerId, foot, dir);
    }
}

void EnemyComponent::SpawnTypeMarkers()
{
    if (!m_strHeadMarkerEffect.empty())
        SpawnMarkerVFX(m_strHeadMarkerEffect.c_str(), m_vfxHeadMarkerId, HEAD_MARKER_Y_OFFSET);
    if (!m_strFootMarkerEffect.empty())
        SpawnMarkerVFX(m_strFootMarkerEffect.c_str(), m_vfxFootMarkerId, FOOT_MARKER_Y_OFFSET);
}

void EnemyComponent::StopTypeMarkers()
{
    StopStatusVFX(m_vfxHeadMarkerId);
    StopStatusVFX(m_vfxFootMarkerId);
}

void EnemyComponent::TrackTypeMarkers(float dt)
{
    if (!m_pOwner || !m_pOwner->GetTransform()) return;
    if (!m_pHeadMarker && !m_pFootMarker && !m_pHeadMarkerInner && !m_pFootMarkerInner) return;

    XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();

    // 외곽 70 deg/s · 내부 -110 deg/s (대조 회전으로 마법진 살아있는 느낌)
    m_fMarkerRotation += dt * 70.f;
    if (m_fMarkerRotation > 360.f) m_fMarkerRotation -= 360.f;
    else if (m_fMarkerRotation < 0.f) m_fMarkerRotation += 360.f;

    // 펄스 (0.0 ~ 1.0 사이 sin) — 스케일 미세 변동으로 빛나는 마법진 느낌
    m_fMarkerPulse += dt * 3.5f;  // 약 0.5초 주기
    const float pulse = 1.0f + 0.08f * sinf(m_fMarkerPulse);

    auto place = [&](GameObject* pGO, float yOff, float rotDeg, float baseScale) {
        if (!pGO || !pGO->GetTransform()) return;
        auto* pT = pGO->GetTransform();
        pT->SetPosition(pos.x, pos.y + yOff, pos.z);
        pT->SetRotation(0.f, rotDeg, 0.f);
        const float s = baseScale * pulse;
        pT->SetScale(s, 1.0f, s);
    };

    // 외곽/내부 반대 방향 회전, 내부는 약간 더 위(헤드)/위(발)에 띄워 겹침 방지
    place(m_pFootMarker,      FOOT_MARKER_Y_OFFSET,        -m_fMarkerRotation,         m_fFootMarkerScale);
    place(m_pFootMarkerInner, FOOT_MARKER_Y_OFFSET + 0.05f, m_fMarkerRotation * 1.6f,  m_fFootMarkerInnerScale);
    place(m_pHeadMarker,      HEAD_MARKER_Y_OFFSET,         m_fMarkerRotation,         m_fHeadMarkerScale);
    place(m_pHeadMarkerInner, HEAD_MARKER_Y_OFFSET + 0.10f,-m_fMarkerRotation * 1.6f,  m_fHeadMarkerInnerScale);
}

void EnemyComponent::HideTypeMarkers()
{
    auto hide = [](GameObject* pGO) {
        if (pGO && pGO->GetTransform())
            pGO->GetTransform()->SetPosition(0.f, -1000.f, 0.f);
    };
    hide(m_pHeadMarker);
    hide(m_pHeadMarkerInner);
    hide(m_pFootMarker);
    hide(m_pFootMarkerInner);
}

void EnemyComponent::RefreshStatusOutline()
{
    if (!m_pOwner) return;

    // 빙결 > 화상 > 균열 우선순위로 아웃라인 색상 결정
    XMFLOAT4 color   = { 0.f, 0.f, 0.f, 0.f };
    float    intensity = 0.f;

    if (m_bFrozen)
    {
        color     = { 0.35f, 0.75f, 1.0f, 1.f };  // 얼음 파랑
        intensity = 0.90f;
    }
    else if (m_Burn.IsActive())
    {
        float t   = static_cast<float>(m_Burn.stacks) / static_cast<float>(m_Burn.maxStacks);
        color     = { 1.0f, 0.30f, 0.02f, 1.f };  // 오렌지-빨강
        intensity = 0.25f + t * 0.55f;
    }
    else if (m_Chill.IsActive())
    {
        float t   = static_cast<float>(m_Chill.stacks) / static_cast<float>(m_Chill.maxStacks);
        color     = { 0.35f, 0.75f, 1.0f, 1.f };  // 수속 파랑
        intensity = 0.20f + t * 0.50f;
    }
    else if (m_Fracture.IsActive())
    {
        float t   = static_cast<float>(m_Fracture.stacks) / static_cast<float>(m_Fracture.maxStacks);
        color     = { 0.85f, 0.55f, 0.10f, 1.f };  // 흙 황갈색
        intensity = 0.20f + t * 0.50f;
    }

    m_pOwner->SetStatusColorAll(color, intensity);
}

void EnemyComponent::UpdateStatusEffects(float dt)
{
    TrackStatusVFX();
    TrackMarkerVFX();
    TrackTypeMarkers(dt);

    // ── 완전 빙결 ─────────────────────────────────────────────────────────────
    if (m_bFrozen)
    {
        m_fFrozenTimer -= dt;
        if (m_fFrozenTimer <= 0.f)
        {
            m_bFrozen = false;
            StopStatusVFX(m_vfxFreezeId);
            RefreshStatusOutline();
        }
        return;  // 빙결 중 다른 상태이상 틱 정지
    }

    // ── 화상 (Burn) ────────────────────────────────────────────────────────────
    if (m_Burn.IsActive())
    {
        m_Burn.timer -= dt;
        if (m_Burn.timer <= 0.f)
        {
            m_Burn.Clear();
            StopStatusVFX(m_vfxBurnId);
            RefreshStatusOutline();
        }
        else
        {
            m_Burn.tickTimer -= dt;
            if (m_Burn.tickTimer <= 0.f)
            {
                m_Burn.tickTimer = BURN_TICK_INTERVAL;
                float tickDmg = m_Burn.tickDmgBase * m_Burn.stacks;
                TakeDamage(tickDmg, false);

                if (m_onStatusChanged && m_pOwner && m_pOwner->GetTransform())
                    m_onStatusChanged(ElementType::Fire, m_Burn.stacks,
                                      m_pOwner->GetTransform()->GetPosition());
            }
        }
    }

    // ── 빙결 중첩 (Chill) ─────────────────────────────────────────────────────
    if (m_Chill.IsActive())
    {
        m_Chill.timer -= dt;
        if (m_Chill.timer <= 0.f)
        {
            m_Chill.Clear();
            StopStatusVFX(m_vfxChillId);
            RefreshStatusOutline();
        }
    }

    // ── 균열 (Fracture) ───────────────────────────────────────────────────────
    if (m_Fracture.IsActive())
    {
        m_Fracture.timer -= dt;
        if (m_Fracture.timer <= 0.f)
        {
            m_Fracture.Clear();
            StopStatusVFX(m_vfxFractureId);
            if (m_fDefenseDebuffTimer <= 0.f)
                m_fDefenseMult = 1.f;
            RefreshStatusOutline();
        }
    }
}

void EnemyComponent::SetAttackBehavior(std::unique_ptr<IAttackBehavior> pBehavior)
{
    m_pAttackBehavior = std::move(pBehavior);
}

void EnemyComponent::SetSpecialAttackBehavior(std::unique_ptr<IAttackBehavior> pBehavior)
{
    m_pSpecialAttackBehavior = std::move(pBehavior);
}

void EnemyComponent::DebugForceSpecialAttack(std::unique_ptr<IAttackBehavior> pBehavior)
{
    if (m_eCurrentState == EnemyState::Dead) return;
    // 진행 중인 attack 안전하게 중단 — ChangeState 가 Reset 호출.
    if (m_eCurrentState == EnemyState::Attack)
        ChangeState(EnemyState::Idle);

    m_pSpecialAttackBehavior = std::move(pBehavior);
    m_bUsingSpecialAttack    = true;
    m_bUsingFlyingAttack     = false;
    m_fAttackCooldownTimer   = 0.0f;   // 쿨다운 무시
    ChangeState(EnemyState::Attack);
}

void EnemyComponent::SetFlyingAttackBehavior(std::unique_ptr<IAttackBehavior> pBehavior)
{
    m_pFlyingAttackBehavior = std::move(pBehavior);
}

float EnemyComponent::GetDistanceToTarget() const
{
    if (!m_pTarget || !m_pOwner) return FLT_MAX;

    TransformComponent* pMyTransform = m_pOwner->GetTransform();
    TransformComponent* pTargetTransform = m_pTarget->GetTransform();

    if (!pMyTransform || !pTargetTransform) return FLT_MAX;

    return MathUtils::Distance2D(pMyTransform->GetPosition(), pTargetTransform->GetPosition());
}

void EnemyComponent::FaceTarget(float dt, bool bInstant)
{
    if (!m_pTarget || !m_pOwner) return;

    // Stationary 보스: 회전도 완전 봉쇄 — 스폰 시 방향 그대로 유지, 모든 패턴은 방향 무관(방사형)
    if (m_bStationary) return;

    TransformComponent* pMyTransform = m_pOwner->GetTransform();
    TransformComponent* pTargetTransform = m_pTarget->GetTransform();

    if (!pMyTransform || !pTargetTransform) return;

    XMFLOAT3 myPos = pMyTransform->GetPosition();
    XMFLOAT3 targetPos = pTargetTransform->GetPosition();

    // Get normalized direction to target on XZ plane
    XMFLOAT2 dir = MathUtils::Direction2D(myPos, targetPos);
    if (dir.x == 0.0f && dir.y == 0.0f) return;

    // Calculate target yaw angle
    float targetYawRad = atan2f(dir.x, dir.y);
    float targetYawDeg = XMConvertToDegrees(targetYawRad);

    const XMFLOAT3& currentRot = pMyTransform->GetRotation();
    float currentYaw = currentRot.y;

    // Calculate shortest angle difference (-180 to 180)
    float angleDiff = targetYawDeg - currentYaw;
    while (angleDiff > 180.0f) angleDiff -= 360.0f;
    while (angleDiff < -180.0f) angleDiff += 360.0f;

    // Stationary 보스는 bInstant 플래그 무시하고 항상 스무스 회전 (무거운 석상 느낌)
    bool bUseSmooth = (!bInstant || dt > 0.0f) || m_bStationary;
    if (bInstant && !m_bStationary)
    {
        // Boss: limit instant rotation to prevent sudden 180 degree turns
        // This prevents the jarring "snap" when player moves behind boss
        if (m_bIsBoss)
        {
            const float MAX_INSTANT_ROTATION = 90.0f;  // Max 90 degree turn at once
            if (fabsf(angleDiff) > MAX_INSTANT_ROTATION)
            {
                // Clamp the rotation
                float rotation = (angleDiff > 0.0f) ? MAX_INSTANT_ROTATION : -MAX_INSTANT_ROTATION;
                float newYaw = currentYaw + rotation;
                pMyTransform->SetRotation(currentRot.x, newYaw, currentRot.z);
                return;
            }
        }
        // Non-boss or small angle: instant rotation
        pMyTransform->SetRotation(currentRot.x, targetYawDeg, currentRot.z);
    }
    else
    {
        // Smooth rotation
        // Boss has faster rotation speed for better tracking
        // Stationary 는 m_fRotationSpeed 그대로 사용 (프리셋에서 낮은 값으로 세팅)
        float rotSpeed = m_bStationary ? m_fRotationSpeed
                                        : (m_bIsBoss ? m_fRotationSpeed * 1.5f : m_fRotationSpeed);
        float maxRotation = rotSpeed * dt;
        float rotation = 0.0f;

        if (fabsf(angleDiff) <= maxRotation)
        {
            rotation = angleDiff;
        }
        else
        {
            rotation = (angleDiff > 0.0f) ? maxRotation : -maxRotation;
        }

        float newYaw = currentYaw + rotation;
        pMyTransform->SetRotation(currentRot.x, newYaw, currentRot.z);
    }
}

void EnemyComponent::MoveTowardsTarget(float dt)
{
    if (!m_pTarget || !m_pOwner) return;
    if (m_bFrozen) return;

    TransformComponent* pMyTransform = m_pOwner->GetTransform();
    TransformComponent* pTargetTransform = m_pTarget->GetTransform();

    if (!pMyTransform || !pTargetTransform) return;

    XMFLOAT3 myPos = pMyTransform->GetPosition();
    XMFLOAT3 targetPos = pTargetTransform->GetPosition();

    // Get normalized direction to target on XZ plane
    XMFLOAT2 dir = MathUtils::Direction2D(myPos, targetPos);

    // Calculate separation force from other enemies in the room
    XMFLOAT2 separationForce = { 0.0f, 0.0f };
    if (m_pRoom)
    {
        const auto& enemies = m_pRoom->GetEnemies();
        for (EnemyComponent* pOther : enemies)
        {
            if (pOther == this || !pOther || pOther->IsDead()) continue;

            GameObject* pOtherOwner = pOther->GetOwner();
            if (!pOtherOwner) continue;

            TransformComponent* pOtherTransform = pOtherOwner->GetTransform();
            if (!pOtherTransform) continue;

            XMFLOAT3 otherPos = pOtherTransform->GetPosition();
            float dist = MathUtils::Distance2D(myPos, otherPos);

            // Apply separation if too close
            if (dist > 0.001f && dist < m_fSeparationRadius)
            {
                // Direction away from other enemy
                XMFLOAT2 awayDir = MathUtils::Direction2D(otherPos, myPos);
                // Stronger force when closer (inverse proportional)
                float strength = (m_fSeparationRadius - dist) / m_fSeparationRadius;
                separationForce.x += awayDir.x * strength;
                separationForce.y += awayDir.y * strength;
            }
        }
    }

    // 일반 몹은 전투 템포 가속 — 보스는 페이즈 별 튜닝이라 제외.
    //   WaterPuddleBehavior 가 m_fSpeedMultiplier 를 일시적으로 낮춰 슬로우 줌 (1.0 미만일 때만 적용)
    float fEffectiveSpeed = m_Stats.m_fMoveSpeed * m_fSpeedMultiplier;
    if (!m_bIsBoss) fEffectiveSpeed *= 1.35f;

    // Combine movement direction with separation force
    float slowMult = GetChillSlowMult();
    float moveX = dir.x * fEffectiveSpeed * slowMult;
    float moveZ = dir.y * fEffectiveSpeed * slowMult;

    // Add separation force
    moveX += separationForce.x * m_fSeparationStrength;
    moveZ += separationForce.y * m_fSeparationStrength;

    // Apply movement
    myPos.x += moveX * dt;
    myPos.z += moveZ * dt;
    // Y is controlled by gravity in Update()

    pMyTransform->SetPosition(myPos);
}

void EnemyComponent::UpdateIdle(float dt)
{
    // If target exists, immediately start chasing
    if (m_pTarget)
    {
        ChangeState(EnemyState::Chase);
    }
}

void EnemyComponent::UpdateChase(float dt)
{
    if (!m_pTarget)
    {
        ChangeState(EnemyState::Idle);
        return;
    }

    // 보스 페이즈 전환 중이면 대기
    if (m_pPhaseController && m_pPhaseController->IsInTransition())
    {
        return;
    }

    // Stationary 보스: 이동/회전 모두 없음, 거리 무관 — 쿨타임 끝나면 그냥 공격
    //   모든 패턴은 방사형/고정 지향이어야 함 (추적 공격 X)
    if (m_bStationary)
    {
        // 방어: 애니 재생이 중단돼 있으면 Chase 클립 강제 재시작 (loop anim 이 얼어붙는 케이스 방어)
        if (m_pAnimationComp && !m_AnimConfig.m_strChaseClip.empty())
        {
            if (!m_pAnimationComp->IsPlaying())
            {
                m_pAnimationComp->CrossFade(m_AnimConfig.m_strChaseClip, 0.1f, m_AnimConfig.m_bLoopChase, true);
            }
        }

        if (m_fAttackCooldownTimer > 0.0f)
            return;

        // 특수 공격 먼저 시도
        if (m_fSpecialCooldownTimer <= 0.0f)
        {
            if (m_pPhaseController)
            {
                const BossPhaseData& phase = m_pPhaseController->GetCurrentPhaseData();
                if (phase.m_fnSpecialAttack && (rand() % 100) < m_nSpecialAttackChance)
                {
                    m_pSpecialAttackBehavior = phase.m_fnSpecialAttack();
                    m_bUsingSpecialAttack = true;
                    ChangeState(EnemyState::Attack);
                    return;
                }
            }
            else if ((m_fnSpecialAttackFactory || m_pSpecialAttackBehavior) && (rand() % 100) < m_nSpecialAttackChance)
            {
                if (m_fnSpecialAttackFactory) m_pSpecialAttackBehavior = m_fnSpecialAttackFactory();
                else m_pSpecialAttackBehavior->Reset();
                m_bUsingSpecialAttack = true;
                ChangeState(EnemyState::Attack);
                return;
            }
        }

        // 기본 공격
        if (m_fnAttackFactory) m_pAttackBehavior = m_fnAttackFactory();
        else if (m_pAttackBehavior) m_pAttackBehavior->Reset();
        ChangeState(EnemyState::Attack);
        return;
    }

    float distance = GetDistanceToTarget();

    // Boss: 거리 기반 공격 선택 (카이팅 방지)
    if (m_bIsBoss && m_fAttackCooldownTimer <= 0.0f)
    {
        m_bUsingSpecialAttack = false;
        m_bUsingFlyingAttack = false;

        // 원거리 (30+ units): 비행 공격 또는 브레스 우선
        if (distance >= m_Stats.m_fLongRangeThreshold)
        {
            FaceTarget(dt, true);

            // 비행 공격 시도 (높은 확률)
            if (CanUseFlyingAttack() && m_fFlyingCooldownTimer <= 0.0f)
            {
                int flyChance = 70;  // 원거리에서는 70% 확률로 비행 공격
                if (m_pPhaseController)
                {
                    flyChance = std::max(flyChance, m_pPhaseController->GetFlyingAttackChance());
                }
                if ((rand() % 100) < flyChance)
                {
                    if (m_pPhaseController)
                    {
                        const BossPhaseData& phase = m_pPhaseController->GetCurrentPhaseData();
                        if (phase.m_fnFlyingAttack)
                        {
                            m_pFlyingAttackBehavior = phase.m_fnFlyingAttack();
                        }
                    }
                    if (m_pFlyingAttackBehavior)
                    {
                        m_bUsingFlyingAttack = true;
                        ChangeState(EnemyState::Attack);
                        return;
                    }
                }
            }

            // 비행 불가 시 기본 공격 (팩토리 있으면 새로 생성)
            if (m_fnAttackFactory) m_pAttackBehavior = m_fnAttackFactory();
            else if (m_pAttackBehavior) m_pAttackBehavior->Reset();
            ChangeState(EnemyState::Attack);
            return;
        }
        // 중거리 (15-30 units): 특수 공격 또는 비행 공격
        else if (distance >= m_Stats.m_fMidRangeThreshold)
        {
            FaceTarget(dt, true);

            // 비행 공격 시도
            if (CanUseFlyingAttack() && m_fFlyingCooldownTimer <= 0.0f)
            {
                int flyChance = m_nFlyingAttackChance;
                if (m_pPhaseController)
                {
                    flyChance = m_pPhaseController->GetFlyingAttackChance();
                }
                if ((rand() % 100) < flyChance)
                {
                    if (m_pPhaseController)
                    {
                        const BossPhaseData& phase = m_pPhaseController->GetCurrentPhaseData();
                        if (phase.m_fnFlyingAttack)
                        {
                            m_pFlyingAttackBehavior = phase.m_fnFlyingAttack();
                        }
                    }
                    if (m_pFlyingAttackBehavior)
                    {
                        m_bUsingFlyingAttack = true;
                        ChangeState(EnemyState::Attack);
                        return;
                    }
                }
            }

            // 특수 공격 시도 (중거리에서 높은 확률)
            if (m_fSpecialCooldownTimer <= 0.0f)
            {
                int specialChance = 50;  // 중거리에서 50% 확률
                if (m_pPhaseController)
                {
                    const BossPhaseData& phase = m_pPhaseController->GetCurrentPhaseData();
                    if (phase.m_fnSpecialAttack && (rand() % 100) < specialChance)
                    {
                        m_pSpecialAttackBehavior = phase.m_fnSpecialAttack();
                        m_bUsingSpecialAttack = true;
                        ChangeState(EnemyState::Attack);
                        return;
                    }
                }
                else if ((m_fnSpecialAttackFactory || m_pSpecialAttackBehavior) && (rand() % 100) < m_nSpecialAttackChance)
                {
                    if (m_fnSpecialAttackFactory)
                        m_pSpecialAttackBehavior = m_fnSpecialAttackFactory();
                    else
                        m_pSpecialAttackBehavior->Reset();
                    m_bUsingSpecialAttack = true;
                    ChangeState(EnemyState::Attack);
                    return;
                }
            }

            // 그 외엔 기본 공격 (팩토리 있으면 새로 생성)
            if (m_fnAttackFactory) m_pAttackBehavior = m_fnAttackFactory();
            else if (m_pAttackBehavior) m_pAttackBehavior->Reset();
            ChangeState(EnemyState::Attack);
            return;
        }
        // 근거리 (< 15 units): 근접 콤보 / 특수 공격
        else
        {
            FaceTarget(dt, true);

            // 비행 공격 (낮은 확률)
            if (CanUseFlyingAttack() && m_fFlyingCooldownTimer <= 0.0f)
            {
                int flyChance = m_nFlyingAttackChance / 2;  // 근접에서는 확률 절반
                if (m_pPhaseController)
                {
                    flyChance = m_pPhaseController->GetFlyingAttackChance() / 2;
                }
                if ((rand() % 100) < flyChance)
                {
                    if (m_pPhaseController)
                    {
                        const BossPhaseData& phase = m_pPhaseController->GetCurrentPhaseData();
                        if (phase.m_fnFlyingAttack)
                        {
                            m_pFlyingAttackBehavior = phase.m_fnFlyingAttack();
                        }
                    }
                    if (m_pFlyingAttackBehavior)
                    {
                        m_bUsingFlyingAttack = true;
                        ChangeState(EnemyState::Attack);
                        return;
                    }
                }
            }

            // 특수 공격 시도
            if (m_fSpecialCooldownTimer <= 0.0f)
            {
                if (m_pPhaseController)
                {
                    const BossPhaseData& phase = m_pPhaseController->GetCurrentPhaseData();
                    if (phase.m_fnSpecialAttack && (rand() % 100) < m_nSpecialAttackChance)
                    {
                        m_pSpecialAttackBehavior = phase.m_fnSpecialAttack();
                        m_bUsingSpecialAttack = true;
                        ChangeState(EnemyState::Attack);
                        return;
                    }
                }
                else if ((m_fnSpecialAttackFactory || m_pSpecialAttackBehavior) && (rand() % 100) < m_nSpecialAttackChance)
                {
                    if (m_fnSpecialAttackFactory)
                        m_pSpecialAttackBehavior = m_fnSpecialAttackFactory();
                    else
                        m_pSpecialAttackBehavior->Reset();
                    m_bUsingSpecialAttack = true;
                    ChangeState(EnemyState::Attack);
                    return;
                }
            }

            // 기본 공격 (팩토리 있으면 새로 생성해 패턴 변화)
            if (m_fnAttackFactory) m_pAttackBehavior = m_fnAttackFactory();
            else if (m_pAttackBehavior) m_pAttackBehavior->Reset();
            ChangeState(EnemyState::Attack);
            return;
        }
    }

    // 일반 적: 기존 사거리 기반 공격
    if (distance <= m_Stats.m_fAttackRange)
    {
        if (m_fAttackCooldownTimer <= 0.0f)
        {
            FaceTarget(dt, true);
            ChangeState(EnemyState::Attack);
        }
        else
        {
            FaceTarget(dt);
        }
    }
    else
    {
        // Move towards target with smooth rotation
        FaceTarget(dt);
        MoveTowardsTarget(dt);
    }
}

void EnemyComponent::UpdateAttack(float dt)
{
    // Select which behavior to use (priority: flying > special > primary)
    IAttackBehavior* pCurrentBehavior = nullptr;
    if (m_bUsingFlyingAttack && m_pFlyingAttackBehavior)
    {
        pCurrentBehavior = m_pFlyingAttackBehavior.get();
    }
    else if (m_bUsingSpecialAttack && m_pSpecialAttackBehavior)
    {
        pCurrentBehavior = m_pSpecialAttackBehavior.get();
    }
    else
    {
        pCurrentBehavior = m_pAttackBehavior.get();
    }

    if (pCurrentBehavior)
    {
        pCurrentBehavior->Update(dt, this);

        // 공격 중 인디케이터 매 프레임 재배치 (보스 위치/타겟 수면 Y 추적 + 그로우인 펄스)
        m_fIndicatorTimer += dt;
        ShowIndicators();

        if (pCurrentBehavior->IsFinished())
        {
            // Reset cooldowns
            m_fAttackCooldownTimer = m_Stats.m_fAttackCooldown;

            if (m_bUsingFlyingAttack)
            {
                m_fFlyingCooldownTimer = m_fFlyingAttackCooldown;
                m_bUsingFlyingAttack = false;
                // 비행 공격 후 비행 공격 behavior 리셋
                if (m_pFlyingAttackBehavior)
                {
                    m_pFlyingAttackBehavior->Reset();
                }
            }
            else if (m_bUsingSpecialAttack)
            {
                m_fSpecialCooldownTimer = m_fSpecialAttackCooldown;
                m_bUsingSpecialAttack = false;
            }

            // Return to chase
            ChangeState(EnemyState::Chase);
        }
    }
    else
    {
        // No attack behavior, just go back to chase
        m_fAttackCooldownTimer = m_Stats.m_fAttackCooldown;
        ChangeState(EnemyState::Chase);
    }
}

void EnemyComponent::UpdateStagger(float dt)
{
    m_fStaggerTimer -= dt;

    if (m_fStaggerTimer <= 0.0f)
    {
        ChangeState(EnemyState::Chase);
    }
}

void EnemyComponent::UpdateDead(float dt)
{
    m_fDeadTimer -= dt;

    // 보스는 죽음 애니메이션 끝난 후에도 시체로 남아있음
    if (m_bIsBoss)
    {
        // 애니메이션 재생 중이면 타이머 일시정지
        if (m_pAnimationComp && m_pAnimationComp->IsPlaying())
        {
            // 무한 대기 방지 - 최대 8초
            if (m_fDeadTimer > -6.0f)
            {
                m_fDeadTimer = 0.1f;  // 타이머 유지 (삭제 안 함)
                return;
            }
        }
        // 애니메이션 끝났으면 추가 3초 대기 (시체 상태)
        else if (m_fDeadTimer > -3.0f)
        {
            return;  // 3초 더 기다림
        }
    }

    // When timer expires, request deletion
    if (m_fDeadTimer <= 0.0f)
    {
        // Try to get Scene from Room first, fallback to Dx12App if not available
        Scene* pScene = nullptr;
        if (m_pRoom)
        {
            pScene = m_pRoom->GetScene();
        }

        // Fallback: get Scene directly from Dx12App if Room doesn't have it
        if (!pScene)
        {
            pScene = Dx12App::GetInstance()->GetScene();
        }

        if (pScene)
        {
            // Mark indicator objects for deletion first
            DestroyIndicators(pScene);

            // Mark self for deletion (will also clean up child hierarchy)
            if (m_pOwner)
            {
                pScene->MarkForDeletion(m_pOwner);
            }
        }
    }
}

void EnemyComponent::ShowIndicators()
{
    if (!m_pOwner || !m_pTarget) { return; }
    if (m_IndicatorConfig.m_eType == IndicatorType::None) { return; }

    TransformComponent* pMyTransform = m_pOwner->GetTransform();
    TransformComponent* pTargetTransform = m_pTarget->GetTransform();
    if (!pMyTransform || !pTargetTransform) return;

    XMFLOAT3 myPos = pMyTransform->GetPosition();
    XMFLOAT3 targetPos = pTargetTransform->GetPosition();

    // Direction from enemy to target on XZ plane (fixed at attack start)
    XMFLOAT2 dir = MathUtils::Direction2D(myPos, targetPos);
    if (dir.x == 0.0f && dir.y == 0.0f) return;

    float yawRad = atan2f(dir.x, dir.y);
    float yawDeg = XMConvertToDegrees(yawRad);

    // ── 활성 behavior 조회 + type/size override 결정 ──
    IAttackBehavior* pActive = m_bUsingFlyingAttack  ? m_pFlyingAttackBehavior.get()
                             : m_bUsingSpecialAttack ? m_pSpecialAttackBehavior.get()
                             :                          m_pAttackBehavior.get();

    IndicatorType effectiveType = m_IndicatorConfig.m_eType;
    if (pActive)
    {
        int typeOverride = pActive->GetIndicatorTypeOverride();
        if (typeOverride >= 0) effectiveType = static_cast<IndicatorType>(typeOverride);
    }

    // emit 헬퍼 — 펄스(slow→fast)/임박감(urgency)/베이스 램프 결합한 emissive 스칼라.
    //   baseLo→baseHi: fillProgress 따라 선형 램프 (정적 강도).
    //   urgencyBoost: fillProgress > 0.6 부터 가속, 마지막 40%에 +urgencyBoost 만큼 추가.
    //   pulse: ~3Hz → ~8Hz 로 urgency 따라 빨라지며 swing 폭도 0.15 → 0.35 확장.
    auto emitFor = [this](float fillProgress, float baseLo, float baseHi, float urgencyBoost) -> float
    {
        float baseEmit = baseLo + (baseHi - baseLo) * fillProgress;
        float urgency  = std::clamp((fillProgress - 0.60f) / 0.40f, 0.0f, 1.0f);
        float slow     = 0.5f + 0.5f * sinf(m_fIndicatorTimer * 18.85f);   // ~3Hz
        float fast     = 0.5f + 0.5f * sinf(m_fIndicatorTimer * 50.27f);   // ~8Hz
        float pulse    = slow + (fast - slow) * urgency;
        float swing    = 0.15f + 0.20f * urgency;
        return baseEmit + urgencyBoost * urgency + swing * (pulse - 0.5f);
    };

    // alpha 헬퍼 — m_cDiffuse.a 캡. PS 의 V축 edge fade 와 곱해져서 외곽은 더 투명, 중심은 이 값까지.
    //   1.0 으로 두면 평면 스티커처럼 보여서 0.5 부근으로 낮춰 바닥 텍스처가 비쳐 보이게.
    //   Fill 은 fillProgress 따라 불투명도 ↑ → "임박할수록 단단해지는" 효과.
    auto alphaFor = [](float fillProgress, float baseLo, float baseHi) -> float
    {
        return baseLo + (baseHi - baseLo) * std::clamp(fillProgress, 0.0f, 1.0f);
    };

    if (effectiveType == IndicatorType::Circle)
    {

        // 발사형(Breath 등)은 지면 인디케이터 억제 — 오해 방지
        if (pActive && !pActive->ShouldShowHitZone())
        {
            HideIndicators();
            return;
        }

        // 공격 원점 기준 — 크라켄은 몸이 아니라 촉수 앞에서 공격이 나감
        XMFLOAT3 attackOrigin = GetAttackOrigin();
        // 행동별 월드 위치 override (GrenadeThrow 의 착지 지점 등)
        if (pActive)
        {
            XMFLOAT3 worldPosOverride;
            if (pActive->GetIndicatorWorldPos(this, worldPosOverride))
            {
                attackOrigin = worldPosOverride;
            }
        }

        float fTimeToHit = pActive ? pActive->GetTimeToHit() : 0.0f;
        if (fTimeToHit <= 0.0f) fTimeToHit = 0.8f;  // 기본값
        float fillProgress = (std::min)(m_fIndicatorTimer / fTimeToHit, 1.0f);

        float baseY = (std::max)(attackOrigin.y, targetPos.y);
        float indY  = baseY + 1.2f;
        // 행동별 radius override 가 있으면 그 값 사용 (패턴마다 다른 범위 표시)
        float fullR = (pActive && pActive->GetIndicatorRadius() > 0.0f)
                    ? pActive->GetIndicatorRadius()
                    : m_IndicatorConfig.m_fHitRadius;

        // 원소/패턴별 틴트 — behavior 의 GetIndicatorTint 사용. (1,1,1) = 기본 빨간색.
        XMFLOAT3 tint = pActive ? pActive->GetIndicatorTint() : XMFLOAT3(1.0f, 1.0f, 1.0f);
        bool bUseTint = !(tint.x == 1.0f && tint.y == 1.0f && tint.z == 1.0f);

        // ─── 테두리 링: 공격 내내 고정. 약한 펄스, 임박감 적게. ─
        if (m_pHitZoneIndicator)
        {
            TransformComponent* pT = m_pHitZoneIndicator->GetTransform();
            if (pT)
            {
                pT->SetPosition(attackOrigin.x, indY + 0.05f, attackOrigin.z);
                float borderR = fullR * 1.03f;
                pT->SetScale(borderR, 1.0f, borderR);

                XMFLOAT3 c = bUseTint ? tint : XMFLOAT3(1.0f, 0.20f, 0.10f);
                float e = emitFor(fillProgress, 0.35f, 0.50f, 0.15f);
                float a = alphaFor(fillProgress, 0.50f, 0.65f);

                MATERIAL mat;
                mat.m_cAmbient  = XMFLOAT4(0.20f * c.x, 0.20f * c.y, 0.20f * c.z, 1.0f);
                mat.m_cDiffuse  = XMFLOAT4(c.x, c.y, c.z, a);
                mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                mat.m_cEmissive = XMFLOAT4(e * c.x, e * c.y, e * c.z, 1.0f);
                m_pHitZoneIndicator->SetMaterial(mat);
            }
        }

        // ─── 내부 Fill: 0 → fullR 까지 차오름. 강한 펄스 + urgency 가속. ─
        if (m_pHitZoneFillIndicator)
        {
            TransformComponent* pT = m_pHitZoneFillIndicator->GetTransform();
            if (pT)
            {
                pT->SetPosition(attackOrigin.x, indY, attackOrigin.z);
                // fillProgress=1.0 일 때 fill 이 정확히 border (fullR * 1.03) 까지 도달하도록
                //   동일 1.03 배율 적용 — 이전엔 border 가 fill 보다 3% 컸어서 "끝까지 안 찬"
                //   인상이 있었음.
                float r = (fullR * 1.03f) * fillProgress;
                if (r < 0.01f) r = 0.01f;  // 0 스케일 방지
                pT->SetScale(r, 1.0f, r);

                // Fill 은 살짝 노랑 쪽 (가열 메타포) — 틴트 있으면 그대로 사용
                XMFLOAT3 c = bUseTint ? tint : XMFLOAT3(1.0f, 0.30f, 0.08f);
                float e = emitFor(fillProgress, 0.45f, 0.85f, 0.40f);
                float a = alphaFor(fillProgress, 0.40f, 0.80f);

                MATERIAL mat;
                mat.m_cAmbient  = XMFLOAT4(0.20f * c.x, 0.20f * c.y, 0.20f * c.z, 1.0f);
                mat.m_cDiffuse  = XMFLOAT4(c.x, c.y, c.z, a);
                mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                mat.m_cEmissive = XMFLOAT4(e * c.x, e * c.y, e * c.z, 1.0f);
                m_pHitZoneFillIndicator->SetMaterial(mat);
            }
        }
    }
    else if (effectiveType == IndicatorType::ForwardBox)
    {
        if (pActive && !pActive->ShouldShowHitZone())
        {
            HideIndicators();
            return;
        }

        float fTimeToHit = pActive ? pActive->GetTimeToHit() : 0.0f;
        if (fTimeToHit <= 0.0f) fTimeToHit = 0.8f;
        float fillProgress = (std::min)(m_fIndicatorTimer / fTimeToHit, 1.0f);

        // 보스의 실제 yaw 기준 forward (타겟 방향 dir 이 아님 — 보스가 회전 중일 때 정확)
        float bossYawRad = XMConvertToRadians(pMyTransform->GetRotation().y);
        float fwdX = sinf(bossYawRad);
        float fwdZ = cosf(bossYawRad);
        float bossYawDeg = pMyTransform->GetRotation().y;

        XMFLOAT3 bossPos = myPos;
        // 행동별 length override 반영 (TailSweep rect 모드 등)
        float fLen   = (pActive && pActive->GetIndicatorLength() > 0.0f)
                     ? pActive->GetIndicatorLength()
                     : m_IndicatorConfig.m_fHitLength;
        float fHalfW = (pActive && pActive->GetIndicatorRadius() > 0.0f)
                     ? pActive->GetIndicatorRadius()
                     : m_IndicatorConfig.m_fHitRadius;  // half-width (override 반영)
        // 보스 위치를 corridor 의 시작점으로 — Z-scale 그대로 사용해서 보스 뒤로 새지 않게
        float centerX = bossPos.x + fwdX * (fLen * 0.5f);
        float centerZ = bossPos.z + fwdZ * (fLen * 0.5f);

        float baseY = (std::max)(bossPos.y, targetPos.y);
        float indY  = baseY + 1.2f;

        // behavior 별 색상 — 기본 (1,1,1) = 빨강 그대로, 그 외엔 tint 를 베이스 컬러로 사용
        XMFLOAT3 tint = pActive ? pActive->GetIndicatorTint() : XMFLOAT3(1.0f, 1.0f, 1.0f);
        bool bUseTint = !(tint.x == 1.0f && tint.y == 1.0f && tint.z == 1.0f);

        // 외곽 box — 약한 펄스, 임박감 적게 (테두리).
        if (m_pHitZoneIndicator)
        {
            TransformComponent* pT = m_pHitZoneIndicator->GetTransform();
            if (pT)
            {
                pT->SetPosition(centerX, indY + 0.02f, centerZ);
                pT->SetRotation(0.0f, bossYawDeg, 0.0f);
                pT->SetScale(fHalfW * 2.0f * 1.06f, 1.0f, fLen);

                XMFLOAT3 c = bUseTint ? tint : XMFLOAT3(1.0f, 0.20f, 0.10f);
                float e = emitFor(fillProgress, 0.35f, 0.50f, 0.15f);
                float a = alphaFor(fillProgress, 0.50f, 0.65f);

                MATERIAL mat;
                mat.m_cAmbient  = XMFLOAT4(0.20f * c.x, 0.20f * c.y, 0.20f * c.z, 1.0f);
                mat.m_cDiffuse  = XMFLOAT4(c.x, c.y, c.z, a);
                mat.m_cEmissive = XMFLOAT4(e * c.x, e * c.y, e * c.z, 1.0f);
                mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                m_pHitZoneIndicator->SetMaterial(mat);
            }
        }

        // 내부 fill — 강한 펄스 + urgency 가속.
        if (m_pHitZoneFillIndicator)
        {
            TransformComponent* pT = m_pHitZoneFillIndicator->GetTransform();
            if (pT)
            {
                float curLen = fLen * fillProgress;
                if (curLen < 0.01f) curLen = 0.01f;
                float fillCenterX = bossPos.x + fwdX * (curLen * 0.5f);
                float fillCenterZ = bossPos.z + fwdZ * (curLen * 0.5f);

                pT->SetPosition(fillCenterX, indY, fillCenterZ);
                pT->SetRotation(0.0f, bossYawDeg, 0.0f);
                pT->SetScale(fHalfW * 2.0f, 1.0f, curLen);

                XMFLOAT3 c = bUseTint ? tint : XMFLOAT3(1.0f, 0.30f, 0.08f);
                float e = emitFor(fillProgress, 0.45f, 0.85f, 0.40f);
                float a = alphaFor(fillProgress, 0.40f, 0.80f);

                MATERIAL mat;
                mat.m_cAmbient  = XMFLOAT4(0.20f * c.x, 0.20f * c.y, 0.20f * c.z, 1.0f);
                mat.m_cDiffuse  = XMFLOAT4(c.x, c.y, c.z, a);
                mat.m_cEmissive = XMFLOAT4(e * c.x, e * c.y, e * c.z, 1.0f);
                mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                m_pHitZoneFillIndicator->SetMaterial(mat);
            }
        }
    }

    else if (effectiveType == IndicatorType::RushCircle ||
        effectiveType == IndicatorType::RushCone)
    {
        float rushDist = m_IndicatorConfig.m_fRushDistance;

        // Rush line: from enemy position, pointing toward target
        if (m_pRushLineIndicator)
        {
            TransformComponent* pT = m_pRushLineIndicator->GetTransform();
            if (pT)
            {
                pT->SetPosition(myPos.x, myPos.y + 0.15f, myPos.z);
                pT->SetRotation(0.0f, yawDeg, 0.0f);
                pT->SetScale(1.0f, 1.0f, rushDist);
            }
        }

        // Hit zone: at rush destination
        if (m_pHitZoneIndicator)
        {
            float destX = myPos.x + dir.x * rushDist;
            float destZ = myPos.z + dir.y * rushDist;
            TransformComponent* pT = m_pHitZoneIndicator->GetTransform();
            if (pT)
            {
                pT->SetPosition(destX, myPos.y + 0.15f, destZ);
                float r = m_IndicatorConfig.m_fHitRadius;
                pT->SetScale(r, 1.0f, r);

                if (m_IndicatorConfig.m_eType == IndicatorType::RushCone)
                {
                    pT->SetRotation(0.0f, yawDeg, 0.0f);
                }
            }
        }
    }
}

void EnemyComponent::HideIndicators()
{
    if (m_pRushLineIndicator)
    {
        TransformComponent* pT = m_pRushLineIndicator->GetTransform();
        if (pT)
        {
            pT->SetPosition(0.0f, -1000.0f, 0.0f);
            pT->SetScale(0.0f, 0.0f, 0.0f);
        }
    }
    if (m_pHitZoneIndicator)
    {
        TransformComponent* pT = m_pHitZoneIndicator->GetTransform();
        if (pT)
        {
            pT->SetPosition(0.0f, -1000.0f, 0.0f);
            pT->SetScale(0.0f, 0.0f, 0.0f);  // overlay PSO 로 그려도 투영 안 되도록 퇴화
        }
    }
    if (m_pHitZoneFillIndicator)
    {
        TransformComponent* pT = m_pHitZoneFillIndicator->GetTransform();
        if (pT)
        {
            pT->SetPosition(0.0f, -1000.0f, 0.0f);
            pT->SetScale(0.0f, 0.0f, 0.0f);
        }
    }
}

void EnemyComponent::DestroyIndicators(Scene* pScene)
{
    // 먼저 화면에서 즉시 숨긴다.
    HideIndicators();

    auto destroyObj = [pScene](GameObject*& pObj)
        {
            if (!pObj)
                return;

            // MarkForDeletion이 다음 프레임에 처리되므로,
            // 그 사이 한 프레임이라도 보이지 않게 먼저 숨긴다.
            if (TransformComponent* pT = pObj->GetTransform())
            {
                pT->SetPosition(0.0f, -1000.0f, 0.0f);
                pT->SetScale(0.0f, 0.0f, 0.0f);
            }

            if (pScene)
            {
                pScene->MarkForDeletion(pObj);
            }

            pObj = nullptr;
        };

    // 공격 telegraph / hit zone
    destroyObj(m_pRushLineIndicator);
    destroyObj(m_pHitZoneIndicator);
    destroyObj(m_pHitZoneFillIndicator);

    // 타입 식별 메쉬 마커도 남아 있으면 같이 정리
    destroyObj(m_pHeadMarker);
    destroyObj(m_pHeadMarkerInner);
    destroyObj(m_pFootMarker);
    destroyObj(m_pFootMarkerInner);
}

XMFLOAT3 EnemyComponent::GetAttackOrigin() const
{
    if (!m_pOwner) return XMFLOAT3(0.0f, 0.0f, 0.0f);
    auto* pT = m_pOwner->GetTransform();
    if (!pT) return XMFLOAT3(0.0f, 0.0f, 0.0f);
    XMFLOAT3 pos = pT->GetPosition();
    if (m_fAttackOriginForwardOffset == 0.0f) return pos;

    float yawRad = XMConvertToRadians(pT->GetRotation().y);
    float fwdX = sinf(yawRad);
    float fwdZ = cosf(yawRad);
    pos.x += fwdX * m_fAttackOriginForwardOffset;
    pos.z += fwdZ * m_fAttackOriginForwardOffset;
    return pos;
}

bool EnemyComponent::IsTargetInForwardRect(float fWidthHalf, float fLength) const
{
    if (!m_pOwner || !m_pTarget) return false;
    auto* pMyT = m_pOwner->GetTransform();
    auto* pTgT = m_pTarget->GetTransform();
    if (!pMyT || !pTgT) return false;

    // 공격 원점 (forward offset 적용된 위치) 기준으로 로컬 좌표 계산
    XMFLOAT3 origin = GetAttackOrigin();
    XMFLOAT3 tp = pTgT->GetPosition();
    float dx = tp.x - origin.x;
    float dz = tp.z - origin.z;

    // 보스 yaw 로 역회전 → 로컬 좌표
    float yawRad = XMConvertToRadians(pMyT->GetRotation().y);
    float c = cosf(yawRad);
    float s = sinf(yawRad);
    // world → local : (x,z) * R^(-1) where R = yaw (sin,cos / cos,sin 방향 주의)
    // 보스 forward = (sin(yaw), cos(yaw)) → local z = 전방 성분
    float localZ =  dx * s + dz * c;   // 전방
    float localX =  dx * c - dz * s;   // 측면

    return (localZ >= 0.0f && localZ <= fLength && fabsf(localX) <= fWidthHalf);
}

void EnemyComponent::Die()
{
    OutputDebugString(L"[Enemy] Died!\n");

    // HideIndicators()만 하고 포인터를 nullptr로 바꾸면,
    // indicator GameObject가 Scene에 남아도 다시 삭제할 방법이 사라진다.
    // 따라서 사망 시에는 Scene에서 실제 삭제 예약까지 처리한다.
    Scene* pScene = nullptr;

    if (m_pRoom)
    {
        pScene = m_pRoom->GetScene();
    }

    if (!pScene && Dx12App::GetInstance())
    {
        pScene = Dx12App::GetInstance()->GetScene();
    }

    DestroyIndicators(pScene);

    // 타입 식별 파티클 VFX 정리
    StopTypeMarkers();

    // Notify room/callback
    if (m_OnDeathCallback)
    {
        m_OnDeathCallback(this);
    }
}

void EnemyComponent::StartBossIntro(float fStartHeight)
{
    m_eIntroPhase = BossIntroPhase::FlyingIn;
    m_fIntroTimer = 0.0f;
    m_fIntroStartHeight = fStartHeight;

    // Set target to ground level
    auto* pTransform = m_pOwner ? m_pOwner->GetTransform() : nullptr;
    if (pTransform)
    {
        XMFLOAT3 pos = pTransform->GetPosition();
        m_fIntroTargetHeight = GROUND_Y;  // Land at ground level (0)
        // Move to start position (high in sky)
        pos.y = fStartHeight;
        pTransform->SetPosition(pos);
    }

    // Start with glide animation
    if (m_pAnimationComp)
    {
        m_pAnimationComp->CrossFade("Fly Glide", 0.2f, true);
    }

    OutputDebugString(L"[Boss] Intro started - Flying In\n");
}

void EnemyComponent::UpdateBossIntro(float dt)
{
    auto* pTransform = m_pOwner ? m_pOwner->GetTransform() : nullptr;
    if (!pTransform) return;

    m_fIntroTimer += dt;

    switch (m_eIntroPhase)
    {
    case BossIntroPhase::FlyingIn:
    {
        // Descend from sky
        XMFLOAT3 pos = pTransform->GetPosition();
        float fDescendSpeed = 8.0f;
        pos.y -= fDescendSpeed * dt;

        // Face the player while descending (smooth rotation)
        if (m_pTarget)
        {
            FaceTarget(dt);
        }

        if (pos.y <= m_fIntroTargetHeight + 0.5f)
        {
            pos.y = m_fIntroTargetHeight;
            pTransform->SetPosition(pos);

            // Transition to landing
            m_eIntroPhase = BossIntroPhase::Landing;
            m_fIntroTimer = 0.0f;

            if (m_pAnimationComp)
            {
                m_pAnimationComp->CrossFade("Land", 0.15f, false);
            }
            OutputDebugString(L"[Boss] Landing\n");
        }
        else
        {
            pTransform->SetPosition(pos);
        }
        break;
    }

    case BossIntroPhase::Landing:
    {
        // Wait for landing animation (approx 1.5 seconds)
        if (m_fIntroTimer >= 1.5f)
        {
            m_eIntroPhase = BossIntroPhase::Roaring;
            m_fIntroTimer = 0.0f;

            if (m_pAnimationComp)
            {
                m_pAnimationComp->CrossFade("Scream", 0.15f, false);
            }
            OutputDebugString(L"[Boss] Roaring\n");
        }
        break;
    }

    case BossIntroPhase::Roaring:
    {
        // Wait for roar animation (approx 2 seconds)
        if (m_fIntroTimer >= 2.0f)
        {
            m_eIntroPhase = BossIntroPhase::Done;
            m_fIntroTimer = 0.0f;

            // Disable flying mode for ground combat
            m_bIsFlying = false;
            m_bOnGround = true;

            // Switch to ground combat animations
            m_AnimConfig.m_strIdleClip = "Idle01";
            m_AnimConfig.m_strChaseClip = "Walk";
            m_AnimConfig.m_strAttackClip = "Flame Attack";

            // Start combat
            ChangeState(EnemyState::Idle);
            OutputDebugString(L"[Boss] Intro complete - Combat started\n");
        }
        break;
    }

    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Threat (Aggro) System
// ─────────────────────────────────────────────────────────────────────────────

void EnemyComponent::RegisterAllPlayers(const std::vector<GameObject*>& players)
{
    m_ThreatTable.Clear();
    for (GameObject* pPlayer : players)
    {
        if (pPlayer)
        {
            m_ThreatTable.RegisterPlayer(pPlayer, ThreatConstants::INITIAL_THREAT);
        }
    }

    // 첫 번째 타겟 설정
    if (!m_pTarget && !players.empty())
    {
        m_pTarget = m_ThreatTable.GetHighestThreatTarget();
    }

#ifdef _DEBUG
    wchar_t buf[128];
    swprintf_s(buf, L"[Enemy] Registered %zu players to threat table\n", players.size());
    OutputDebugString(buf);
#endif
}

void EnemyComponent::AddThreat(GameObject* pPlayer, float fAmount)
{
    m_ThreatTable.AddThreat(pPlayer, fAmount);
}

void EnemyComponent::ReduceThreat(GameObject* pPlayer, float fAmount)
{
    m_ThreatTable.ReduceThreat(pPlayer, fAmount);
}

void EnemyComponent::ReevaluateTarget()
{
    // 죽은 플레이어 정리
    m_ThreatTable.CleanupDeadPlayers();

    // 가장 높은 위협도의 플레이어를 타겟으로 설정
    GameObject* pNewTarget = m_ThreatTable.GetHighestThreatTarget(m_pTarget);
    if (pNewTarget && pNewTarget != m_pTarget)
    {
        m_pTarget = pNewTarget;

#ifdef _DEBUG
#endif
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Boss Phase System
// ─────────────────────────────────────────────────────────────────────────────

void EnemyComponent::SetBossPhaseController(std::unique_ptr<BossPhaseController> pController)
{
    m_pPhaseController = std::move(pController);
}

bool EnemyComponent::CanUseFlyingAttack() const
{
    // 보스가 아니거나 페이즈 컨트롤러가 없으면 항상 false
    if (!m_bIsBoss || !m_pPhaseController) return false;

    // 현재 페이즈에서 비행이 허용되는지 확인
    return m_pPhaseController->CanFly();
}
