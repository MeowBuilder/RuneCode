#include "stdafx.h"
#include "SkillComponent.h"
#include "ISkillBehavior.h"
#include "SkillData.h"
#include "InputSystem.h"
#include "Camera.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "Dx12App.h" // For runtime window size
#include "NetworkManager.h" // For skill sync
#include "PlayerComponent.h" // 원격 동기화 시 element 별 wire 포맷 분기용
#include "EnemyComponent.h" // 메아리/설치 룬 적 탐색
#include "Scene.h"          // FindNearestEnemy
#include "Room.h"           // GetGameObjects (설치 룬 감지)
#include "VFXSpriteManager.h"
#include "EffectRegistry.h"
#include "FluidParticle.h"  // FluidElementColors
#include "RuneRegistry.h"
#include "FluidSkillVFXManager.h"
#include <set>
#include <random>

SkillComponent::SkillComponent(GameObject* pOwner)
    : Component(pOwner)
    , m_ChargeTargetPosition{ 0.0f, 0.0f, 0.0f }
    , m_ChannelTargetPosition{ 0.0f, 0.0f, 0.0f }
{
    // Initialize all cooldowns to 0 (ready)
    m_CooldownTimers.fill(0.0f);

    // Initialize all skill states to Ready
    m_SkillStates.fill(SkillState::Ready);

    m_chargeGatherVFXIds.fill(-1);
    m_chargeScaleSteps.fill(0);
    m_overheatConsecutive.fill(0);
    m_overheatReady.fill(false);

    // m_SkillRunes is value-initialized; EquippedRune default ctor sets runeId="" (empty)
}

SkillComponent::~SkillComponent()
{
}

void SkillComponent::SpawnChargeGatherVFX(int step)
{
    if (!m_pVFXManager) return;
    size_t slotIdx = static_cast<size_t>(m_ChargingSlot);

    if (m_chargeGatherVFXIds[slotIdx] >= 0)
    {
        m_pVFXManager->StopEffect(m_chargeGatherVFXIds[slotIdx]);
        m_chargeGatherVFXIds[slotIdx] = -1;
    }

    if (!EffectRegistry::Get().HasEffect("charge_gather")) return;

    // 변환 룬 원소 > 플레이어 기본 원소 순으로 색상 결정
    ElementType elem = ElementType::None;
    if (m_pOwner)
    {
        if (m_ChargingSlot != SkillSlot::Count)
        {
            size_t chgIdx = static_cast<size_t>(m_ChargingSlot);
            if (chgIdx < m_Skills.size() && m_Skills[chgIdx])
            {
                SkillStats sts = BuildSkillStats(m_ChargingSlot,
                    m_Skills[chgIdx]->GetSkillData().activationType);
                if (!sts.elementSet.empty()) elem = sts.elementSet[0];
            }
        }
        if (elem == ElementType::None)
            if (auto* pPC = m_pOwner->GetComponent<PlayerComponent>())
                elem = pPC->GetElementType();
    }

    static const float kScales[] = { 0.65f, 1.0f, 1.45f, 1.9f };
    int clampedStep = step < 0 ? 0 : (step > 3 ? 3 : step);
    float scale = kScales[clampedStep];

    EffectDef def = EffectRegistry::Get().GetEffect("charge_gather");
    for (auto& l : def.layers)
    {
        l.sizeScale    *= scale;
        l.particleCount = int(l.particleCount * scale);
    }
    if (elem != ElementType::None)
        ApplyElementToEffectDef(def, elem);

    DirectX::XMFLOAT3 pos = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3 up  = { 0.f, 1.f, 0.f };
    if (m_pOwner && m_pOwner->GetTransform())
        pos = m_pOwner->GetTransform()->GetPosition();

    m_chargeGatherVFXIds[slotIdx] = m_pVFXManager->SpawnEffectDef(pos, up, def, true);

    // 첫 스폰 이후 단계 전환마다 펄스 발사
    if (step > 0 && EffectRegistry::Get().HasEffect("charge_pulse"))
    {
        EffectDef pulseDef = EffectRegistry::Get().GetEffect("charge_pulse");
        for (auto& l : pulseDef.layers)
            l.sizeScale *= scale;
        if (elem != ElementType::None)
            ApplyElementToEffectDef(pulseDef, elem);
        m_pVFXManager->SpawnEffectDef(pos, up, pulseDef, false);
    }
}

