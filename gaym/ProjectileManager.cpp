#include "stdafx.h"
#include "ProjectileManager.h"
#include "VFXSpriteManager.h"
#include "DamageNumberManager.h"
#include "FluidSkillVFXManager.h"
#include "VFXManager.h"
#include "VFXTypes.h"
#include "EffectRegistry.h"
#include "Scene.h"
#include "Room.h"
#include "GameObject.h"
#include "EnemyComponent.h"
#include "PlayerComponent.h"
#include "SkillComponent.h"
#include "TransformComponent.h"
#include "Mesh.h"
#include "DescriptorHeap.h"
#include "NetworkManager.h"
#include <DirectXCollision.h>
#include <random>

extern void WriteNetworkLog(const std::string& msg);

ProjectileManager::ProjectileManager()
{
    m_Projectiles.reserve(MAX_PROJECTILES);
}

ProjectileManager::~ProjectileManager()
{
    if (m_pd3dcbProjectiles && m_pcbMappedProjectiles)
    {
        m_pd3dcbProjectiles->Unmap(0, nullptr);
        m_pcbMappedProjectiles = nullptr;
    }
}

void ProjectileManager::Init(Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                              CDescriptorHeap* pDescriptorHeap, UINT nStartDescriptorIndex)
{
    m_pScene = pScene;
    m_pDescriptorHeap = pDescriptorHeap;
    m_nDescriptorStartIndex = nStartDescriptorIndex;

    // Get VFX manager from scene
    m_pVFXManager     = pScene->GetVFXManager();

    // Create projectile mesh (small cube)
    m_pProjectileMesh = std::make_unique<CubeMesh>(pDevice, pCommandList, 0.5f, 0.5f, 0.5f);

    // Create constant buffer for renderable projectiles
    UINT nSingleCBSize = (sizeof(ProjectileConstants) + 255) & ~255;
    UINT nTotalCBSize = nSingleCBSize * MAX_RENDERED_PROJECTILES;
    m_pd3dcbProjectiles = CreateBufferResource(pDevice, pCommandList, nullptr, nTotalCBSize,
                                                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr);

    m_pd3dcbProjectiles->Map(0, nullptr, (void**)&m_pcbMappedProjectiles);

    // Create CBV for each projectile slot
    for (size_t i = 0; i < MAX_RENDERED_PROJECTILES; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
        cbvDesc.BufferLocation = m_pd3dcbProjectiles->GetGPUVirtualAddress() + (i * nSingleCBSize);
        cbvDesc.SizeInBytes = nSingleCBSize;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = pDescriptorHeap->GetCPUHandle(nStartDescriptorIndex + static_cast<UINT>(i));
        pDevice->CreateConstantBufferView(&cbvDesc, cpuHandle);
    }

    OutputDebugString(L"[ProjectileManager] Rendering resources initialized\n");
}

