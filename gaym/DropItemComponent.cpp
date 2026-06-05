#include "stdafx.h"
#include "DropItemComponent.h"
#include "RuneRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include <random>
#include <algorithm>

DropItemComponent::DropItemComponent(GameObject* pOwner)
    : Component(pOwner)
{
    GenerateRandomRunes();
}

DropItemComponent::~DropItemComponent()
{
}

void DropItemComponent::Update(float deltaTime)
{
    if (!m_bIsActive) return;
    if (!m_pOwner) return;

    TransformComponent* pTransform = m_pOwner->GetTransform();
    if (!pTransform) return;

    XMFLOAT3 pos = pTransform->GetPosition();

    // Apply gravity until landing
    if (!m_bOnGround)
    {
        m_fVelocityY -= GRAVITY * deltaTime;
        pos.y += m_fVelocityY * deltaTime;

        if (pos.y <= GROUND_Y)
        {
            pos.y = GROUND_Y;
            m_fVelocityY = 0.0f;
            m_bOnGround = true;
            m_fBaseY = pos.y;
        }
        pTransform->SetPosition(pos);
    }
    else
    {
        // Floating bob animation after landing
        m_fBobTime += deltaTime * m_fBobSpeed;
        m_fBobOffset = sinf(m_fBobTime) * m_fBobAmplitude;
        pTransform->SetPosition(pos.x, m_fBaseY + m_fBobOffset, pos.z);
    }
}

void DropItemComponent::GenerateRandomRunes()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());

    // Grade weights: Normal 50%, Rare 30%, Epic 15%, Unique 4%, Legendary 1%
    static const std::pair<RuneGrade, int> gradeWeights[] = {
        { RuneGrade::Normal,    50 },
        { RuneGrade::Rare,      30 },
        { RuneGrade::Epic,      15 },
        { RuneGrade::Unique,     4 },
        { RuneGrade::Legendary,  1 },
    };

    auto pickGrade = [&]() -> RuneGrade {
        std::uniform_int_distribution<int> d(1, 100);
        int roll = d(gen);
        int accum = 0;
        for (auto& [grade, w] : gradeWeights) {
            accum += w;
            if (roll <= accum) return grade;
        }
        return RuneGrade::Normal;
    };

    const RuneRegistry& reg = RuneRegistry::Get();

    // Pick 3 distinct rune IDs
    std::vector<std::string> picked;
    for (int attempt = 0; attempt < 30 && static_cast<int>(picked.size()) < 3; ++attempt)
    {
        RuneGrade grade = pickGrade();
        auto ids = reg.GetIdsByGrade(grade);
        if (ids.empty()) continue;

        std::uniform_int_distribution<int> d(0, static_cast<int>(ids.size()) - 1);
        std::string id = ids[d(gen)];
        if (std::find(picked.begin(), picked.end(), id) == picked.end())
            picked.push_back(id);
    }
    while (static_cast<int>(picked.size()) < 3) picked.push_back("X01"); // fallback

    for (int i = 0; i < 3; ++i)
        m_RuneOptions[i] = { picked[i], 1 };

    wchar_t buffer[256];
    swprintf_s(buffer, L"[Drop] Generated runes: [1] %hs, [2] %hs, [3] %hs\n",
        picked[0].c_str(), picked[1].c_str(), picked[2].c_str());
    OutputDebugString(buffer);
}

EquippedRune DropItemComponent::GetRuneOption(int index) const
{
    if (index < 0 || index >= 3) return {};
    return m_RuneOptions[index];
}

void DropItemComponent::SetRuneOptions(const std::array<EquippedRune, 3>& options)
{
    // 서버가 결정한 룬 선택지로 덮어쓴다.
    m_RuneOptions = options;

    wchar_t buffer[256];
    swprintf_s(buffer, L"[Drop] Server rune options applied: [1] %hs, [2] %hs, [3] %hs\n",
        m_RuneOptions[0].runeId.c_str(),
        m_RuneOptions[1].runeId.c_str(),
        m_RuneOptions[2].runeId.c_str());
    OutputDebugString(buffer);
}

RuneGrade DropItemComponent::GetHighestGrade() const
{
    RuneGrade highest = RuneGrade::Normal;
    const RuneRegistry& reg = RuneRegistry::Get();
    for (const auto& rune : m_RuneOptions)
    {
        if (rune.IsEmpty()) continue;
        const RuneDef* def = reg.Find(rune.runeId);
        if (def && def->grade > highest)
            highest = def->grade;
    }
    return highest;
}

XMFLOAT4 DropItemComponent::GetGradeColor(RuneGrade grade)
{
    switch (grade)
    {
    case RuneGrade::Normal:    return { 0.80f, 0.80f, 0.80f, 1.f }; // 회백
    case RuneGrade::Rare:      return { 0.20f, 0.45f, 1.00f, 1.f }; // 파랑
    case RuneGrade::Epic:      return { 0.60f, 0.05f, 1.00f, 1.f }; // 보라
    case RuneGrade::Unique:    return { 1.00f, 0.08f, 0.04f, 1.f }; // 빨강 
    case RuneGrade::Legendary: return { 1.00f, 0.85f, 0.00f, 1.f }; // 황금
    default:                   return { 1.00f, 1.00f, 1.00f, 1.f };
    }
}

void DropItemComponent::SetFloatingBaseY(float baseY)
{
    // 기본 DropItemComponent는 GROUND_Y까지 떨어진 뒤 bobbing한다.
    // 네트워크 보상 룬 오브젝트는 서버가 내려준 높이를 기준으로 떠 있어야 하므로,
    // 생성 직후 ground 상태로 고정하고 baseY를 서버 위치로 맞춘다.
    m_bOnGround = true;
    m_fVelocityY = 0.0f;
    m_fBaseY = baseY;
    m_fBobTime = 0.0f;
    m_fBobOffset = 0.0f;

    if (m_pOwner)
    {
        TransformComponent* pTransform = m_pOwner->GetTransform();
        if (pTransform)
        {
            XMFLOAT3 pos = pTransform->GetPosition();
            pTransform->SetPosition(pos.x, baseY, pos.z);
        }
    }
}