void SkillComponent::Update(float deltaTime)
{
    NetworkManager* pNet = NetworkManager::GetInstance();
    if (pNet && pNet->IsConnected() && pNet->IsCutscenePlaying())
    {
        if (m_bIsCharging)
        {
            size_t idx = static_cast<size_t>(m_ChargingSlot);
            if (m_pVFXManager && idx < m_chargeGatherVFXIds.size() && m_chargeGatherVFXIds[idx] >= 0)
            {
                m_pVFXManager->StopEffect(m_chargeGatherVFXIds[idx]);
                m_chargeGatherVFXIds[idx] = -1;
            }

            m_bIsCharging = false;
            m_fChargeTime = 0.0f;
            m_ChargingSlot = SkillSlot::Count;
        }

        if (m_bIsChanneling)
        {
            m_bIsChanneling = false;
            m_fChannelTime = 0.0f;
            m_fChannelTickAccum = 0.0f;
            m_bChannelTickFiredThisFrame = false;
            m_ActiveSkillSlot = SkillSlot::Count;
        }

        return;
    }
    
    // 무한 룬 VFX 위치 추적
    if (m_infRuneVFXSlot >= 0)
    {
        m_infRuneVFXTimer -= deltaTime;
        if (m_infRuneVFXTimer <= 0.f)
            m_infRuneVFXSlot = -1;
        else if (m_pOwner && m_pOwner->GetTransform())
        {
            DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
            pos.y += 0.8f;
            VFXSpriteManager::Get().SetPosition(m_infRuneVFXSlot, pos);
        }
    }

    // 과열 발동 폭발 VFX 추적 — 플레이어 몸통
    if (m_overheatBurstVFXSlot >= 0)
    {
        m_overheatBurstVFXTimer -= deltaTime;
        if (m_overheatBurstVFXTimer <= 0.f)
            m_overheatBurstVFXSlot = -1;
        else if (m_pOwner && m_pOwner->GetTransform())
        {
            DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
            pos.y += 1.2f;
            VFXSpriteManager::Get().SetPosition(m_overheatBurstVFXSlot, pos);
        }
    }

    // 원소 공명(ABY_RES) 오라 추적 — 링을 바깥으로 펼치고 위로 띄우며 플레이어 추종
    if (m_resonanceTimer > 0.f)
    {
        m_resonanceTimer -= deltaTime;
        if (m_resonanceTimer <= 0.f)
        {
            for (auto& r : m_resonanceRing) r.slot = -1;
            m_resonanceRingCount = 0;
            m_resonanceCoreSlot  = -1;
        }
        else if (m_pOwner && m_pOwner->GetTransform())
        {
            DirectX::XMFLOAT3 center = m_pOwner->GetTransform()->GetPosition();

            // 진행도 0→1 (ease-out 으로 초반 빠르게 펼쳐짐)
            float t  = 1.f - (m_resonanceTimer / m_resonanceLife);
            float eo = 1.f - (1.f - t) * (1.f - t);
            float radius = 0.4f + eo * 2.6f;      // 0.4 → 3.0
            float rise   = 0.6f + eo * 1.4f;      // 0.6 → 2.0

            // 중앙 펄스
            if (m_resonanceCoreSlot >= 0)
            {
                DirectX::XMFLOAT3 cpos = center; cpos.y += 1.2f;
                VFXSpriteManager::Get().SetPosition(m_resonanceCoreSlot, cpos);
            }
            // 링: 회전하며 확장 + 상승
            float spin = t * DirectX::XM_2PI * 0.6f;   // 전체적으로 천천히 회전
            for (int i = 0; i < m_resonanceRingCount; ++i)
            {
                if (m_resonanceRing[i].slot < 0) continue;
                float a = m_resonanceRing[i].angle + spin;
                DirectX::XMFLOAT3 p = center;
                p.x += cosf(a) * radius;
                p.z += sinf(a) * radius;
                p.y += rise;
                VFXSpriteManager::Get().SetPosition(m_resonanceRing[i].slot, p);
            }
        }
    }

    // 과열 룬 스택 불꽃 오라 추적 — 플레이어 머리 위 가로 배치
    if (!m_overheatStackVFX.empty())
    {
        m_overheatVFXTimer -= deltaTime;
        if (m_overheatVFXTimer <= 0.f)
        {
            m_overheatStackVFX.clear();
        }
        else if (m_pOwner && m_pOwner->GetTransform())
        {
            DirectX::XMFLOAT3 base = m_pOwner->GetTransform()->GetPosition();
            base.y += 2.2f;
            int n = static_cast<int>(m_overheatStackVFX.size());
            constexpr float spacing = 0.72f;
            for (int i = 0; i < n; ++i)
            {
                DirectX::XMFLOAT3 p = base;
                p.x += (static_cast<float>(i) - (n - 1) * 0.5f) * spacing;
                VFXSpriteManager::Get().SetPosition(m_overheatStackVFX[i], p);
            }
        }
    }

    // Update cooldown timers
    for (size_t i = 0; i < static_cast<size_t>(SkillSlot::Count); ++i)
    {
        if (m_CooldownTimers[i] > 0.0f)
        {
            m_CooldownTimers[i] -= deltaTime;
            if (m_CooldownTimers[i] <= 0.0f)
            {
                m_CooldownTimers[i] = 0.0f;
                if (m_SkillStates[i] == SkillState::Cooldown)
                {
                    m_SkillStates[i] = SkillState::Ready;
                }
            }
        }

        // 무한 룬 Pending reset 소비 — Casting 끝나서 Cooldown 으로 전환된 직후 즉시 Ready 로
        if (m_pendingCooldownReset[i] && m_SkillStates[i] != SkillState::Casting)
        {
            m_CooldownTimers[i] = 0.0f;
            if (m_SkillStates[i] == SkillState::Cooldown)
                m_SkillStates[i] = SkillState::Ready;
            m_pendingCooldownReset[i] = false;
        }
    }

    // R 스킬 지연 발동 처리 (마법진 reveal 완료 후 실제 스킬 발사)
    for (auto& dc : m_delayedCasts)
        dc.timeRemain -= deltaTime;

    m_delayedCasts.erase(
        std::remove_if(m_delayedCasts.begin(), m_delayedCasts.end(),
            [&](DelayedCast& dc) -> bool
            {
                if (dc.timeRemain > 0.f) return false;
                // 상태를 Ready로 되돌려 ExecuteWithActivationType 통과
                m_SkillStates[dc.skillIndex] = SkillState::Ready;
                m_bRSkillExecuting = true;
                ExecuteWithActivationType(dc.slot, dc.target);
                m_bRSkillExecuting = false;
                return true;
            }),
        m_delayedCasts.end());

    // Update charge timer
    if (m_bIsCharging)
    {
        m_fChargeTime += deltaTime;
        float ratio = min(1.f, m_fChargeTime / m_fMaxChargeTime);

        size_t chgIdx = static_cast<size_t>(m_ChargingSlot);
        if (chgIdx < m_Skills.size() && m_Skills[chgIdx])
            m_Skills[chgIdx]->OnChargeUpdate(m_pOwner, ratio);

        // 차지 결집 VFX 위치 추적
        size_t chgSlotIdx = static_cast<size_t>(m_ChargingSlot);
        if (m_pVFXManager && m_chargeGatherVFXIds[chgSlotIdx] >= 0 && m_pOwner && m_pOwner->GetTransform())
        {
            DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
            DirectX::XMFLOAT3 up  = { 0.f, 1.f, 0.f };
            m_pVFXManager->TrackEffect(m_chargeGatherVFXIds[chgSlotIdx], pos, up);
        }

        // 차지 비율에 따라 VFX 단계 성장 (0.33 → 1단계, 0.66 → 2단계, 1.0 → 3단계)
        int newStep = (ratio >= 1.0f) ? 3 : (ratio >= 0.66f) ? 2 : (ratio >= 0.33f) ? 1 : 0;
        if (newStep > m_chargeScaleSteps[chgSlotIdx])
        {
            m_chargeScaleSteps[chgSlotIdx] = newStep;
            SpawnChargeGatherVFX(newStep);
        }
    }

    // Update channel timer
    if (m_bIsChanneling)
    {
        m_fChannelTime += deltaTime;
        m_fChannelTickAccum += deltaTime;

        // Fire tick if enough time passed
        if (m_fChannelTickAccum >= m_fChannelTickRate)
        {
            m_fChannelTickAccum -= m_fChannelTickRate;
            m_bChannelTickFiredThisFrame = true;  // 네트워크 tick 판정용 플래그

            size_t index = static_cast<size_t>(m_ActiveSkillSlot);
            if (index < m_Skills.size() && m_Skills[index])
            {
                // Combo-based channel tick damage
                RuneCombo combo = GetRuneCombo(m_ActiveSkillSlot);
                float tickMult = 0.3f;
                if (combo.hasEnhance) tickMult *= 2.0f;

                // 활성화 VFX 컨텍스트 — 채널 틱
                m_currentChargeRatio    = 0.f;
                m_bCurrentIsChannelTick = true;
                m_bCurrentEnhanceUsed  = false;

                SkillCategory tickCat = m_Skills[index]->GetCategory();
                ActivationType tickDefType = m_Skills[index]->GetSkillData().activationType;

                if (tickCat == SkillCategory::Projectile || tickDefType == ActivationType::Channel)
                {
                    // 투사체: 반복 발사 / 고유 채널(Beam 등): 기존 틱 로직
                    // 설치 룬 + 채널: 진입 시 이미 SpawnPlaceTrap 완료 → 틱에서는 skip
                    if (!combo.hasPlace)
                        ExecuteOrSplit(index, m_ChannelTargetPosition, tickMult);
                }
                else
                {
                    // Wave/AoE/Summon/Dash: 커스텀 채널 틱 (Execute 재호출 대신 OnChannelTick)
                    m_Skills[index]->OnChannelTick(m_pOwner, m_ChannelTargetPosition, tickMult);
                }
            }
        }

        // 채널링 중에도 스킬 Update() 호출 (방향 추적 등)
        {
            size_t chIndex = static_cast<size_t>(m_ActiveSkillSlot);
            if (chIndex < m_Skills.size() && m_Skills[chIndex])
            {
                m_Skills[chIndex]->Update(deltaTime);
            }
        }

        // 채널링 중 네트워크 동기화 (방향 업데이트 전송 + 채널링 tick 에 맞춰 공격 판정 요청)
        NetworkManager* pNetMgr = NetworkManager::GetInstance();
        if (pNetMgr && pNetMgr->IsConnected() && m_pOwner)
        {
            TransformComponent* pTransform = m_pOwner->GetTransform();
            if (pTransform)
            {
                // 현재 채널 중인 스킬 슬롯 → skillType. 이전엔 E 로 하드코딩되어 있어
                // Tornado(R 채널) 가 동기화되지 않았음.
                int skillType = 0;
                switch (m_ActiveSkillSlot)
                {
                case SkillSlot::Q:          skillType = 1; break;
                case SkillSlot::E:          skillType = 2; break;
                case SkillSlot::R:          skillType = 3; break;
                case SkillSlot::RightClick: skillType = 4; break;
                default:                    skillType = 0; break;
                }

                const DirectX::XMFLOAT3& pos = pTransform->GetPosition();
                DirectX::XMVECTOR lookVec = pTransform->GetLook();
                DirectX::XMFLOAT3 lookDir;
                DirectX::XMStoreFloat3(&lookDir, lookVec);

                // dir 슬롯에 target/lookDir 중 어느 걸 보낼지 element/slot 별로 결정.
                // - R : 항상 target (수신 측이 case 3 에서 target 으로 해석)
                // - Water Q (WaterPuddle), Water E (Vortex) : target (target 위치 기반 VFX)
                // - 그 외 : lookDir
                ElementType elem = ElementType::None;
                if (auto* pc = m_pOwner->GetComponent<PlayerComponent>())
                    elem = pc->GetElementType();
                bool sendTarget =
                    (skillType == 3) ||
                    (elem == ElementType::Water && (skillType == 1 || skillType == 2));

                if (sendTarget)
                {
                    pNetMgr->SendSkill(skillType, pos.x, pos.y, pos.z,
                        m_ChannelTargetPosition.x, m_ChannelTargetPosition.y, m_ChannelTargetPosition.z);
                }
                else
                {
                    pNetMgr->SendSkill(skillType, pos.x, pos.y, pos.z,
                        lookDir.x, lookDir.y, lookDir.z);
                }

                // 채널링 tick 이 발생한 이번 프레임이면 서버에 공격 판정 요청.
                if (m_bChannelTickFiredThisFrame)
                {
                    pNetMgr->SendPlayerAttack(skillType,
                        pos.x, pos.y, pos.z,
                        lookDir.x, lookDir.y, lookDir.z,
                        m_ChannelTargetPosition.x, m_ChannelTargetPosition.y, m_ChannelTargetPosition.z);
                }
            }
        }
        m_bChannelTickFiredThisFrame = false;

        // Check if channel duration expired
        if (m_fChannelTime >= m_fChannelDuration)
        {
            OutputDebugString(L"[Skill] Channel complete!\n");
            NotifyActionNet(PLAYER_ACTION_CHANNEL_END, m_ActiveSkillSlot);
            m_bIsChanneling = false;
            m_fChannelTime = 0.0f;
            m_fChannelTickAccum = 0.0f;

            size_t index = static_cast<size_t>(m_ActiveSkillSlot);
            if (index < m_Skills.size() && m_Skills[index])
            {
                m_Skills[index]->OnChannelComplete(m_pOwner, m_ChannelTargetPosition);
                m_Skills[index]->OnChannelEnd(m_pOwner);
                m_bChannelInterrupted[index] = false;
                bool keepCasting = !m_Skills[index]->IsFinished() && m_Skills[index]->HasPostChannelWork();
                if (keepCasting)
                {
                    // PostChannelWork 완료(IsFinished) 후에 쿨타임 세팅
                    m_SkillStates[index] = SkillState::Casting;
                }
                else
                {
                    m_CooldownTimers[index] = GetEffectiveCooldown(index);
                    m_SkillStates[index] = SkillState::Cooldown;
                    m_Skills[index]->Reset();
                }
            }
            m_ActiveSkillSlot = SkillSlot::Count;
        }
    }

    // Update enhance timer
    if (m_bIsEnhanced)
    {
        m_fEnhanceTimer -= deltaTime;
        if (m_fEnhanceTimer <= 0.0f)
        {
            m_bIsEnhanced = false;
            m_fEnhanceTimer = 0.0f;
            OutputDebugString(L"[Skill] Enhancement expired\n");
            NotifyActionNet(PLAYER_ACTION_ENHANCE_END, SkillSlot::Count);
        }
    }

    // Update all skills that are currently casting (channel/charge handled above)
    for (size_t slotIndex = 0; slotIndex < m_Skills.size(); ++slotIndex)
    {
        if (m_SkillStates[slotIndex] != SkillState::Casting) continue;
        if (!m_Skills[slotIndex]) continue;

        // Channel/charge 스킬은 위 블록에서 이미 처리됨
        SkillSlot thisSlot = static_cast<SkillSlot>(slotIndex);
        if (m_bIsChanneling && thisSlot == m_ActiveSkillSlot) continue;
        if (m_bIsCharging   && thisSlot == m_ChargingSlot)    continue;

        m_Skills[slotIndex]->Update(deltaTime);

        if (m_Skills[slotIndex]->IsFinished())
        {
            bool bEchoRun = m_echoRunningSlots.erase(slotIndex) > 0;
            m_Skills[slotIndex]->Reset();
            // echo 발동이었으면 쿨다운 타이머는 건드리지 않음 (이미 진행 중)
            if (!bEchoRun)
            {
                // 채널 중단 후 PostChannelWork 완료 → 50% 페널티 적용
                float cdMult = m_bChannelInterrupted[slotIndex] ? 0.5f : 1.0f;
                m_bChannelInterrupted[slotIndex] = false;
                m_CooldownTimers[slotIndex] = GetEffectiveCooldown(slotIndex) * cdMult;
            }
            // echo 종료 시 쿨다운이 이미 소진됐으면 바로 Ready
            // (echo Casting 중 타이머가 0에 도달해도 Casting 상태라 Ready 전환이 skip됐던 경우 대응)
            if (m_CooldownTimers[slotIndex] <= 0.0f)
                m_SkillStates[slotIndex] = SkillState::Ready;
            else
                m_SkillStates[slotIndex] = SkillState::Cooldown;

            if (m_ActiveSkillSlot == thisSlot)
                m_ActiveSkillSlot = SkillSlot::Count;
        }
    }

    // 메아리 지연 큐 처리 (ABY_ECO: 2초 후 가장 가까운 적을 향해 재발동)
    if (!m_echoQueue.empty())
    {
        Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
        for (auto& echo : m_echoQueue)
        {
            echo.timer -= deltaTime;
            // 마법진이 타겟 적을 따라다니도록 위치 갱신
            if (echo.decalSlot >= 0 && echo.pTarget && !echo.pTarget->IsDead())
            {
                auto* pT = echo.pTarget->GetOwner() ? echo.pTarget->GetOwner()->GetTransform() : nullptr;
                if (pT) VFXSpriteManager::Get().SetPosition(echo.decalSlot, pT->GetPosition());
            }
        }

        m_echoQueue.erase(
            std::remove_if(m_echoQueue.begin(), m_echoQueue.end(),
                [&](DeferredEcho& echo) -> bool
                {
                    if (echo.timer > 0.f) return false;
                    if (!pScene || !m_pOwner || !m_pOwner->GetTransform()) return true;

                    // 표시된 적이 살아있으면 최우선 타겟, 아니면 가장 가까운 적
                    EnemyComponent* pTarget = nullptr;
                    if (echo.pTarget && !echo.pTarget->IsDead() &&
                        echo.pTarget->GetOwner() && echo.pTarget->GetOwner()->GetTransform())
                    {
                        pTarget = echo.pTarget;
                    }
                    else
                    {
                        pTarget = pScene->FindNearestEnemy(m_pOwner->GetTransform()->GetPosition());
                    }
                    if (!pTarget || !pTarget->GetOwner() || !pTarget->GetOwner()->GetTransform())
                        return true;

                    XMFLOAT3 enemyPos = pTarget->GetOwner()->GetTransform()->GetPosition();
                    m_currentChargeRatio    = 0.f;
                    m_bCurrentIsChannelTick = false;
                    m_bCurrentEnhanceUsed  = false;
                    if (echo.index < m_Skills.size() && m_Skills[echo.index])
                    {
                        m_Skills[echo.index]->OnEchoFire(m_pOwner, enemyPos, echo.mult);
                        // IsFinished()=false면 빔 등 지속형 스킬 → Casting으로 전환해서 Update() 호출
                        if (!m_Skills[echo.index]->IsFinished())
                        {
                            m_echoRunningSlots.insert(echo.index);
                            m_SkillStates[echo.index] = SkillState::Casting;
                        }
                    }
                    return true;
                }),
            m_echoQueue.end());
    }

    // 설치 룬 함정 처리 (TRF_DEP)
    if (!m_placeQueue.empty())
    {
        Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
        CRoom* pRoom  = pScene ? pScene->GetCurrentRoom() : nullptr;

        for (auto& trap : m_placeQueue)
        {
            if (trap.windGate) continue; // 바람 문은 ProcessSkillInput에서 처리

            if (!trap.playerTrigger)
            {
                // 적이 밟으면 발동
                if (!pRoom) continue;
                for (const auto& obj : pRoom->GetGameObjects())
                {
                    if (!obj) continue;
                    auto* pEnemy = obj->GetComponent<EnemyComponent>();
                    if (!pEnemy || pEnemy->IsDead()) continue;
                    auto* pT = obj->GetTransform();
                    if (!pT) continue;
                    XMFLOAT3 ep = pT->GetPosition();
                    float dx = ep.x - trap.worldPos.x;
                    float dz = ep.z - trap.worldPos.z;
                    if (dx * dx + dz * dz <= trap.activateRadius * trap.activateRadius)
                    {
                        trap.triggered = true;
                        break;
                    }
                }
            }
            else
            {
                // 플레이어가 밟으면 발동
                if (m_pOwner && m_pOwner->GetTransform())
                {
                    XMFLOAT3 pp = m_pOwner->GetTransform()->GetPosition();
                    float dx = pp.x - trap.worldPos.x;
                    float dz = pp.z - trap.worldPos.z;
                    if (dx * dx + dz * dz <= trap.activateRadius * trap.activateRadius)
                        trap.triggered = true;
                }
            }
        }

        m_placeQueue.erase(
            std::remove_if(m_placeQueue.begin(), m_placeQueue.end(),
                [this](PlacedTrap& trap) -> bool
                {
                    if (trap.windGate) return false;
                    if (!trap.triggered) return false;
                    FirePlacedTrap(trap, trap.worldPos);
                    return true;
                }),
            m_placeQueue.end());
    }
}