void ProjectileManager::SpawnProjectile(const Projectile& projectile)
{
    if (m_Projectiles.size() >= MAX_PROJECTILES)
    {
        // 1차: inactive 정리
        CleanupInactiveProjectiles();
        if (m_Projectiles.size() >= MAX_PROJECTILES)
        {
            // 2차: 들어올 게 player projectile 이면 oldest non-player 강제 evict 해서 자리 만들어 줌.
            //   (보스 VFX 가 풀 채워서 플레이어 스킬 시각이 reject 되는 케이스 방지)
            if (projectile.isPlayerProjectile)
            {
                bool bEvicted = false;
                for (auto eIt = m_Projectiles.begin(); eIt != m_Projectiles.end(); ++eIt)
                {
                    if (!eIt->isPlayerProjectile)
                    {
                        eIt->isActive = false;        // 안전하게 inactive 표시
                        m_Projectiles.erase(eIt);     // 자리 비우기
                        bEvicted = true;
                        break;
                    }
                }
                if (!bEvicted)
                {
                    OutputDebugString(L"[ProjectileManager] Pool full, no non-player to evict, drop player projectile\n");
                    return;
                }
            }
            else
            {
                OutputDebugString(L"[ProjectileManager] Max projectiles reached, drop non-player\n");
                return;
            }
        }
    }

    m_Projectiles.push_back(projectile);

    // Create fluid VFX trail for this projectile
    {
        auto& proj = m_Projectiles.back();

        if (proj.isPlayerProjectile && m_pVFXManager)
        {
            // 플레이어 투사체: 통합 VFXManager의 player 슬롯 풀 → SSF 파이프라인
            uint32_t runeFlags = ToRuneFlags(proj.runeCombo);
            SkillSlot vfxSlot  = (proj.skillSlot != SkillSlot::Count) ? proj.skillSlot : SkillSlot::RightClick;

            // 1차 원소: elementSet이 있으면 그 색상 사용, 없으면 기본 element
            ElementType primaryElem = proj.elementSet.empty() ? proj.element : proj.elementSet[0];

            // SkillSlot + 원소 → EffectRegistry effectId 매핑
            auto slotToEffectId = [](SkillSlot slot, ElementType elem) -> const char* {
                switch (slot)
                {
                case SkillSlot::Q:          return "Q_WaveSlash";
                case SkillSlot::E:
                    return (elem == ElementType::Earth) ? "E_RockThrow" : "E_FireBeam_Core";
                case SkillSlot::R:          return "R_Meteor";
                case SkillSlot::RightClick: return "RC_Fireball";
                default:                    return "RC_Fireball";
                }
            };
            const char* effectId = slotToEffectId(vfxSlot, primaryElem);

            // 1차 원소 VFX: EffectRegistry → EffectDef → 색상 오버라이드 후 SpawnEffectDef
            // primaryElem: elementSet이 있으면 룬 원소, 없으면 proj.element (캐릭터 기본 원소)
            EffectDef def = EffectRegistry::Get().GetEffect(effectId, runeFlags);
            {
                FluidElementColor ec = FluidElementColors::Get(primaryElem);
                def.element = primaryElem;
                for (auto& l : def.layers) {
                    l.element   = primaryElem;
                    l.coreColor = ec.coreColor;
                    l.edgeColor = ec.edgeColor;
                    if (proj.elementSet.size() > 1) {
                        bool isSPH = (l.type >= EmitterType::SPH_Attract &&
                                      l.type <= EmitterType::SPH_Beam);
                        if (isSPH)
                            l.sph.particleCount = max(100, (int)(l.sph.particleCount * 0.6f));
                        else
                            l.particleCount = max(100, (int)(l.particleCount * 0.6f));
                    }
                }
            }
            proj.fluidVFXId = m_pVFXManager->GetPlayerVFX()
                ? m_pVFXManager->GetPlayerVFX()->SpawnEffectDef(proj.position, proj.direction, def, true)
                : -1;
            if (proj.fluidVFXId >= 0)
                proj.fluidVFXId += VFXManager::PLAYER_ID_BASE;

            // 추가 원소 VFX (2차 이상)
            for (size_t ei = 1; ei < proj.elementSet.size(); ++ei)
            {
                EffectDef extraDef = EffectRegistry::Get().GetEffect(effectId, runeFlags);
                FluidElementColor ec = FluidElementColors::Get(proj.elementSet[ei]);
                extraDef.element = proj.elementSet[ei];
                for (auto& l : extraDef.layers) {
                    l.element   = proj.elementSet[ei];
                    l.coreColor = ec.coreColor;
                    l.edgeColor = ec.edgeColor;
                    bool isSPH = (l.type >= EmitterType::SPH_Attract &&
                                  l.type <= EmitterType::SPH_Beam);
                    if (isSPH)
                        l.sph.particleCount = max(100, (int)(l.sph.particleCount * 0.6f));
                    else
                        l.particleCount = max(100, (int)(l.particleCount * 0.6f));
                }
                int eid = m_pVFXManager->GetPlayerVFX()
                    ? m_pVFXManager->GetPlayerVFX()->SpawnEffectDef(proj.position, proj.direction, extraDef, true)
                    : -1;
                if (eid >= 0) proj.extraVFXIds.push_back(eid + VFXManager::PLAYER_ID_BASE);
            }

            // 서브 파티클 VFX 스폰 (EffectRegistry에 등록된 sub_* 이펙트 사용)
            for (const auto& subId : proj.subVFXDefIds)
            {
                if (!EffectRegistry::Get().HasEffect(subId)) continue;
                int sid = m_pVFXManager->Spawn(
                    subId, proj.position, proj.direction, runeFlags, /*isPlayer*/true);
                if (sid >= 0) proj.subVFXSlotIds.push_back(sid);
            }
        }
        else if (!proj.isPlayerProjectile && m_pVFXManager)
        {
            // 적 투사체: 레거시 경로 유지, 파티클 수 축소
            FluidSkillVFXDef vfxDef = FluidSkillVFXManager::GetVFXDef(
                proj.element, proj.runeCombo, proj.chargeRatio);
            vfxDef.particleCount = static_cast<int>(vfxDef.particleCount * 0.3f);
            if (vfxDef.particleCount < 100) vfxDef.particleCount = 100;

            // 적 투사체 기본 파티클 크기 (0.35→0.75) — 존재감 강화
            vfxDef.particleSize = 0.75f;

            // proj.scale을 VFX 크기에 반영 (기본 scale=1.5 기준 배율)
            // 개별 파티클 크기 캡 기준값: 레드 드래곤 기준 s (원하는 "좋은 느낌")
            constexpr float PARTICLE_SIZE_S_CAP = 2.0f;      // Red scale=3.0 기준
            constexpr float SMOOTHING_S_CAP     = 2.5f;      // SPH 안정성 — 너무 크면 진동
            if (proj.scale > 1.5f)
            {
                float s = proj.scale / 1.5f;
                float orbitMult = sqrtf(s);  // 궤도는 sqrt로 덜 확대 → 꽉 뭉친 느낌
                for (auto& cpd : vfxDef.cpDescs)
                {
                    bool isNucleus = (cpd.orbitRadius < 0.001f);
                    if (isNucleus)
                    {
                        // 핵: 큰 반경 + 강한 인력 → 중심으로 빨아들임
                        cpd.sphereRadius       *= s * 2.5f;
                        cpd.attractionStrength *= s * 1.4f;
                    }
                    else
                    {
                        // 위성: 궤도는 sqrt(s)로 덜 확대, 인력은 원값 유지 → 타이트
                        cpd.orbitRadius        *= orbitMult;
                        cpd.sphereRadius       *= s;
                    }
                }
                vfxDef.spawnRadius    *= orbitMult;
                // smoothingRadius는 캡으로 고정 → 큰 s에서도 SPH 안정적
                vfxDef.smoothingRadius = 1.0f * (std::min)(s, SMOOTHING_S_CAP);
                vfxDef.restDensity     = 11.0f;             // 꽉 뭉치게
                // 개별 파티클 크기 캡 → 레드 기준 유지, 블루도 동일 크기
                vfxDef.particleSize    = 0.75f * (std::min)(s, PARTICLE_SIZE_S_CAP);
                // 파티클 수: s에 따른 성장 완만히 (sqrt 기반) — 너무 많으면 움직임 산만
                vfxDef.particleCount   = static_cast<int>(vfxDef.particleCount * (std::min)(sqrtf(s) * 1.5f, 6.0f));
            }
            else
            {
                // 기본 scale 투사체도 더 뭉치게
                vfxDef.restDensity = 11.0f;
            }

            proj.fluidVFXId = m_pVFXManager->SpawnEffect(
                proj.position, proj.direction, vfxDef);
        }
    }

    // 궤도 동반 투사체 (궤도/중궤도 룬): SkillStats에서 orbitalCount 읽어 헬릭스 투사체 생성
    if (!projectile.isOrbital && projectile.isPlayerProjectile
        && projectile.skillSlot != SkillSlot::Count && projectile.owner)
    {
        SkillComponent* pSkillComp = projectile.owner->GetComponent<SkillComponent>();
        if (pSkillComp)
        {
            int orbCount = pSkillComp->BuildSkillStats(projectile.skillSlot).orbitalCount;
            if (orbCount > 0)
            {
                // 재할당으로 참조 무효화 방지: 필요한 데이터를 값으로 복사
                Projectile base = projectile;
                constexpr float PI2 = 6.28318530f;
                float angleStep = PI2 / orbCount;
                for (int oi = 0; oi < orbCount && m_Projectiles.size() < MAX_PROJECTILES; ++oi)
                {
                    Projectile orb   = base;
                    orb.isOrbital    = true;
                    orb.orbitalCenter     = base.position;
                    orb.orbitalForwardDir = base.direction;
                    orb.orbitalAngle      = angleStep * oi;  // 균등 위상 배분
                    orb.orbitalRadius     = 2.0f;
                    orb.orbitalAngularSpeed = 5.0f;
                    orb.damage       = base.damage * 0.35f;
                    orb.scale        = 0.55f;
                    orb.fluidVFXId   = -1;
                    orb.extraVFXIds.clear();
                    orb.spawnOnHitCount = 0;  // 궤도 투사체의 반향 방지
                    m_Projectiles.push_back(orb);
                }
            }
        }
    }

    wchar_t buffer[256];
    swprintf_s(buffer, 256, L"[ProjectileManager] Spawned projectile at (%.1f, %.1f, %.1f) -> dir (%.2f, %.2f, %.2f)\n",
        projectile.position.x, projectile.position.y, projectile.position.z,
        projectile.direction.x, projectile.direction.y, projectile.direction.z);
    OutputDebugString(buffer);
}

