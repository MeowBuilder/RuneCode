#pragma once
#include "stdafx.h"
#include "EnemySpawnData.h"
#include "MeshLoader.h"
#include <map>
#include <string>
#include <memory>

class GameObject;
class CRoom;
class Scene;
class Shader;
class EnemyComponent;
class FluidSkillVFXManager;
class Mesh;
class LineMesh;
class FanMesh;

class EnemySpawner
{
public:
    EnemySpawner();
    ~EnemySpawner();

    // Initialize with device and command list for creating resources
    void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, Scene* pScene, Shader* pShader);

    // Register enemy presets
    void RegisterEnemyPreset(const std::string& name, const EnemySpawnData& data);

    // Spawn an enemy from a preset
    GameObject* SpawnEnemy(CRoom* pRoom, const std::string& preset, const XMFLOAT3& position, GameObject* pTarget);

    // Spawn a test enemy using CubeMesh (for testing without mesh files)
    GameObject* SpawnTestEnemy(CRoom* pRoom, const XMFLOAT3& position, GameObject* pTarget);

    // Spawn all enemies for a room based on its spawn config
    void SpawnRoomEnemies(CRoom* pRoom, const RoomSpawnConfig& config, GameObject* pTarget);

    // Get registered presets
    bool HasPreset(const std::string& name) const;

    // ── 네트워크 보스용 인디케이터 ─────────────────────────────────────────
    //   서버 권위 보스(EnemyComponent 없음)가 공격 시 텔레그래프(Circle/ForwardBox) 표시할 수 있게
    //   border + fill 4개를 미리 생성. NetworkManager 가 보스 스폰 시 호출 → ProcessMonsterAttack 에서
    //   타입별로 위치/스케일 갱신.
    struct NetBossIndicatorSet
    {
        GameObject* circleBorder = nullptr;   // 원형 테두리 링
        GameObject* circleFill   = nullptr;   // 원형 fill (원판)
        GameObject* boxBorder    = nullptr;   // ForwardBox 외곽
        GameObject* boxFill      = nullptr;   // ForwardBox 내부 fill
    };
    NetBossIndicatorSet CreateNetBossIndicators();

private:
    // Create a cube mesh enemy for testing
    GameObject* CreateCubeEnemy(CRoom* pRoom, const XMFLOAT3& position, const XMFLOAT3& scale, const XMFLOAT4& color);

    // Create an enemy with loaded mesh from file
    GameObject* CreateMeshEnemy(CRoom* pRoom, const XMFLOAT3& position, const EnemySpawnData& data);

    // Add render components recursively to game object hierarchy
    void AddRenderComponentsToHierarchy(GameObject* pGameObject);

    // Apply color tint to all meshes in game object hierarchy
    void ApplyColorToHierarchy(GameObject* pGameObject, const XMFLOAT4& color);

    // Load texture to all meshes in game object hierarchy
    void LoadTextureToHierarchy(GameObject* pGameObject, const std::string& texturePath);

    // Setup common enemy components
    void SetupEnemyComponents(GameObject* pEnemy, const EnemySpawnData& data, CRoom* pRoom, GameObject* pTarget);

    // Create indicator game objects
    GameObject* CreateIndicatorObject(CRoom* pRoom, Mesh* pMesh);

    // Setup attack indicators for an enemy
    void SetupAttackIndicators(GameObject* pEnemy, EnemyComponent* pEnemyComp, const AttackIndicatorConfig& config, CRoom* pRoom);

private:
    std::map<std::string, EnemySpawnData> m_mapPresets;

    ID3D12Device* m_pDevice = nullptr;
    ID3D12GraphicsCommandList* m_pCommandList = nullptr;
    Scene* m_pScene = nullptr;
    Shader* m_pShader = nullptr;
    FluidSkillVFXManager* m_pVFXManager = nullptr;

public:
    void SetVFXManager(FluidSkillVFXManager* mgr) { m_pVFXManager = mgr; }

    // Shared meshes for indicators
    RingMesh* m_pRingMesh = nullptr;   // 테두리 링 (공격 범위 윤곽)
    RingMesh* m_pDiscMesh = nullptr;   // 꽉 찬 원판 (공격 차오름 fill)
    LineMesh* m_pLineMesh = nullptr;
    FanMesh*  m_pFanMesh  = nullptr;
    Mesh*     m_pBoxMesh  = nullptr;   // 평평한 사각형 (ForwardBox 전방 직사각형)
};