void SkillComponent::ProcessSkillInput(InputSystem* pInputSystem, CCamera* pCamera)
{
    NetworkManager* pNet = NetworkManager::GetInstance();
    if (pNet && pNet->IsConnected() && pNet->IsCutscenePlaying())
        return;

    if (!pInputSystem || !pCamera) return;

    // Process rune input (1-5 keys to change activation type)
    ProcessRuneInput(pInputSystem);

    // Calculate target position
    DirectX::XMFLOAT3 targetPos = CalculateTargetPosition(pInputSystem, pCamera);

    // Handle charging state
    if (m_bIsCharging)
    {
        // Check if key is still held
        if (IsSkillKeyPressed(m_ChargingSlot, pInputSystem))
        {
            // Continue charging
            // (charge time is updated in ExecuteWithActivationType via deltaTime from Update)
        }
        else
        {
            // Key released - fire the charged skill
            size_t index = static_cast<size_t>(m_ChargingSlot);
            if (index < m_Skills.size() && m_Skills[index])
            {
                RuneCombo combo = GetRuneCombo(m_ChargingSlot);

                float chargeRatio = m_fChargeTime / m_fMaxChargeTime;
                chargeRatio = min(1.0f, chargeRatio);

                // Apply charge multiplier (1.0x to 3.0x based on charge)
                float damageMultiplier = 1.0f + chargeRatio * 2.0f;

                // Combo: Charge+Enhance rune
                if (combo.hasEnhance) damageMultiplier *= 2.0f;

                // Consume existing enhance buff
                if (m_bIsEnhanced)
                {
                    m_Skills[index]->OnEnhanceConsumed(m_pOwner, targetPos);
                    damageMultiplier *= m_fEnhanceMultiplier;
                    m_bIsEnhanced = false;
                    m_fEnhanceTimer = 0.0f;
                    OutputDebugString(L"[Skill] Enhancement consumed with Charge!\n");
                    NotifyActionNet(PLAYER_ACTION_ENHANCE_END, static_cast<SkillSlot>(index));
                }

                wchar_t buffer[128];
                swprintf_s(buffer, 128, L"[Skill] Charge released! Charge: %.0f%%, Multiplier: %.1fx\n",
                    chargeRatio * 100.0f, damageMultiplier);
                OutputDebugString(buffer);

                // 활성화 VFX 컨텍스트 세팅 (Execute 직전)
                m_currentChargeRatio = chargeRatio;
                m_bCurrentIsChannelTick = false;
                m_bCurrentEnhanceUsed = (m_bIsEnhanced); // 이미 소모됨

                if (combo.hasPlace)
                {
                    // 1. 설치 룬이면 클라에서는 함정을 생성한다.
                    SpawnPlaceTrap(index, targetPos, damageMultiplier, combo);
                }
                else
                {
                    // 2. 일반 차징 스킬이면 클라에서 스킬을 실행한다.
                    ExecuteOrSplit(index, targetPos, damageMultiplier);
                }

                // 3. 스킬 상태 갱신
                m_SkillStates[index] = SkillState::Casting;
                m_ActiveSkillSlot = m_ChargingSlot;

                // 4. 차징 해제 시점에 서버 공격 판정 요청
                //    일반 차징은 SendSkill + SendPlayerAttack 둘 다 전송
                //    설치 + 차징은 SendSkill은 생략하고 SendPlayerAttack만 전송
                SendSkillNet(
                    m_ChargingSlot,
                    targetPos,
                    chargeRatio,
                    !combo.hasPlace);

                // 쿨타임은 IsFinished() 후 Update 루프에서 세팅한다.
                // 여기서 바로 m_CooldownTimers[index]를 세팅하면 UI가 즉시 돌다가 다시 리셋될 수 있다.
            }

            size_t relIdx = static_cast<size_t>(m_ChargingSlot);
            if (m_pVFXManager && m_chargeGatherVFXIds[relIdx] >= 0)
            {
                m_pVFXManager->StopEffect(m_chargeGatherVFXIds[relIdx]);
                m_chargeGatherVFXIds[relIdx] = -1;
            }
            m_chargeScaleSteps[relIdx] = 0;
            SkillSlot endedSlot = m_ChargingSlot;
            m_bIsCharging = false;
            m_fChargeTime = 0.0f;
            m_ChargingSlot = SkillSlot::Count;
            NotifyActionNet(PLAYER_ACTION_CHARGE_END, endedSlot);
        }
        return;  // Don't process other inputs while charging
    }

    // Handle channeling state
    if (m_bIsChanneling)
    {
        m_ChannelTargetPosition = targetPos;  // 매 프레임 방향 업데이트
        if (IsSkillKeyPressed(m_ActiveSkillSlot, pInputSystem))
        {
            // Continue channeling - handled in Update
        }
        else
        {
            // Key released - stop channeling
            OutputDebugString(L"[Skill] Channel interrupted\n");
            NotifyActionNet(PLAYER_ACTION_CHANNEL_END, m_ActiveSkillSlot);
            m_bIsChanneling = false;
            m_fChannelTime = 0.0f;
            m_fChannelTickAccum = 0.0f;

            size_t index = static_cast<size_t>(m_ActiveSkillSlot);
            if (index < m_Skills.size() && m_Skills[index])
            {
                m_Skills[index]->OnChannelEnd(m_pOwner);
                m_bChannelInterrupted[index] = true;
                bool keepCasting = !m_Skills[index]->IsFinished() && m_Skills[index]->HasPostChannelWork();
                if (keepCasting)
                {
                    // PostChannelWork 완료(IsFinished) 후에 50% 페널티 쿨타임 세팅
                    m_SkillStates[index] = SkillState::Casting;
                }
                else
                {
                    m_CooldownTimers[index] = GetEffectiveCooldown(index) * 0.5f;
                    m_SkillStates[index] = SkillState::Cooldown;
                    m_Skills[index]->Reset();
                    m_bChannelInterrupted[index] = false;
                }
            }
            m_ActiveSkillSlot = SkillSlot::Count;
        }
        return;
    }

    // Check each skill slot for input
    for (size_t i = 0; i < static_cast<size_t>(SkillSlot::Count); ++i)
    {
        SkillSlot slot = static_cast<SkillSlot>(i);
        if (!IsSkillKeyPressed(slot, pInputSystem)) continue;

        // 바람 문(WindGate) 재입력: 이미 해당 슬롯에 windGate 함정이 있으면 텔레포트 돌진
        RuneCombo combo = GetRuneCombo(slot);
        if (combo.hasPlace)
        {
            auto it = std::find_if(m_placeQueue.begin(), m_placeQueue.end(),
                [i](const PlacedTrap& t){ return t.skillIndex == i && t.windGate; });
            if (it != m_placeQueue.end())
            {
                PlacedTrap trap = *it;
                m_placeQueue.erase(it);
                if (m_SkillStates[i] == SkillState::Ready || m_SkillStates[i] == SkillState::Cooldown)
                {
                    FirePlacedTrap(trap, targetPos);
                    m_SkillStates[i] = SkillState::Casting;
                    m_ActiveSkillSlot = slot;
                }
                break;
            }
        }

        ExecuteWithActivationType(slot, targetPos);
        break;  // Only use one skill per frame
    }
}

