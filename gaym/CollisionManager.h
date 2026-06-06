#pragma once
#include <vector>
#include <set>
#include <cstdint>

class ColliderComponent;

// Collision pair identifier (uses collider IDs to create unique pair)
struct CollisionPair
{
    uint32_t id1;
    uint32_t id2;

    CollisionPair(uint32_t a, uint32_t b)
    {
        // Always store smaller ID first for consistent comparison
        if (a < b) { id1 = a; id2 = b; }
        else       { id1 = b; id2 = a; }
    }

    bool operator<(const CollisionPair& other) const
    {
        if (id1 != other.id1) return id1 < other.id1;
        return id2 < other.id2;
    }

    bool operator==(const CollisionPair& other) const
    {
        return id1 == other.id1 && id2 == other.id2;
    }
};

class CollisionManager
{
public:
    CollisionManager();
    ~CollisionManager();

    // Register/unregister colliders (optional, for optimization)
    void RegisterCollider(ColliderComponent* pCollider);
    void UnregisterCollider(ColliderComponent* pCollider);

    // Main update function - checks collisions and fires callbacks
    void Update(const std::vector<ColliderComponent*>& globalColliders,
                const std::vector<ColliderComponent*>& roomColliders);

    // Clear all collision state (call when changing rooms)
    void ClearCollisionState();

    // Warmup — 로딩 화면 중 호출. 컬라이더 벡터 capacity 예약 → 인게임 push_back 시 realloc 0.
    void ReserveCapacity(size_t nColliders);

private:
    void CheckCollision(ColliderComponent* pA, ColliderComponent* pB);
    // Push a dynamic object (Player/Enemy) out of a Wall collider on the XZ plane
    static void ResolveWallPenetration(ColliderComponent* pDynamic, ColliderComponent* pWall);

    // Collision state tracking
    std::set<CollisionPair> m_previousFrameCollisions;
    std::set<CollisionPair> m_currentFrameCollisions;

    // Registered colliders (optional - can be used for optimization)
    std::vector<ColliderComponent*> m_registeredColliders;
};
