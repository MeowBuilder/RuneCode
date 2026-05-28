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
#include "RuneRegistry.h"
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

    // m_SkillRunes is value-initialized; EquippedRune default ctor sets runeId="" (empty)
}

SkillComponent::~SkillComponent()
{
}

void SkillComponent::Update(float deltaTime)
{
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
    }

    // Update charge timer
    if (m_bIsCharging)
    {
        m_fChargeTime += deltaTime;
        size_t chgIdx = static_cast<size_t>(m_ChargingSlot);
        if (chgIdx < m_Skills.size() && m_Skills[chgIdx])
            m_Skills[chgIdx]->OnChargeUpdate(m_pOwner, min(1.f, m_fChargeTime / m_fMaxChargeTime));
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
                    if (combo.hasPlace)
                        m_Skills[index]->Execute(m_pOwner, m_ChannelTargetPosition, -(tickMult * 1.5f));
                    else
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
            m_bIsChanneling = false;
            m_fChannelTime = 0.0f;
            m_fChannelTickAccum = 0.0f;

            size_t index = static_cast<size_t>(m_ActiveSkillSlot);
            if (index < m_Skills.size() && m_Skills[index])
            {
                m_Skills[index]->OnChannelComplete(m_pOwner, m_ChannelTargetPosition);
                m_Skills[index]->OnChannelEnd(m_pOwner);
                m_CooldownTimers[index] = GetEffectiveCooldown(index);
                bool keepCasting = !m_Skills[index]->IsFinished() && m_Skills[index]->HasPostChannelWork();
                if (keepCasting)
                {
                    m_SkillStates[index] = SkillState::Casting;
                }
                else
                {
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
            m_CooldownTimers[slotIndex] = GetEffectiveCooldown(slotIndex);
            m_SkillStates[slotIndex] = SkillState::Cooldown;
            m_Skills[slotIndex]->Reset();

            if (m_ActiveSkillSlot == thisSlot)
                m_ActiveSkillSlot = SkillSlot::Count;
        }
    }
}

