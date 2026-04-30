#include "stdafx.h"
#include "LavaGeyserManager.h"
#include "LavaGeyserComponent.h"
#include "FluidParticleSystem.h"
#include "FluidParticle.h"
#include "VFXManager.h"
#include "Room.h"
#include "Scene.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "RenderComponent.h"
#include "Mesh.h"
#include "Shader.h"
#include "DescriptorHeap.h"

LavaGeyserManager::LavaGeyserManager()
{
    m_Indicators.fill(nullptr);
    m_Components.fill(nullptr);
}

LavaGeyserManager::~LavaGeyserManager()
{
    if (m_pRingMesh)
    {
        m_pRingMesh->Release();
        m_pRingMesh = nullptr;
    }
    if (m_pCircleMesh)
    {
        m_pCircleMesh->Release();
        m_pCircleMesh = nullptr;
    }
}

void LavaGeyserManager::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                              CRoom* pRoom, Shader* pShader, CDescriptorHeap* pDescriptorHeap, UINT nDescriptorIndex)
{
    if (m_bInitialized) return;

    m_pRoom = pRoom;
    m_pScene = pRoom->GetScene();
    m_pShader = pShader;
    m_pDevice = pDevice;
    m_pCommandList = pCommandList;

    if (!m_pScene)
    {
        OutputDebugString(L"[LavaGeyserManager] ERROR: Room has no Scene!\n");
        return;
    }

    // RingMesh 생성 (외곽선 - 두꺼운 링)
    m_pRingMesh = new RingMesh(pDevice, pCommandList, 1.0f, 0.85f, 64);
    m_pRingMesh->AddRef();

    // FluidParticleSystem 생성
    m_pFluidSystem = std::make_unique<FluidParticleSystem>();
    m_pFluidSystem->Init(pDevice, pCommandList, pDescriptorHeap, nDescriptorIndex);

    // 인디케이터 풀 생성 (Scene::CreateGameObject 사용!)
    VFXManager* pVFXManager = m_pScene->GetVFXManager();

    for (int i = 0; i < POOL_SIZE; ++i)
    {
        m_Indicators[i] = CreateIndicatorObject(pDevice, pCommandList, pShader);

        if (m_Indicators[i])
        {
            // LavaGeyserComponent 추가
            auto* pComp = m_Indicators[i]->AddComponent<LavaGeyserComponent>();
            pComp->SetIndicator(m_Indicators[i]);  // 자기 자신이 인디케이터
            pComp->SetFluidSystem(m_pFluidSystem.get());
            pComp->SetVFXManager(pVFXManager);  // LightEmitterSystem 경유
            pComp->SetRoom(pRoom);
            m_Components[i] = pComp;
        }
    }

    m_bInitialized = true;
    OutputDebugString(L"[LavaGeyserManager] Initialized with 5 geyser pool\n");
}

GameObject* LavaGeyserManager::CreateIndicatorObject(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, Shader* pShader)
{
    if (!m_pScene || !m_pRingMesh || !pShader) return nullptr;

    // Scene::CreateGameObject로 생성해야 상수 버퍼가 제대로 초기화됨!
    GameObject* pIndicator = m_pScene->CreateGameObject(pDevice, pCommandList);
    if (!pIndicator) return nullptr;

    // 초기 위치: 카메라 아래 (숨김 상태)
    TransformComponent* pTransform = pIndicator->GetTransform();
    pTransform->SetPosition(0.0f, -1000.0f, 0.0f);

    // 메시 설정
    m_pRingMesh->AddRef();
    pIndicator->SetMesh(m_pRingMesh);

    // 강렬한 빨간색 머티리얼 (적 공격 인디케이터와 동일)
    MATERIAL redMaterial;
    redMaterial.m_cAmbient = XMFLOAT4(0.5f, 0.0f, 0.0f, 1.0f);
    redMaterial.m_cDiffuse = XMFLOAT4(1.0f, 0.2f, 0.1f, 1.0f);
    redMaterial.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    redMaterial.m_cEmissive = XMFLOAT4(1.5f, 0.2f, 0.1f, 1.0f);  // 강한 빨간 이미시브
    pIndicator->SetMaterial(redMaterial);

    // 렌더 컴포넌트 추가
    auto* pRenderComp = pIndicator->AddComponent<RenderComponent>();
    pRenderComp->SetMesh(m_pRingMesh);
    pShader->AddRenderComponent(pRenderComp);

    return pIndicator;
}

