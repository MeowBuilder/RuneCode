#pragma once

#include "Projectile.h"
#include <vector>
#include <memory>

class Scene;
class GameObject;
class EnemyComponent;
class Mesh;
class CDescriptorHeap;
class VFXManager;

// Forward declare MATERIAL from GameObject.h
struct MATERIAL;

// Constant buffer for projectile rendering (must match ObjectConstants layout)
struct ProjectileConstants
{
    XMFLOAT4X4 m_xmf4x4World;
    UINT m_nMaterialIndex = 0;
    UINT m_bIsSkinned = 0;
    UINT m_bHasTexture = 0;
    UINT m_bIsLava = 0;
    UINT m_bHasEmissiveTexture = 0; UINT cbPad1 = 0; UINT cbPad2 = 0; UINT cbPad3 = 0;
    // Material embedded directly (same layout as MATERIAL)
    XMFLOAT4 m_cAmbient;
    XMFLOAT4 m_cDiffuse;
    XMFLOAT4 m_cSpecular;
    XMFLOAT4 m_cEmissive;
    XMFLOAT4X4 m_xmf4x4BoneTransforms[96];  // Not used, but needed for layout match
};

class ProjectileManager
{
public:
    ProjectileManager();
    ~ProjectileManager();

    // Initialize with scene reference and graphics resources
    void Init(Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
              CDescriptorHeap* pDescriptorHeap, UINT nStartDescriptorIndex);

    // Spawn a new projectile
    void SpawnProjectile(const Projectile& projectile);

    // Convenience method for common projectile creation
    void SpawnProjectile(
        const XMFLOAT3& startPos,
        const XMFLOAT3& targetPos,
        float damage,
        float speed,
        float radius,
        float explosionRadius,
        ElementType element,
        GameObject* owner,
        bool isPlayerProjectile = true,
        float scale = 1.0f,
        const RuneCombo& runeCombo = {},
        float chargeRatio = 0.0f,
        float maxDistance = 100.f,
        bool isPiercing = false,
        bool isHoming = false,
        float lifestealRatio = 0.f,
        float execDamageBonus = 0.f,
        float cdResetChance = 0.f,
        SkillSlot skillSlot = SkillSlot::Count,
        const std::vector<ElementType>& elementSet = {},
        const std::vector<std::string>& subVFXDefIds = {}
    );

    // Update all projectiles (movement + collision)
    void Update(float deltaTime);

    // Render all active projectiles
    void Render(ID3D12GraphicsCommandList* pCommandList);

    // Get active projectile count
    size_t GetActiveCount() const;

    // Clear all projectiles
    void Clear();

    // Spawn explosion particles at position — 네트워크 모드에서 서버 권위 피격 통지를 받아 클라가 직접 폭발 VFX 생성할 때도 사용
    void SpawnExplosionParticles(const XMFLOAT3& position, ElementType element);

private:
    // Check collisions for a single projectile
    void CheckProjectileCollisions(Projectile& projectile);

    // Apply damage to enemy
    void ApplyDamage(Projectile& projectile, EnemyComponent* pEnemy);

    // Apply AoE damage
    void ApplyAoEDamage(Projectile& projectile, const XMFLOAT3& impactPoint);

    // Remove inactive projectiles
    void CleanupInactiveProjectiles();

    // Get color based on element type
    XMFLOAT4 GetElementColor(ElementType element) const;

private:
    std::vector<Projectile> m_Projectiles;
    Scene* m_pScene = nullptr;
    VFXManager* m_pVFXManager = nullptr;  // 통합 VFX 파사드 (player/enemy 슬롯 풀 통합)

    // Rendering resources
    std::unique_ptr<Mesh> m_pProjectileMesh;
    ComPtr<ID3D12Resource> m_pd3dcbProjectiles;
    ProjectileConstants* m_pcbMappedProjectiles = nullptr;
    UINT m_nDescriptorStartIndex = 0;
    CDescriptorHeap* m_pDescriptorHeap = nullptr;

    // Pool settings
    static constexpr size_t MAX_PROJECTILES = 256;
    static constexpr size_t MAX_RENDERED_PROJECTILES = 64;  // Max projectiles to render at once
    static constexpr size_t CLEANUP_THRESHOLD = 32; // Cleanup when this many are inactive
};
