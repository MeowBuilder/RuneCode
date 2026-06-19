#include "stdafx.h"
#include "CollisionManager.h"
#include "ColliderComponent.h"
#include "TransformComponent.h"
#include "GameObject.h"
#include "CollisionLayer.h"
#include <algorithm>

CollisionManager::CollisionManager()
{
}

CollisionManager::~CollisionManager()
{
}

void CollisionManager::ReserveCapacity(size_t nColliders)
{
    m_registeredColliders.reserve(nColliders);
}

void CollisionManager::RegisterCollider(ColliderComponent* pCollider)
{
    if (pCollider)
    {
        // Avoid duplicates
        auto it = std::find(m_registeredColliders.begin(), m_registeredColliders.end(), pCollider);
        if (it == m_registeredColliders.end())
        {
            m_registeredColliders.push_back(pCollider);
        }
    }
}

void CollisionManager::UnregisterCollider(ColliderComponent* pCollider)
{
    if (pCollider)
    {
        auto it = std::find(m_registeredColliders.begin(), m_registeredColliders.end(), pCollider);
        if (it != m_registeredColliders.end())
        {
            m_registeredColliders.erase(it);
        }
    }
}

void CollisionManager::Update(const std::vector<ColliderComponent*>& globalColliders,
                              const std::vector<ColliderComponent*>& roomColliders)
{
    // Swap current to previous, clear current
    m_previousFrameCollisions = std::move(m_currentFrameCollisions);
    m_currentFrameCollisions.clear();

    // Combine all colliders for checking
    std::vector<ColliderComponent*> allColliders;
    allColliders.reserve(globalColliders.size() + roomColliders.size());
    allColliders.insert(allColliders.end(), globalColliders.begin(), globalColliders.end());
    allColliders.insert(allColliders.end(), roomColliders.begin(), roomColliders.end());

    // Check all pairs (O(n^2) - can be optimized with spatial partitioning later)
    for (size_t i = 0; i < allColliders.size(); ++i)
    {
        for (size_t j = i + 1; j < allColliders.size(); ++j)
        {
            CheckCollision(allColliders[i], allColliders[j]);
        }
    }

    // Check for collision exits (pairs that were colliding last frame but not this frame)
    for (const auto& pair : m_previousFrameCollisions)
    {
        if (m_currentFrameCollisions.find(pair) == m_currentFrameCollisions.end())
        {
            // Find the colliders by ID
            ColliderComponent* pA = nullptr;
            ColliderComponent* pB = nullptr;

            for (auto* pCollider : allColliders)
            {
                if (pCollider->GetColliderID() == pair.id1) pA = pCollider;
                if (pCollider->GetColliderID() == pair.id2) pB = pCollider;
                if (pA && pB) break;
            }

            // Notify exit
            if (pA && pB)
            {
                pA->NotifyCollisionExit(pB);
                pB->NotifyCollisionExit(pA);
            }
        }
    }
}

void CollisionManager::CheckCollision(ColliderComponent* pA, ColliderComponent* pB)
{
    if (!pA || !pB)
        return;

    // Skip disabled colliders
    if (!pA->IsEnabled() || !pB->IsEnabled())
        return;

    // Check layer compatibility
    if (!pA->ShouldCollideWith(*pB))
        return;

    // Check physical intersection
    if (!pA->Intersects(*pB))
        return;

    // Collision detected!
    CollisionPair pair(pA->GetColliderID(), pB->GetColliderID());
    m_currentFrameCollisions.insert(pair);

    // Check if this is a new collision or ongoing
    bool wasColliding = (m_previousFrameCollisions.find(pair) != m_previousFrameCollisions.end());

    // ── Physical resolution: push dynamic objects out of walls (XZ only) ──────
    bool aIsWall = (pA->GetLayer() == CollisionLayer::Wall);
    bool bIsWall = (pB->GetLayer() == CollisionLayer::Wall);
    bool aIsDynamic = (pA->GetLayer() == CollisionLayer::Player ||
                       pA->GetLayer() == CollisionLayer::Enemy);
    bool bIsDynamic = (pB->GetLayer() == CollisionLayer::Player ||
                       pB->GetLayer() == CollisionLayer::Enemy);

    if (aIsWall && bIsDynamic) ResolveWallPenetration(pB, pA);
    else if (bIsWall && aIsDynamic) ResolveWallPenetration(pA, pB);

    if (wasColliding)
    {
        pA->NotifyCollisionStay(pB);
        pB->NotifyCollisionStay(pA);
    }
    else
    {
        pA->NotifyCollisionEnter(pB);
        pB->NotifyCollisionEnter(pA);
    }
}

void CollisionManager::ResolveWallPenetration(ColliderComponent* pDynamic, ColliderComponent* pWall)
{
    if (!pDynamic || !pWall) return;

    auto* pTransform = pDynamic->GetOwner()
        ? pDynamic->GetOwner()->GetComponent<TransformComponent>()
        : nullptr;
    if (!pTransform) return;

    const auto& dynBox  = pDynamic->GetBoundingBox();
    const auto& wallBox = pWall->GetBoundingBox();

    // Compute penetration depth on XZ axes only (Y is managed separately)
    float overlapX = (dynBox.Extents.x + wallBox.Extents.x) - fabsf(dynBox.Center.x - wallBox.Center.x);
    float overlapZ = (dynBox.Extents.z + wallBox.Extents.z) - fabsf(dynBox.Center.z - wallBox.Center.z);

    if (overlapX <= 0.0f || overlapZ <= 0.0f) return;

    // 침투량 캡 — 침투가 작을 땐 전부 해소, 깊을 땐 1.5/frame 까지 허용 (이전 0.5 → 깊은 침투에서
    // 여러 프레임 동안 못 빠져나와 흔들리던 문제 해결).
    const float MAX_PUSH = 1.5f;
    float pushX = min(overlapX, MAX_PUSH);
    float pushZ = min(overlapZ, MAX_PUSH);

    XMFLOAT3 pos = pTransform->GetPosition();

    // MTV(minimum translation vector) — 더 얕은 침투 축으로 push.
    //   코너에서 X/Z 침투가 비슷하면 매 프레임 다른 축 선택되며 떨림.
    //   비슷할 때(차이 < 0.05) 는 양 축 동시 push 로 코너 빠짐.
    const float EPS = 0.05f;
    bool similar = fabsf(overlapX - overlapZ) < EPS;

    if (similar)
    {
        // 코너 — 양 축 동시에 해소 (떨림 방지)
        float signX = (dynBox.Center.x >= wallBox.Center.x) ? 1.0f : -1.0f;
        float signZ = (dynBox.Center.z >= wallBox.Center.z) ? 1.0f : -1.0f;
        pos.x += signX * pushX * 0.707f;   // √2/2 — 대각선 분배
        pos.z += signZ * pushZ * 0.707f;
    }
    else if (overlapX < overlapZ)
    {
        float sign = (dynBox.Center.x >= wallBox.Center.x) ? 1.0f : -1.0f;
        pos.x += sign * pushX;
    }
    else
    {
        float sign = (dynBox.Center.z >= wallBox.Center.z) ? 1.0f : -1.0f;
        pos.z += sign * pushZ;
    }

    pTransform->SetPosition(pos);
}

void CollisionManager::ClearCollisionState()
{
    m_previousFrameCollisions.clear();
    m_currentFrameCollisions.clear();
}
