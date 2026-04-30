#pragma once

#include "Component.h"
#include "stdafx.h"

class FluidParticleSystem;
class VFXManager;
class Mesh;
class Shader;
class CRoom;

enum class GeyserState {
    Idle,       // 비활성 상태
    Warning,    // 경고 표시 중 (1.5초)
    Erupting    // 폭발 중 (0.3초)
};

class LavaGeyserComponent : public Component
{
public:
    LavaGeyserComponent(GameObject* pOwner);
    virtual ~LavaGeyserComponent();

    virtual void Update(float deltaTime) override;

    // 장판 활성화 (지정 위치에서 경고 시작)
    void Activate(const XMFLOAT3& position);

    // 상태 확인
    bool IsIdle() const { return m_eState == GeyserState::Idle; }
    GeyserState GetState() const { return m_eState; }

    // 설정
    void SetRadius(float radius) { m_fRadius = radius; }
    void SetDamage(float damage) { m_fDamage = damage; }
    void SetWarningDuration(float duration) { m_fWarningDuration = duration; }
    void SetEruptDuration(float duration) { m_fEruptDuration = duration; }

    float GetRadius() const { return m_fRadius; }

    // 인디케이터 설정 (Manager에서 호출)
    void SetIndicator(GameObject* pIndicator) { m_pIndicator = pIndicator; }

    // FluidParticleSystem 설정 (Manager에서 호출)
    void SetFluidSystem(FluidParticleSystem* pFluid) { m_pFluidSystem = pFluid; }

    // VFXManager 설정 (용암 기둥 효과용 — LightEmitterSystem 경유)
    void SetVFXManager(VFXManager* pVFX) { m_pVFXManager = pVFX; }

    // Room 설정 (데미지 처리용)
    void SetRoom(CRoom* pRoom) { m_pRoom = pRoom; }

private:
    void ShowIndicator();
    void HideIndicator();
    void Erupt();
    void DealDamage();

    GeyserState m_eState = GeyserState::Idle;
    float m_fTimer = 0.0f;

    // 설정값
    float m_fRadius = 10.0f;         // 더 큰 범위
    float m_fDamage = 20.0f;
    float m_fWarningDuration = 1.5f;
    float m_fEruptDuration = 1.2f;   // 더 오래 지속

    // 폭발 위치
    XMFLOAT3 m_vTargetPosition = { 0.0f, 0.0f, 0.0f };

    // 인디케이터 (RingMesh GameObject)
    GameObject* m_pIndicator = nullptr;

    // 유체 파티클 시스템 (용암 VFX) - 현재 미사용
    FluidParticleSystem* m_pFluidSystem = nullptr;

    // 통합 VFX 매니저 (용암 기둥 효과 - LightEmitterSystem 경유)
    VFXManager* m_pVFXManager = nullptr;
    bool m_bEruptSpawned = false;  // 분출 VFX가 이번 사이클에 이미 스폰되었는지

    // Room 참조 (데미지 처리용)
    CRoom* m_pRoom = nullptr;
};