void SkillComponent::ProcessSkillInput(InputSystem* pInputSystem, CCamera* pCamera)
{
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
                }

                wchar_t buffer[128];
                swprintf_s(buffer, 128, L"[Skill] Charge released! Charge: %.0f%%, Multiplier: %.1fx\n",
                    chargeRatio * 100.0f, damageMultiplier);
                OutputDebugString(buffer);

                // 활성화 VFX 컨텍스트 세팅 (Execute 직전)
                m_currentChargeRatio    = chargeRatio;
                m_bCurrentIsChannelTick = false;
                m_bCurrentEnhanceUsed  = (m_bIsEnhanced); // 이미 소모됨

                // Combo: Charge+Place → trap with charge damage
                if (combo.hasPlace)
                {
                    m_Skills[index]->Execute(m_pOwner, targetPos, -(damageMultiplier * 1.5f));
                }
                else
                {
                    ExecuteOrSplit(index, targetPos, damageMultiplier);
                }
                m_SkillStates[index] = SkillState::Casting;
                m_ActiveSkillSlot = m_ChargingSlot;

                // Start cooldown (cooldownMult 룬 적용)
                m_CooldownTimers[index] = GetEffectiveCooldown(index);
            }

            m_bIsCharging = false;
            m_fChargeTime = 0.0f;
            m_ChargingSlot = SkillSlot::Count;
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
            m_bIsChanneling = false;
            m_fChannelTime = 0.0f;
            m_fChannelTickAccum = 0.0f;

            size_t index = static_cast<size_t>(m_ActiveSkillSlot);
            if (index < m_Skills.size() && m_Skills[index])
            {
                m_Skills[index]->OnChannelEnd(m_pOwner);
                m_CooldownTimers[index] = GetEffectiveCooldown(index) * 0.5f;
                bool keepCasting = !m_Skills[index]->IsFinished() && m_Skills[index]->HasPostChannelWork();
                if (keepCasting)
                {
                    m_SkillStates[index] = SkillState::Casting;
                }
                else
                {
                    m_SkillStates[index] = SkillState::Cooldown;
                    m_Skills[index]->Reset();
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
        if (IsSkillKeyPressed(slot, pInputSystem))
        {
            ExecuteWithActivationType(slot, targetPos);
            break;  // Only use one skill per frame
        }
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

void SkillComponent::ResetCooldown(SkillSlot slot)
{
    size_t index = static_cast<size_t>(slot);
    if (index < m_CooldownTimers.size())
    {
        m_CooldownTimers[index] = 0.0f;
        if (index < m_SkillStates.size() && m_SkillStates[index] == SkillState::Cooldown)
            m_SkillStates[index] = SkillState::Ready;
    }
}

void SkillComponent::ReduceCooldown(SkillSlot slot, float seconds)
{
    size_t index = static_cast<size_t>(slot);
    if (index >= m_CooldownTimers.size()) return;
    m_CooldownTimers[index] = max(0.f, m_CooldownTimers[index] - seconds);
    if (m_CooldownTimers[index] == 0.f && index < m_SkillStates.size()
        && m_SkillStates[index] == SkillState::Cooldown)
        m_SkillStates[index] = SkillState::Ready;
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

SkillStats SkillComponent::BuildSkillStats(SkillSlot skill, ActivationType defaultType) const
{
    SkillStats stats;
    stats.activationType = defaultType;

    size_t skillIdx = static_cast<size_t>(skill);
    if (skillIdx >= static_cast<size_t>(SkillSlot::Count))
        return stats;

    const RuneRegistry& reg = RuneRegistry::Get();
    bool hasL03 = false;
    std::set<ElementType> uniqueElements;
    for (int i = 0; i < RUNES_PER_SKILL; ++i)
    {
        const EquippedRune& er = m_SkillRunes[skillIdx][i];
        if (er.IsEmpty()) continue;
        const RuneDef* def = reg.Find(er.runeId);
        if (!def) continue;
        def->ApplyTo(stats, er.stackCount);
        if (er.runeId == "L03") hasL03 = true;
        if (def->element != ElementType::None) uniqueElements.insert(def->element);
    }

    // 원소 증폭(L03): 2개 이상 다른 원소 장착 시 +30% 데미지
    if (hasL03 && uniqueElements.size() >= 2)
        stats.damageMult *= 1.30f;

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
    m_activationVFXMod = BuildActivationVFXMod(slot, m_currentChargeRatio,
                                                m_bCurrentIsChannelTick, m_bCurrentEnhanceUsed);
    // 다음 호출을 위해 초기화
    m_currentChargeRatio    = 0.f;
    m_bCurrentIsChannelTick = false;
    m_bCurrentEnhanceUsed  = false;

    // 룬 데미지 배율 적용 — Execute에 넘기는 mult에 포함시켜 모든 스킬에 일괄 적용
    mult *= stats.damageMult;

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
        // 쌍둥이별: 같은 방향으로 즉시 한 번 더 발사 (데미지 50%)
        if (stats.doublecast)
            m_Skills[index]->Execute(m_pOwner, target, mult * 0.5f);
        // 메아리: 즉시 50% 데미지로 재시전 (doublecast와 별개)
        if (stats.echoOnCast)
            m_Skills[index]->Execute(m_pOwner, target, mult * 0.5f);
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
    if (stats.doublecast)
    {
        m_Skills[index]->Execute(m_pOwner, t1, mult * 0.5f);
        m_Skills[index]->Execute(m_pOwner, t2, mult * 0.5f);
    }
    if (stats.echoOnCast)
    {
        m_Skills[index]->Execute(m_pOwner, t1, mult * 0.5f);
        m_Skills[index]->Execute(m_pOwner, t2, mult * 0.5f);
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
        m_Skills[index]->OnChargeBegin(m_pOwner);
        OutputDebugString(L"[Skill] Charging started... Hold to charge, release to fire\n");
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
            }
            m_currentChargeRatio    = 0.f;
            m_bCurrentIsChannelTick = true;
            m_bCurrentEnhanceUsed  = false;
            if (combo.hasPlace)
                m_Skills[index]->Execute(m_pOwner, targetPosition, -(tickMult * 1.5f));
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
            }
            m_currentChargeRatio    = 0.f;
            m_bCurrentIsChannelTick = false;
            m_bCurrentEnhanceUsed  = (damageMultiplier > 1.5f);
            if (combo.hasPlace)
                m_Skills[index]->Execute(m_pOwner, targetPosition, -(damageMultiplier * 1.5f));
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

        m_currentChargeRatio    = 0.f;
        m_bCurrentIsChannelTick = false;
        m_bCurrentEnhanceUsed  = false;

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
        }

        m_currentChargeRatio    = 0.f;
        m_bCurrentIsChannelTick = false;
        m_bCurrentEnhanceUsed  = (damageMultiplier > 1.5f);

        if (combo.hasPlace)
        {
            OutputDebugString(L"[Skill] Placing at target location!\n");
            m_Skills[index]->Execute(m_pOwner, targetPosition, -(damageMultiplier * 1.5f));
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
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr && pNetMgr->IsConnected())
    {
        // 스킬 슬롯을 Protocol::SkillType으로 변환
        int skillType = 0;
        switch (slot)
        {
        case SkillSlot::Q:          skillType = 1; break; // SKILL_TYPE_Q
        case SkillSlot::E:          skillType = 2; break; // SKILL_TYPE_E
        case SkillSlot::R:          skillType = 3; break; // SKILL_TYPE_R
        case SkillSlot::RightClick: skillType = 4; break; // SKILL_TYPE_MOUSE_RIGHT
        default:                    skillType = 0; break;
        }

        // 플레이어 위치와 방향 가져오기
        TransformComponent* pTransform = m_pOwner->GetTransform();
        if (pTransform)
        {
            const DirectX::XMFLOAT3& pos = pTransform->GetPosition();
            DirectX::XMVECTOR lookVec = pTransform->GetLook();
            DirectX::XMFLOAT3 lookDir;
            DirectX::XMStoreFloat3(&lookDir, lookVec);

            // R 스킬 + Water Q/E(WaterPuddle/Vortex) 는 target 위치가 visual 본질.
            // dirX/Y/Z 슬롯을 absolute target 좌표로 재활용 (수신 측이 element 보고 해석).
            ElementType elem = ElementType::None;
            if (auto* pc = m_pOwner->GetComponent<PlayerComponent>())
                elem = pc->GetElementType();
            bool sendTarget =
                (slot == SkillSlot::R) ||
                (elem == ElementType::Water && (slot == SkillSlot::Q || slot == SkillSlot::E));

            if (sendTarget)
            {
                pNetMgr->SendSkill(skillType, pos.x, pos.y, pos.z,
                    targetPosition.x, targetPosition.y, targetPosition.z);
            }
            else
            {
                pNetMgr->SendSkill(skillType, pos.x, pos.y, pos.z, lookDir.x, lookDir.y, lookDir.z);
            }

            // 서버 히트 판정 요청 — C_PLAYER_ATTACK.
            // 서버는 target 좌표 기준 반경 판정 (Q=6, E=8, R=10, 우클릭=5).
            // Q/E/우클릭 모두 마우스로 조준해서 쓰는 원거리 스킬이므로, 조준 월드 좌표(targetPosition)를
            // target 으로 쓰는 게 R 과 동일하게 가장 자연스러움. 플레이어가 조준한 지점 반경 내 몬스터가 맞는다.
            pNetMgr->SendPlayerAttack(skillType,
                pos.x, pos.y, pos.z,
                lookDir.x, lookDir.y, lookDir.z,
                targetPosition.x, targetPosition.y, targetPosition.z);
        }
    }
}
