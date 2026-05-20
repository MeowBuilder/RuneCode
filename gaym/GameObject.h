#pragma once
#include "stdafx.h"
#include <vector>
#include <memory>
#include <string>
#include "Mesh.h"

struct ID3D12GraphicsCommandList; // 전방 선언
struct ID3D12Device; // 전방 선언
class Component;                   // 전방 선언
class TransformComponent;          // 전방 선언
class InputSystem;                 // 전방 선언 for InputSystem

struct MATERIAL
{
    XMFLOAT4 m_cAmbient;
    XMFLOAT4 m_cDiffuse;
    XMFLOAT4 m_cSpecular; // a = power
    XMFLOAT4 m_cEmissive;
};

struct ObjectConstants
{
	XMFLOAT4X4 m_xmf4x4World;
	UINT m_nMaterialIndex = 0;
    UINT m_bIsSkinned = 0;
    UINT m_bHasTexture = 0;
    UINT m_bIsLava = 0;
    UINT m_bIsWater = 0;
    UINT m_bHasEmissiveTexture = 0;
    float m_fHitFlash = 0.f;
    UINT m_bIsRocky = 0;  // HLSL: 7 uints + g_HitFlash float = 32B → 다음 offset 96
    UINT m_bIsGrass = 0;  // offset 96 — 절차적 풀(grass) 셰이딩(VS sway + PS vertex 그라데이션)
    UINT m_bIsPortal = 0; // offset 100 — 차원문 와류 셰이딩 (시안/마젠타 듀얼톤 + 블랙홀)
    UINT m_bIsDecal  = 0; // offset 104 — 지면 데칼 플래그
    UINT m_grassPad2 = 0; // offset 108
    // Status effect outline: element color (RGB) + intensity (A)
    XMFLOAT4 m_vStatusColor    = { 0.f, 0.f, 0.f, 0.f }; // offset 112
    float    m_fStatusIntensity = 0.f;                     // offset 128
    float    m_statusPad0 = 0.f;
    float    m_statusPad1 = 0.f;
    float    m_statusPad2 = 0.f;
	MATERIAL mMaterial;
    XMFLOAT4X4 m_xmf4x4BoneTransforms[128];
};


class GameObject
{
public:
	GameObject();
	~GameObject();

