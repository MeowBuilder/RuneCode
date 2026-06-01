#pragma once

#include "Component.h"
#include "SkillTypes.h"
#include "RuneDef.h"
#include <array>
#include <DirectXMath.h>
using namespace DirectX;

class DropItemComponent : public Component
{
public:
    DropItemComponent(GameObject* pOwner);
    virtual ~DropItemComponent();

    virtual void Update(float deltaTime) override;

    // Generate 3 random runes for selection
    void GenerateRandomRunes();

    // Get the rune options
    const std::array<EquippedRune, 3>& GetRuneOptions() const { return m_RuneOptions; }
    EquippedRune GetRuneOption(int index) const;
    
    // 서버가 내려준 룬 3개로 선택지를 덮어쓴다.
    void SetRuneOptions(const std::array<EquippedRune, 3>& options);

    // Check if drop is active (can be picked up)
    bool IsActive() const { return m_bIsActive; }
    void SetActive(bool active) { m_bIsActive = active; }

    // Grade/color helpers
    RuneGrade GetHighestGrade() const;
    static XMFLOAT4 GetGradeColor(RuneGrade grade);

    // Animation helpers
    float GetBobOffset() const { return m_fBobOffset; }

    // 네트워크 보상 룬 오브젝트용
    void SetFloatingBaseY(float baseY);

private:
    std::array<EquippedRune, 3> m_RuneOptions;
    bool m_bIsActive = true;

    // Gravity system
    float m_fVelocityY = 0.0f;
    bool m_bOnGround = false;
    float m_fBaseY = 0.0f;  // Base Y after landing (for bob animation)
    static constexpr float GRAVITY = 50.0f;
    static constexpr float GROUND_Y = 0.0f;

    // Floating animation
    float m_fBobTime = 0.0f;
    float m_fBobSpeed = 2.0f;
    float m_fBobAmplitude = 0.3f;
    float m_fBobOffset = 0.0f;
};