void SkillComponent::EquipSkill(SkillSlot slot, std::unique_ptr<ISkillBehavior> pBehavior)
{
    size_t index = static_cast<size_t>(slot);
    if (index < m_Skills.size())
    {
        m_Skills[index] = std::move(pBehavior);
        m_Skills[index]->SetSlot(slot);
        m_CooldownTimers[index] = 0.0f;
        m_SkillStates[index] = SkillState::Ready;
    }
}

void SkillComponent::UnequipSkill(SkillSlot slot)
{
    size_t index = static_cast<size_t>(slot);
    if (index < m_Skills.size())
    {
        m_Skills[index].reset();
        m_CooldownTimers[index] = 0.0f;
        m_SkillStates[index] = SkillState::Ready;
    }
}

ISkillBehavior* SkillComponent::GetSkill(SkillSlot slot) const
{
    size_t index = static_cast<size_t>(slot);
    if (index < m_Skills.size())
    {
        return m_Skills[index].get();
    }
    return nullptr;
}

bool SkillComponent::IsSkillReady(SkillSlot slot) const
{
    size_t index = static_cast<size_t>(slot);
    if (index >= m_Skills.size() || !m_Skills[index])
    {
        return false;
    }
    return m_SkillStates[index] == SkillState::Ready;
}

float SkillComponent::GetCooldownRemaining(SkillSlot slot) const
{
    size_t index = static_cast<size_t>(slot);
    if (index < m_CooldownTimers.size())
    {
        return m_CooldownTimers[index];
    }
    return 0.0f;
}

float SkillComponent::GetEffectiveCooldown(size_t slotIndex) const
{
    if (slotIndex >= m_Skills.size() || !m_Skills[slotIndex]) return 0.f;
    float base = m_Skills[slotIndex]->GetSkillData().cooldown;
    ActivationType defType = m_Skills[slotIndex]->GetSkillData().activationType;
    SkillStats stats = BuildSkillStats(static_cast<SkillSlot>(slotIndex), defType);
    return base * stats.cooldownMult;
}

void SkillComponent::NotifyActionNet(uint32_t actionType, SkillSlot slot, float extraY)
{
    NetworkManager* pNet = NetworkManager::GetInstance();
    if (!pNet || !pNet->IsConnected()) return;
    if (!m_pOwner || !m_pOwner->GetTransform()) return;
    DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
    pNet->SendPlayerAction(actionType, pos.x, pos.y, pos.z,
                            static_cast<float>(slot), extraY, 0.f);
}

void SkillComponent::NotifyActionNetAt(uint32_t actionType, SkillSlot slot, const DirectX::XMFLOAT3& pos, float extraY)
{
    NetworkManager* pNet = NetworkManager::GetInstance();
    if (!pNet || !pNet->IsConnected()) return;
    pNet->SendPlayerAction(actionType, pos.x, pos.y, pos.z,
                            static_cast<float>(slot), extraY, 0.f);
}

void SkillComponent::ResetCooldown(SkillSlot slot)
{
    size_t index = static_cast<size_t>(slot);
    if (index >= m_CooldownTimers.size()) return;

    m_CooldownTimers[index] = 0.0f;

    if (index < m_SkillStates.size())
    {
        if (m_SkillStates[index] == SkillState::Cooldown)
        {
            m_SkillStates[index] = SkillState::Ready;
        }
        else if (m_SkillStates[index] == SkillState::Casting)
        {
            // Casting 종료 후 라인 345/298 에서 timer=GetEffectiveCooldown 으로 덮어써지는 것을 막기 위해 pending 플래그
            m_pendingCooldownReset[index] = true;
        }
    }
}

void SkillComponent::TryTriggerInfiniteRune(SkillSlot slot, const DirectX::XMFLOAT3& hitPos)
{
    // 멀티: 무한 룬은 전적으로 서버 권위 (BuildSkillStats 에서 cdResetChance=0 으로 무효화됨).
    //       클라가 독립적으로 굴리면 이중 발동/desync 발생하므로 즉시 return.
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr && pNetMgr->IsConnected()) return;

    if (slot == SkillSlot::Count) return;

    // 슬롯에 장착된 룬 누적 스탯에서 쿨다운 초기화 확률 조회
    // (defaultType 은 fallback 용이며 cdResetChance 에 영향 없음 → 기본 Instant 사용)
    SkillStats stats = BuildSkillStats(slot, ActivationType::Instant);
    if (stats.cdResetChance <= 0.f) return;

    // RNG 롤 (ProjectileManager::ApplyDamage 와 동일 패턴)
    static std::mt19937 rng{ std::random_device{}() };
    static std::uniform_real_distribution<float> dist(0.f, 1.f);
    if (dist(rng) >= stats.cdResetChance) return;

    // 성공: 쿨다운 즉시 초기화 + 플레이어 위치 twirl (쿨타임 리셋 피드백)
    ResetCooldown(slot);

    if (m_pOwner && m_pOwner->GetTransform())
    {
        constexpr float kLifetime = 0.65f;
        DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
        pos.y += 0.8f;
        m_infRuneVFXSlot  = VFXSpriteManager::Get().Spawn("twirl1", pos, 230.f, kLifetime,
            { 1.0f, 0.88f, 0.25f, 1.0f }, 9.0f, VFXSpriteAnim::FadeOut);
        m_infRuneVFXTimer = kLifetime;
    }
}

void SkillComponent::ReduceCooldown(SkillSlot slot, float seconds)
{
    size_t index = static_cast<size_t>(slot);
    if (index >= m_CooldownTimers.size()) return;
    float prev = m_CooldownTimers[index];
    m_CooldownTimers[index] = max(0.f, m_CooldownTimers[index] - seconds);
    if (m_CooldownTimers[index] == 0.f && index < m_SkillStates.size()
        && m_SkillStates[index] == SkillState::Cooldown)
        m_SkillStates[index] = SkillState::Ready;

    // 시간 역행 룬 VFX — 실제로 쿨다운이 줄어든 경우에만 표시 (이미 0 이거나 변화 없으면 skip)
    if (prev > m_CooldownTimers[index] && m_pOwner && m_pOwner->GetTransform())
    {
        DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
        pos.y += 1.0f;
        // magic3 — 청록 시계 후광, 빠른 역회전(음수 회전 = 시간 역행 느낌)
        VFXSpriteManager::Get().Spawn("magic3", pos, 200.f, 0.55f,
            DirectX::XMFLOAT4(0.45f, 0.95f, 1.0f, 1.0f), -3.5f, VFXSpriteAnim::FadeOut);
    }
}

float SkillComponent::GetCooldownProgress(SkillSlot slot) const
{
    size_t index = static_cast<size_t>(slot);
    if (index >= m_Skills.size() || !m_Skills[index])
    {
        return 1.0f;
    }

    float cooldown = m_Skills[index]->GetSkillData().cooldown;
    if (cooldown <= 0.0f)
    {
        return 1.0f;
    }

    float remaining = m_CooldownTimers[index];
    return 1.0f - (remaining / cooldown);
}

void SkillComponent::ResetAllCooldowns()
{
    for (size_t i = 0; i < static_cast<size_t>(SkillSlot::Count); ++i)
    {
        m_CooldownTimers[i] = 0.f;
        if (m_SkillStates[i] == SkillState::Cooldown)
            m_SkillStates[i] = SkillState::Ready;
    }
    OutputDebugString(L"[Debug] All skill cooldowns reset\n");
}

bool SkillComponent::TryUseSkill(SkillSlot slot, const DirectX::XMFLOAT3& targetPosition)
{
    size_t index = static_cast<size_t>(slot);

    // Check if skill exists and is ready
    if (index >= m_Skills.size() || !m_Skills[index])
    {
        return false;
    }

    if (m_SkillStates[index] != SkillState::Ready)
    {
        return false;
    }

    // Execute the skill
    m_Skills[index]->Execute(m_pOwner, targetPosition);
    m_SkillStates[index] = SkillState::Casting;
    m_ActiveSkillSlot = slot;

    return true;
}

DirectX::XMFLOAT3 SkillComponent::CalculateTargetPosition(InputSystem* pInputSystem, CCamera* pCamera) const
{
    using namespace DirectX;

    // Get mouse position in screen space
    XMFLOAT2 mousePos = pInputSystem->GetMousePosition();

    // Convert to Normalized Device Coordinates (NDC)
    // 런타임 윈도우 크기 사용 (고DPI/해상도 변경 대응)
    float windowWidth = static_cast<float>(Dx12App::GetInstance()->GetWindowWidth());
    float windowHeight = static_cast<float>(Dx12App::GetInstance()->GetWindowHeight());
    float ndcX = (2.0f * mousePos.x / windowWidth) - 1.0f;
    float ndcY = 1.0f - (2.0f * mousePos.y / windowHeight);

    // Unproject from NDC to World Space to form a ray
    XMMATRIX viewMatrix = XMLoadFloat4x4(&pCamera->GetViewMatrix());
    XMMATRIX projMatrix = XMLoadFloat4x4(&pCamera->GetProjectionMatrix());
    XMMATRIX viewProjMatrix = viewMatrix * projMatrix;
    XMMATRIX invViewProjMatrix = XMMatrixInverse(nullptr, viewProjMatrix);

    XMVECTOR rayOrigin = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProjMatrix);
    XMVECTOR rayEnd = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProjMatrix);
    XMVECTOR rayDir = XMVector3Normalize(rayEnd - rayOrigin);

    // Define the ground plane at the owner's actual floor height
    float ownerY = (m_pOwner && m_pOwner->GetTransform())
        ? m_pOwner->GetTransform()->GetPosition().y : 0.0f;
    XMVECTOR groundPlane = XMPlaneFromPointNormal(XMVectorSet(0.0f, ownerY, 0.0f, 0.0f), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

    // Find intersection of the ray and the ground plane
    XMVECTOR intersectionPoint = XMPlaneIntersectLine(groundPlane, rayOrigin, rayOrigin + rayDir * 1000.0f);

    XMFLOAT3 result;
    XMStoreFloat3(&result, intersectionPoint);
    return result;
}