void ProjectileManager::SpawnProjectile(
    const XMFLOAT3& startPos,
    const XMFLOAT3& targetPos,
    float damage,
    float speed,
    float radius,
    float explosionRadius,
    ElementType element,
    GameObject* owner,
    bool isPlayerProjectile,
    float scale,
    const RuneCombo& runeCombo,
    float chargeRatio,
    float maxDistance,
    bool isPiercing,
    bool isHoming,
    float lifestealRatio,
    float execDamageBonus,
    float cdResetChance,
    SkillSlot skillSlot,
    const std::vector<ElementType>& elementSet,
    const std::vector<std::string>& subVFXDefIds)
{
    Projectile proj;
    proj.position         = startPos;
    proj.damage           = damage;
    proj.speed            = speed;
    proj.radius           = radius;
    proj.explosionRadius  = explosionRadius;
    proj.element          = element;
    proj.owner            = owner;
    proj.isPlayerProjectile = isPlayerProjectile;
    proj.maxDistance      = maxDistance;
    proj.distanceTraveled = 0.0f;
    proj.isActive         = true;
    proj.scale            = scale;
    proj.runeCombo        = runeCombo;
    proj.chargeRatio      = chargeRatio;
    proj.isPiercing       = isPiercing;
    proj.isHoming         = isHoming;
    proj.lifestealRatio   = lifestealRatio;
    proj.execDamageBonus  = execDamageBonus;
    proj.cdResetChance    = cdResetChance;
    proj.skillSlot        = skillSlot;
    proj.elementSet       = elementSet;
    proj.subVFXDefIds     = subVFXDefIds;

    // Calculate direction
    XMVECTOR start = XMLoadFloat3(&startPos);
    XMVECTOR target = XMLoadFloat3(&targetPos);
    XMVECTOR dir = XMVector3Normalize(target - start);
    XMStoreFloat3(&proj.direction, dir);

    SpawnProjectile(proj);
}

