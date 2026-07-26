#pragma once

#include <DirectXMath.h>
#include <vector>
#include <string>
#include <unordered_set>
#include "SkillTypes.h"

class EnemyComponent;

using namespace DirectX;

// Projectile data structure
struct Projectile
{
    // Transform
    XMFLOAT3 position;
    XMFLOAT3 direction;
    float speed = 30.0f;

    // Combat
    float damage = 10.0f;
    float radius = 0.5f;          // Collision radius
    float explosionRadius = 0.0f; // AoE explosion radius (0 = single target)
    ElementType element = ElementType::None;

    // Ownership
    class GameObject* owner = nullptr;
    bool isPlayerProjectile = true;

    // Lifetime
    float maxDistance = 100.0f;
    float distanceTraveled = 0.0f;
    bool isActive = true;

    // Visual
    float scale = 1.0f;
    float chargeRatio = 0.0f;
    int fluidVFXId = -1;             // Associated fluid VFX effect (-1 = none)
    std::vector<int> extraVFXIds;    // Extra VFX slots for multi-element runes
    RuneCombo runeCombo;             // Rune combo for VFX customization
    std::vector<ElementType> elementSet; // Rune element set for multi-element VFX
    std::vector<std::string> subVFXDefIds;  // 서브 파티클 VFX def ID (스폰 시 사용)
    std::vector<int>         subVFXSlotIds; // 스폰된 서브 VFX 슬롯 IDs (Track/Stop용)
    bool wasHit = false;         // True if deactivated by collision (not range)

    // Rune-driven runtime flags (set at spawn from SkillStats)
    bool  isPiercing      = false;  // 관통: 충돌 후에도 계속 진행
    bool  isHoming        = false;  // 유도: 매 프레임 가장 가까운 적 방향으로 회전
    float lifestealRatio  = 0.f;    // 흡수: 피해량 * ratio 만큼 시전자 HP 회복
    float execDamageBonus = 0.f;    // 처형자: 대상 HP 30% 이하 시 추가 배율
    float cdResetChance   = 0.f;    // 무한: 적중 시 쿨다운 초기화 확률
    int   spawnOnHitCount = 0;      // 반향: 적중 시 주변 적으로 추가 투사체 생성
    SkillSlot skillSlot   = SkillSlot::Count; // 적중 시 onHit 훅 호출용 슬롯 정보

    // 설치 함정/메아리처럼 현재 룬이 아니라
// 예약 당시 ORB 스택을 사용해야 할 때 설정한다.
    int forcedOrbitalCount = -1;

    // 네트워크 유도 룬 타겟 정보
    bool useNetworkHomingTarget = false;
    uint64 networkHomingTargetMonsterId = 0;
    XMFLOAT3 networkHomingTargetPos = { 0.0f, 0.0f, 0.0f };

    // 관통 히트 추적 — 이미 맞은 적은 재타격하지 않음
    std::unordered_set<EnemyComponent*> piercedEnemies;

    // 궤도 운동 (궤도/중궤도 룬)
    bool      isOrbital          = false;
    XMFLOAT3  orbitalCenter      = {};     // 매 프레임 direction으로 전진하는 궤도 중심
    XMFLOAT3  orbitalForwardDir  = {};     // 중심 이동 방향
    float     orbitalRadius      = 2.0f;
    float     orbitalAngle       = 0.f;    // 현재 궤도 각도 (라디안)
    float     orbitalAngularSpeed = 5.0f;  // 회전 속도 (rad/s)

    // Helper to update position
    void Update(float deltaTime)
    {
        if (!isActive) return;

        float moveDistance = speed * deltaTime;
        distanceTraveled += moveDistance;
        if (distanceTraveled >= maxDistance) { isActive = false; return; }

        if (isOrbital)
        {
            // 궤도 중심을 forward 방향으로 전진
            orbitalCenter.x += orbitalForwardDir.x * moveDistance;
            orbitalCenter.y += orbitalForwardDir.y * moveDistance;
            orbitalCenter.z += orbitalForwardDir.z * moveDistance;

            // 각도 증가
            orbitalAngle += orbitalAngularSpeed * deltaTime;

            // 헬릭스 위치: center + right*cos + up*sin
            XMVECTOR fwd   = XMVector3Normalize(XMLoadFloat3(&orbitalForwardDir));
            XMVECTOR up    = XMVectorSet(0, 1, 0, 0);
            float    dot   = XMVectorGetX(XMVector3Dot(fwd, up));
            XMVECTOR right = (fabsf(dot) > 0.99f)
                ? XMVectorSet(1, 0, 0, 0)
                : XMVector3Normalize(XMVector3Cross(up, fwd));
            XMVECTOR centerV = XMLoadFloat3(&orbitalCenter);
            XMVECTOR pos = centerV
                + right * (orbitalRadius * cosf(orbitalAngle))
                + up    * (orbitalRadius * sinf(orbitalAngle));
            XMStoreFloat3(&position, pos);
        }
        else
        {
            position.x += direction.x * moveDistance;
            position.y += direction.y * moveDistance;
            position.z += direction.z * moveDistance;
        }
    }

    // Get bounding sphere for collision
    DirectX::BoundingSphere GetBoundingSphere() const
    {
        return DirectX::BoundingSphere(position, radius);
    }
};