bool SkillComponent::IsSkillKeyPressed(SkillSlot slot, InputSystem* pInputSystem) const
{
    switch (slot)
    {
    case SkillSlot::Q:
        return pInputSystem->IsKeyDown('Q');
    case SkillSlot::E:
        return pInputSystem->IsKeyDown('E');
    case SkillSlot::R:
        return pInputSystem->IsKeyDown('R');
    case SkillSlot::RightClick:
        return pInputSystem->IsMouseButtonDown(1);  // Right mouse button
    default:
        return false;
    }
}

void SkillComponent::SetActivationType(ActivationType type)
{
    if (m_CurrentActivationType != type)
    {
        m_CurrentActivationType = type;

        const wchar_t* typeNames[] = { L"None", L"Instant", L"Charge", L"Channel", L"Place", L"Enhance", L"Split" };
        wchar_t buffer[128];
        swprintf_s(buffer, 128, L"[Skill] Activation type changed to: %s\n", typeNames[static_cast<int>(type)]);
        OutputDebugString(buffer);
    }
}

void SkillComponent::SetRuneSlot(SkillSlot skill, int runeIndex,
                                   const std::string& runeId, int stackCount)
{
    size_t skillIdx = static_cast<size_t>(skill);
    if (skillIdx >= static_cast<size_t>(SkillSlot::Count) || runeIndex < 0 || runeIndex >= RUNES_PER_SKILL)
        return;

    m_SkillRunes[skillIdx][runeIndex] = { runeId, stackCount };

    const wchar_t* slotNames[] = { L"Q", L"E", L"R", L"RMB" };
    wchar_t buffer[128];
    std::wstring wid(runeId.begin(), runeId.end());
    swprintf_s(buffer, 128, L"[Skill] Rune set: %s slot %d = %s\n",
        slotNames[skillIdx], runeIndex + 1, wid.c_str());
    OutputDebugString(buffer);
}

EquippedRune SkillComponent::GetRuneSlot(SkillSlot skill, int runeIndex) const
{
    size_t skillIdx = static_cast<size_t>(skill);
    if (skillIdx >= static_cast<size_t>(SkillSlot::Count) || runeIndex < 0 || runeIndex >= RUNES_PER_SKILL)
        return {};
    return m_SkillRunes[skillIdx][runeIndex];
}

void SkillComponent::ClearRuneSlot(SkillSlot skill, int runeIndex)
{
    size_t skillIdx = static_cast<size_t>(skill);
    if (skillIdx >= static_cast<size_t>(SkillSlot::Count) || runeIndex < 0 || runeIndex >= RUNES_PER_SKILL)
        return;
    m_SkillRunes[skillIdx][runeIndex] = {};
}

int SkillComponent::GetEquippedRuneCount(SkillSlot skill) const
{
    size_t skillIdx = static_cast<size_t>(skill);
    if (skillIdx >= static_cast<size_t>(SkillSlot::Count))
        return 0;

    int count = 0;
    for (int i = 0; i < RUNES_PER_SKILL; ++i)
        if (!m_SkillRunes[skillIdx][i].IsEmpty()) ++count;
    return count;
}

bool SkillComponent::HasRuneEquipped(SkillSlot skill, const char* runeId) const
{
    size_t skillIdx = static_cast<size_t>(skill);
    if (skillIdx >= static_cast<size_t>(SkillSlot::Count)) return false;
    for (int i = 0; i < RUNES_PER_SKILL; ++i)
        if (m_SkillRunes[skillIdx][i].runeId == runeId) return true;
    return false;
}

SkillStats SkillComponent::BuildSkillStats(SkillSlot skill, ActivationType defaultType) const
{
    SkillStats stats;
    stats.activationType = defaultType;

    size_t skillIdx = static_cast<size_t>(skill);
    if (skillIdx >= static_cast<size_t>(SkillSlot::Count))
        return stats;

    const RuneRegistry& reg = RuneRegistry::Get();
    bool hasL03 = false;
    bool hasABY_RES = false;
    std::set<ElementType> uniqueElements;
    for (int i = 0; i < RUNES_PER_SKILL; ++i)
    {
        const EquippedRune& er = m_SkillRunes[skillIdx][i];
        if (er.IsEmpty()) continue;
        const RuneDef* def = reg.Find(er.runeId);
        if (!def) continue;
        def->ApplyTo(stats, er.stackCount);
        if (er.runeId == "L03")     hasL03 = true;
        if (er.runeId == "ABY_RES") hasABY_RES = true;
        if (def->element != ElementType::None) uniqueElements.insert(def->element);
    }

    // 원소 증폭(L03): 2개 이상 다른 원소 장착 시 +30% 데미지
    if (hasL03 && uniqueElements.size() >= 2)
        stats.damageMult *= 1.30f;

    // 원소 공명(ABY_RES): 서로 다른 원소 변환 룬 2종 이상 장착 시 +50% 데미지
    //   (전설 슬롯 1 + 변환 룬 2개로 슬롯 3개를 모두 소모하므로 L03(+30%)보다 높게 책정)
    if (hasABY_RES && uniqueElements.size() >= 2)
    {
        stats.damageMult     *= 1.50f;
        stats.resonanceActive = true;  // ExecuteOrSplit / SpawnPlaceTrap 에서 공명 펄스 트리거용
    }

    // elementSet: VFX 색상 오버라이드용 (순서 보존)
    stats.elementSet.assign(uniqueElements.begin(), uniqueElements.end());

    // 원소 변환(L04): 시전마다 원소 무작위 변경
    if (stats.randomElementOnCast)
    {
        static std::mt19937 rng{ std::random_device{}() };
        static std::uniform_int_distribution<int> dist(0, 3);
        constexpr ElementType elements[] = {
            ElementType::Water, ElementType::Fire,
            ElementType::Earth, ElementType::Wind
        };
        stats.elementOverride = elements[dist(rng)];
    }

    // ── 멀티 게이트 ─────────────────────────────────────────────────────────
    //   서버 연결 시 룬 발동은 전적으로 서버 권위. 클라가 독립적으로 onCast/onHit 훅,
    //   RNG 기반 트리거(INF/ECO), 카운터 기반 트리거(OVL), 피격 응답(RVG)을 굴리면
    //   서버와 desync 가 나거나 이중 발동된다.
    //   → 트리거 계열 필드만 zero 화. damageMult/cooldownMult/activationType/elementSet
    //     /subVFXIds 등은 시각·입력 동작에 필요하므로 유지.
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr && pNetMgr->IsConnected())
    {
        stats.onCastHooks.clear();
        stats.onHitHooks.clear();
        stats.cdResetChance   = 0.f;  // ABY_INF
        stats.lifestealRatio  = 0.f;  // ABY_VMP
        stats.execDamageBonus = 0.f;  // ABY_EXC
        stats.revengeBonus    = 0.f;  // ABY_RVG
        stats.overheatBonus   = 0.f;  // ABY_OVL
        stats.echoOnCast      = false; // ABY_ECO
    }

    return stats;
}

ActivationType SkillComponent::GetSkillActivationType(SkillSlot skill) const
{
    ActivationType defaultType = ActivationType::Instant;
    size_t idx = static_cast<size_t>(skill);
    if (idx < m_Skills.size() && m_Skills[idx])
        defaultType = m_Skills[idx]->GetSkillData().activationType;
    return BuildSkillStats(skill, defaultType).activationType;
}

RuneCombo SkillComponent::GetRuneCombo(SkillSlot skill) const
{
    ActivationType defaultType = ActivationType::Instant;
    size_t idx = static_cast<size_t>(skill);
    if (idx < m_Skills.size() && m_Skills[idx])
        defaultType = m_Skills[idx]->GetSkillData().activationType;
    SkillStats stats = BuildSkillStats(skill, defaultType);
    // dummy — keep old count field populated
    RuneCombo combo = stats.ToRuneCombo();
    combo.count = GetEquippedRuneCount(skill);

    // legacy split flag: also set when extraProjectiles > 0
    if (stats.extraProjectiles > 0) combo.hasSplit = true;

    return combo;
}

float SkillComponent::GetChargeProgress() const
{
    if (!m_bIsCharging) return 0.0f;
    return min(1.0f, m_fChargeTime / m_fMaxChargeTime);
}

VFXModifier SkillComponent::BuildActivationVFXMod(SkillSlot slot, float chargeRatio,
                                                    bool isChannelTick, bool isEnhanceConsumed) const
{
    VFXModifier mod;
    SkillCategory cat = SkillCategory::Projectile;
    size_t idx = static_cast<size_t>(slot);
    if (idx < m_Skills.size() && m_Skills[idx])
        cat = m_Skills[idx]->GetCategory();

    if (chargeRatio > 0.01f)
    {
        float t = chargeRatio;
        mod.particleCountMult = 1.f + t * 1.0f;
        mod.strengthMult      = 1.f + t * 1.2f;
        // Wave: scale lateral spread more; Beam: scale thickness; Projectile: scale size
        if (cat == SkillCategory::Wave)
        {
            mod.sizeScaleMult = 1.f + t * 1.0f;
            mod.speedMult     = 1.f + t * 0.6f;
        }
        else if (cat == SkillCategory::Beam)
        {
            mod.sizeScaleMult = 1.f + t * 0.8f;
            mod.speedMult     = 1.f + t * 0.5f;
        }
        else  // Projectile / AoE / default
        {
            mod.sizeScaleMult = 1.f + t * 1.5f;
            mod.speedMult     = 1.f + t * 0.4f;
        }
    }
    if (isChannelTick)
    {
        mod.particleCountMult = 0.45f;
        mod.sizeScaleMult     = 0.60f;
        mod.speedMult         = 1.25f;
        mod.strengthMult      = 0.70f;
    }
    if (isEnhanceConsumed)
    {
        mod.particleCountMult *= 1.35f;
        mod.sizeScaleMult     *= 1.50f;
        mod.strengthMult      *= 2.00f;
        mod.speedMult         *= 1.10f;
    }
    return mod;
}

