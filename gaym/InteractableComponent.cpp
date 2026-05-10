#include "stdafx.h"
#include "InteractableComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"

InteractableComponent::InteractableComponent(GameObject* pOwner)
    : Component(pOwner)
{
}

void InteractableComponent::Update(float deltaTime)
{
    if (!m_bIsActive || !m_pOwner) return;

    // 정적 오브젝트(포탈 등) — 중력/bobbing 모두 스킵
    if (m_bSkipPhysics) return;

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
        float bobOffset = sinf(m_fBobTime) * m_fBobAmplitude;
        pTransform->SetPosition(pos.x, m_fBaseY + bobOffset, pos.z);
    }
}

bool InteractableComponent::IsPlayerInRange(GameObject* pPlayer) const
{
    if (!m_bIsActive || !m_pOwner || !pPlayer)
        return false;

    TransformComponent* pMyTransform = m_pOwner->GetTransform();
    TransformComponent* pPlayerTransform = pPlayer->GetTransform();

    if (!pMyTransform || !pPlayerTransform)
        return false;

    float distance = MathUtils::Distance3D(
        pMyTransform->GetPosition(),
        pPlayerTransform->GetPosition()
    );

    return distance <= m_fInteractionDistance;
}

void InteractableComponent::Interact()
{
    if (!m_bIsActive)
        return;

    OutputDebugString(L"[Interactable] Interaction triggered\n");

    if (m_OnInteract)
    {
        m_OnInteract(this);
    }
}

void InteractableComponent::Hide()
{
    m_bIsActive = false;

    if (m_pOwner)
    {
        TransformComponent* pTransform = m_pOwner->GetTransform();
        if (pTransform)
        {
            XMFLOAT3 pos = pTransform->GetPosition();
            pTransform->SetPosition(pos.x, -1000.0f, pos.z);
        }
    }

    OutputDebugString(L"[Interactable] Hidden\n");
}
