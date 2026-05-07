#include "stdafx.h"
#include "FireballBehavior.h"
#include "ProjectileManager.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "PlayerComponent.h"

FireballBehavior::FireballBehavior()
    : m_SkillData(FireSkillPresets::Fireball())
{
}

FireballBehavior::FireballBehavior(const SkillData& customData)
    : m_SkillData(customData)
{
}

void FireballBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bIsFinished = false;
    m_TargetPosition = targetPosition;

    // Get caster position for start point
    if (caster && caster->GetTransform())
    {
        m_StartPosition = caster->GetTransform()->GetPosition();
        m_StartPosition.y += 5.0f;
    }

    // Handle different activation modes based on damageMultiplier
    if (damageMultiplier < 0.0f)
    {
        // Placement mode (trap/turret) - absolute value is the actual multiplier
        ExecutePlacement(caster, targetPosition, fabsf(damageMultiplier));
    }
    else if (damageMultiplier == 0.0f)
    {
        // Enhance VFX only (self-buff visual)
        ExecuteEnhanceVFX(caster, m_StartPosition);
    }
    else
    {
        // Normal/Charged/Channel execution
        ExecuteInstant(caster, targetPosition, damageMultiplier);
    }

    m_bIsFinished = true;
}

void FireballBehavior::ExecuteInstant(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    if (!m_pProjectileManager)
    {
        OutputDebugString(L"[Skill] Warning: No ProjectileManager set!\n");
        return;
    }

    float finalDamage = m_SkillData.damage * damageMultiplier;

    // Determine projectile properties based on damage multiplier
    float speed = PROJECTILE_SPEED;
    float collisionRadius = 0.5f;
    float explosionRadius = m_SkillData.radius;
    float scale = 1.0f;

    if (damageMultiplier >= 2.5f)
    {
        // Fully charged - big slow fireball
        speed = CHARGED_PROJECTILE_SPEED;
        collisionRadius = 1.0f;
        explosionRadius = m_SkillData.radius * 1.5f;
        scale = 2.0f;
        OutputDebugString(L"[Skill] MEGA Fireball! (Fully Charged)\n");
    }
    else if (damageMultiplier >= 1.5f)
    {
        // Partially charged
        speed = PROJECTILE_SPEED * 0.8f;
        collisionRadius = 0.7f;
        explosionRadius = m_SkillData.radius * 1.2f;
        scale = 1.5f;
        OutputDebugString(L"[Skill] Charged Fireball!\n");
    }
    else if (damageMultiplier < 0.5f)
    {
        // Channel tick - small fast fireballs
        speed = PROJECTILE_SPEED * 1.5f;
        collisionRadius = 0.3f;
        explosionRadius = 0.0f;  // No AoE for channel ticks
        scale = 0.5f;
    }

    wchar_t buffer[256];
    swprintf_s(buffer, 256,
        L"[Skill] %hs: Damage=%.0f, Multiplier=%.1fx, Scale=%.1f\n",
        m_SkillData.name.c_str(), finalDamage, damageMultiplier, scale);
    OutputDebugString(buffer);

    // 캐스터 원소 동기화
    if (caster) {
        if (auto* pPC = caster->GetComponent<PlayerComponent>())
            m_SkillData.element = pPC->GetElementType();
    }

    // 룬 스탯 및 VFX 콤보 읽기
    RuneCombo combo;
    SkillStats stats;
    if (caster) {
        auto* pSkillComp = caster->GetComponent<SkillComponent>();
        if (pSkillComp && m_slot != SkillSlot::Count) {
            combo = pSkillComp->GetRuneCombo(m_slot);
            stats = pSkillComp->BuildSkillStats(m_slot, m_SkillData.activationType);
        }
    }

    // radiusMult / rangeMult 룬 적용
    explosionRadius *= stats.radiusMult;
    float maxDist = 100.f * stats.rangeMult;

    float chargeRatio = fmaxf(0.0f, fminf(1.0f, (damageMultiplier - 1.0f) / 2.0f));

    DirectX::XMFLOAT3 flatTarget = targetPosition;
    flatTarget.y = m_StartPosition.y;

    m_pProjectileManager->SpawnProjectile(
        m_StartPosition,
        flatTarget,
        finalDamage,
        speed,
        collisionRadius,
        explosionRadius,
        m_SkillData.element,
        caster,
        true,
        scale,
        combo,
        chargeRatio,
        maxDist,
        stats.piercing,
        stats.homing,
        stats.lifestealRatio,
        stats.execDamageBonus,
        stats.cdResetChance,
        m_slot,
        stats.elementSet,
        stats.subVFXIds
    );
}

void FireballBehavior::ExecutePlacement(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    if (!m_pProjectileManager)
    {
        OutputDebugString(L"[Skill] Warning: No ProjectileManager set!\n");
        return;
    }

    float finalDamage = m_SkillData.damage * damageMultiplier;
    // Scale visual size proportional to damage multiplier
    float scale = 1.5f + (damageMultiplier - 1.0f) * 0.2f;
    if (scale < 1.0f) scale = 1.0f;

    wchar_t buffer[256];
    swprintf_s(buffer, 256,
        L"[Skill] Placing Fire Trap at (%.1f, %.1f, %.1f) DMG=%.0f (%.1fx)\n",
        targetPosition.x, targetPosition.y, targetPosition.z, finalDamage, damageMultiplier);
    OutputDebugString(buffer);

    // Spawn a stationary "projectile" that acts as a trap
    DirectX::XMFLOAT3 trapPos = targetPosition;
    trapPos.y += 0.5f;  // Slightly above ground

    m_pProjectileManager->SpawnProjectile(
        trapPos,                    // Start at target
        trapPos,                    // Same target (stationary)
        finalDamage,                // Combo-based damage
        0.1f,                       // Very slow (almost stationary)
        1.5f,                       // Larger trigger radius
        m_SkillData.radius * 2.0f,  // Larger explosion
        m_SkillData.element,
        caster,
        true,
        scale                       // Damage-proportional visual scale
    );
}

void FireballBehavior::ExecuteEnhanceVFX(GameObject* caster, const DirectX::XMFLOAT3& selfPosition)
{
    if (!m_pProjectileManager)
    {
        return;
    }

    OutputDebugString(L"[Skill] Enhancement VFX at self\n");

    // Create a burst of small particles around the caster (visual only)
    // Spawn multiple small projectiles that go outward and up
    for (int i = 0; i < 8; ++i)
    {
        float angle = (float)i * (XM_2PI / 8.0f);
        DirectX::XMFLOAT3 targetDir;
        targetDir.x = selfPosition.x + cosf(angle) * 3.0f;
        targetDir.y = selfPosition.y + 3.0f;  // Go upward
        targetDir.z = selfPosition.z + sinf(angle) * 3.0f;

        m_pProjectileManager->SpawnProjectile(
            selfPosition,
            targetDir,
            0.0f,                       // No damage (VFX only)
            15.0f,                      // Fast
            0.1f,                       // Tiny collision
            0.0f,                       // No explosion
            m_SkillData.element,
            caster,
            true,
            0.3f                        // Small visual scale
        );
    }
}

void FireballBehavior::Update(float deltaTime)
{
    // Projectile movement and collision is handled by ProjectileManager
}

bool FireballBehavior::IsFinished() const
{
    return m_bIsFinished;
}

void FireballBehavior::Reset()
{
    m_bIsFinished = true;
    m_StartPosition = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    m_TargetPosition = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
}