void SkillComponent::SpawnOverheatStackVFX(int stackCount)
{
    if (!m_pOwner || !m_pOwner->GetTransform() || stackCount <= 0) return;

    // 기존 스택 불꽃 정리 후 새로 스폰 (개수가 갱신되므로)
    for (int slot : m_overheatStackVFX)
        VFXSpriteManager::Get().Stop(slot);
    m_overheatStackVFX.clear();

    // 스택이 쌓일수록 색이 진해짐: 1스택 밝은 노랑 → 3스택 진한 주황/빨강 (모두 불투명)
    constexpr DirectX::XMFLOAT4 kStackColors[3] = {
        { 1.0f, 0.88f, 0.30f, 1.0f },  // 1스택
        { 1.0f, 0.58f, 0.12f, 1.0f },  // 2스택
        { 1.0f, 0.30f, 0.06f, 1.0f },  // 3스택 (READY)
    };
    const DirectX::XMFLOAT4& color = kStackColors[(stackCount - 1) % 3];

    // 스택 수만큼 불꽃을 머리 위에 배치 (위치는 Update에서 매 프레임 추적)
    constexpr float kLifetime = 1.3f;
    float size = 80.f + stackCount * 24.f;  // 스택 높을수록 크게 (1:104 / 2:128 / 3:152)
    DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
    pos.y += 2.2f;
    for (int i = 0; i < stackCount; ++i)
    {
        // 각 불꽃을 서로 다른 각도로 기울이고 미세하게 회전시켜 텍스처 반복이 티 안 나게
        float initRot   = (static_cast<float>(i) - (stackCount - 1) * 0.5f) * 0.6f;
        float spinSpeed = (i % 2 == 0 ? 1.f : -1.f) * 0.5f;
        int slot = VFXSpriteManager::Get().Spawn("fire1", pos, size, kLifetime,
            color, spinSpeed, VFXSpriteAnim::FadeOut, initRot);
        if (slot >= 0) m_overheatStackVFX.push_back(slot);
    }
    m_overheatVFXTimer = kLifetime;
}

void SkillComponent::SpawnOverheatBurstVFX()
{
    if (!m_pOwner || !m_pOwner->GetTransform()) return;

    // 발동: 진행 중이던 스택 불꽃 즉시 정리 (터지는 느낌)
    for (int slot : m_overheatStackVFX)
        VFXSpriteManager::Get().Stop(slot);
    m_overheatStackVFX.clear();
    m_overheatVFXTimer = 0.f;

    // 플레이어 위치에 큰 화염 폭발 한 방 (회전하며 페이드, Update에서 위치 추적)
    constexpr float kBurstLife = 0.6f;
    DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
    pos.y += 1.2f;
    m_overheatBurstVFXSlot  = VFXSpriteManager::Get().Spawn("flare1", pos, 380.f, kBurstLife,
        { 1.0f, 0.55f, 0.15f, 1.0f }, 4.0f, VFXSpriteAnim::FadeOut);
    m_overheatBurstVFXTimer = kBurstLife;
}

void SkillComponent::SpawnResonanceAura(const std::vector<ElementType>& elements)
{
    if (!m_pOwner || !m_pOwner->GetTransform()) return;

    // 이전 오라가 남아 있으면 정리 (중복 방지)
    for (auto& r : m_resonanceRing)
        if (r.slot >= 0) { VFXSpriteManager::Get().Stop(r.slot); r.slot = -1; }
    if (m_resonanceCoreSlot  >= 0) { VFXSpriteManager::Get().Stop(m_resonanceCoreSlot);  m_resonanceCoreSlot  = -1; }
    m_resonanceRingCount = 0;

    constexpr float kLife = 0.85f;
    m_resonanceLife  = kLife;
    m_resonanceTimer = kLife;

    DirectX::XMFLOAT3 center = m_pOwner->GetTransform()->GetPosition();

    // 장착된 원소가 2종 미만이면(이론상 없음) 기본 라벤더로 채움
    std::vector<ElementType> elems = elements;
    if (elems.empty())
        elems.push_back(ElementType::None);

    // 중앙 펄스 — 흰빛 flare, Update 에서 위치 추적
    {
        DirectX::XMFLOAT3 cpos = center; cpos.y += 1.2f;
        m_resonanceCoreSlot = VFXSpriteManager::Get().Spawn("flare1", cpos, 300.f, kLife,
            { 1.0f, 0.95f, 1.0f, 1.0f }, 2.5f, VFXSpriteAnim::FadeOut);
    }

    // 다원소 링 — 원 둘레에 원소 색을 번갈아 배치한 star_08 (Update 에서 펼침/상승)
    const int ringCount = kResonanceRingMax;  // 12개
    m_resonanceRingCount = ringCount;
    for (int i = 0; i < ringCount; ++i)
    {
        float angle = (DirectX::XM_2PI * static_cast<float>(i)) / static_cast<float>(ringCount);
        ElementType e = elems[static_cast<size_t>(i) % elems.size()];
        FluidElementColor ec = FluidElementColors::Get(e);
        DirectX::XMFLOAT4 col = ec.coreColor; col.w = 1.0f;

        // 초기 위치 (반지름 작게 시작, Update 에서 확장)
        DirectX::XMFLOAT3 p = center;
        p.x += cosf(angle) * 0.4f;
        p.z += sinf(angle) * 0.4f;
        p.y += 0.6f;

        int slot = VFXSpriteManager::Get().Spawn("star_08", p, 130.f, kLife,
            col, 7.0f, VFXSpriteAnim::FadeOut,
            angle);  // initialRotation 으로 별 방향 분산
        m_resonanceRing[i] = { slot, angle };
    }
}

int SkillComponent::SpawnEchoTriggerVFX(ElementType element, const XMFLOAT3& targetPos, EnemyComponent** pOutTarget)
{
    if (!m_pOwner || !m_pOwner->GetTransform()) return -1;
    Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;

    // 타겟 위치 기준으로 가장 가까운 적 탐색
    EnemyComponent* pTarget = pScene ? pScene->FindNearestEnemy(targetPos) : nullptr;
    if (pOutTarget) *pOutTarget = pTarget;

    // 마법진 위치: 적이 있으면 적 발 위치, 없으면 타겟 위치
    XMFLOAT3 spawnPos = targetPos;
    if (pTarget && pTarget->GetOwner() && pTarget->GetOwner()->GetTransform())
        spawnPos = pTarget->GetOwner()->GetTransform()->GetPosition();

    FluidElementColor ec = FluidElementColors::Get(element);
    // MagicCircle → VFXSpriteManager 직접 위임 (원소 색상 적용)
    return VFXSpriteManager::Get().Spawn(
        "magic3", spawnPos, 140.f, 2.5f, ec.coreColor, 1.5f);
}

void SkillComponent::SpawnPlaceTrap(size_t skillIndex, const XMFLOAT3& pos, float mult, const RuneCombo& /*combo*/)
{
    if (skillIndex >= m_Skills.size() || !m_Skills[skillIndex]) return;

    // 룬 데미지 배율 적용 — 설치 경로는 ExecuteOrSplit 를 거치지 않으므로
    //   원소공명(+50%)·원소증폭(L03)·기타 배율 룬이 누락된다. 여기서 직접 반영한다.
    ActivationType placeDefType = m_Skills[skillIndex]->GetSkillData().activationType;
    SkillStats placeStats = BuildSkillStats(static_cast<SkillSlot>(skillIndex), placeDefType);
    mult *= placeStats.damageMult;

    XMFLOAT3 groundPos = pos;
    groundPos.y = 0.f;

    bool playerTrig = m_Skills[skillIndex]->IsPlayerTriggered();

    // GaleRush (Dash 카테고리) → 바람 문 방식
    bool windGate = (m_Skills[skillIndex]->GetCategory() == SkillCategory::Dash);

    // 원소 색상
    ElementType elem = m_Skills[skillIndex]->GetSkillData().element;
    FluidElementColor ec = FluidElementColors::Get(elem);

    auto* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
    DecalManager* pDecal = pScene ? pScene->GetDecalManager() : nullptr;

    // 기존 함정 제거 (한 슬롯 1개 유지)
    for (auto& t : m_placeQueue)
    {
        if (t.skillIndex == skillIndex)
        {
            if (t.spriteSlot >= 0 && pDecal) pDecal->Stop(t.spriteSlot);
            t.spriteSlot = -1;
        }
    }
    m_placeQueue.erase(
        std::remove_if(m_placeQueue.begin(), m_placeQueue.end(),
            [skillIndex](const PlacedTrap& t){ return t.skillIndex == skillIndex; }),
        m_placeQueue.end());

    int spriteSlot = pDecal
        ? pDecal->Spawn(DecalTexture::Star08, groundPos, 8.f, 0.f, 30.f, ec.coreColor, 1.2f)
        : -1;

    m_placeQueue.push_back({ skillIndex, mult, groundPos, spriteSlot, 3.0f, playerTrig, windGate });
    OutputDebugString(L"[Skill] PlacedTrap spawned\n");

    // 원소 공명(ABY_RES) 오라 — 설치 시점에 다원소 링 연출.
    if (placeStats.resonanceActive)
        SpawnResonanceAura(placeStats.elementSet);

    NotifyActionNetAt(PLAYER_ACTION_PLACE_SPAWN, static_cast<SkillSlot>(skillIndex), groundPos);
}

