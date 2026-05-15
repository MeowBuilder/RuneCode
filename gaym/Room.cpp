#include "stdafx.h"
#include "Room.h"
#include "EnemyComponent.h"
#include "EnemySpawner.h"
#include "Scene.h"
#include "Dx12App.h"
#include "DropItemComponent.h"
#include "InteractableComponent.h"
#include "RenderComponent.h"
#include "TransformComponent.h"
#include "Mesh.h"
#include "Shader.h"
#include "LavaGeyserManager.h"
#include "RockfallManager.h"
#include "NetworkManager.h"

CRoom::CRoom()
{
}

CRoom::~CRoom()
{
    // 미처 등장 못 한 웨이브 포탈 VFX 정리
    CleanupPendingSpawns();

    // Portal VFX 3 레이어 정리 — Scene/VFXManager 가 살아있을 때만
    if (m_pScene)
    {
        if (auto* pVFX = m_pScene->GetVFXManager())
        {
            if (m_nPortalCubeRingVFXId    >= 0) pVFX->Stop(m_nPortalCubeRingVFXId);
            if (m_nPortalCubeSuctionVFXId >= 0) pVFX->Stop(m_nPortalCubeSuctionVFXId);
            if (m_nPortalCubeBeamVFXId    >= 0) pVFX->Stop(m_nPortalCubeBeamVFXId);
        }
        m_nPortalCubeRingVFXId    = -1;
        m_nPortalCubeSuctionVFXId = -1;
        m_nPortalCubeBeamVFXId    = -1;
    }
}

void CRoom::ClearPortalCube()
{
    if (m_pScene)
    {
        if (auto* pVFX = m_pScene->GetVFXManager())
        {
            if (m_nPortalCubeRingVFXId    >= 0) pVFX->Stop(m_nPortalCubeRingVFXId);
            if (m_nPortalCubeSuctionVFXId >= 0) pVFX->Stop(m_nPortalCubeSuctionVFXId);
            if (m_nPortalCubeBeamVFXId    >= 0) pVFX->Stop(m_nPortalCubeBeamVFXId);
        }
        m_nPortalCubeRingVFXId    = -1;
        m_nPortalCubeSuctionVFXId = -1;
        m_nPortalCubeBeamVFXId    = -1;
    }
    m_fPortalCubeRingRespawnTimer = 0.0f;
    m_pPortalCube = nullptr;
}

