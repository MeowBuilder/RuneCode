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
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kTransformChannelDamageRatio = 0.35f;
    constexpr float kDefaultChannelDamageRatio = 0.30f;
    constexpr float kTransformEnhanceMultiplier = 1.80f;

    float ClampRuneRatio(float ratio)
    {
        return std::clamp(ratio, 0.0f, 1.0f);
    }

    float CalculateChargeMultiplier(float chargeRatio)
    {
        chargeRatio = ClampRuneRatio(chargeRatio);

        if (chargeRatio >= 1.0f)
            return 2.5f;

        if (chargeRatio >= 0.5f)
            return 1.5f;

        return 1.0f + chargeRatio;
    }

    // 설치 룬의 대표 색상 정책.
//
// 1. 장착된 원소 룬이 있으면
//    Fire -> Water -> Wind -> Earth 순서의 첫 번째 원소
//
// 2. 원소 오버라이드만 있으면 해당 원소
//
// 3. 둘 다 없으면 원래 스킬 원소
    ElementType ResolvePrimaryPlaceVisualElement(
        const SkillStats& stats,
        ElementType fallbackElement)
    {
        if (!stats.elementSet.empty())
            return stats.elementSet.front();

        if (stats.elementOverride.has_value())
            return *stats.elementOverride;

        return fallbackElement;
    }
}

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
    if (!m_pVFXManager)
        return;

    if (m_ChargingSlot == SkillSlot::Count)
        return;

    size_t slotIdx = static_cast<size_t>(m_ChargingSlot);

    if (slotIdx >= m_chargeGatherVFXIds.size())
        return;

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
            // 컷신 등으로 채널을 강제 종료할 때, 채널 behavior(FireBeam 등)의 VFX를
            // 반드시 Reset 해야 한다. 호출하지 않으면 빔이 살아남아 무한 지속되고,
            // 이후 다른 스킬(우클릭 화염구 등)과 시선 방향이 겹쳐 "빔이 화염구에서
            // 나가는" 것처럼 보인다.
            size_t chIdx = static_cast<size_t>(m_ActiveSkillSlot);
            if (chIdx < m_Skills.size() && m_Skills[chIdx])
            {
                m_Skills[chIdx]->OnChannelEnd(m_pOwner);
                m_Skills[chIdx]->Reset();
                if (m_SkillStates[chIdx] == SkillState::Casting)
                    m_SkillStates[chIdx] = SkillState::Cooldown;
            }
            ClearCombinedChannelState();
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

    // 시간역행 룬 시계 VFX 위치 추적
    if (m_timeRewindVFXSlot >= 0)
    {
        m_timeRewindVFXTimer -= deltaTime;
        if (m_timeRewindVFXTimer <= 0.f)
            m_timeRewindVFXSlot = -1;
        else if (m_pOwner && m_pOwner->GetTransform())
        {
            DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
            pos.y += 2.2f;
            VFXSpriteManager::Get().SetPosition(m_timeRewindVFXSlot, pos);
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

        // Casting 종료(→Cooldown 전환) 직후, 시전 중 누적된 룬 쿨다운 효과를 실제 타이머에 소비.
        // (시전 중에는 쿨다운이 0이라 즉시 적용하면 버려지므로 여기서 일괄 반영)
        if (m_SkillStates[i] != SkillState::Casting)
        {
            // 시간역행(ABY_TIM): 누적 감소량 적용
            if (m_pendingCooldownReduce[i] > 0.0f)
            {
                m_CooldownTimers[i] = max(0.0f, m_CooldownTimers[i] - m_pendingCooldownReduce[i]);
                m_pendingCooldownReduce[i] = 0.0f;
                if (m_CooldownTimers[i] <= 0.0f && m_SkillStates[i] == SkillState::Cooldown)
                    m_SkillStates[i] = SkillState::Ready;
            }
            // 무한(ABY_INF): 즉시 초기화 (reset 이 reduce 보다 우선 — 어차피 0)
            if (m_pendingCooldownReset[i])
            {
                m_CooldownTimers[i] = 0.0f;
                if (m_SkillStates[i] == SkillState::Cooldown)
                    m_SkillStates[i] = SkillState::Ready;
                m_pendingCooldownReset[i] = false;
            }
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
                // 다른 스킬이 채널/차지 중이면 발동을 보류한다.
                //   채널/차지 상태는 m_ActiveSkillSlot / m_ChargingSlot 같은 전역 변수 하나로
                //   관리되므로, 지금 지연 스킬을 발동하면 그 변수를 덮어써 진행 중인 채널이
                //   영원히 종료되지 않는다(예: 파이어빔 무한 지속). 채널/차지가 끝날 때까지
                //   큐에 유지했다가 그 직후 프레임에 발동한다.
                if (m_bIsChanneling || m_bIsCharging) return false;
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
        // 상태 불일치 방어:
        // charging은 true인데 슬롯이 Count이면 배열에 접근하지 않고 상태를 종료한다.
        if (m_ChargingSlot == SkillSlot::Count)
        {
            m_bIsCharging = false;
            m_fChargeTime = 0.0f;
        }
        else
        {
            const size_t chgSlotIdx =
                static_cast<size_t>(m_ChargingSlot);

            const bool validChargeSlot =
                chgSlotIdx < m_Skills.size() &&
                chgSlotIdx < m_chargeGatherVFXIds.size() &&
                chgSlotIdx < m_chargeScaleSteps.size();

            if (!validChargeSlot)
            {
                m_bIsCharging = false;
                m_fChargeTime = 0.0f;
                m_ChargingSlot = SkillSlot::Count;
            }
            else
            {
                m_fChargeTime += deltaTime;

                const float ratio = ClampRuneRatio(
                    m_fChargeTime / m_fMaxChargeTime);

                if (m_Skills[chgSlotIdx])
                {
                    m_Skills[chgSlotIdx]->OnChargeUpdate(
                        m_pOwner,
                        ratio);
                }

                // 차지 결집 VFX 위치 추적
                if (m_pVFXManager &&
                    m_chargeGatherVFXIds[chgSlotIdx] >= 0 &&
                    m_pOwner &&
                    m_pOwner->GetTransform())
                {
                    DirectX::XMFLOAT3 pos =
                        m_pOwner->GetTransform()->GetPosition();

                    DirectX::XMFLOAT3 up =
                    { 0.f, 1.f, 0.f };

                    m_pVFXManager->TrackEffect(
                        m_chargeGatherVFXIds[chgSlotIdx],
                        pos,
                        up);
                }

                // 차지 비율에 따라 VFX 단계 성장
                const int newStep =
                    ratio >= 1.0f ? 3 :
                    ratio >= 0.66f ? 2 :
                    ratio >= 0.33f ? 1 : 0;

                if (newStep >
                    m_chargeScaleSteps[chgSlotIdx])
                {
                    m_chargeScaleSteps[chgSlotIdx] =
                        newStep;

                    SpawnChargeGatherVFX(newStep);
                }
            }
        }
    }

    // Update channel timer
    if (m_bIsChanneling)
    {
        m_fChannelTime += deltaTime;
        m_fChannelTickAccum += deltaTime;

        // 실제 틱 간격이 지난 경우에만 공격 실행 및 네트워크 전송
        while (m_fChannelTickAccum >= m_fChannelTickRate)
        {
            m_fChannelTickAccum -= m_fChannelTickRate;

            ExecuteCombinedChannelTick();
        }

        // 채널링 중 스킬 자체 Update 호출
        const size_t channelIndex =
            static_cast<size_t>(m_ActiveSkillSlot);

        if (channelIndex < m_Skills.size() &&
            m_Skills[channelIndex])
        {
            m_Skills[channelIndex]->Update(deltaTime);
        }

        // 채널 지속시간 종료
        if (m_fChannelTime >= m_fChannelDuration)
        {
            const SkillSlot endedSlot =
                m_ActiveSkillSlot;

            const size_t endedIndex =
                static_cast<size_t>(endedSlot);

            const bool wasPlaceChannel =
                m_bChannelPlaceMode;

            const DirectX::XMFLOAT3 placeOrigin =
                m_ChannelOriginPosition;

            NotifyActionNet(
                PLAYER_ACTION_CHANNEL_END,
                endedSlot);

            if (wasPlaceChannel)
            {
                NotifyActionNetAt(
                    PLAYER_ACTION_PLACE_FIRE,
                    endedSlot,
                    placeOrigin);
            }

            if (endedIndex < m_Skills.size() &&
                m_Skills[endedIndex])
            {
                m_Skills[endedIndex]->OnChannelComplete(
                    m_pOwner,
                    m_ChannelTargetPosition);

                m_Skills[endedIndex]->OnChannelEnd(
                    m_pOwner);

                m_bChannelInterrupted[endedIndex] =
                    false;

                const bool keepCasting =
                    !m_Skills[endedIndex]->IsFinished() &&
                    m_Skills[endedIndex]->HasPostChannelWork();

                if (keepCasting)
                {
                    m_SkillStates[endedIndex] =
                        SkillState::Casting;
                }
                else
                {
                    m_CooldownTimers[endedIndex] =
                        GetEffectiveCooldown(endedIndex);

                    m_SkillStates[endedIndex] =
                        SkillState::Cooldown;

                    m_Skills[endedIndex]->Reset();
                }
            }

            ClearCombinedChannelState();
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
        // 키를 누르고 있으면 Update()에서 차징을 계속한다.
        if (IsSkillKeyPressed(m_ChargingSlot, pInputSystem))
        {
            return;
        }

        // 키가 떼어진 시점의 슬롯과 차지 비율을 먼저 보존한다.
        const SkillSlot endedSlot = m_ChargingSlot;
        const size_t index = static_cast<size_t>(endedSlot);

        const float chargeRatio =
            ClampRuneRatio(m_fChargeTime / m_fMaxChargeTime);

        RuneCombo combo{};

        const bool validSkill =
            endedSlot != SkillSlot::Count &&
            index < m_Skills.size() &&
            m_Skills[index] != nullptr;

        if (validSkill)
            combo = GetRuneCombo(endedSlot);

        // 차징 VFX 정리
        if (endedSlot != SkillSlot::Count)
        {
            if (index < m_chargeGatherVFXIds.size())
            {
                if (m_pVFXManager &&
                    m_chargeGatherVFXIds[index] >= 0)
                {
                    m_pVFXManager->StopEffect(
                        m_chargeGatherVFXIds[index]);

                    m_chargeGatherVFXIds[index] = -1;
                }
            }

            if (index < m_chargeScaleSteps.size())
                m_chargeScaleSteps[index] = 0;
        }

        // 차징 상태를 먼저 끝낸다.
        // CHG+CHN일 경우 이후 채널 상태로 전환되므로
        // 이전 차징 상태가 남아 있으면 안 된다.
        m_bIsCharging = false;
        m_fChargeTime = 0.0f;
        m_ChargingSlot = SkillSlot::Count;

        if (endedSlot != SkillSlot::Count)
        {
            NotifyActionNet(
                PLAYER_ACTION_CHARGE_END,
                endedSlot);
        }

        if (!validSkill)
            return;

        // CHG + CHN:
        // 차징 해제와 동시에 저장된 차지 비율로 채널을 시작한다.
        if (combo.hasChannel)
        {
            BeginCombinedChannel(
                endedSlot,
                targetPos,
                chargeRatio);

            return;
        }

        // 서버와 동일한 차징 배율
        float damageMultiplier =
            CalculateChargeMultiplier(chargeRatio);

        // CHG + EMP:
        // 증강 룬이 이번 시전에 결합된 경우 현재 공격에 바로 적용한다.
        const bool embeddedEnhance = combo.hasEnhance;

        if (embeddedEnhance)
            damageMultiplier *= kTransformEnhanceMultiplier;

        // 과거 EMP 단독 시전으로 저장해 둔 버프 소비
        const bool storedEnhanceUsed = m_bIsEnhanced;

        if (storedEnhanceUsed)
        {
            m_Skills[index]->OnEnhanceConsumed(
                m_pOwner,
                targetPos);

            damageMultiplier *= m_fEnhanceMultiplier;

            m_bIsEnhanced = false;
            m_fEnhanceTimer = 0.0f;

            NotifyActionNet(
                PLAYER_ACTION_ENHANCE_END,
                endedSlot);
        }

        // VFX 실행 정보
        m_currentChargeRatio = chargeRatio;
        m_bCurrentIsChannelTick = false;
        m_bCurrentEnhanceUsed =
            embeddedEnhance || storedEnhanceUsed;

        NetworkManager* pNetMgr =
            NetworkManager::GetInstance();

        const bool online =
            pNetMgr &&
            pNetMgr->IsConnected();

        if (combo.hasPlace)
        {
            // 오프라인에서는 클라이언트가 직접 함정을 관리한다.
            if (!online)
            {
                SpawnPlaceTrap(
                    index,
                    targetPos,
                    damageMultiplier,
                    combo);
            }

            // 온라인에서는 서버가 설치 공격을 생성하므로
            // 일반 스킬 VFX 패킷 없이 공격 요청만 보낸다.
            if (online)
            {
                SendSkillNet(
                    endedSlot,
                    targetPos,
                    chargeRatio,
                    false);
            }
        }
        else
        {
            ExecuteOrSplit(
                index,
                targetPos,
                damageMultiplier);

            SendSkillNet(
                endedSlot,
                targetPos,
                chargeRatio,
                true);
        }

        m_SkillStates[index] = SkillState::Casting;
        m_ActiveSkillSlot = endedSlot;

        return;
    }

    // Handle channeling state
    if (m_bIsChanneling)
    {
        m_ChannelTargetPosition =
            targetPos;

        if (IsSkillKeyPressed(
            m_ActiveSkillSlot,
            pInputSystem))
        {
            return;
        }

        const SkillSlot endedSlot =
            m_ActiveSkillSlot;

        const size_t index =
            static_cast<size_t>(endedSlot);

        const bool wasPlaceChannel =
            m_bChannelPlaceMode;

        const DirectX::XMFLOAT3 placeOrigin =
            m_ChannelOriginPosition;

        OutputDebugString(
            L"[Skill] Channel interrupted\n");

        NotifyActionNet(
            PLAYER_ACTION_CHANNEL_END,
            endedSlot);

        if (wasPlaceChannel)
        {
            NotifyActionNetAt(
                PLAYER_ACTION_PLACE_FIRE,
                endedSlot,
                placeOrigin);
        }

        if (index < m_Skills.size() &&
            m_Skills[index])
        {
            m_Skills[index]->OnChannelEnd(
                m_pOwner);

            m_bChannelInterrupted[index] =
                true;

            const bool keepCasting =
                !m_Skills[index]->IsFinished() &&
                m_Skills[index]->HasPostChannelWork();

            if (keepCasting)
            {
                m_SkillStates[index] =
                    SkillState::Casting;
            }
            else
            {
                m_CooldownTimers[index] =
                    GetEffectiveCooldown(index) *
                    0.5f;

                m_SkillStates[index] =
                    SkillState::Cooldown;

                m_Skills[index]->Reset();

                m_bChannelInterrupted[index] =
                    false;
            }
        }

        ClearCombinedChannelState();
        m_ActiveSkillSlot = SkillSlot::Count;

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

void SkillComponent::ReduceCooldown(SkillSlot slot, float seconds, bool playClockVFX)
{
    size_t index = static_cast<size_t>(slot);
    if (index >= m_CooldownTimers.size() || seconds <= 0.f) return;

    bool applied = false;
    if (index < m_SkillStates.size() && m_SkillStates[index] == SkillState::Casting)
    {
        // 시전 중 → 쿨다운이 아직 시작되지 않음(0). 감소량을 누적했다가
        // Casting 종료 후 Update 의 소비 블록에서 실제 타이머에 반영.
        m_pendingCooldownReduce[index] += seconds;
        applied = true;
    }
    else
    {
        float prev = m_CooldownTimers[index];
        m_CooldownTimers[index] = max(0.f, m_CooldownTimers[index] - seconds);
        if (m_CooldownTimers[index] == 0.f && index < m_SkillStates.size()
            && m_SkillStates[index] == SkillState::Cooldown)
            m_SkillStates[index] = SkillState::Ready;
        applied = prev > m_CooldownTimers[index];
    }

    // 시간 역행 룬 VFX — 감소가 등록된 경우(live 적용 또는 시전 중 누적) 표시.
    // 이전 시계가 남아 있으면 Stop 후 교체 → 항상 하나만 플레이어를 따라다님.
    if (applied && playClockVFX && m_pOwner && m_pOwner->GetTransform())
    {
        constexpr float kClockLife = 0.85f;
        if (m_timeRewindVFXSlot >= 0) VFXSpriteManager::Get().Stop(m_timeRewindVFXSlot);
        DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
        pos.y += 2.2f;
        // clock — 청록 시계가 거꾸로 도는 연출 (음수 회전 = 시간 역행)
        m_timeRewindVFXSlot  = VFXSpriteManager::Get().Spawn("clock", pos, 150.f, kClockLife,
            DirectX::XMFLOAT4(0.45f, 0.95f, 1.0f, 1.0f), -4.5f, VFXSpriteAnim::FadeOut);
        m_timeRewindVFXTimer = kClockLife;
    }
}

void SkillComponent::ApplyOnHitRunes(SkillSlot slot, const SkillStats& stats,
                                     float baseDamage, float dealtDamage,
                                     void* hitEnemy, const DirectX::XMFLOAT3& hitEnemyPos,
                                     void* scene)
{
    // onHit 훅 (시간역행 ABY_TIM, 연쇄 등) — skillSlot 이 채워져야 슬롯 의존 훅이 동작
    if (!stats.onHitHooks.empty())
    {
        SkillContext ctx;
        ctx.caster             = m_pOwner;
        ctx.baseDamage         = baseDamage;
        ctx.damageDealt        = dealtDamage;
        ctx.skillSlot          = slot;
        ctx.scene              = scene;
        ctx.hitEnemy           = hitEnemy;
        ctx.hitEnemyPos        = hitEnemyPos;
        ctx.statusChanceMult   = stats.statusChanceMult;
        ctx.statusDurationMult = stats.statusDurationMult;
        for (auto& hook : stats.onHitHooks) hook(ctx);
    }

    // 흡혈 (ABY_VMP) — 피해의 lifestealRatio 만큼 시전자 HP 회복
    if (stats.lifestealRatio > 0.f && m_pOwner)
    {
        if (auto* pPlayer = m_pOwner->GetComponent<PlayerComponent>())
        {
            float healAmount = dealtDamage * stats.lifestealRatio;
            if (healAmount > 0.f)
            {
                pPlayer->Heal(healAmount);
                // 멀티에서는 서버 권위 RUNE_TRIGGER 가 시각화 → 오프라인 한정 펄스
                NetworkManager* pNet = NetworkManager::GetInstance();
                if (!(pNet && pNet->IsConnected()))
                    pPlayer->TriggerLifestealVFX(healAmount);
            }
        }
    }

    // 무한 (ABY_INF) — RNG/멀티 게이트/쿨다운 초기화/VFX 는 함수 내부에서 일괄 처리
    if (slot != SkillSlot::Count)
        TryTriggerInfiniteRune(slot, hitEnemyPos);
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

    size_t skillIdx = static_cast<size_t>(skill);

    if (skillIdx >= static_cast<size_t>(SkillSlot::Count))
    {
        stats.ApplyDefaultActivation(defaultType);
        stats.activationType = stats.ResolvePrimaryActivation();
        return stats;
    }

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

        stats.cdResetChance = 0.f;
        stats.lifestealRatio = 0.f;
        stats.execDamageBonus = 0.f;
        stats.revengeBonus = 0.f;
        stats.overheatBonus = 0.f;
        stats.echoOnCast = false;
    }

    // 발동 룬이 하나도 없으면 스킬의 기존 발동 방식을 사용한다.
    stats.ApplyDefaultActivation(defaultType);

    // UI와 레거시 코드에서 사용하는 대표 발동 타입을 계산한다.
    // 실제 복합 룬 실행은 activation 플래그들과 RuneCombo를 사용한다.
    stats.activationType = stats.ResolvePrimaryActivation();

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

    // 온라인에서는 설치 함정과 데칼을 서버가 확정한다.
//
// 여기서 로컬 m_placeQueue와 데칼까지 만들면
// 서버 S_RUNE_TRIGGER를 받을 때 같은 위치에 데칼이 한 번 더 생성된다.
    NetworkManager* pNetworkManager =
        NetworkManager::GetInstance();

    if (pNetworkManager &&
        pNetworkManager->IsConnected())
    {
        OutputDebugString(
            L"[Skill] Online place trap waits for server confirmation.\n");

        return;
    }

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

    // 서버와 원격 클라이언트가 사용하는 정책과 동일하게
 // 장착된 원소 룬 조합에서 대표 원소를 결정한다.
    const ElementType elem =
        ResolvePrimaryPlaceVisualElement(
            placeStats,
            m_Skills[skillIndex]
            ->GetSkillData()
            .element);

    FluidElementColor ec =
        FluidElementColors::Get(elem);

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

    // 오프라인 설치 함정도 서버 정책과 동일하게
// 함정이 실제로 발동한 시점에 ABY_ECO 확률을 판정한다.
//
// 온라인에서는 BuildSkillStats가 echoOnCast=false로 만들기 때문에
// 서버와 이중 발동하지 않는다.
    {
        const SkillSlot echoSkillSlot =
            static_cast<SkillSlot>(
                trap.skillIndex);

        const ActivationType defaultType =
            m_Skills[trap.skillIndex]
            ->GetSkillData()
            .activationType;

        const SkillStats echoStats =
            BuildSkillStats(
                echoSkillSlot,
                defaultType);

        if (echoStats.echoOnCast &&
            (rand() % 100) < 50)
        {
            const ElementType echoElement =
                echoStats
                .elementOverride
                .value_or(
                    m_Skills
                    [trap.skillIndex]
                    ->GetSkillData()
                    .element);

            EnemyComponent* pEchoTarget =
                nullptr;

            const int echoVFXSlot =
                SpawnEchoTriggerVFX(
                    echoElement,
                    trap.worldPos,
                    &pEchoTarget);

            // 설치 당시 저장된 배율을 기준으로
            // 2초 뒤 50% 위력 메아리 발동
            m_echoQueue.push_back(
                {
                    trap.skillIndex,
                    trap.damageMultiplier * 0.5f,
                    2.0f,
                    echoVFXSlot,
                    pEchoTarget
                });
        }
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

    // 오프라인 ABY_ECO 예약.
//
// 다연발로 실제 투사체가 여러 개 생성되더라도
// 메아리는 스킬 시전 1회당 한 번만 판정한다.
    auto ScheduleEchoOnce = [&]()
        {
            if (!stats.echoOnCast)
                return;

            if (wasChannelTick)
                return;

            if ((rand() % 100) >= 50)
                return;

            const ElementType element =
                stats.elementOverride
                .value_or(
                    m_Skills[index]
                    ? m_Skills[index]
                    ->GetSkillData()
                    .element
                    : ElementType::None);

            EnemyComponent* targetEnemy =
                nullptr;

            const int decalSlot =
                SpawnEchoTriggerVFX(
                    element,
                    target,
                    &targetEnemy);

            m_echoQueue.push_back(
                {
                    index,
                    mult * 0.5f,
                    2.0f,
                    decalSlot,
                    targetEnemy
                });
        };

    // TRF_MLT는 클라이언트 정의상
    // 투사체형 스킬에만 적용한다.
    const bool isProjectileSkill =
        m_Skills[index] &&
        m_Skills[index]
        ->GetCategory() ==
        SkillCategory::Projectile;

    // 클라이언트 RuneRegistry 정책:
    //
    // 기본 1발 + 스택당 추가 2발
    const int projectileCount =
        isProjectileSkill
        ? std::max(
            1,
            1 +
            stats.extraProjectiles)
        : 1;

    // 다연발이 아니거나 투사체 스킬이 아니면
    // 기존 스킬을 한 번만 실행한다.
    if (projectileCount <= 1)
    {
        m_Skills[index]->Execute(
            m_pOwner,
            target,
            mult);

        invokeOnCast();
        ScheduleEchoOnce();

        return;
    }

    if (m_pOwner == nullptr ||
        m_pOwner->GetTransform() ==
        nullptr)
    {
        m_Skills[index]->Execute(
            m_pOwner,
            target,
            mult);

        invokeOnCast();
        ScheduleEchoOnce();

        return;
    }

    // 원래 클라이언트 구현과 동일하게
    // 목표 지점의 좌우 방향으로 1.5m씩 벌린다.
    const DirectX::XMFLOAT3 origin =
        m_pOwner
        ->GetTransform()
        ->GetPosition();

    const DirectX::XMVECTOR
        originVector =
        DirectX::XMLoadFloat3(
            &origin);

    const DirectX::XMVECTOR
        targetVector =
        DirectX::XMLoadFloat3(
            &target);

    DirectX::XMVECTOR
        toTarget =
        targetVector -
        originVector;

    // 부채꼴은 수평면 기준으로 계산한다.
    toTarget =
        DirectX::XMVectorSetY(
            toTarget,
            0.0f);

    const float directionLengthSq =
        DirectX::XMVectorGetX(
            DirectX::XMVector3LengthSq(
                toTarget));

    if (directionLengthSq <=
        0.000001f)
    {
        m_Skills[index]->Execute(
            m_pOwner,
            target,
            mult);

        invokeOnCast();
        ScheduleEchoOnce();

        return;
    }

    toTarget =
        DirectX::XMVector3Normalize(
            toTarget);

    const DirectX::XMVECTOR worldUp =
        DirectX::XMVectorSet(
            0.0f,
            1.0f,
            0.0f,
            0.0f);

    const DirectX::XMVECTOR right =
        DirectX::XMVector3Normalize(
            DirectX::XMVector3Cross(
                worldUp,
                toTarget));

    // 클라이언트 기존 분열 코드의 간격을 유지한다.
    constexpr float FAN_SPACING =
        1.5f;

    const float center =
        static_cast<float>(
            projectileCount - 1) *
        0.5f;

    for (int projectileIndex = 0;
        projectileIndex <
        projectileCount;
        ++projectileIndex)
    {
        // 예:
        // 3발 → -1, 0, +1
        // 5발 → -2, -1, 0, +1, +2
        const float offsetIndex =
            static_cast<float>(
                projectileIndex) -
            center;

        const float lateralOffset =
            offsetIndex *
            FAN_SPACING;

        DirectX::XMFLOAT3
            projectileTarget;

        DirectX::XMStoreFloat3(
            &projectileTarget,
            targetVector +
            right *
            lateralOffset);

        m_Skills[index]->Execute(
            m_pOwner,
            projectileTarget,
            mult);
    }

    // 투사체가 여러 개여도 onCast는 한 번
    invokeOnCast();

    // 메아리도 시전당 한 번
    ScheduleEchoOnce();
}

void SkillComponent::BeginCombinedChannel(
    SkillSlot slot,
    const DirectX::XMFLOAT3& targetPosition,
    float chargeRatio)
{
    const size_t index = static_cast<size_t>(slot);

    if (index >= m_Skills.size() ||
        !m_Skills[index])
    {
        return;
    }

    // 이전 채널 상태가 남아 있으면 정리
    ClearCombinedChannelState();

    const ActivationType defaultType =
        m_Skills[index]->GetSkillData().activationType;

    const SkillStats stats =
        BuildSkillStats(slot, defaultType);

    const RuneCombo combo =
        stats.ToRuneCombo();

    m_bIsChanneling = true;
    m_fChannelTime = 0.0f;
    m_fChannelTickAccum = 0.0f;

    m_fChannelDuration =
        2.0f * stats.channelDurationMult;

    m_fChannelChargeRatio =
        ClampRuneRatio(chargeRatio);

    // 차징이 결합된 경우 그 배율을 모든 채널 틱에 유지
    m_fChannelBaseMultiplier =
        m_fChannelChargeRatio > 0.0f
        ? CalculateChargeMultiplier(m_fChannelChargeRatio)
        : 1.0f;

    // TRF_CHN 룬 채널은 틱당 35%,
    // 스킬 자체가 원래 채널인 경우는 기존 30%
    m_fChannelTickDamageRatio =
        HasRuneEquipped(slot, "TRF_CHN")
        ? kTransformChannelDamageRatio
        : kDefaultChannelDamageRatio;

    m_bChannelPlaceMode =
        combo.hasPlace;

    // EMP가 구조적 발동 룬과 같이 장착되면
    // 버프 저장이 아니라 이번 채널 전체에 직접 적용
    m_bChannelEmbeddedEnhance =
        combo.hasEnhance;

    // 이전에 EMP 단독 시전으로 저장된 버프가 있다면
    // 채널 첫 틱에서만 소비
    m_bChannelConsumeStoredEnhance =
        m_bIsEnhanced;

    m_ChannelTargetPosition =
        targetPosition;

    m_ActiveSkillSlot =
        slot;

    m_SkillStates[index] =
        SkillState::Casting;

    // 일반 채널은 플레이어 위치,
    // 설치 채널은 지정한 설치 위치를 공격 원점으로 사용
    if (m_bChannelPlaceMode)
    {
        m_ChannelOriginPosition =
            targetPosition;

        // 설치형 채널 바닥 표식
        Scene* pScene =
            Dx12App::GetInstance()
            ? Dx12App::GetInstance()->GetScene()
            : nullptr;

        DecalManager* pDecal =
            pScene
            ? pScene->GetDecalManager()
            : nullptr;

        const ElementType element =
            ResolvePrimaryPlaceVisualElement(
                stats,
                m_Skills[index]
                ->GetSkillData()
                .element);

        const FluidElementColor color =
            FluidElementColors::Get(element);

        if (pDecal)
        {
            m_ChannelPlaceDecalSlot =
                pDecal->Spawn(
                    DecalTexture::Star08,
                    m_ChannelOriginPosition,
                    8.0f,
                    0.0f,
                    m_fChannelDuration + 1.0f,
                    color.coreColor,
                    1.2f);
        }

        NotifyActionNetAt(
            PLAYER_ACTION_PLACE_SPAWN,
            slot,
            m_ChannelOriginPosition);
    }
    else if (m_pOwner &&
        m_pOwner->GetTransform())
    {
        m_ChannelOriginPosition =
            m_pOwner
            ->GetTransform()
            ->GetPosition();
    }

    m_Skills[index]->OnChannelBegin(
        m_pOwner,
        targetPosition);

    NotifyActionNet(
        PLAYER_ACTION_CHANNEL_BEGIN,
        slot);

    // ABY_ECO는 채널 틱마다가 아니라
 // 채널 전체 시전 시작 시 한 번만 확률 판정한다.
 //
 // 설치+채널 조합도 동일하게 허용한다.
    if (stats.echoOnCast &&
        (rand() % 100) < 50)
    {
        float echoMultiplier =
            m_fChannelTickDamageRatio *
            m_fChannelBaseMultiplier;

        if (m_bChannelEmbeddedEnhance)
            echoMultiplier *= kTransformEnhanceMultiplier;

        if (m_bChannelConsumeStoredEnhance)
            echoMultiplier *= m_fEnhanceMultiplier;

        const ElementType echoElement =
            stats.elementOverride.value_or(
                m_Skills[index]
                ->GetSkillData()
                .element);

        EnemyComponent* pEchoTarget = nullptr;

        const int echoSlot =
            SpawnEchoTriggerVFX(
                echoElement,
                targetPosition,
                &pEchoTarget);

        m_echoQueue.push_back({
            index,
            echoMultiplier * 0.5f,
            2.0f,
            echoSlot,
            pEchoTarget
            });
    }

    // 첫 채널 틱은 입력 직후 즉시 실행
    ExecuteCombinedChannelTick();
}

void SkillComponent::ExecuteCombinedChannelTick()
{
    if (!m_bIsChanneling ||
        m_ActiveSkillSlot == SkillSlot::Count)
    {
        return;
    }

    const SkillSlot slot =
        m_ActiveSkillSlot;

    const size_t index =
        static_cast<size_t>(slot);

    if (index >= m_Skills.size() ||
        !m_Skills[index])
    {
        return;
    }

    const ActivationType defaultType =
        m_Skills[index]
        ->GetSkillData()
        .activationType;

    const SkillStats stats =
        BuildSkillStats(
            slot,
            defaultType);

    float tickMultiplier =
        m_fChannelTickDamageRatio *
        m_fChannelBaseMultiplier;

    // 장착된 EMP가 현재 복합 채널에 직접 결합된 경우
    if (m_bChannelEmbeddedEnhance)
        tickMultiplier *= kTransformEnhanceMultiplier;

    bool storedEnhanceUsed = false;

    // 과거에 저장해 둔 EMP 버프는 첫 틱에서만 소비
    if (m_bChannelConsumeStoredEnhance)
    {
        m_bChannelConsumeStoredEnhance = false;

        if (m_bIsEnhanced)
        {
            storedEnhanceUsed = true;

            m_Skills[index]->OnEnhanceConsumed(
                m_pOwner,
                m_ChannelTargetPosition);

            tickMultiplier *=
                m_fEnhanceMultiplier;

            m_bIsEnhanced = false;
            m_fEnhanceTimer = 0.0f;

            NotifyActionNet(
                PLAYER_ACTION_ENHANCE_END,
                slot);
        }
    }

    // 즉시 실행된 첫 틱인지 확인
    const bool firstTick =
        m_fChannelTime <= 0.0001f;

    m_currentChargeRatio =
        m_fChannelChargeRatio;

    m_bCurrentIsChannelTick =
        true;

    m_bCurrentEnhanceUsed =
        m_bChannelEmbeddedEnhance ||
        storedEnhanceUsed;

    DirectX::XMFLOAT3 originPosition =
        m_ChannelOriginPosition;

    DirectX::XMFLOAT3 executionTarget =
        m_ChannelTargetPosition;

    bool movedCaster = false;
    DirectX::XMFLOAT3 savedPosition{};

    if (m_bChannelPlaceMode)
    {
        // 설치 위치에서 가장 가까운 적을 우선 타겟으로 사용
        Scene* pScene =
            Dx12App::GetInstance()
            ? Dx12App::GetInstance()->GetScene()
            : nullptr;

        if (pScene)
        {
            EnemyComponent* pNearest =
                pScene->FindNearestEnemy(
                    m_ChannelOriginPosition);

            if (pNearest &&
                pNearest->GetOwner() &&
                pNearest->GetOwner()->GetTransform())
            {
                executionTarget =
                    pNearest
                    ->GetOwner()
                    ->GetTransform()
                    ->GetPosition();
            }
        }

        // 기존 FirePlacedTrap과 같은 방식으로
        // 캐스터를 잠시 설치 위치로 옮겨 origin을 고정
        if (m_pOwner &&
            m_pOwner->GetTransform())
        {
            savedPosition =
                m_pOwner
                ->GetTransform()
                ->GetPosition();

            m_pOwner
                ->GetTransform()
                ->SetPosition(
                    m_ChannelOriginPosition);

            movedCaster = true;
        }
    }
    else if (m_pOwner &&
        m_pOwner->GetTransform())
    {
        originPosition =
            m_pOwner
            ->GetTransform()
            ->GetPosition();
    }

    const SkillCategory category =
        m_Skills[index]->GetCategory();

    if (category == SkillCategory::Projectile ||
        defaultType == ActivationType::Channel)
    {
        ExecuteOrSplit(
            index,
            executionTarget,
            tickMultiplier);
    }
    else
    {
        // ExecuteOrSplit을 거치지 않는 커스텀 채널 틱은
        // 룬 피해 배율을 여기서 직접 적용한다.
        const float customTickMultiplier =
            tickMultiplier *
            stats.damageMult;

        m_activationVFXMod =
            BuildActivationVFXMod(
                slot,
                m_fChannelChargeRatio,
                true,
                m_bChannelEmbeddedEnhance ||
                storedEnhanceUsed);

        m_Skills[index]->OnChannelTick(
            m_pOwner,
            executionTarget,
            customTickMultiplier);

        m_currentChargeRatio = 0.0f;
        m_bCurrentIsChannelTick = false;
        m_bCurrentEnhanceUsed = false;
    }

    if (movedCaster &&
        m_pOwner &&
        m_pOwner->GetTransform())
    {
        m_pOwner
            ->GetTransform()
            ->SetPosition(savedPosition);
    }

    // 네트워크는 실제 채널 틱이 실행된 시점에만 전송
    SendSkillNetFrom(
        slot,
        originPosition,
        executionTarget,
        m_fChannelChargeRatio,
        true,
        firstTick);
}

void SkillComponent::ClearCombinedChannelState()
{
    // 설치형 채널의 로컬 바닥 표식 제거
    if (m_ChannelPlaceDecalSlot >= 0)
    {
        Scene* pScene =
            Dx12App::GetInstance()
            ? Dx12App::GetInstance()->GetScene()
            : nullptr;

        if (pScene &&
            pScene->GetDecalManager())
        {
            pScene
                ->GetDecalManager()
                ->Stop(
                    m_ChannelPlaceDecalSlot);
        }

        m_ChannelPlaceDecalSlot = -1;
    }

    m_bIsChanneling = false;

    m_fChannelTime = 0.0f;
    m_fChannelTickAccum = 0.0f;

    m_fChannelChargeRatio = 0.0f;
    m_fChannelTickDamageRatio =
        kDefaultChannelDamageRatio;

    m_fChannelBaseMultiplier = 1.0f;

    m_bChannelPlaceMode = false;
    m_bChannelEmbeddedEnhance = false;
    m_bChannelConsumeStoredEnhance = false;

    m_ChannelOriginPosition =
    { 0.0f, 0.0f, 0.0f };

    m_ChannelTargetPosition =
    { 0.0f, 0.0f, 0.0f };

    m_bChannelTickFiredThisFrame = false;
}

void SkillComponent::ExecuteWithActivationType(
    SkillSlot slot,
    const DirectX::XMFLOAT3& targetPosition)
{
    const size_t index =
        static_cast<size_t>(slot);

    if (index >= m_Skills.size() ||
        !m_Skills[index])
    {
        return;
    }

    if (m_SkillStates[index] !=
        SkillState::Ready)
    {
        return;
    }

    // 다른 슬롯이 차징·채널 중이면 실행 금지
    if ((m_bIsChanneling &&
        slot != m_ActiveSkillSlot) ||
        (m_bIsCharging &&
            slot != m_ChargingSlot))
    {
        return;
    }

    // R 스킬 마법진 지연 발동
    if (slot == SkillSlot::R &&
        !m_bRSkillExecuting &&
        m_pOwner &&
        m_pOwner->GetTransform())
    {
        constexpr float kRevealDuration =
            0.6f;

        Scene* pScene =
            Dx12App::GetInstance()
            ? Dx12App::GetInstance()->GetScene()
            : nullptr;

        const DirectX::XMFLOAT3 feetPos =
            m_pOwner
            ->GetTransform()
            ->GetPosition();

        if (pScene &&
            pScene->GetDecalManager())
        {
            const ElementType element =
                m_Skills[index]
                ->GetSkillData()
                .element;

            const FluidElementColor color =
                FluidElementColors::Get(element);

            pScene
                ->GetDecalManager()
                ->Spawn(
                    DecalTexture::MagicCircle,
                    feetPos,
                    12.0f,
                    0.0f,
                    3.5f,
                    color.coreColor,
                    2.0f,
                    kRevealDuration);
        }

        NotifyActionNetAt(
            PLAYER_ACTION_R_MAGIC_CIRCLE,
            slot,
            feetPos,
            kRevealDuration);

        m_delayedCasts.push_back({
            slot,
            index,
            targetPosition,
            kRevealDuration
            });

        m_SkillStates[index] =
            SkillState::Casting;

        return;
    }

    const ActivationType defaultType =
        m_Skills[index]
        ->GetSkillData()
        .activationType;

    const SkillStats stats =
        BuildSkillStats(
            slot,
            defaultType);

    const RuneCombo combo =
        stats.ToRuneCombo();

    const bool enhanceOnly =
        combo.hasEnhance &&
        !combo.hasCharge &&
        !combo.hasChannel &&
        !combo.hasPlace &&
        !combo.hasInstant;

    // 1. 차징 단계
    if (combo.hasCharge)
    {
        m_bIsCharging = true;
        m_fChargeTime = 0.0f;
        m_ChargingSlot = slot;
        m_ChargeTargetPosition =
            targetPosition;

        m_SkillStates[index] =
            SkillState::Casting;

        NotifyActionNet(
            PLAYER_ACTION_CHARGE_BEGIN,
            slot);

        m_Skills[index]->OnChargeBegin(
            m_pOwner);

        if (index < m_chargeScaleSteps.size())
            m_chargeScaleSteps[index] = 0;

        SpawnChargeGatherVFX(0);

        return;
    }

    // 2. 채널 단계
    if (combo.hasChannel)
    {
        BeginCombinedChannel(
            slot,
            targetPosition,
            0.0f);

        return;
    }

    // 3. EMP 단독:
    // 현재 스킬을 공격으로 실행하지 않고 다음 공격 버프 저장
    if (enhanceOnly)
    {
        m_bIsEnhanced = true;
        m_fEnhanceTimer =
            m_fEnhanceDuration;

        m_SkillStates[index] =
            SkillState::Casting;

        m_ActiveSkillSlot =
            slot;

        NotifyActionNet(
            PLAYER_ACTION_ENHANCE_BEGIN,
            slot,
            m_fEnhanceDuration);

        m_currentChargeRatio = 0.0f;
        m_bCurrentIsChannelTick = false;
        m_bCurrentEnhanceUsed = false;

        if (m_pOwner &&
            m_pOwner->GetTransform())
        {
            const DirectX::XMFLOAT3 selfPosition =
                m_pOwner
                ->GetTransform()
                ->GetPosition();

            m_Skills[index]->OnEnhanceActivate(
                m_pOwner);

            m_Skills[index]->Execute(
                m_pOwner,
                selfPosition,
                0.0f);
        }

        OutputDebugString(
            L"[Skill] Enhanced! Next attack deals 1.8x damage for 5 seconds\n");

        return;
    }

    // 4. 즉시 또는 설치 공격
    float damageMultiplier = 1.0f;

    const bool embeddedEnhance =
        combo.hasEnhance;

    if (embeddedEnhance)
        damageMultiplier *= kTransformEnhanceMultiplier;

    const bool storedEnhanceUsed =
        m_bIsEnhanced;

    if (storedEnhanceUsed)
    {
        m_Skills[index]->OnEnhanceConsumed(
            m_pOwner,
            targetPosition);

        damageMultiplier *=
            m_fEnhanceMultiplier;

        m_bIsEnhanced = false;
        m_fEnhanceTimer = 0.0f;

        NotifyActionNet(
            PLAYER_ACTION_ENHANCE_END,
            slot);
    }

    m_currentChargeRatio = 0.0f;
    m_bCurrentIsChannelTick = false;
    m_bCurrentEnhanceUsed =
        embeddedEnhance ||
        storedEnhanceUsed;

    NetworkManager* pNetMgr =
        NetworkManager::GetInstance();

    const bool online =
        pNetMgr &&
        pNetMgr->IsConnected();

    if (combo.hasPlace)
    {
        if (!online)
        {
            SpawnPlaceTrap(
                index,
                targetPosition,
                damageMultiplier,
                combo);
        }
        else
        {
            // 온라인 설치는 서버가 생성.
            // 일반 스킬 표시 패킷 없이 공격 요청만 전송.
            SendSkillNet(
                slot,
                targetPosition,
                0.0f,
                false);
        }
    }
    else
    {
        ExecuteOrSplit(
            index,
            targetPosition,
            damageMultiplier);

        SendSkillNet(
            slot,
            targetPosition,
            0.0f,
            true);
    }

    m_SkillStates[index] =
        SkillState::Casting;

    m_ActiveSkillSlot =
        slot;
}

void SkillComponent::SendSkillNet(
    SkillSlot slot,
    const DirectX::XMFLOAT3& targetPosition,
    float chargeRatio,
    bool sendSkill)
{
    if (!m_pOwner ||
        !m_pOwner->GetTransform())
    {
        return;
    }

    const DirectX::XMFLOAT3 originPosition =
        m_pOwner
        ->GetTransform()
        ->GetPosition();

    SendSkillNetFrom(
        slot,
        originPosition,
        targetPosition,
        chargeRatio,
        sendSkill,
        true);
}

void SkillComponent::SendSkillNetFrom(
    SkillSlot slot,
    const DirectX::XMFLOAT3& originPosition,
    const DirectX::XMFLOAT3& targetPosition,
    float chargeRatio,
    bool sendSkill,
    bool countAsSkillUse)
{
    NetworkManager* pNetMgr =
        NetworkManager::GetInstance();

    if (!pNetMgr ||
        !pNetMgr->IsConnected() ||
        pNetMgr->IsCutscenePlaying())
    {
        return;
    }

    int skillType = 0;

    switch (slot)
    {
    case SkillSlot::Q:
        skillType = 1;
        break;

    case SkillSlot::E:
        skillType = 2;
        break;

    case SkillSlot::R:
        skillType = 3;
        break;

    case SkillSlot::RightClick:
        skillType = 4;
        break;

    default:
        return;
    }

    // 명시적인 origin → target 방향 계산
    DirectX::XMFLOAT3 direction = {
        targetPosition.x - originPosition.x,
        targetPosition.y - originPosition.y,
        targetPosition.z - originPosition.z
    };

    const float lengthSq =
        direction.x * direction.x +
        direction.y * direction.y +
        direction.z * direction.z;

    if (lengthSq > 0.000001f)
    {
        const float invLength =
            1.0f / std::sqrt(lengthSq);

        direction.x *= invLength;
        direction.y *= invLength;
        direction.z *= invLength;
    }
    else
    {
        // target과 origin이 같은 경우 플레이어 시선 사용
        direction = { 0.0f, 0.0f, 1.0f };

        if (m_pOwner &&
            m_pOwner->GetTransform())
        {
            const DirectX::XMVECTOR lookVector =
                m_pOwner
                ->GetTransform()
                ->GetLook();

            DirectX::XMStoreFloat3(
                &direction,
                lookVector);
        }
    }

    ElementType element =
        ElementType::None;

    if (m_pOwner)
    {
        if (PlayerComponent* player =
            m_pOwner
            ->GetComponent<PlayerComponent>())
        {
            element =
                player->GetElementType();
        }
    }

    const bool sendTarget =
        slot == SkillSlot::R ||
        (element == ElementType::Water &&
            (slot == SkillSlot::Q ||
                slot == SkillSlot::E));

    if (sendSkill)
    {
        if (sendTarget)
        {
            pNetMgr->SendSkill(
                skillType,
                originPosition.x,
                originPosition.y,
                originPosition.z,
                targetPosition.x,
                targetPosition.y,
                targetPosition.z,
                countAsSkillUse);
        }
        else
        {
            pNetMgr->SendSkill(
                skillType,
                originPosition.x,
                originPosition.y,
                originPosition.z,
                direction.x,
                direction.y,
                direction.z,
                countAsSkillUse);
        }
    }

    pNetMgr->SendPlayerAttack(
        skillType,
        originPosition.x,
        originPosition.y,
        originPosition.z,
        direction.x,
        direction.y,
        direction.z,
        targetPosition.x,
        targetPosition.y,
        targetPosition.z,
        ClampRuneRatio(chargeRatio),
        countAsSkillUse);
}