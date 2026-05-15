#pragma once
#include "stdafx.h"
#include "GameObject.h"
#include "EnemySpawnData.h"

class EnemyComponent;
class EnemySpawner;
class Scene;
class LavaGeyserManager;
class RockfallManager;
class Shader;
class CDescriptorHeap;

enum class RoomState {
    Inactive,   // 플레이어가 아직 진입하지 않음
    Active,     // 플레이어가 진입하여 전투 중
    Cleared     // 모든 적을 처치하여 클리어됨
};

class CRoom {
public:
    CRoom();
    virtual ~CRoom();

    virtual void Update(float deltaTime);
    virtual void Render(ID3D12GraphicsCommandList* pCommandList);

    // 오브젝트 관리
    void AddGameObject(std::unique_ptr<GameObject> pGameObject);
    void RemoveGameObject(GameObject* pGameObject);
    const std::vector<std::unique_ptr<GameObject>>& GetGameObjects() const { return m_vGameObjects; }

    // 상태 및 영역 설정
    void SetState(RoomState state);
    RoomState GetState() const { return m_eState; }

    void SetBoundingBox(const BoundingBox& box) { m_BoundingBox = box; }
    const BoundingBox& GetBoundingBox() const { return m_BoundingBox; }

    // 플레이어 진입 체크
    bool IsPlayerInside(const XMFLOAT3& playerPos);

    // 방 클리어 조건 체크 (기본적으로는 적이 모두 제거되었는지 확인)
    virtual void CheckClearCondition();

    // Enemy management
    void SetSpawnConfig(const RoomSpawnConfig& config) { m_SpawnConfig = config; }
    const RoomSpawnConfig& GetSpawnConfig() const { return m_SpawnConfig; }

    void SetEnemySpawner(EnemySpawner* pSpawner) { m_pSpawner = pSpawner; }
    void SetPlayerTarget(GameObject* pPlayer) { m_pPlayerTarget = pPlayer; }

    // Enemy tracking
    void RegisterEnemy(EnemyComponent* pEnemy);
    void OnEnemyDeath(EnemyComponent* pEnemy);
    void AddExpectedEnemies(int n) { m_nTotalEnemies += n; }
    int GetAliveEnemyCount() const { return m_nTotalEnemies - m_nDeadEnemies; }
    int GetTotalEnemyCount() const { return m_nTotalEnemies; }
    const std::vector<EnemyComponent*>& GetEnemies() const { return m_vEnemies; }

    // Spawn enemies when room becomes active
    void SpawnEnemies();
    bool HasSpawnedEnemies() const { return m_bEnemiesSpawned; }

    // Drop item system
    void SetScene(Scene* pScene) { m_pScene = pScene; }
    Scene* GetScene() const { return m_pScene; }
    void SpawnDropItem();
    GameObject* GetDropItem() const { return m_pDropItem; }
    bool HasDropItem() const { return m_pDropItem != nullptr; }
    void ClearDropItem() { m_pDropItem = nullptr; }

    // Portal cube system
    void SpawnPortalCube();
    GameObject* GetPortalCube() const { return m_pPortalCube; }
    bool HasPortalCube() const { return m_pPortalCube != nullptr; }
    void ClearPortalCube();

    // Lava Geyser system
    void InitLavaGeyserManager(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                               Shader* pShader, CDescriptorHeap* pDescriptorHeap, UINT nDescriptorIndex);
    void SetLavaGeyserEnabled(bool bEnabled);
    LavaGeyserManager* GetLavaGeyserManager() const { return m_pGeyserManager.get(); }

    // Rockfall system (Earth)
    void InitRockfallManager(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, Shader* pShader);
    void SetRockfallEnabled(bool bEnabled);
    RockfallManager* GetRockfallManager() const { return m_pRockfallManager.get(); }

protected:
    std::vector<std::unique_ptr<GameObject>> m_vGameObjects; // 방에 속한 모든 오브젝트
    RoomState m_eState = RoomState::Inactive;
    BoundingBox m_BoundingBox; // 방의 영역 (AABB)

    // Enemy management
    std::vector<EnemyComponent*> m_vEnemies;  // Pointers to enemy components (not owned)
    int m_nTotalEnemies = 0;
    int m_nDeadEnemies = 0;
    RoomSpawnConfig m_SpawnConfig;
    EnemySpawner* m_pSpawner = nullptr;
    GameObject* m_pPlayerTarget = nullptr;
    bool m_bEnemiesSpawned = false;

    // 다중 웨이브 상태 — m_SpawnConfig.HasMultiWave() 일 때만 사용 (오프라인 한정)
    //   m_nNextWaveIndex == m_vWaves.size() 이면 모든 웨이브 스폰 완료 → 클리어 판정 활성
    int   m_nNextWaveIndex            = 0;
    float m_fSinceLastWaveSpawn       = 0.0f;   // AfterTimer 트리거용
    int   m_nKillsAtLastWaveSpawn     = 0;      // 직전 웨이브 스폰 시점의 누적 dead count
    int   m_nLastWaveSpawnedSize      = 0;      // 직전 웨이브가 실제 스폰한 적 마릿수

    // 웨이브 등장 연출: 2 단계
    //   Phase A (portal countdown): 공중 포탈 VFX 만 표시, 적 아직 미등장 (fDelay > 0).
    //   Phase B (fall): 적이 sky 위치에 실제 스폰 → kFallDuration 동안 ease-in 낙하.
    //                   pEnemy != nullptr 면 fall 단계.
    struct PendingWaveSpawn
    {
        std::string preset;
        XMFLOAT3    pos               = { 0, 0, 0 };   // 최종 ground 위치
        float       fDelay            = 0.0f;          // Phase A 남은 카운트다운
        int         nPortalVFXId      = -1;            // 공중 포탈
        int         nGroundVFXId      = -1;            // 지면 경고 링

        GameObject* pEnemy            = nullptr;       // Phase B 진입 시점에 세팅
        float       fFallTimer        = 0.0f;
        float       fSkyY             = 0.0f;          // 낙하 시작 Y
        float       fGroundY          = 0.0f;          // 낙하 종료 Y
    };
    std::vector<PendingWaveSpawn> m_vPendingSpawns;

    // 헬퍼 — m_SpawnConfig 의 트리거 평가 + 스폰. 오프라인에서만 호출.
    void TrySpawnNextWave(float dt);
    void SpawnSingleWave(const EnemyWave& wave);
    void UpdatePendingSpawns(float dt);
    void CleanupPendingSpawns();

    // Drop item system
    Scene* m_pScene = nullptr;
    GameObject* m_pDropItem = nullptr;

    // Portal cube system
    GameObject* m_pPortalCube = nullptr;
    // Portal VFX (3 레이어 무한 루프) — 각 이미터 lifetime 짧아 주기 재 spawn 필요
    int   m_nPortalCubeRingVFXId    = -1; // Portal_Ring (회전 링)
    int   m_nPortalCubeSuctionVFXId = -1; // Portal_Suction (흡입)
    int   m_nPortalCubeBeamVFXId    = -1; // Portal_Beam (수직 광주)
    float m_fPortalCubeRingRespawnTimer = 0.0f;

    // Lava Geyser system
    std::unique_ptr<LavaGeyserManager> m_pGeyserManager;

    // Rockfall system (Earth stage)
    std::unique_ptr<RockfallManager> m_pRockfallManager;
};