void CRoom::Update(float deltaTime)
{
    // Inactive 상태에서는 아무것도 하지 않음
    if (m_eState == RoomState::Inactive)
        return;

    // Active 상태: 적 스폰 및 클리어 체크
    if (m_eState == RoomState::Active)
    {
        // Spawn enemies if not yet spawned
        if (!m_bEnemiesSpawned)
        {
            SpawnEnemies();
        }

        // 다중 웨이브: 첫 스폰 이후에도 다음 웨이브 트리거를 매 프레임 평가
        //   오프라인 모드에서만 동작 — 온라인은 서버 권위 (SpawnEnemies 안에서 skip 됨)
        if (m_SpawnConfig.HasMultiWave())
        {
            NetworkManager* pNet = NetworkManager::GetInstance();
            bool bOnline = (pNet && pNet->IsConnected());
            if (!bOnline)
            {
                UpdatePendingSpawns(deltaTime);
                TrySpawnNextWave(deltaTime);
            }
        }

        // Update lava geyser manager
        if (m_pGeyserManager)
        {
            m_pGeyserManager->Update(deltaTime);
        }

        // Update rockfall manager (Earth)
        if (m_pRockfallManager)
        {
            m_pRockfallManager->Update(deltaTime);
        }

        CheckClearCondition();
    }

    // Active 및 Cleared 상태 모두에서 오브젝트 업데이트 (드랍 아이템 등)
    for (auto& pGameObject : m_vGameObjects)
    {
        pGameObject->Update(deltaTime);
    }

    // ── PortalCube Portal_Ring VFX 관리 (InteractionCube 와 동일 패턴) ─────────
    //   Ring 이미터는 1회 spawn 후 입자가 lifetime 끝나면 사라지므로 1.5s 마다 stop+respawn 으로 continuous 유지
    if (m_pPortalCube && m_pScene)
    {
        VFXManager* pVFX = m_pScene->GetVFXManager();
        auto* pInteractable = m_pPortalCube->GetComponent<InteractableComponent>();
        bool bActive = pVFX && pInteractable && pInteractable->IsActive();

        if (bActive)
        {
            DirectX::XMFLOAT3 cubePos = m_pPortalCube->GetTransform()->GetPosition();
            // 포탈이 바닥에 눕혀짐(XZ 평면) → Ring/Beam normal 모두 Y
            DirectX::XMFLOAT3 vfxNormal{ 0.0f, 1.0f, 0.0f };
            DirectX::XMFLOAT3 beamNormal{ 0.0f, 1.0f, 0.0f };

            constexpr float PORTAL_RING_RESPAWN_INTERVAL = 1.5f;
            m_fPortalCubeRingRespawnTimer += deltaTime;

            bool bNeedSpawn = (m_nPortalCubeRingVFXId < 0)
                           || (m_fPortalCubeRingRespawnTimer >= PORTAL_RING_RESPAWN_INTERVAL);

            if (bNeedSpawn)
            {
                if (m_nPortalCubeRingVFXId    >= 0) pVFX->Stop(m_nPortalCubeRingVFXId);
                if (m_nPortalCubeSuctionVFXId >= 0) pVFX->Stop(m_nPortalCubeSuctionVFXId);
                if (m_nPortalCubeBeamVFXId    >= 0) pVFX->Stop(m_nPortalCubeBeamVFXId);
                m_nPortalCubeRingVFXId    = pVFX->Spawn("Portal_Ring",    cubePos, vfxNormal, 0u, false);
                m_nPortalCubeSuctionVFXId = pVFX->Spawn("Portal_Suction", cubePos, vfxNormal, 0u, false);
                m_nPortalCubeBeamVFXId    = pVFX->Spawn("Portal_Beam",    cubePos, beamNormal, 0u, false);
                m_fPortalCubeRingRespawnTimer = 0.0f;
            }
            else
            {
                if (m_nPortalCubeRingVFXId    >= 0) pVFX->Track(m_nPortalCubeRingVFXId,    cubePos, vfxNormal);
                if (m_nPortalCubeSuctionVFXId >= 0) pVFX->Track(m_nPortalCubeSuctionVFXId, cubePos, vfxNormal);
                if (m_nPortalCubeBeamVFXId    >= 0) pVFX->Track(m_nPortalCubeBeamVFXId,    cubePos, beamNormal);
            }
        }
        else
        {
            if (m_nPortalCubeRingVFXId >= 0)
            {
                pVFX->Stop(m_nPortalCubeRingVFXId);
                m_nPortalCubeRingVFXId = -1;
            }
            if (m_nPortalCubeSuctionVFXId >= 0)
            {
                pVFX->Stop(m_nPortalCubeSuctionVFXId);
                m_nPortalCubeSuctionVFXId = -1;
            }
            if (m_nPortalCubeBeamVFXId >= 0)
            {
                pVFX->Stop(m_nPortalCubeBeamVFXId);
                m_nPortalCubeBeamVFXId = -1;
            }
            m_fPortalCubeRingRespawnTimer = 0.0f;
        }
    }
}

void CRoom::Render(ID3D12GraphicsCommandList* pCommandList)
{
    // Inactive가 아닐 때만 렌더링 (또는 거리에 따라 판단 가능)
    if (m_eState != RoomState::Inactive)
    {
        for (auto& pGameObject : m_vGameObjects)
        {
            pGameObject->Render(pCommandList);
        }
    }
}

void CRoom::AddGameObject(std::unique_ptr<GameObject> pGameObject)
{
    m_vGameObjects.push_back(std::move(pGameObject));
}

void CRoom::RemoveGameObject(GameObject* pGameObject)
{
    if (!pGameObject) return;

    // Also remove from enemies list if it's an enemy
    auto* pEnemyComp = pGameObject->GetComponent<EnemyComponent>();
    if (pEnemyComp)
    {
        auto it = std::find(m_vEnemies.begin(), m_vEnemies.end(), pEnemyComp);
        if (it != m_vEnemies.end())
        {
            m_vEnemies.erase(it);
        }
    }

    // Remove from game objects list
    for (auto it = m_vGameObjects.begin(); it != m_vGameObjects.end(); ++it)
    {
        if (it->get() == pGameObject)
        {
            m_vGameObjects.erase(it);
            OutputDebugString(L"[Room] Deleted GameObject from Room\n");
            return;
        }
    }
}