void ProjectileManager::Update(float deltaTime)
{
    size_t inactiveCount = 0;

    for (auto& projectile : m_Projectiles)
    {
        if (!projectile.isActive)
        {
            inactiveCount++;
            continue;
        }

        // 유도: 가장 가까운 적 방향으로 방향 벡터를 서서히 회전
        if (projectile.isHoming && projectile.isPlayerProjectile && m_pScene)
        {
            CRoom* pRoom = m_pScene->GetCurrentRoom();
            if (pRoom)
            {
                float bestDist = FLT_MAX;
                XMFLOAT3 bestPos = projectile.position;
                for (const auto& obj : pRoom->GetGameObjects())
                {
                    if (!obj) continue;
                    EnemyComponent* pEnemy = obj->GetComponent<EnemyComponent>();
                    if (!pEnemy || pEnemy->IsDead()) continue;
                    XMFLOAT3 ePos = obj->GetTransform()->GetPosition();
                    XMVECTOR diff = XMLoadFloat3(&ePos) - XMLoadFloat3(&projectile.position);
                    float d = XMVectorGetX(XMVector3Length(diff));
                    if (d < bestDist) { bestDist = d; bestPos = ePos; }
                }
                if (bestDist < FLT_MAX)
                {
                    XMVECTOR cur = XMLoadFloat3(&projectile.direction);
                    XMVECTOR toTarget = XMVector3Normalize(
                        XMLoadFloat3(&bestPos) - XMLoadFloat3(&projectile.position));
                    constexpr float TURN_SPEED = 3.5f; // rad/s
                    XMVECTOR newDir = XMVector3Normalize(
                        cur + toTarget * (TURN_SPEED * deltaTime));
                    XMStoreFloat3(&projectile.direction, newDir);
                }
            }
        }

        // Update position
        projectile.Update(deltaTime);

        // 통합 VFXManager: ID로 player/enemy 슬롯 자동 라우팅
        VFXManager* pVFX = m_pVFXManager;

        // Update fluid VFX position
        if (pVFX && projectile.fluidVFXId >= 0)
            pVFX->Track(projectile.fluidVFXId, projectile.position, projectile.direction);
        if (pVFX)
        {
            for (int eid : projectile.extraVFXIds)
                if (eid >= 0) pVFX->Track(eid, projectile.position, projectile.direction);
            for (int sid : projectile.subVFXSlotIds)
                if (sid >= 0) pVFX->Track(sid, projectile.position, projectile.direction);
        }

        // Check collisions
        if (projectile.isActive)
        {
            CheckProjectileCollisions(projectile);
        }

        // If projectile became inactive (hit something or expired), handle fluid VFX and spawn explosion
        if (!projectile.isActive)
        {
            if (pVFX && projectile.fluidVFXId >= 0)
            {
                if (projectile.wasHit)
                {
                    if (projectile.isPlayerProjectile)
                        pVFX->Explode(projectile.fluidVFXId, projectile.position);
                    else
                        pVFX->Impact(projectile.fluidVFXId, projectile.position);
                }
                else
                    pVFX->Stop(projectile.fluidVFXId);
            }
            // Extra element VFX + 서브 파티클 VFX
            if (pVFX)
            {
                for (int eid : projectile.extraVFXIds)
                {
                    if (eid < 0) continue;
                    if (projectile.wasHit && projectile.isPlayerProjectile)
                        pVFX->Explode(eid, projectile.position);
                    else
                        pVFX->Stop(eid);
                }
                for (int sid : projectile.subVFXSlotIds)
                {
                    if (sid < 0) continue;
                    if (projectile.wasHit && projectile.isPlayerProjectile)
                        pVFX->Explode(sid, projectile.position);
                    else
                        pVFX->Stop(sid);
                }
            }
            // Spawn explosion particles for each element
            if (projectile.elementSet.empty())
                SpawnExplosionParticles(projectile.position, projectile.element);
            else
                for (ElementType e : projectile.elementSet)
                    SpawnExplosionParticles(projectile.position, e);
            inactiveCount++;
        }
    }

    // Cleanup if too many inactive
    if (inactiveCount >= CLEANUP_THRESHOLD)
    {
        CleanupInactiveProjectiles();
    }
}

size_t ProjectileManager::GetActiveCount() const
{
    size_t count = 0;
    for (const auto& proj : m_Projectiles)
    {
        if (proj.isActive) count++;
    }
    return count;
}