void SkillComponent::FirePlacedTrap(PlacedTrap& trap, const XMFLOAT3& currentTargetPos)
{
    NotifyActionNetAt(PLAYER_ACTION_PLACE_FIRE, static_cast<SkillSlot>(trap.skillIndex), trap.worldPos);

    if (trap.spriteSlot >= 0)
    {
        auto* pScene2 = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
        if (pScene2 && pScene2->GetDecalManager()) pScene2->GetDecalManager()->Stop(trap.spriteSlot);
        trap.spriteSlot = -1;
    }
    if (trap.skillIndex >= m_Skills.size() || !m_Skills[trap.skillIndex]) return;

    m_currentChargeRatio    = 0.f;
    m_bCurrentIsChannelTick = false;
    m_bCurrentEnhanceUsed  = false;

    if (trap.windGate)
    {
        // GaleRush 바람 문: 실제로 캐스터를 함정 위치로 이동 후 커서 방향 돌진
        if (m_pOwner && m_pOwner->GetTransform())
            m_pOwner->GetTransform()->SetPosition(trap.worldPos);
        m_Skills[trap.skillIndex]->OnPlaceTrigger(m_pOwner, currentTargetPos, trap.damageMultiplier);
    }
    else
    {
        // 일반 트리거: 캐스터를 임시로 함정 위치로 이동해 Execute의 origin을 함정 위치로 고정
        XMFLOAT3 savedPos = {};
        bool moved = false;
        if (m_pOwner && m_pOwner->GetTransform())
        {
            savedPos = m_pOwner->GetTransform()->GetPosition();
            m_pOwner->GetTransform()->SetPosition(trap.worldPos);
            moved = true;
        }

        // 방향 타겟: 함정 위치에서 가장 가까운 적, 없으면 현재 커서 위치
        Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
        XMFLOAT3 dirTarget = currentTargetPos;
        if (pScene)
        {
            EnemyComponent* pNearest = pScene->FindNearestEnemy(trap.worldPos);
            if (pNearest && pNearest->GetOwner() && pNearest->GetOwner()->GetTransform())
                dirTarget = pNearest->GetOwner()->GetTransform()->GetPosition();
        }

        m_Skills[trap.skillIndex]->OnPlaceTrigger(m_pOwner, dirTarget, trap.damageMultiplier);

        // 캐스터 위치 복원 (GaleRush windGate가 아닌 경우 플레이어 위치 유지)
        if (moved)
            m_pOwner->GetTransform()->SetPosition(savedPos);
    }

    if (!m_Skills[trap.skillIndex]->IsFinished())
    {
        m_placeRunningSlots.insert(trap.skillIndex);
        m_SkillStates[trap.skillIndex] = SkillState::Casting;
    }
    OutputDebugString(L"[Skill] PlacedTrap triggered!\n");
}

void SkillComponent::ProcessRuneInput(InputSystem* pInputSystem)
{
    // Rune input is now handled through the drop item UI system
    // This function is kept for potential future use
}

void SkillComponent::ExecuteOrSplit(size_t index, const XMFLOAT3& target, float mult)
{
    using namespace DirectX;

    SkillSlot slot = static_cast<SkillSlot>(index);
    ActivationType defType = m_Skills[index] ? m_Skills[index]->GetSkillData().activationType : ActivationType::Instant;
    SkillStats stats = BuildSkillStats(slot, defType);
    RuneCombo combo = GetRuneCombo(slot);

    // 활성화 VFX mod 계산 + 저장 (행동 클래스가 GetCurrentActivationVFXMod()로 읽음)
    // echo guard 용: 리셋 전에 캡처 (채널 틱 여부를 echo 체크까지 보존)
    bool wasChannelTick = m_bCurrentIsChannelTick;
    m_activationVFXMod = BuildActivationVFXMod(slot, m_currentChargeRatio,
                                                m_bCurrentIsChannelTick, m_bCurrentEnhanceUsed);
    // 다음 호출을 위해 초기화
    m_currentChargeRatio    = 0.f;
    m_bCurrentIsChannelTick = false;
    m_bCurrentEnhanceUsed  = false;

    // 룬 데미지 배율 적용 — Execute에 넘기는 mult에 포함시켜 모든 스킬에 일괄 적용
    mult *= stats.damageMult;

    // 과열 보너스 (ABY_OVL): 동일 스킬 연속 3회 누적 → 4회째 +60% 발동
    //   멀티 게이트: BuildSkillStats 가 overheatBonus 를 0 으로 만들므로 클라는 데미지 mult 를
    //   적용하지 않는다 (서버 권위). 단 스택/READY/Burst 시각 누적은 ABY_RVG 와 마찬가지로
    //   서버와 동일한 동기 카운트 규칙이므로 클라가 자체 추적해도 desync 없다.
    {
        size_t slotIdx = static_cast<size_t>(slot);
        const bool hasOVL = HasRuneEquipped(slot, "ABY_OVL");
        const bool applyDmg = (stats.overheatBonus > 0.f);  // 멀티에서는 false

        if (m_overheatReady[slotIdx] && hasOVL)
        {
            // ─ 발동(4회째): 시각 burst + VFX 강화. 데미지 +60% 는 오프라인만 ─
            if (applyDmg)
                mult *= (1.f + stats.overheatBonus);
            m_overheatReady[slotIdx] = false;
            m_overheatConsecutive[slotIdx] = 0;

            m_activationVFXMod.sizeScaleMult     *= 1.45f;
            m_activationVFXMod.particleCountMult *= 1.4f;
            m_activationVFXMod.strengthMult      *= 1.6f;

            SpawnOverheatBurstVFX();
        }
        else if (hasOVL)
        {
            int cnt = ++m_overheatConsecutive[slotIdx];
            if (cnt >= 3)
            {
                m_overheatReady[slotIdx] = true;
                m_overheatConsecutive[slotIdx] = 0;
                SpawnOverheatStackVFX(3);
            }
            else
            {
                SpawnOverheatStackVFX(cnt);
            }
        }
        else
        {
            m_overheatConsecutive[slotIdx] = 0;
        }
    }

    // 보복 보너스 (ABY_RVG): 피격 후 다음 스킬 +30%
    if (stats.revengeBonus > 0.f && m_pOwner)
    {
        auto* pPlayer = m_pOwner->GetComponent<PlayerComponent>();
        if (pPlayer && pPlayer->ConsumeVengeance())
            mult *= (1.f + stats.revengeBonus);
    }

    // 원소 공명(ABY_RES) 오라 — 시전 시점에 다원소 링 + 중앙 펄스 + 소용돌이.
    //   채널 틱마다 중복 안 뜨도록 wasChannelTick 가드.
    if (stats.resonanceActive && !wasChannelTick)
        SpawnResonanceAura(stats.elementSet);

    auto invokeOnCast = [&]() {
        if (!stats.onCastHooks.empty() && m_pOwner)
        {
            SkillContext ctx;
            ctx.caster    = m_pOwner;
            ctx.targetPos = target;
            ctx.element   = m_Skills[index] ? m_Skills[index]->GetSkillData().element : ElementType::None;
            ctx.baseDamage = m_Skills[index] ? m_Skills[index]->GetSkillData().damage * mult : 0.f;
            for (auto& hook : stats.onCastHooks) hook(ctx);
        }
    };

    if (!combo.hasSplit)
    {
        m_Skills[index]->Execute(m_pOwner, target, mult);
        invokeOnCast();
        // 메아리(ABY_ECO): 최초 발동 시에만 체크 (채널 틱마다 중복 등록 방지)
        if (stats.echoOnCast && !wasChannelTick && (rand() % 100) < 50)
        {
            ElementType elem = stats.elementOverride.value_or(
                m_Skills[index] ? m_Skills[index]->GetSkillData().element : ElementType::None);
            EnemyComponent* pTarget = nullptr;
            int decalSlot = SpawnEchoTriggerVFX(elem, target, &pTarget);
            m_echoQueue.push_back({ index, mult * 0.5f, 2.0f, decalSlot, pTarget });
        }
        return;
    }

    // Split: 2개 투사체 좌우로 퍼뜨림
    XMVECTOR originV = (m_pOwner && m_pOwner->GetTransform())
        ? XMLoadFloat3(&m_pOwner->GetTransform()->GetPosition())
        : XMVectorZero();
    XMVECTOR toTarget = XMVector3Normalize(XMLoadFloat3(&target) - originV);
    XMVECTOR worldUp  = XMVectorSet(0, 1, 0, 0);
    float dot = XMVectorGetX(XMVector3Dot(toTarget, worldUp));
    XMVECTOR right = (fabsf(dot) > 0.99f)
        ? XMVectorSet(1, 0, 0, 0)
        : XMVector3Normalize(XMVector3Cross(worldUp, toTarget));
    constexpr float SPREAD = 1.5f;
    XMFLOAT3 t1, t2;
    XMStoreFloat3(&t1, XMLoadFloat3(&target) + right * SPREAD);
    XMStoreFloat3(&t2, XMLoadFloat3(&target) - right * SPREAD);
    m_Skills[index]->Execute(m_pOwner, t1, mult);
    m_Skills[index]->Execute(m_pOwner, t2, mult);
    invokeOnCast();
    // 메아리(ABY_ECO) — Split 경우도 50% 확률, 2초 후 단일 발사 (가장 가까운 적 향)
    if (stats.echoOnCast && (rand() % 100) < 50)
    {
        ElementType elem = stats.elementOverride.value_or(
            m_Skills[index] ? m_Skills[index]->GetSkillData().element : ElementType::None);
        EnemyComponent* pTarget = nullptr;
        int decalSlot = SpawnEchoTriggerVFX(elem, target, &pTarget);
        m_echoQueue.push_back({ index, mult * 0.5f, 2.0f, decalSlot, pTarget });
    }
}