void CRoom::SetState(RoomState state)
{
    if (m_eState == state) return;

    RoomState oldState = m_eState;
    m_eState = state;

    // Debug output
    const wchar_t* stateNames[] = { L"Inactive", L"Active", L"Cleared" };
    wchar_t buffer[128];
    swprintf_s(buffer, L"[Room] State changed: %s -> %s\n",
        stateNames[static_cast<int>(oldState)],
        stateNames[static_cast<int>(state)]);
    OutputDebugString(buffer);

    // 상태 변경 시 필요한 로직
    switch (m_eState)
    {
    case RoomState::Active:
        OutputDebugString(L"[Room] Room activated - enemies will spawn\n");
        // Enemies will be spawned in Update
        if (m_pGeyserManager)   m_pGeyserManager->SetActive(true);
        if (m_pRockfallManager) m_pRockfallManager->SetActive(true);
        break;
    case RoomState::Cleared:
        OutputDebugString(L"[Room] Room cleared!\n");
        if (m_pGeyserManager)   m_pGeyserManager->SetActive(false);
        if (m_pRockfallManager) m_pRockfallManager->SetActive(false);
        break;
    }
}

bool CRoom::IsPlayerInside(const XMFLOAT3& playerPos)
{
    return m_BoundingBox.Contains(XMLoadFloat3(&playerPos)) != DISJOINT;
}

void CRoom::CheckClearCondition()
{
    // Only check if active and enemies have been spawned
    if (m_eState != RoomState::Active || !m_bEnemiesSpawned)
        return;

    // 다중 웨이브: 모든 웨이브 스폰 + 모든 pending 등장 연출 완료 전에는 클리어 판정 안 함
    //   - m_nNextWaveIndex < size:  아직 트리거 안 된 웨이브 남음
    //   - !m_vPendingSpawns.empty(): 마지막 웨이브 포탈 카운트다운 중 (실제 적 아직 미등장)
    //     이 race 가 빠지면 직전 웨이브 전멸 직후 ClearCondition 이 오판됨.
    if (m_SpawnConfig.HasMultiWave())
    {
        if (m_nNextWaveIndex < static_cast<int>(m_SpawnConfig.m_vWaves.size()))
            return;
        if (!m_vPendingSpawns.empty())
            return;
    }

    // Check if all enemies are dead
    if (m_nTotalEnemies > 0 && m_nDeadEnemies >= m_nTotalEnemies)
    {
        SetState(RoomState::Cleared);
        SpawnDropItem();
        SpawnPortalCube();
    }
}

void CRoom::RegisterEnemy(EnemyComponent* pEnemy)
{
    if (!pEnemy) return;

    m_vEnemies.push_back(pEnemy);
    m_nTotalEnemies++;

    wchar_t buffer[64];
    swprintf_s(buffer, L"[Room] Registered enemy %d\n", m_nTotalEnemies);
    OutputDebugString(buffer);
}

void CRoom::OnEnemyDeath(EnemyComponent* pEnemy)
{
    m_nDeadEnemies++;

    wchar_t buffer[128];
    swprintf_s(buffer, L"[Room] Enemy died! (%d/%d dead)\n", m_nDeadEnemies, m_nTotalEnemies);
    OutputDebugString(buffer);

    // Clear condition will be checked in Update
}