void ProjectileManager::Clear()
{
    // 투사체 벡터만 지우면, 투사체가 만들어둔 유체 VFX 슬롯은 계속 살아남을 수 있다.
    // 방 전환 시에는 projectile trail / extra VFX / sub VFX를 먼저 Stop한 뒤 벡터를 비운다.
    if (m_pVFXManager)
    {
        for (auto& projectile : m_Projectiles)
        {
            if (projectile.fluidVFXId >= 0)
            {
                m_pVFXManager->Stop(projectile.fluidVFXId);
                projectile.fluidVFXId = -1;
            }

            for (int eid : projectile.extraVFXIds)
            {
                if (eid >= 0)
                    m_pVFXManager->Stop(eid);
            }
            projectile.extraVFXIds.clear();

            for (int sid : projectile.subVFXSlotIds)
            {
                if (sid >= 0)
                    m_pVFXManager->Stop(sid);
            }
            projectile.subVFXSlotIds.clear();
        }
    }

    m_Projectiles.clear();

    WriteNetworkLog("[ProjectileManager] Clear projectiles and VFX");
}

void ProjectileManager::CheckProjectileCollisions(Projectile& projectile)
{
    if (!m_pScene) return;

    BoundingSphere projSphere = projectile.GetBoundingSphere();

    if (projectile.isPlayerProjectile)
    {
        // Player projectile: check against enemies in the room
        CRoom* pRoom = m_pScene->GetCurrentRoom();
        if (!pRoom) return;

        const auto& gameObjects = pRoom->GetGameObjects();
        for (const auto& obj : gameObjects)
        {
            if (!obj) continue;

            EnemyComponent* pEnemy = obj->GetComponent<EnemyComponent>();
            if (!pEnemy || pEnemy->IsDead()) continue;

            TransformComponent* pTransform = obj->GetTransform();
            if (!pTransform) continue;

            XMFLOAT3 enemyPos = pTransform->GetPosition();
            XMFLOAT3 enemyScale = pTransform->GetScale();

            // Scale collision sphere based on enemy size (1.2 → 1.5 — 뚱뚱한 메시도 커버)
            float maxScale = max(enemyScale.x, max(enemyScale.y, enemyScale.z));
            float radius = max(1.5f, maxScale * 1.5f);

            BoundingSphere enemySphere(enemyPos, radius);
            enemySphere.Center.y += radius * 0.7f;

            if (projSphere.Intersects(enemySphere))
            {
                // 관통: 이미 타격한 적은 건너뜀 (매 프레임 재히트 방지)
                if (projectile.isPiercing && projectile.piercedEnemies.count(pEnemy))
                    continue;

                if (projectile.explosionRadius > 0.0f)
                    ApplyAoEDamage(projectile, projectile.position);
                else
                    ApplyDamage(projectile, pEnemy);

                projectile.wasHit = true;
                if (!projectile.isPiercing)
                {
                    projectile.isActive = false;
                    return;
                }
                projectile.piercedEnemies.insert(pEnemy);
            }
        }

        // 네트워크 모드: 서버 몬스터 (EnemyComponent 없음, 로컬 Room 밖에 존재) 도 충돌 체크.
        // 데미지는 서버 권위 — 여기선 폭발 VFX 트리거 + projectile 비활성화만.
        // wasHit=true 면 Update 루프가 pVFX->Explode 를 불러 큰 폭발 VFX 가 자연 생성됨.
        // 관통은 collision 검사 자체 스킵 — visual 도 통과 (Wind RC).
        NetworkManager* pNetMgr = NetworkManager::GetInstance();
        if (pNetMgr && pNetMgr->IsConnected() && projectile.isActive && !projectile.isPiercing)
        {
            for (const auto& kv : pNetMgr->GetServerMonsters())
            {
                GameObject* netMonster = kv.second;
                if (!netMonster) continue;
                TransformComponent* pT = netMonster->GetTransform();
                if (!pT) continue;

                XMFLOAT3 mPos = pT->GetPosition();
                XMFLOAT3 mScale = pT->GetScale();
                float maxScale = max(mScale.x, max(mScale.y, mScale.z));
                float radius = max(1.5f, maxScale * 1.5f);

                BoundingSphere mSphere(mPos, radius);
                mSphere.Center.y += radius * 0.7f;

                if (projSphere.Intersects(mSphere))
                {
                    projectile.wasHit = true;
                    projectile.isActive = false;
                    return;  // 비관통 — 첫 서버 몬스터 충돌 시 폭발
                }
            }
        }
    }
    else
    {
        // Enemy projectile: check against player
        GameObject* pPlayer = m_pScene->GetPlayer();
        if (!pPlayer) return;

        TransformComponent* pPlayerTransform = pPlayer->GetTransform();
        if (!pPlayerTransform) return;

        XMFLOAT3 playerPos = pPlayerTransform->GetPosition();

        BoundingSphere playerSphere(playerPos, 1.5f);
        playerSphere.Center.y += 1.0f;

        if (projSphere.Intersects(playerSphere))
        {
            projectile.wasHit = true;
            projectile.isActive = false;

            // Deal damage to player
            PlayerComponent* pPlayerComp = pPlayer->GetComponent<PlayerComponent>();
            if (pPlayerComp)
            {
                pPlayerComp->TakeDamage(projectile.damage);

                wchar_t buffer[128];
                swprintf_s(buffer, 128, L"[ProjectileManager] Enemy projectile hit player! Dealt %.0f damage (HP: %.1f/%.1f)\n",
                    projectile.damage, pPlayerComp->GetCurrentHP(), pPlayerComp->GetMaxHP());
                OutputDebugString(buffer);
            }
            return;
        }
    }
}