	void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList);
	void Update(float deltaTime);
	void Render(ID3D12GraphicsCommandList* pCommandList);

	template<typename T> T* GetComponent();
	template<typename T, typename... TArgs> T* AddComponent(TArgs&&... args);

	TransformComponent* GetTransform() { return m_pTransform; }

	void CreateConstantBuffer(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, UINT nBufferSize, D3D12_CPU_DESCRIPTOR_HANDLE d3dCbvCPUDescriptorHandle);
	// 기존 리소스 재사용 (맵 전환 시 CB 풀링용) — CBV는 이미 힙에 있으므로 재생성 불필요
	void ReuseConstantBuffer(const ComPtr<ID3D12Resource>& pCB, ObjectConstants* pMapped)
	{
		m_pd3dcbGameObject = pCB;
		m_pcbMappedGameObject = pMapped;
		// 이전 오너(예: 스킨드 적)가 남긴 bIsSkinned 플래그를 초기화.
		// AnimationComponent가 있으면 Update()에서 SetSkinned(true)로 다시 설정한다.
		if (m_pcbMappedGameObject)
		{
			// 이전 오너의 모든 플래그/상태를 초기화 (bIsWater, bIsLava, bIsSkinned 등)
			// Update()가 매 프레임 world matrix, material, bHasTexture를 다시 쓴다.
			ZeroMemory(m_pcbMappedGameObject, sizeof(ObjectConstants));
		}
	}
	ComPtr<ID3D12Resource> GetConstantBufferResource() const { return m_pd3dcbGameObject; }

	D3D12_GPU_DESCRIPTOR_HANDLE GetGpuDescriptorHandle() const { return m_cbvGPUDescriptorHandle; }
	void SetGpuDescriptorHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) { m_cbvGPUDescriptorHandle = handle; }
	void SetMaterialIndex(UINT index) { m_nMaterialIndex = index; }
	UINT GetMaterialIndex() const { return m_nMaterialIndex; }

	void SetMesh(Mesh* pMesh);
	Mesh* GetMesh() { return m_pMesh; }
	void SetChild(GameObject* pChild);
	void SetTransform(const XMFLOAT4X4& transform);

	void SetMaterial(const MATERIAL& material); // New method for setting material
	const MATERIAL& GetMaterial() const { return m_Material; }
	void SetTextureName(const std::string& strName) { m_strTextureName = strName; }
	std::string GetTextureName() const { return m_strTextureName; }

	void LoadTexture(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle);
    void SetSrvGpuDescriptorHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) { m_srvGPUDescriptorHandle = handle; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvDescriptorHandle() const { return m_srvGPUDescriptorHandle; }
    bool HasTexture() const { return m_pd3dTexture != nullptr; }

    // Normal map texture support
    void SetNormalMapName(const std::string& strName) { m_strNormalMapName = strName; }
    void LoadNormalMap(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle);
    void SetNormalMapSrvGpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) { m_normalMapSrvGPUHandle = handle; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetNormalMapSrvHandle() const { return m_normalMapSrvGPUHandle; }
    bool HasNormalMap() const { return m_pd3dNormalMap != nullptr; }

    // Height map texture support
    void SetHeightMapName(const std::string& strName) { m_strHeightMapName = strName; }
    void LoadHeightMap(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle);
    void SetHeightMapSrvGpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) { m_heightMapSrvGPUHandle = handle; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetHeightMapSrvHandle() const { return m_heightMapSrvGPUHandle; }
    bool HasHeightMap() const { return m_pd3dHeightMap != nullptr; }

    // AO map texture support (for stylized water)
    void SetAOMapName(const std::string& strName) { m_strAOMapName = strName; }
    void LoadAOMap(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle);
    void SetAOMapSrvGpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) { m_aoMapSrvGPUHandle = handle; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetAOMapSrvHandle() const { return m_aoMapSrvGPUHandle; }
    bool HasAOMap() const { return m_pd3dAOMap != nullptr; }

    // Roughness map texture support (for stylized water)
    void SetRoughnessMapName(const std::string& strName) { m_strRoughnessMapName = strName; }
    void LoadRoughnessMap(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle);
    void SetRoughnessMapSrvGpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) { m_roughnessMapSrvGPUHandle = handle; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetRoughnessMapSrvHandle() const { return m_roughnessMapSrvGPUHandle; }
    bool HasRoughnessMap() const { return m_pd3dRoughnessMap != nullptr; }

    // Emissive map texture support
    void SetEmissiveTextureName(const std::string& name) { m_strEmissiveTextureName = name; }
    void LoadEmissiveTexture(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle);
    void SetEmissiveSrvGpuDescriptorHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) { m_emissiveSrvGPUDescriptorHandle = handle; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetEmissiveSrvDescriptorHandle() const { return m_emissiveSrvGPUDescriptorHandle; }
    bool HasEmissiveTexture() const { return m_pd3dEmissiveTexture != nullptr; }
    void SetHasEmissiveTexture(bool b)
    {
        if (m_pcbMappedGameObject)
            m_pcbMappedGameObject->m_bHasEmissiveTexture = b ? 1 : 0;
    }

    void SetBoneTransform(int index, const XMFLOAT4X4& matrix)
    {
        if (m_pcbMappedGameObject && index < 128)
        {
            m_pcbMappedGameObject->m_xmf4x4BoneTransforms[index] = matrix;
        }
    }
    void SetSkinned(bool bSkinned)
    {
        if (m_pcbMappedGameObject)
        {
            m_pcbMappedGameObject->m_bIsSkinned = bSkinned ? 1 : 0;
        }
    }
    void SetLava(bool bIsLava)
    {
        if (m_pcbMappedGameObject)
        {
            m_pcbMappedGameObject->m_bIsLava = bIsLava ? 1 : 0;
        }
    }
    void SetWater(bool bIsWater)
    {
        if (m_pcbMappedGameObject)
        {
            m_pcbMappedGameObject->m_bIsWater = bIsWater ? 1 : 0;
        }
    }
    void SetRocky(bool bIsRocky)
    {
        if (m_pcbMappedGameObject)
        {
            m_pcbMappedGameObject->m_bIsRocky = bIsRocky ? 1 : 0;
        }
    }
    void SetGrass(bool bIsGrass)
    {
        if (m_pcbMappedGameObject)
        {
            m_pcbMappedGameObject->m_bIsGrass = bIsGrass ? 1 : 0;
        }
    }
    void SetPortal(bool bIsPortal)
    {
        if (m_pcbMappedGameObject)
        {
            m_pcbMappedGameObject->m_bIsPortal = bIsPortal ? 1 : 0;
        }
    }

    void SetHitFlash(float f)
    {
        if (m_pcbMappedGameObject)
            m_pcbMappedGameObject->m_fHitFlash = f;
    }
    // 계층 전체에 HitFlash 전파 — 자식 메시들이 별도 CB를 써서 루트 호출만으론 안 먹힘
    //   (플레이어/PBR 모델처럼 children 트리가 있는 GameObject 전용)
    void SetHitFlashAll(float f)
    {
        SetHitFlash(f);
        if (m_pChild)   m_pChild->SetHitFlashAll(f);
        if (m_pSibling) m_pSibling->SetHitFlashAll(f);
    }

    void SetStatusColor(const XMFLOAT4& color, float intensity)
    {
        if (m_pcbMappedGameObject)
        {
            m_pcbMappedGameObject->m_vStatusColor     = color;
            m_pcbMappedGameObject->m_fStatusIntensity = intensity;
        }
    }
    void SetStatusColorAll(const XMFLOAT4& color, float intensity)
    {
        SetStatusColor(color, intensity);
        if (m_pChild)   m_pChild->SetStatusColorAll(color, intensity);
        if (m_pSibling) m_pSibling->SetStatusColorAll(color, intensity);
    }

    // Debug: F4 = force all objects to render without texture (see raw geometry/material)
    static bool s_bDebugNoTexture;

	void ReleaseUploadBuffers();

public:
	char			m_pstrFrameName[64];

	GameObject* m_pParent = nullptr;
	GameObject* m_pChild = nullptr;
	GameObject* m_pSibling = nullptr;

private:
	std::vector<std::unique_ptr<Component>> m_vComponents;
	TransformComponent* m_pTransform = nullptr;

	ComPtr<ID3D12Resource> m_pd3dcbGameObject = nullptr;
	ObjectConstants* m_pcbMappedGameObject = nullptr;
	D3D12_GPU_DESCRIPTOR_HANDLE m_cbvGPUDescriptorHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGPUDescriptorHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE m_emissiveSrvGPUDescriptorHandle = {};

	UINT m_nMaterialIndex = 0;
	MATERIAL m_Material;
	std::string m_strTextureName;
    std::string m_strEmissiveTextureName;

	ComPtr<ID3D12Resource> m_pd3dTexture = nullptr;
	ComPtr<ID3D12Resource> m_pd3dTextureUploadBuffer = nullptr;
    ComPtr<ID3D12Resource> m_pd3dEmissiveTexture = nullptr;
    ComPtr<ID3D12Resource> m_pd3dEmissiveTextureUploadBuffer = nullptr;

    // Normal map texture
    std::string m_strNormalMapName;
    ComPtr<ID3D12Resource> m_pd3dNormalMap = nullptr;
    ComPtr<ID3D12Resource> m_pd3dNormalMapUploadBuffer = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE m_normalMapSrvGPUHandle = {};

    // Height map texture
    std::string m_strHeightMapName;
    ComPtr<ID3D12Resource> m_pd3dHeightMap = nullptr;
    ComPtr<ID3D12Resource> m_pd3dHeightMapUploadBuffer = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE m_heightMapSrvGPUHandle = {};

    // AO map texture (for stylized water)
    std::string m_strAOMapName;
    ComPtr<ID3D12Resource> m_pd3dAOMap = nullptr;
    ComPtr<ID3D12Resource> m_pd3dAOMapUploadBuffer = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE m_aoMapSrvGPUHandle = {};

    // Roughness map texture (for stylized water)
    std::string m_strRoughnessMapName;
    ComPtr<ID3D12Resource> m_pd3dRoughnessMap = nullptr;
    ComPtr<ID3D12Resource> m_pd3dRoughnessMapUploadBuffer = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE m_roughnessMapSrvGPUHandle = {};

	Mesh* m_pMesh = nullptr; // Re-added m_pMesh member
};

#include "GameObject.inl"