void CRoom::SpawnEnemies()
{
    if (m_bEnemiesSpawned) return;

    m_bEnemiesSpawned = true;

    // 네트워크 연결 상태면 서버가 몬스터를 관리하므로 로컬 스폰 스킵
    NetworkManager* pNet = NetworkManager::GetInstance();
    if (pNet && pNet->IsConnected())
    {
        OutputDebugString(L"[Room] Skipping local enemy spawn — server authoritative mode\n");
        return;
    }

    if (!m_pSpawner)
    {
        OutputDebugString(L"[Room] No spawner set - cannot spawn enemies\n");
        return;
    }

    // 다중 웨이브 모드: 첫 웨이브만 스폰. 나머지는 Update 의 TrySpawnNextWave 가 처리.
    if (m_SpawnConfig.HasMultiWave())
    {
        wchar_t buf[128];
        swprintf_s(buf, L"[Room] Multi-wave config (%zu waves) — spawning wave 0\n",
                   m_SpawnConfig.m_vWaves.size());
        OutputDebugString(buf);

        // 첫 웨이브는 강제로 즉시 스폰 (trigger 설정과 무관)
        const EnemyWave& first = m_SpawnConfig.m_vWaves[0];
        SpawnSingleWave(first);
        m_nNextWaveIndex          = 1;
        m_fSinceLastWaveSpawn     = 0.0f;
        m_nKillsAtLastWaveSpawn   = m_nDeadEnemies;
        m_nLastWaveSpawnedSize    = static_cast<int>(first.spawns.size());
        return;
    }

    if (m_SpawnConfig.m_vEnemySpawns.empty())
    {
        OutputDebugString(L"[Room] No spawn config set - no enemies to spawn\n");
        return;
    }

    wchar_t buffer[128];
    swprintf_s(buffer, L"[Room] Spawning %zu enemies...\n", m_SpawnConfig.m_vEnemySpawns.size());
    OutputDebugString(buffer);

    m_pSpawner->SpawnRoomEnemies(this, m_SpawnConfig, m_pPlayerTarget);
}

// 등장 연출 파라미터 (CRoom 전역)
namespace {
    constexpr float kSpawnPortalDelay = 0.7f;   // Phase A: 포탈 표시 시간
    constexpr float kSpawnPortalY     = 6.0f;   // 공중 포탈/낙하 시작 높이
    constexpr float kSpawnFallTime    = 0.4f;   // Phase B: 낙하 시간
}

void CRoom::SpawnSingleWave(const EnemyWave& wave)
{
    if (!m_pSpawner) return;
    if (wave.spawns.empty()) return;

    VFXManager* pVFX = (m_pScene ? m_pScene->GetVFXManager() : nullptr);

    // 스폰 stagger — 한 프레임에 mesh/texture 로드가 몰리면 hitch 발생.
    //   포탈은 즉시 다 띄우고, 실제 적 등장(Phase A 종료)만 살짝씩 시간 차.
    constexpr float kStaggerPerSpawn = 0.08f;
    int spawnIdx = 0;

    for (const auto& s : wave.spawns)
    {
        PendingWaveSpawn pending;
        pending.preset = s.first;
        pending.pos    = s.second;
        pending.fDelay = kSpawnPortalDelay + spawnIdx * kStaggerPerSpawn;

        if (pVFX)
        {
            XMFLOAT3 up(0.0f, 1.0f, 0.0f);
            // 공중 포탈 (모두 즉시 표시 — VFX 자체는 가벼움)
            XMFLOAT3 portalPos{ s.second.x, s.second.y + kSpawnPortalY, s.second.z };
            pending.nPortalVFXId = pVFX->Spawn("Spawn_Portal", portalPos, up, 0u, false);
            XMFLOAT3 groundPos{ s.second.x, s.second.y + 0.15f, s.second.z };
            pending.nGroundVFXId = pVFX->Spawn("Spawn_PortalGround", groundPos, up, 0u, false);
        }

        m_vPendingSpawns.push_back(std::move(pending));
        ++spawnIdx;
    }

    // 웨이브 시작 임팩트 — 카메라 쉐이크 (가볍게)
    if (m_pScene)
    {
        if (auto* pCam = m_pScene->GetCamera())
            pCam->StartShake(0.6f, 0.3f);
    }

    wchar_t buf[160];
    swprintf_s(buf, L"[Room] Scheduled wave with %zu pending spawns (portal %.2fs + fall %.2fs)\n",
               wave.spawns.size(), kSpawnPortalDelay, kSpawnFallTime);
    OutputDebugString(buf);
}