void ProjectileManager::ApplyDamage(Projectile& projectile, EnemyComponent* pEnemy)
{
    if (!pEnemy) return;

    float dmg = projectile.damage;

    // 처형자: HP 30% 이하 시 추가 피해 (데미지 보너스 조건)
    if (projectile.execDamageBonus > 0.f && pEnemy->GetHpRatio() < 0.3f)
        dmg *= (1.f + projectile.execDamageBonus);

    pEnemy->TakeDamage(dmg, false, projectile.execDamageBonus > 0.f);

    // 처형자: 룬이 장착된 상태에서 적이 죽으면 skull 표시 (HP 조건 무관)

    // 흡수: 피해량 * ratio 만큼 시전자 HP 회복
    if (projectile.lifestealRatio > 0.f && projectile.owner)
    {
        PlayerComponent* pPlayer = projectile.owner->GetComponent<PlayerComponent>();
        if (pPlayer) pPlayer->Heal(dmg * projectile.lifestealRatio);
    }

    // 무한: 확률로 시전자 스킬 쿨다운 즉시 초기화
    if (projectile.cdResetChance > 0.f && projectile.skillSlot != SkillSlot::Count && projectile.owner)
    {
        static std::mt19937 rng{ std::random_device{}() };
        static std::uniform_real_distribution<float> dist(0.f, 1.f);
        if (dist(rng) < projectile.cdResetChance)
        {
            SkillComponent* pSkill = projectile.owner->GetComponent<SkillComponent>();
            if (pSkill) pSkill->ResetCooldown(projectile.skillSlot);
        }
    }

    // onHit 훅 + 반향(spawnOnHitCount) 처리
    if (projectile.skillSlot != SkillSlot::Count && projectile.owner)
    {
        SkillComponent* pSkillComp = projectile.owner->GetComponent<SkillComponent>();
        if (pSkillComp)
        {
            ActivationType defaultType = ActivationType::Instant;
            SkillStats stats = pSkillComp->BuildSkillStats(projectile.skillSlot, defaultType);

            // onHit 훅
            if (!stats.onHitHooks.empty())
            {
                SkillContext ctx;
                ctx.caster             = projectile.owner;
                ctx.element            = projectile.element;
                ctx.baseDamage         = projectile.damage;
                ctx.damageDealt        = dmg;
                ctx.skillSlot          = projectile.skillSlot;
                ctx.scene              = m_pScene;
                ctx.hitEnemy           = pEnemy;
                ctx.statusChanceMult   = stats.statusChanceMult;
                ctx.statusDurationMult = stats.statusDurationMult;
                if (pEnemy && pEnemy->GetOwner())
                {
                    TransformComponent* pT = pEnemy->GetOwner()->GetTransform();
                    if (pT) ctx.hitEnemyPos = pT->GetPosition();
                }
                for (auto& hook : stats.onHitHooks) hook(ctx);
            }

            // 반향: 적중 위치에서 근처의 다른 적들을 향해 N개 추가 투사체 생성
            if (stats.spawnOnHitCount > 0 && m_pScene)
            {
                CRoom* pRoom = m_pScene->GetCurrentRoom();
                if (pRoom)
                {
                    XMFLOAT3 hitPos = {};
                    if (pEnemy && pEnemy->GetOwner())
                        if (auto* pT = pEnemy->GetOwner()->GetTransform())
                            hitPos = pT->GetPosition();

                    // 가장 가까운 다른 적들 최대 spawnOnHitCount개 수집
                    std::vector<std::pair<float, EnemyComponent*>> candidates;
                    XMVECTOR hitV = XMLoadFloat3(&hitPos);
                    for (const auto& obj : pRoom->GetGameObjects())
                    {
                        if (!obj) continue;
                        EnemyComponent* e = obj->GetComponent<EnemyComponent>();
                        if (!e || e->IsDead() || e == pEnemy) continue;
                        auto* pT = obj->GetTransform();
                        if (!pT) continue;
                        XMFLOAT3 ep = pT->GetPosition();
                        float d = XMVectorGetX(XMVector3Length(XMLoadFloat3(&ep) - hitV));
                        candidates.push_back({ d, e });
                    }
                    std::sort(candidates.begin(), candidates.end(),
                        [](const auto& a, const auto& b){ return a.first < b.first; });

                    int spawnCount = min(stats.spawnOnHitCount, (int)candidates.size());
                    for (int si = 0; si < spawnCount; ++si)
                    {
                        EnemyComponent* pTarget = candidates[si].second;
                        auto* pT = pTarget->GetOwner() ? pTarget->GetOwner()->GetTransform() : nullptr;
                        if (!pT) continue;
                        XMFLOAT3 targetPos = pT->GetPosition();

                        Projectile echo = projectile;
                        echo.position        = hitPos;
                        echo.damage          = projectile.damage * 0.5f;
                        echo.distanceTraveled = 0.f;
                        echo.isActive        = true;
                        echo.wasHit          = false;
                        echo.spawnOnHitCount = 0; // 반향의 반향 방지
                        echo.fluidVFXId      = -1;
                        echo.extraVFXIds.clear();

                        XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&targetPos) - hitV);
                        XMStoreFloat3(&echo.direction, dir);
                        SpawnProjectile(echo);
                    }
                }
            }
        }
    }
}