void SkillComponent::ExecuteWithActivationType(SkillSlot slot, const DirectX::XMFLOAT3& targetPosition)
{
    size_t index = static_cast<size_t>(slot);

    // Check if skill exists and is ready
    if (index >= m_Skills.size() || !m_Skills[index])
    {
        return;
    }

    if (m_SkillStates[index] != SkillState::Ready)
    {
        return;
    }

    // R 스킬 발동 시 마법진 연출 후 지연 발동
    if (slot == SkillSlot::R && !m_bRSkillExecuting && m_pOwner && m_pOwner->GetTransform())
    {
        constexpr float kRevealDuration = 0.6f;

        auto* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
        XMFLOAT3 feetPos = m_pOwner->GetTransform()->GetPosition();

        if (pScene && pScene->GetDecalManager())
        {
            ElementType elem = m_Skills[index] ? m_Skills[index]->GetSkillData().element : ElementType::None;
            FluidElementColor ec = FluidElementColors::Get(elem);

            pScene->GetDecalManager()->Spawn(
                DecalTexture::MagicCircle,
                feetPos,
                12.f,
                0.f,
                3.5f,
                ec.coreColor,
                2.0f,
                kRevealDuration);
        }

        // 원격 클라에도 R 스킬 발동 마법진 표시
        NotifyActionNetAt(
            PLAYER_ACTION_R_MAGIC_CIRCLE,
            slot,
            feetPos,
            kRevealDuration);

        // 실제 발동은 reveal 완료 후
        m_delayedCasts.push_back({ slot, index, targetPosition, kRevealDuration });
        m_SkillStates[index] = SkillState::Casting;
        return;
    }

    RuneCombo combo = GetRuneCombo(slot);
    bool enhanceOnly = combo.hasEnhance && !combo.hasCharge && !combo.hasChannel && !combo.hasPlace && !combo.hasInstant;

    // 스킬의 기본 activationType을 fallback으로 사용
    ActivationType defaultType = (m_Skills[index])
        ? m_Skills[index]->GetSkillData().activationType
        : ActivationType::Instant;

    SkillCategory cat = SkillCategory::Projectile;
    if (m_Skills[index])
        cat = m_Skills[index]->GetCategory();

    if (combo.hasCharge)
    {
        m_bIsCharging = true;
        m_fChargeTime = 0.0f;
        m_ChargingSlot = slot;
        m_ChargeTargetPosition = targetPosition;
        m_SkillStates[index] = SkillState::Casting;
        NotifyActionNet(PLAYER_ACTION_CHARGE_BEGIN, slot);
        m_Skills[index]->OnChargeBegin(m_pOwner);
        m_chargeScaleSteps[index] = 0;
        SpawnChargeGatherVFX(0);
        OutputDebugString(L"[Skill] Charging started... Hold to charge, release to fire\n");
        return;
    }
    else if (combo.hasChannel || defaultType == ActivationType::Channel)
    {
        SkillStats chStats = BuildSkillStats(slot, defaultType);
        m_bIsChanneling = true;
        m_fChannelTime = 0.0f;
        m_fChannelTickAccum = 0.0f;
        m_fChannelDuration = 2.0f * chStats.channelDurationMult;
        m_ActiveSkillSlot = slot;
        m_ChannelTargetPosition = targetPosition;
        m_SkillStates[index] = SkillState::Casting;
        m_Skills[index]->OnChannelBegin(m_pOwner, targetPosition);
        OutputDebugString(L"[Skill] Channeling started... Hold to continue\n");
        NotifyActionNet(PLAYER_ACTION_CHANNEL_BEGIN, slot);

        if (cat == SkillCategory::Projectile || defaultType == ActivationType::Channel)
        {
            // 투사체 / 고유 채널(Beam): 첫 틱 즉시 발사
            float tickMult = 0.3f;
            if (combo.hasEnhance) tickMult *= 2.0f;
            if (m_bIsEnhanced)
            {
                m_Skills[index]->OnEnhanceConsumed(m_pOwner, targetPosition);
                tickMult *= m_fEnhanceMultiplier;
                m_bIsEnhanced = false;
                m_fEnhanceTimer = 0.0f;
                NotifyActionNet(PLAYER_ACTION_ENHANCE_END, static_cast<SkillSlot>(index));
            }

            // 메아리: 채널/빔 스킬은 발동 시작 시 여기서 한 번만 등록
            // (첫 틱도 m_bCurrentIsChannelTick=true로 처리되므로 ExecuteOrSplit 내 체크로는 등록 불가)
            if (chStats.echoOnCast && !combo.hasPlace && (rand() % 100) < 50)
            {
                ElementType echoElem = chStats.elementOverride.value_or(
                    m_Skills[index] ? m_Skills[index]->GetSkillData().element : ElementType::None);
                EnemyComponent* pEchoTarget = nullptr;
                int echoSlot = SpawnEchoTriggerVFX(echoElem, targetPosition, &pEchoTarget);
                m_echoQueue.push_back({ index, tickMult * 0.5f, 2.0f, echoSlot, pEchoTarget });
            }

            m_currentChargeRatio = 0.f;
            m_bCurrentIsChannelTick = true;
            m_bCurrentEnhanceUsed = false;
            if (combo.hasPlace)
                SpawnPlaceTrap(index, targetPosition, tickMult, combo);
            else
                ExecuteOrSplit(index, targetPosition, tickMult);
        }
        else
        {
            // Wave/AoE/Summon/Dash + 채널 룬: 진입 시 메인 스킬 1회 정상 발동
            float damageMultiplier = 1.0f;
            if (combo.hasEnhance) damageMultiplier *= 2.0f;
            if (m_bIsEnhanced)
            {
                m_Skills[index]->OnEnhanceConsumed(m_pOwner, targetPosition);
                damageMultiplier *= m_fEnhanceMultiplier;
                m_bIsEnhanced = false;
                m_fEnhanceTimer = 0.0f;
                NotifyActionNet(PLAYER_ACTION_ENHANCE_END, static_cast<SkillSlot>(index));
            }
            m_currentChargeRatio = 0.f;
            m_bCurrentIsChannelTick = false;
            m_bCurrentEnhanceUsed = (damageMultiplier > 1.5f);
            if (combo.hasPlace)
                SpawnPlaceTrap(index, targetPosition, damageMultiplier, combo);
            else
                ExecuteOrSplit(index, targetPosition, damageMultiplier);
        }
    }
    else if (enhanceOnly)
    {
        m_bIsEnhanced = true;
        m_fEnhanceTimer = m_fEnhanceDuration;
        m_SkillStates[index] = SkillState::Casting;
        m_ActiveSkillSlot = slot;
        NotifyActionNet(PLAYER_ACTION_ENHANCE_BEGIN, slot, m_fEnhanceDuration);

        m_currentChargeRatio = 0.f;
        m_bCurrentIsChannelTick = false;
        m_bCurrentEnhanceUsed = false;

        DirectX::XMFLOAT3 selfPos = m_pOwner->GetTransform()->GetPosition();
        m_Skills[index]->OnEnhanceActivate(m_pOwner);
        m_Skills[index]->Execute(m_pOwner, selfPos, 0.0f);
        OutputDebugString(L"[Skill] Enhanced! Next attack deals 2x damage for 5 seconds\n");
    }
    else
    {
        float damageMultiplier = 1.0f;
        if (combo.hasEnhance) damageMultiplier *= 2.0f;

        if (m_bIsEnhanced)
        {
            m_Skills[index]->OnEnhanceConsumed(m_pOwner, targetPosition);
            damageMultiplier *= m_fEnhanceMultiplier;
            m_bIsEnhanced = false;
            m_fEnhanceTimer = 0.0f;
            OutputDebugString(L"[Skill] Enhancement consumed! 2x damage!\n");
            NotifyActionNet(PLAYER_ACTION_ENHANCE_END, static_cast<SkillSlot>(index));
        }

        m_currentChargeRatio = 0.f;
        m_bCurrentIsChannelTick = false;
        m_bCurrentEnhanceUsed = (damageMultiplier > 1.5f);

        if (combo.hasPlace)
        {
            SpawnPlaceTrap(index, targetPosition, damageMultiplier, combo);
            OutputDebugString(L"[Skill] Trap placed!\n");
        }
        else
        {
            ExecuteOrSplit(index, targetPosition, damageMultiplier);
            OutputDebugString(L"[Skill] Instant cast!\n");
        }
        m_SkillStates[index] = SkillState::Casting;
        m_ActiveSkillSlot = slot;
    }

    // 네트워크로 스킬 전송
    //   설치 룬: PLACE_SPAWN 액션을 이미 보냈으므로 SendSkill 까지 보내면 원격이 일반 투사체도 함께 spawn (이중 발사).
    //   차지 룬: press 시점에 SendSkill 보내면 원격이 차지 buildup 없이 즉시 발사 VFX 를 그림.
    //            차지 발사는 release 시점에 ProcessSkillInput 의 charge 종료 블록에서 SendSkillNet 가 처리.
    if (combo.hasPlace || combo.hasCharge)
        return;

    // 네트워크로 스킬 전송
    // 일반 스킬은 chargeRatio가 없으므로 0.0f를 보낸다.
    SendSkillNet(slot, targetPosition, 0.0f, true);
}

void SkillComponent::SendSkillNet(
    SkillSlot slot,
    const DirectX::XMFLOAT3& targetPosition,
    float chargeRatio,
    bool sendSkill)
{
    // 1. 네트워크 매니저 확인
    NetworkManager* pNetMgr = NetworkManager::GetInstance();

    if (!pNetMgr || !pNetMgr->IsConnected())
        return;

    if (pNetMgr->IsCutscenePlaying())
        return;

    if (!m_pOwner)
        return;

    // 2. 스킬 슬롯을 서버 SkillType으로 변환
    int skillType = 0;

    switch (slot)
    {
    case SkillSlot::Q:          skillType = 1; break; // SKILL_TYPE_Q
    case SkillSlot::E:          skillType = 2; break; // SKILL_TYPE_E
    case SkillSlot::R:          skillType = 3; break; // SKILL_TYPE_R
    case SkillSlot::RightClick: skillType = 4; break; // SKILL_TYPE_MOUSE_RIGHT
    default:                    return;
    }

    // 3. 플레이어 위치와 방향 가져오기
    TransformComponent* pTransform = m_pOwner->GetTransform();

    if (!pTransform)
        return;

    const DirectX::XMFLOAT3& pos = pTransform->GetPosition();

    DirectX::XMVECTOR lookVec = pTransform->GetLook();
    DirectX::XMFLOAT3 lookDir;
    DirectX::XMStoreFloat3(&lookDir, lookVec);

    // 4. R 스킬과 Water Q/E는 방향 대신 target 좌표를 전송한다.
    ElementType elem = ElementType::None;

    if (auto* pc = m_pOwner->GetComponent<PlayerComponent>())
        elem = pc->GetElementType();

    bool sendTarget =
        (slot == SkillSlot::R) ||
        (elem == ElementType::Water && (slot == SkillSlot::Q || slot == SkillSlot::E));

    // 5. 원격 클라 연출용 스킬 패킷 전송
    //    설치 + 차징 조합처럼 SendSkill을 생략해야 하는 경우 sendSkill=false로 들어온다.
    if (sendSkill)
    {
        if (sendTarget)
        {
            pNetMgr->SendSkill(
                skillType,
                pos.x, pos.y, pos.z,
                targetPosition.x, targetPosition.y, targetPosition.z);
        }
        else
        {
            pNetMgr->SendSkill(
                skillType,
                pos.x, pos.y, pos.z,
                lookDir.x, lookDir.y, lookDir.z);
        }
    }

    // 6. 서버 히트 판정 요청
    //    일반 공격은 chargeRatio=0.0f,
    //    차징 해제 공격은 실제 chargeRatio를 보낸다.
    pNetMgr->SendPlayerAttack(
        skillType,
        pos.x, pos.y, pos.z,
        lookDir.x, lookDir.y, lookDir.z,
        targetPosition.x, targetPosition.y, targetPosition.z,
        chargeRatio);
}