void CRoom::UpdatePendingSpawns(float dt)
{
    if (m_vPendingSpawns.empty()) return;

    VFXManager* pVFX = (m_pScene ? m_pScene->GetVFXManager() : nullptr);

    for (auto it = m_vPendingSpawns.begin(); it != m_vPendingSpawns.end(); )
    {
        // Phase A — 포탈 카운트다운 중
        if (it->pEnemy == nullptr)
        {
            it->fDelay -= dt;
            if (it->fDelay <= 0.0f)
            {
                // 포탈에서 적 emerge — sky 위치에 spawn 후 Phase B 진입
                XMFLOAT3 skyPos = it->pos;
                skyPos.y += kSpawnPortalY;

                GameObject* pNewEnemy = nullptr;
                if (m_pSpawner)
                    pNewEnemy = m_pSpawner->SpawnEnemy(this, it->preset, skyPos, m_pPlayerTarget);

                if (pNewEnemy)
                {
                    it->pEnemy     = pNewEnemy;
                    it->fFallTimer = 0.0f;
                    it->fSkyY      = skyPos.y;
                    it->fGroundY   = it->pos.y;
                    // 포탈은 낙하 동안에도 보이게 그대로 둠 (착지 시 stop)
                }
                else
                {
                    // 스폰 실패 — 포탈만 닫고 제거
                    if (pVFX && it->nPortalVFXId >= 0) pVFX->Stop(it->nPortalVFXId);
                    if (pVFX && it->nGroundVFXId >= 0) pVFX->Stop(it->nGroundVFXId);
                    it = m_vPendingSpawns.erase(it);
                    continue;
                }
            }
            ++it;
            continue;
        }

        // Phase B — 적 낙하 중
        it->fFallTimer += dt;
        float t = (std::min)(it->fFallTimer / kSpawnFallTime, 1.0f);
        // ease-in (gravity 느낌): y = sky - (sky-ground) * t^2
        float curY = it->fSkyY - (it->fSkyY - it->fGroundY) * (t * t);

        if (auto* pT = it->pEnemy->GetTransform())
        {
            XMFLOAT3 p = pT->GetPosition();
            p.y = curY;
            pT->SetPosition(p);
        }

        if (t >= 1.0f)
        {
            // 착지 — 포탈/지면링 stop, 큐에서 제거
            if (pVFX && it->nPortalVFXId >= 0) pVFX->Stop(it->nPortalVFXId);
            if (pVFX && it->nGroundVFXId >= 0) pVFX->Stop(it->nGroundVFXId);
            it = m_vPendingSpawns.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void CRoom::CleanupPendingSpawns()
{
    if (m_vPendingSpawns.empty()) return;
    VFXManager* pVFX = (m_pScene ? m_pScene->GetVFXManager() : nullptr);
    for (auto& p : m_vPendingSpawns)
    {
        if (pVFX && p.nPortalVFXId >= 0) pVFX->Stop(p.nPortalVFXId);
        if (pVFX && p.nGroundVFXId >= 0) pVFX->Stop(p.nGroundVFXId);
    }
    m_vPendingSpawns.clear();
}

void CRoom::TrySpawnNextWave(float dt)
{
    if (m_nNextWaveIndex >= static_cast<int>(m_SpawnConfig.m_vWaves.size()))
        return;   // 더 이상 스폰할 웨이브 없음

    m_fSinceLastWaveSpawn += dt;

    const EnemyWave& next = m_SpawnConfig.m_vWaves[m_nNextWaveIndex];

    bool bTrigger = false;
    switch (next.trigger)
    {
    case EnemyWave::TriggerType::Immediate:
        bTrigger = true;
        break;
    case EnemyWave::TriggerType::AfterTimer:
        bTrigger = (m_fSinceLastWaveSpawn >= next.fTriggerValue);
        break;
    case EnemyWave::TriggerType::AfterPrevCleared:
    {
        // 직전 웨이브가 스폰한 적이 전부 사망했는지 확인
        int killsSinceWaveStart = m_nDeadEnemies - m_nKillsAtLastWaveSpawn;
        bTrigger = (m_nLastWaveSpawnedSize > 0
                 && killsSinceWaveStart >= m_nLastWaveSpawnedSize);
        break;
    }
    case EnemyWave::TriggerType::AfterKillN:
    {
        int killsSinceWaveStart = m_nDeadEnemies - m_nKillsAtLastWaveSpawn;
        bTrigger = (killsSinceWaveStart >= next.nKillThreshold);
        break;
    }
    }

    if (bTrigger)
    {
        wchar_t buf[128];
        swprintf_s(buf, L"[Room] Wave %d triggered\n", m_nNextWaveIndex);
        OutputDebugString(buf);

        SpawnSingleWave(next);
        ++m_nNextWaveIndex;
        m_fSinceLastWaveSpawn   = 0.0f;
        m_nKillsAtLastWaveSpawn = m_nDeadEnemies;
        m_nLastWaveSpawnedSize  = static_cast<int>(next.spawns.size());
    }
}

void CRoom::SpawnDropItem()
{
    if (m_pDropItem) return;  // Already spawned
    if (!m_pScene)
    {
        OutputDebugString(L"[Room] Cannot spawn drop - no Scene pointer\n");
        return;
    }

    OutputDebugString(L"[Room] Spawning drop item...\n");

    // Spawn at player's position
    XMFLOAT3 spawnPos = XMFLOAT3(0.0f, 1.5f, 0.0f);
    if (m_pPlayerTarget)
    {
        spawnPos = m_pPlayerTarget->GetTransform()->GetPosition();
        spawnPos.y += 1.5f;  // Float slightly above the floor (player's actual Y)
    }

    // Create drop item as a room object
    m_pDropItem = m_pScene->CreateGameObject(Dx12App::GetInstance()->GetDevice(),
                                              Dx12App::GetInstance()->GetCommandList());

    if (!m_pDropItem)
    {
        OutputDebugString(L"[Room] Failed to create drop item GameObject\n");
        return;
    }

    // Set position and scale
    m_pDropItem->GetTransform()->SetPosition(spawnPos.x, spawnPos.y, spawnPos.z);
    m_pDropItem->GetTransform()->SetScale(1.5f, 1.5f, 1.5f);

    // Create white cube mesh
    CubeMesh* pCubeMesh = new CubeMesh(Dx12App::GetInstance()->GetDevice(),
                                        Dx12App::GetInstance()->GetCommandList(),
                                        1.0f, 1.0f, 1.0f);
    m_pDropItem->SetMesh(pCubeMesh);

    // Add RenderComponent for visibility
    m_pDropItem->AddComponent<RenderComponent>()->SetMesh(pCubeMesh);

    // Add DropItemComponent first — 룬 생성 후 등급을 알 수 있음
    DropItemComponent* pDropComp = m_pDropItem->AddComponent<DropItemComponent>();

    // 드랍 룬의 최고 등급에 따라 픽업 색상 결정
    XMFLOAT4 gc = DropItemComponent::GetGradeColor(pDropComp->GetHighestGrade());
    MATERIAL gradeMaterial;
    gradeMaterial.m_cAmbient  = XMFLOAT4(gc.x * 0.25f, gc.y * 0.25f, gc.z * 0.25f, 1.f);
    gradeMaterial.m_cDiffuse  = gc;
    gradeMaterial.m_cSpecular = XMFLOAT4(1.0f, 1.0f, 1.0f, 64.0f);
    gradeMaterial.m_cEmissive = XMFLOAT4(gc.x * 0.6f,  gc.y * 0.6f,  gc.z * 0.6f,  1.f);
    m_pDropItem->SetMaterial(gradeMaterial);

    OutputDebugString(L"[Room] Drop item spawned successfully!\n");
}

void CRoom::SpawnPortalCube()
{
    if (m_pPortalCube) return;  // Already spawned
    if (!m_pScene)
    {
        OutputDebugString(L"[Room] Cannot spawn portal - no Scene pointer\n");
        return;
    }

    OutputDebugString(L"[Room] Spawning portal cube...\n");

    // Spawn at player's position + Z offset (so it doesn't overlap with drop item)
    XMFLOAT3 spawnPos = XMFLOAT3(0.0f, 1.5f, 5.0f);
    if (m_pPlayerTarget)
    {
        spawnPos = m_pPlayerTarget->GetTransform()->GetPosition();
        spawnPos.y += 1.5f;  // Float slightly above the floor (player's actual Y)
        spawnPos.z += 5.0f;  // Offset in Z to avoid overlapping with drop item
    }

    // Create portal cube as a room object
    m_pPortalCube = m_pScene->CreateGameObject(Dx12App::GetInstance()->GetDevice(),
                                                Dx12App::GetInstance()->GetCommandList());

    if (!m_pPortalCube)
    {
        OutputDebugString(L"[Room] Failed to create portal cube GameObject\n");
        return;
    }

    // 포탈 비주얼 — 바닥 마법진. 반경 9u (시작방 베이스 크기 매칭). 베이스 표면 위에 깔리도록 y=1.5.
    m_pPortalCube->GetTransform()->SetPosition(spawnPos.x, spawnPos.y, spawnPos.z);
    m_pPortalCube->GetTransform()->SetScale(9.0f, 9.0f, 9.0f);
    m_pPortalCube->GetTransform()->SetRotation(0.0f, 0.0f, 0.0f);

    // 채워진 disc (innerRadius=0) — 포탈 표면
    RingMesh* pPortalDisc = new RingMesh(Dx12App::GetInstance()->GetDevice(),
                                          Dx12App::GetInstance()->GetCommandList(),
                                          1.0f, 0.0f, 64);
    m_pPortalCube->SetMesh(pPortalDisc);

    // 포탈 머티리얼 — fbm 와류로 두 보라 톤을 부드럽게 섞음 (디지털 듀얼톤 회피)
    MATERIAL portalMat;
    portalMat.m_cAmbient  = XMFLOAT4(0.00f, 0.00f, 0.00f, 1.0f);
    portalMat.m_cDiffuse  = XMFLOAT4(0.40f, 0.20f, 0.85f, 1.0f);   // 딥 바이올렛 (외곽 림)
    portalMat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    portalMat.m_cEmissive = XMFLOAT4(1.00f, 0.75f, 0.95f, 1.0f);   // 라벤더-핑크 (코어)
    m_pPortalCube->SetMaterial(portalMat);

    m_pPortalCube->AddComponent<RenderComponent>()->SetMesh(pPortalDisc);
    m_pPortalCube->SetPortal(true);  // bIsPortal — 셰이더 와류/블랙홀 분기 활성

    // Add InteractableComponent
    auto* pInteractable = m_pPortalCube->AddComponent<InteractableComponent>();
    pInteractable->SetPromptText(L"[F] Enter Portal");
    pInteractable->SetInteractionDistance(7.0f); // 세로 disc 반경 만큼 여유
    pInteractable->DisablePhysics();             // 중력/bobbing OFF — Scene 이 위치 직접 결정
    pInteractable->SetOnInteract([this](InteractableComponent* pComp) {
        if (!m_pScene)
            return;

        // 서버 연결 시 서버 권위 방 전환 (S_ROOM_TRANSITION 수신 시 실제 전환)
        NetworkManager* pNet = NetworkManager::GetInstance();
        if (pNet && pNet->IsConnected())
        {
            pNet->SendPortalInteract();
        }
        else
        {
            // 오프라인 폴백
            m_pScene->TransitionToNextRoom();
        }
    });

    OutputDebugString(L"[Room] Portal cube spawned successfully!\n");
}

void CRoom::InitLavaGeyserManager(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                                   Shader* pShader, CDescriptorHeap* pDescriptorHeap, UINT nDescriptorIndex)
{
    if (m_pGeyserManager)
    {
        OutputDebugString(L"[Room] LavaGeyserManager already initialized\n");
        return;
    }

    m_pGeyserManager = std::make_unique<LavaGeyserManager>();
    m_pGeyserManager->Init(pDevice, pCommandList, this, pShader, pDescriptorHeap, nDescriptorIndex);

    OutputDebugString(L"[Room] LavaGeyserManager initialized\n");
}

void CRoom::SetLavaGeyserEnabled(bool bEnabled)
{
    if (m_pGeyserManager)
    {
        m_pGeyserManager->SetActive(bEnabled);
    }
}

void CRoom::InitRockfallManager(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, Shader* pShader)
{
    if (m_pRockfallManager)
    {
        OutputDebugString(L"[Room] RockfallManager already initialized\n");
        return;
    }

    m_pRockfallManager = std::make_unique<RockfallManager>();
    m_pRockfallManager->Init(pDevice, pCommandList, this, pShader);
    OutputDebugString(L"[Room] RockfallManager initialized\n");
}

void CRoom::SetRockfallEnabled(bool bEnabled)
{
    if (m_pRockfallManager)
    {
        m_pRockfallManager->SetActive(bEnabled);
    }
}