void ProjectileManager::ApplyAoEDamage(Projectile& projectile, const XMFLOAT3& impactPoint)
{
    if (!m_pScene) return;

    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    BoundingSphere explosionSphere(impactPoint, projectile.explosionRadius);

    const auto& gameObjects = pRoom->GetGameObjects();
    for (const auto& obj : gameObjects)
    {
        if (!obj) continue;

        EnemyComponent* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;

        TransformComponent* pTransform = obj->GetTransform();
        if (!pTransform) continue;

        XMFLOAT3 enemyPos = pTransform->GetPosition();
        XMFLOAT3 enemyScale = pTransform->GetScale();

        // Scale collision sphere based on enemy size (1.2 → 1.5)
        float maxScale = max(enemyScale.x, max(enemyScale.y, enemyScale.z));
        float radius = max(1.5f, maxScale * 1.5f);

        BoundingSphere enemySphere(enemyPos, radius);
        enemySphere.Center.y += radius * 0.7f;

        if (explosionSphere.Intersects(enemySphere))
        {
            // Calculate damage falloff based on distance (optional)
            XMVECTOR impact = XMLoadFloat3(&impactPoint);
            XMVECTOR enemy = XMLoadFloat3(&enemyPos);
            float distance = XMVectorGetX(XMVector3Length(enemy - impact));

            // Linear falloff: full damage at center, 50% at edge
            float falloff = 1.0f - (distance / projectile.explosionRadius) * 0.5f;
            falloff = max(0.5f, falloff);

            float finalDamage = projectile.damage * falloff;
            pEnemy->TakeDamage(finalDamage, false, projectile.execDamageBonus > 0.f);

            wchar_t buffer[128];
            swprintf_s(buffer, 128, L"[ProjectileManager] AoE hit! Damage: %.0f (falloff: %.2f)\n", finalDamage, falloff);
            OutputDebugString(buffer);
        }
    }
}

void ProjectileManager::CleanupInactiveProjectiles()
{
    auto it = std::remove_if(m_Projectiles.begin(), m_Projectiles.end(),
        [](const Projectile& p) { return !p.isActive; });

    size_t removed = std::distance(it, m_Projectiles.end());
    m_Projectiles.erase(it, m_Projectiles.end());

    if (removed > 0)
    {
        wchar_t buffer[128];
        swprintf_s(buffer, 128, L"[ProjectileManager] Cleaned up %zu inactive projectiles\n", removed);
        OutputDebugString(buffer);
    }
}

void ProjectileManager::Render(ID3D12GraphicsCommandList* pCommandList)
{
    if (!m_pProjectileMesh || !m_pcbMappedProjectiles || !m_pDescriptorHeap) return;

    // Calculate stride for constant buffer indexing
    UINT nSingleCBSize = (sizeof(ProjectileConstants) + 255) & ~255;
    // Cast to byte pointer for proper pointer arithmetic
    BYTE* pMappedBytes = reinterpret_cast<BYTE*>(m_pcbMappedProjectiles);

    size_t renderIndex = 0;
    for (const auto& projectile : m_Projectiles)
    {
        if (!projectile.isActive) continue;
        if (renderIndex >= MAX_RENDERED_PROJECTILES) break;
        // 플레이어/적 모두 fluid VFX로 표현 — 큐브 메시는 렌더하지 않음
        continue;

        // Update constant buffer for this projectile
        ProjectileConstants* pCB = reinterpret_cast<ProjectileConstants*>(pMappedBytes + renderIndex * nSingleCBSize);

        XMMATRIX worldMatrix = XMMatrixScaling(projectile.scale, projectile.scale, projectile.scale) *
                               XMMatrixTranslation(projectile.position.x, projectile.position.y, projectile.position.z);

        XMStoreFloat4x4(&pCB->m_xmf4x4World, XMMatrixTranspose(worldMatrix));

        // Set material properties
        pCB->m_nMaterialIndex = 0;
        pCB->m_bIsSkinned = 0;
        pCB->m_bHasTexture = 0;

        XMFLOAT4 color = GetElementColor(projectile.element);
        pCB->m_cAmbient = color;
        pCB->m_cDiffuse = color;
        pCB->m_cSpecular = XMFLOAT4(0.5f, 0.5f, 0.5f, 32.0f);  // Moderate specular
        pCB->m_cEmissive = XMFLOAT4(color.x * 0.5f, color.y * 0.5f, color.z * 0.5f, 1.0f);  // Slight glow

        // Set the descriptor table for this projectile's CBV
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_pDescriptorHeap->GetGPUHandle(
            m_nDescriptorStartIndex + static_cast<UINT>(renderIndex));
        pCommandList->SetGraphicsRootDescriptorTable(0, gpuHandle);

        // Render the mesh
        m_pProjectileMesh->Render(pCommandList, 0);

        renderIndex++;
    }
}