void LavaGeyserManager::Update(float deltaTime)
{
    if (!m_bActive || !m_bInitialized) return;

    // 1. 모든 장판 컴포넌트 업데이트
    for (auto* pComp : m_Components)
    {
        if (pComp)
        {
            pComp->Update(deltaTime);
        }
    }

    // 2. 유체 시스템 업데이트
    if (m_pFluidSystem)
    {
        m_pFluidSystem->Update(deltaTime);
    }

    // 3. 스폰 타이머
    m_fSpawnTimer += deltaTime;
    if (m_fSpawnTimer >= m_fSpawnInterval)
    {
        m_fSpawnTimer = 0.0f;
        SpawnGeysers();
    }
}

void LavaGeyserManager::Render(ID3D12GraphicsCommandList* pCommandList,
                                const XMFLOAT4X4& viewProj, const XMFLOAT3& cameraRight, const XMFLOAT3& cameraUp)
{
    if (!m_bActive || !m_bInitialized) return;

    // 유체 파티클 렌더링
    if (m_pFluidSystem && m_pFluidSystem->IsActive())
    {
        m_pFluidSystem->Render(pCommandList, viewProj, cameraRight, cameraUp);
    }
}

void LavaGeyserManager::SetActive(bool bActive)
{
    m_bActive = bActive;
    m_fSpawnTimer = 0.0f;

    if (!bActive)
    {
        // 비활성화 시 모든 인디케이터 숨김
        for (auto* pIndicator : m_Indicators)
        {
            if (pIndicator)
            {
                pIndicator->GetTransform()->SetPosition(0.0f, -1000.0f, 0.0f);
            }
        }

        // 유체 파티클 클리어
        if (m_pFluidSystem)
        {
            m_pFluidSystem->Clear();
        }
    }

    wchar_t buffer[64];
    swprintf_s(buffer, L"[LavaGeyserManager] SetActive: %s\n", bActive ? L"true" : L"false");
    OutputDebugString(buffer);
}

void LavaGeyserManager::SpawnGeysers()
{
    XMFLOAT3 playerPos = GetPlayerPosition();

    // 스폰 개수 결정 (2-3개)
    int count = m_nMinSpawnCount + (int)(RandFloat() * (m_nMaxSpawnCount - m_nMinSpawnCount + 1));
    if (count > m_nMaxSpawnCount) count = m_nMaxSpawnCount;

    int spawned = 0;
    for (int i = 0; i < count; ++i)
    {
        // 플레이어 주변 랜덤 위치 (3-8m 거리)
        float angle = RandFloat() * XM_2PI;
        float dist = 3.0f + RandFloat() * 5.0f;

        XMFLOAT3 spawnPos = {
            playerPos.x + cosf(angle) * dist,
            playerPos.y,
            playerPos.z + sinf(angle) * dist
        };

        // Idle 상태인 장판 찾아서 활성화
        for (auto* pComp : m_Components)
        {
            if (pComp && pComp->IsIdle())
            {
                pComp->Activate(spawnPos);
                spawned++;
                break;
            }
        }
    }

    if (spawned > 0)
    {
        wchar_t buffer[64];
        swprintf_s(buffer, L"[LavaGeyserManager] Spawned %d geysers\n", spawned);
        OutputDebugString(buffer);
    }
}

XMFLOAT3 LavaGeyserManager::GetPlayerPosition() const
{
    if (!m_pScene) return XMFLOAT3(0, 0, 0);

    GameObject* pPlayer = m_pScene->GetPlayer();
    if (!pPlayer) return XMFLOAT3(0, 0, 0);

    TransformComponent* pTransform = pPlayer->GetTransform();
    if (!pTransform) return XMFLOAT3(0, 0, 0);

    return pTransform->GetPosition();
}

float LavaGeyserManager::RandFloat()
{
    // Xorshift32 PRNG
    m_nSeed ^= m_nSeed << 13;
    m_nSeed ^= m_nSeed >> 17;
    m_nSeed ^= m_nSeed << 5;
    return static_cast<float>(m_nSeed) / static_cast<float>(0xFFFFFFFF);
}
