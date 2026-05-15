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

    // 3. LightEmitterSystem(Cone, 위 방향)으로 용암 폭발!
    //   duration 명시 필수 — 기본 -1(무한)이면 입자 영원히 분사됨.
    if (m_pVFXManager && !m_bEruptSpawned)
    {
        // [Layer 1] 두꺼운 폭발 콘 (메인) — 넓고 강렬
        EffectLayer burst;
        burst.type          = EmitterType::Cone;
        burst.element       = ElementType::Fire;
        burst.overrideColors = true;
        burst.particleCount = 900;
        burst.coreColor     = { 1.00f, 0.70f, 0.15f, 1.0f };  // 밝은 주황
        burst.edgeColor     = { 0.85f, 0.10f, 0.00f, 0.0f };  // 짙은 적
        burst.sizeScale     = 3.0f;
        burst.speedMin      = 28.0f;
        burst.speedMax      = 78.0f;                           // 위로 더 강하게
        burst.lifetimeMin   = 0.40f;
        burst.lifetimeMax   = 0.95f;
        burst.duration      = 0.5f;                            // 명시! 0.5s 후 emission 종료
        burst.cone.halfAngle     = 20.0f;                      // 38 → 20 좁게 (기둥 컨셉 유지)
        burst.cone.gravityScale  = 0.35f;                      // 약한 중력 — 높게 솟음
        burst.cone.startSizeMult = 1.7f;
        burst.cone.endSizeMult   = 0.3f;
        burst.cone.fadeAlpha     = true;
        burst.cone.fadeSize      = true;

        m_pVFXManager->SpawnLightLayer(m_vTargetPosition, XMFLOAT3(0, 1, 0),
                                       burst, /*isPlayer*/false);

        // [Layer 2] 지면 방사 링 (충격파) — 폭발 확산감
        EffectLayer ring;
        ring.type          = EmitterType::Ring;
        ring.element       = ElementType::Fire;
        ring.overrideColors = true;
        ring.particleCount = 220;
        ring.coreColor     = { 1.00f, 0.55f, 0.10f, 1.0f };
        ring.edgeColor     = { 0.60f, 0.05f, 0.00f, 0.0f };
        ring.sizeScale     = 2.4f;
        ring.speedMin      = 3.0f;
        ring.speedMax      = 6.0f;
        ring.lifetimeMin   = 0.35f;
        ring.lifetimeMax   = 0.7f;
        ring.duration      = 0.4f;
        ring.ring.radius         = m_fRadius * 0.5f;
        ring.ring.width          = 1.0f;
        ring.ring.expandSpeed    = m_fRadius * 1.2f;   // 빠르게 외곽으로
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 2.0f;
        ring.ring.normalSpeedMin = 1.0f;
        ring.ring.normalSpeedMax = 3.0f;

        m_pVFXManager->SpawnLightLayer(m_vTargetPosition, XMFLOAT3(0, 1, 0),
                                       ring, /*isPlayer*/false);

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