XMFLOAT4 ProjectileManager::GetElementColor(ElementType element) const
{
    switch (element)
    {
    case ElementType::Fire:
        return XMFLOAT4(1.0f, 0.3f, 0.1f, 1.0f);  // Orange-red (불)
    case ElementType::Water:
        return XMFLOAT4(0.2f, 0.5f, 1.0f, 1.0f);  // Blue (물)
    case ElementType::Wind:
        return XMFLOAT4(0.7f, 1.0f, 0.7f, 1.0f);  // Light green (바람)
    case ElementType::Earth:
        return XMFLOAT4(0.6f, 0.4f, 0.2f, 1.0f);  // Brown (대지)
    default:
        return XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);  // White
    }
}

void ProjectileManager::SpawnExplosionParticles(const XMFLOAT3& position, ElementType element)
{
    if (!m_pVFXManager) return;

    EffectLayer layer;
    layer.element     = element;

    switch (element)
    {
    case ElementType::Fire:
        layer.type          = EmitterType::Burst;
        layer.particleCount = 24;
        layer.coreColor     = { 1.0f, 0.8f, 0.3f, 1.0f };  // 노랑-오렌지
        layer.edgeColor     = { 0.8f, 0.1f, 0.0f, 0.0f };  // 적색 페이드
        layer.speedMin      = 2.5f;
        layer.speedMax      = 6.0f;
        layer.lifetimeMin   = 0.20f;
        layer.lifetimeMax   = 0.40f;
        layer.sizeScale     = 0.55f;
        layer.burst.bounceCoeff = 0.f;
        layer.burst.fadeOut     = true;
        layer.burst.fadeSize    = true;
        break;
    case ElementType::Water:
        layer.type          = EmitterType::Burst;
        layer.particleCount = 20;
        layer.coreColor     = { 0.4f, 0.7f, 1.0f, 1.0f };
        layer.edgeColor     = { 0.2f, 0.4f, 0.8f, 0.0f };
        layer.speedMin      = 2.5f;
        layer.speedMax      = 5.5f;
        layer.lifetimeMin   = 0.15f;
        layer.lifetimeMax   = 0.35f;
        layer.sizeScale     = 0.5f;
        layer.burst.fadeOut = true;
        layer.burst.fadeSize = true;
        break;
    case ElementType::Wind:
        layer.type          = EmitterType::Sphere;
        layer.particleCount = 16;
        layer.coreColor     = { 0.9f, 1.0f, 0.9f, 0.9f };
        layer.edgeColor     = { 0.7f, 0.95f, 0.7f, 0.0f };
        layer.speedMin      = 4.0f;
        layer.speedMax      = 8.0f;
        layer.lifetimeMin   = 0.15f;
        layer.lifetimeMax   = 0.30f;
        layer.sizeScale     = 0.45f;
        layer.sphere.radius        = 0.5f;
        layer.sphere.shellFraction = 0.f;
        layer.sphere.inward        = false;
        break;
    case ElementType::Earth:
        layer.type          = EmitterType::Burst;
        layer.particleCount = 18;
        layer.coreColor     = { 0.8f, 0.6f, 0.4f, 1.0f };
        layer.edgeColor     = { 0.5f, 0.35f, 0.2f, 0.0f };
        layer.speedMin      = 2.0f;
        layer.speedMax      = 5.0f;
        layer.lifetimeMin   = 0.30f;
        layer.lifetimeMax   = 0.50f;
        layer.sizeScale     = 0.6f;
        layer.burst.bounceCoeff = 0.2f;
        layer.burst.groundY     = position.y - 0.5f;
        layer.burst.fadeOut     = true;
        layer.burst.fadeSize    = true;
        break;
    default:
        layer.type          = EmitterType::Sphere;
        layer.particleCount = 12;
        layer.coreColor     = { 1.0f, 1.0f, 1.0f, 1.0f };
        layer.edgeColor     = { 0.5f, 0.5f, 0.5f, 0.0f };
        layer.speedMin      = 2.0f;
        layer.speedMax      = 5.0f;
        layer.lifetimeMin   = 0.18f;
        layer.lifetimeMax   = 0.36f;
        layer.sizeScale     = 0.5f;
        layer.sphere.radius = 0.4f;
        break;
    }

    // 폭발 파티클은 플레이어 슬롯에 스폰 (적도 터질 때 visible)
    m_pVFXManager->SpawnLightLayer(position, XMFLOAT3(0, 1, 0), layer, /*isPlayer*/true);

}
