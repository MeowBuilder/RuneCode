#include "stdafx.h"
#include "LavaGeyserComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "FluidParticleSystem.h"
#include "FluidParticle.h"
#include "VFXManager.h"
#include "VFXTypes.h"
#include "Room.h"
#include "Scene.h"
#include "PlayerComponent.h"

LavaGeyserComponent::LavaGeyserComponent(GameObject* pOwner)
    : Component(pOwner)
{
}

LavaGeyserComponent::~LavaGeyserComponent()
{
}

void LavaGeyserComponent::Update(float deltaTime)
{
    switch (m_eState)
    {
    case GeyserState::Idle:
        // 대기 상태 - 아무것도 하지 않음
        break;

    case GeyserState::Warning:
        m_fTimer += deltaTime;

        // 점점 차오르는 효과: 스케일이 0에서 최대까지 증가
        {
            float progress = m_fTimer / m_fWarningDuration;
            if (progress > 1.0f) progress = 1.0f;

            // 0 → m_fRadius로 점점 커짐
            float currentScale = m_fRadius * progress;
            if (m_pIndicator)
            {
                TransformComponent* pT = m_pIndicator->GetTransform();
                if (pT)
                {
                    pT->SetScale(currentScale, 1.0f, currentScale);
                }
            }
        }

        if (m_fTimer >= m_fWarningDuration)
        {
            // 경고 종료 → 폭발
            m_fTimer = 0.0f;
            m_eState = GeyserState::Erupting;
            Erupt();
        }
        break;

    case GeyserState::Erupting:
        m_fTimer += deltaTime;

        if (m_fTimer >= m_fEruptDuration)
        {
            // 폭발 종료 → 대기 상태로 복귀
            m_fTimer = 0.0f;
            m_eState = GeyserState::Idle;
            HideIndicator();
            m_bEruptSpawned = false;
        }
        break;
    }
}

void LavaGeyserComponent::Activate(const XMFLOAT3& position)
{
    if (m_eState != GeyserState::Idle) return;

    m_vTargetPosition = position;
    m_fTimer = 0.0f;
    m_eState = GeyserState::Warning;

    ShowIndicator();

    OutputDebugString(L"[LavaGeyser] Activated at position\n");
}

void LavaGeyserComponent::ShowIndicator()
{
    if (!m_pIndicator) return;

    TransformComponent* pT = m_pIndicator->GetTransform();
    if (pT)
    {
        // 위치 설정 (지면 살짝 위)
        pT->SetPosition(m_vTargetPosition.x, m_vTargetPosition.y + 0.1f, m_vTargetPosition.z);

        // 처음에는 스케일 0으로 시작 (점점 차오름)
        pT->SetScale(0.0f, 1.0f, 0.0f);
    }
}

void LavaGeyserComponent::HideIndicator()
{
    if (!m_pIndicator) return;

    TransformComponent* pT = m_pIndicator->GetTransform();
    if (pT)
    {
        // 카메라 아래로 숨김
        pT->SetPosition(0.0f, -1000.0f, 0.0f);
    }
}

void LavaGeyserComponent::Erupt()
{
    OutputDebugString(L"[LavaGeyser] Erupting!\n");

    // 1. 데미지 처리
    DealDamage();

    // 2. 인디케이터 숨기기 (폭발 시 사라짐)
    HideIndicator();

    // 3. LightEmitterSystem(Cone, 위 방향)으로 용암 기둥 폭발!
    if (m_pVFXManager && !m_bEruptSpawned)
    {
        EffectLayer layer;
        layer.type          = EmitterType::Cone;
        layer.element       = ElementType::Fire;
        layer.particleCount = 600;                       // 큰 분출 (1회 Burst)
        layer.coreColor     = { 1.0f, 0.6f, 0.1f, 1.0f };
        layer.edgeColor     = { 0.8f, 0.15f, 0.0f, 0.0f };
        layer.sizeScale     = 1.4f;
        layer.speedMin      = 18.0f;
        layer.speedMax      = 55.0f;
        layer.lifetimeMin   = 0.18f;
        layer.lifetimeMax   = 0.55f;
        layer.cone.halfAngle     = 18.0f;                // 좁은 기둥
        layer.cone.gravityScale  = 0.4f;                 // 약한 중력 (오를 때 자연 감속)
        layer.cone.startSizeMult = 1.0f;
        layer.cone.endSizeMult   = 0.5f;
        layer.cone.fadeAlpha     = true;
        layer.cone.fadeSize      = true;

        // direction = 위쪽 (Y+)
        m_pVFXManager->SpawnLightLayer(m_vTargetPosition, XMFLOAT3(0, 1, 0),
                                       layer, /*isPlayer*/false);
        m_bEruptSpawned = true;
    }
}

void LavaGeyserComponent::DealDamage()
{
    if (!m_pRoom) return;

    Scene* pScene = m_pRoom->GetScene();
    if (!pScene) return;

    GameObject* pPlayer = pScene->GetPlayer();
    if (!pPlayer) return;

    // 플레이어 위치 확인
    TransformComponent* pPlayerTransform = pPlayer->GetTransform();
    if (!pPlayerTransform) return;

    XMFLOAT3 playerPos = pPlayerTransform->GetPosition();

    // 거리 계산 (XZ 평면)
    float dx = playerPos.x - m_vTargetPosition.x;
    float dz = playerPos.z - m_vTargetPosition.z;
    float distSq = dx * dx + dz * dz;
    float radiusSq = m_fRadius * m_fRadius;

    // 범위 내에 있으면 데미지
    if (distSq <= radiusSq)
    {
        PlayerComponent* pPlayerComp = pPlayer->GetComponent<PlayerComponent>();
        if (pPlayerComp)
        {
            pPlayerComp->TakeDamage(m_fDamage);
            OutputDebugString(L"[LavaGeyser] Player hit! Dealing damage.\n");
        }
    }
}
