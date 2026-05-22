#include "stdafx.h"
#include "GameObject.h"
#include "Component.h"
#include "TransformComponent.h"
#include "WICTextureLoader12.h"
#include "D3dx12.h"
#include <map>

// GPU 텍스처 캐시 — 맵 전환 시 동일 파일을 재업로드하지 않도록 유지
static std::map<std::wstring, ComPtr<ID3D12Resource>> s_textureCache;

bool GameObject::s_bDebugNoTexture = false;

GameObject::GameObject()
    : m_Material({XMFLOAT4(0.1f, 0.1f, 0.1f, 1.0f), // Ambient
                  XMFLOAT4(0.7f, 0.7f, 0.7f, 1.0f), // Diffuse
                  XMFLOAT4(1.0f, 1.0f, 1.0f, 10.0f), // Specular (power 10.0f in alpha)
                  XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f)}) // Emissive
{
    m_pTransform = AddComponent<TransformComponent>();
}

GameObject::~GameObject()
{
    if (m_pd3dcbGameObject) m_pd3dcbGameObject->Unmap(0, NULL);
}

void GameObject::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList)
{
    for (auto& component : m_vComponents)
    {
        component->Init(pDevice, pCommandList);
    }
}

void GameObject::Update(float deltaTime)
{
    for (auto& component : m_vComponents)
    {
        component->Update(deltaTime);
    }

    // Update constant buffer
    if (m_pcbMappedGameObject)
    {
        XMMATRIX worldMatrix = XMLoadFloat4x4(&m_pTransform->GetWorldMatrix());
        XMStoreFloat4x4(&m_pcbMappedGameObject->m_xmf4x4World, XMMatrixTranspose(worldMatrix));
        m_pcbMappedGameObject->m_nMaterialIndex = m_nMaterialIndex;
        m_pcbMappedGameObject->mMaterial = m_Material;
        m_pcbMappedGameObject->m_bHasTexture = (HasTexture() && !s_bDebugNoTexture) ? 1 : 0;
    }

    // Recurse for children and siblings
    if (m_pChild) m_pChild->Update(deltaTime);
    if (m_pSibling) m_pSibling->Update(deltaTime);
}

void GameObject::Render(ID3D12GraphicsCommandList* pCommandList)
{
    // Set the descriptor table for this object
    pCommandList->SetGraphicsRootDescriptorTable(0, m_cbvGPUDescriptorHandle);

    if (HasTexture())
    {
        pCommandList->SetGraphicsRootDescriptorTable(2, m_srvGPUDescriptorHandle);
    }

    for (auto& component : m_vComponents)
    {
        component->Render(pCommandList);
    }

    if (m_pMesh) m_pMesh->Render(pCommandList, 0);

	if (m_pSibling) m_pSibling->Render(pCommandList);
	if (m_pChild) m_pChild->Render(pCommandList);
}

void GameObject::SetMesh(Mesh* pMesh)
{
	if (m_pMesh == pMesh) return;  // same pointer: avoid Release() + AddRef() on same object (would delete it)
	if (m_pMesh) m_pMesh->Release();
	m_pMesh = pMesh;
	if (m_pMesh) m_pMesh->AddRef();
}

void GameObject::SetChild(GameObject* pChild)
{
    if (pChild)
    {
        pChild->m_pParent = this;

        if (m_pChild) // If this object already has a first child
        {
            // Find the last sibling in the current child list
            GameObject* pSibling = m_pChild;
            while (pSibling->m_pSibling)
            {
                pSibling = pSibling->m_pSibling;
            }
            // Add the new child as the next sibling
            pSibling->m_pSibling = pChild;
        }
        else // This is the first child
        {
            m_pChild = pChild;
        }
    }
}

void GameObject::SetTransform(const XMFLOAT4X4& transform)
{
	GetTransform()->SetLocalMatrix(transform);
}

void GameObject::SetMaterial(const MATERIAL& material)
{
    m_Material = material;
}

void GameObject::CreateConstantBuffer(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, UINT nBufferSize, D3D12_CPU_DESCRIPTOR_HANDLE d3dCbvCPUDescriptorHandle)
{
    UINT nConstantBufferSize = (sizeof(ObjectConstants) + 255) & ~255; // Align to 256 bytes
    m_pd3dcbGameObject = CreateBufferResource(pDevice, pCommandList, NULL, nConstantBufferSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);

    m_pd3dcbGameObject->Map(0, NULL, (void**)&m_pcbMappedGameObject);

    // D3D12 upload heap 메모리는 zero-init 보장되지 않음 — 이전에 누가 쓰던 메모리가
    // 재할당되면 bIsSkinned/bIsLava/bIsWater 등 flag 가 1 로 남아 shader 가 엉뚱하게 동작.
    // 예: bIsSkinned=1 인데 boneWeight 없는 static mesh → VS 가 모든 정점을 원점으로 collapse
    // → 화면에 안 보임. ReuseConstantBuffer 는 이미 ZeroMemory 하지만 첫 할당은 안 했음.
    if (m_pcbMappedGameObject)
        ZeroMemory(m_pcbMappedGameObject, nConstantBufferSize);

    D3D12_CONSTANT_BUFFER_VIEW_DESC d3dcbvDesc;
    d3dcbvDesc.BufferLocation = m_pd3dcbGameObject->GetGPUVirtualAddress();
    d3dcbvDesc.SizeInBytes = nConstantBufferSize;
    pDevice->CreateConstantBufferView(&d3dcbvDesc, d3dCbvCPUDescriptorHandle);
}

void GameObject::LoadTexture(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle)
{
    if (m_strTextureName.empty()) return;

    // Convert .tga extension to .png (TGA files have been converted to PNG)
    std::string textureName = m_strTextureName;
    size_t tgaPos = textureName.rfind(".tga");
    if (tgaPos != std::string::npos)
    {
        textureName.replace(tgaPos, 4, ".png");
    }

    // Try multiple texture paths
    std::vector<std::string> searchPaths = {
        "Assets/Player/Textures/" + textureName,
        "Assets/Enemies/AirElemental/Textures/" + textureName,
        "Assets/Enemies/Dragon/Textures/" + textureName,
        "Assets/Textures/" + textureName,
        "Animation/" + textureName,  // Legacy path
        textureName  // Direct path
    };

    std::wstring wstrTextureName;
    bool found = false;
    for (const auto& path : searchPaths)
    {
        std::wstring wpath(path.begin(), path.end());
        if (GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            wstrTextureName = wpath;
            found = true;
            break;
        }
    }

    if (!found)
    {
        char buffer[256];
        sprintf_s(buffer, "Texture not found in any search path: %s\n", textureName.c_str());
        OutputDebugStringA(buffer);
        return;
    }

    // 캐시에 이미 있으면 디스크 I/O + GPU 업로드 없이 재사용
    auto cacheIt = s_textureCache.find(wstrTextureName);
    if (cacheIt != s_textureCache.end())
    {
        m_pd3dTexture = cacheIt->second;
    }
    else
    {
        std::unique_ptr<uint8_t[]> decodedData;
        D3D12_SUBRESOURCE_DATA subresource;

        HRESULT hr = DirectX::LoadWICTextureFromFile(pd3dDevice, wstrTextureName.c_str(), m_pd3dTexture.GetAddressOf(), decodedData, subresource);
        if (FAILED(hr))
        {
            char buffer[512];
            sprintf_s(buffer, "Failed to load texture: %ls\n", wstrTextureName.c_str());
            OutputDebugStringA(buffer);
            return;
        }

        UINT64 nBytes = GetRequiredIntermediateSize(m_pd3dTexture.Get(), 0, 1);
        m_pd3dTextureUploadBuffer = CreateBufferResource(pd3dDevice, pd3dCommandList, NULL, nBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
        UpdateSubresources(pd3dCommandList, m_pd3dTexture.Get(), m_pd3dTextureUploadBuffer.Get(), 0, 0, 1, &subresource);

        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pd3dTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        pd3dCommandList->ResourceBarrier(1, &barrier);

        // 캐시에 등록 (이후 전환에서 재사용)
        s_textureCache[wstrTextureName] = m_pd3dTexture;
    }

    // SRV는 새 디스크립터 슬롯에 항상 새로 생성 (슬롯이 매번 달라지므로)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = m_pd3dTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = m_pd3dTexture->GetDesc().MipLevels;
    pd3dDevice->CreateShaderResourceView(m_pd3dTexture.Get(), &srvDesc, srvCpuHandle);
}

void GameObject::LoadEmissiveTexture(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle)
{
    if (m_strEmissiveTextureName.empty()) return;

    std::wstring wstrPath(m_strEmissiveTextureName.begin(), m_strEmissiveTextureName.end());

    auto cacheIt = s_textureCache.find(wstrPath);
    if (cacheIt != s_textureCache.end())
    {
        m_pd3dEmissiveTexture = cacheIt->second;
    }
    else
    {
        std::unique_ptr<uint8_t[]> decodedData;
        D3D12_SUBRESOURCE_DATA subresource;
        HRESULT hr = DirectX::LoadWICTextureFromFile(pd3dDevice, wstrPath.c_str(), m_pd3dEmissiveTexture.GetAddressOf(), decodedData, subresource);
        if (FAILED(hr))
        {
            char buf[512];
            sprintf_s(buf, "[GameObject] Emissive texture not found: %ls\n", wstrPath.c_str());
            OutputDebugStringA(buf);
            return;
        }

        UINT64 nBytes = GetRequiredIntermediateSize(m_pd3dEmissiveTexture.Get(), 0, 1);
        m_pd3dEmissiveTextureUploadBuffer = CreateBufferResource(pd3dDevice, pd3dCommandList, NULL, nBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
        UpdateSubresources(pd3dCommandList, m_pd3dEmissiveTexture.Get(), m_pd3dEmissiveTextureUploadBuffer.Get(), 0, 0, 1, &subresource);

        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pd3dEmissiveTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        pd3dCommandList->ResourceBarrier(1, &barrier);

        s_textureCache[wstrPath] = m_pd3dEmissiveTexture;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = m_pd3dEmissiveTexture->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = m_pd3dEmissiveTexture->GetDesc().MipLevels;
    pd3dDevice->CreateShaderResourceView(m_pd3dEmissiveTexture.Get(), &srvDesc, srvCpuHandle);

    if (m_pcbMappedGameObject)
        m_pcbMappedGameObject->m_bHasEmissiveTexture = 1;
}

void GameObject::ReleaseUploadBuffers()
{
    if (m_pd3dTextureUploadBuffer) m_pd3dTextureUploadBuffer = nullptr;
    if (m_pd3dEmissiveTextureUploadBuffer) m_pd3dEmissiveTextureUploadBuffer = nullptr;
    if (m_pd3dNormalMapUploadBuffer) m_pd3dNormalMapUploadBuffer = nullptr;
    if (m_pd3dHeightMapUploadBuffer) m_pd3dHeightMapUploadBuffer = nullptr;
    if (m_pd3dAOMapUploadBuffer) m_pd3dAOMapUploadBuffer = nullptr;
    if (m_pd3dRoughnessMapUploadBuffer) m_pd3dRoughnessMapUploadBuffer = nullptr;
}

void GameObject::LoadNormalMap(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle)
{
    if (m_strNormalMapName.empty()) return;

    std::wstring wstrNormalMap(m_strNormalMapName.begin(), m_strNormalMapName.end());

    // Check if file exists
    if (GetFileAttributesW(wstrNormalMap.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        char buffer[256];
        sprintf_s(buffer, "Normal map not found: %s\n", m_strNormalMapName.c_str());
        OutputDebugStringA(buffer);
        return;
    }

    // Load texture
    std::unique_ptr<uint8_t[]> decodedData;
    D3D12_SUBRESOURCE_DATA subresource;

    HRESULT hr = DirectX::LoadWICTextureFromFile(pd3dDevice, wstrNormalMap.c_str(), m_pd3dNormalMap.GetAddressOf(), decodedData, subresource);
    if (FAILED(hr))
    {
        char buffer[512];
        sprintf_s(buffer, "Failed to load normal map: %ls\n", wstrNormalMap.c_str());
        OutputDebugStringA(buffer);
        return;
    }

    UINT64 nBytes = GetRequiredIntermediateSize(m_pd3dNormalMap.Get(), 0, 1);
    m_pd3dNormalMapUploadBuffer = CreateBufferResource(pd3dDevice, pd3dCommandList, NULL, nBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
    UpdateSubresources(pd3dCommandList, m_pd3dNormalMap.Get(), m_pd3dNormalMapUploadBuffer.Get(), 0, 0, 1, &subresource);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pd3dNormalMap.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pd3dCommandList->ResourceBarrier(1, &barrier);

    // Create SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = m_pd3dNormalMap->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = m_pd3dNormalMap->GetDesc().MipLevels;
    pd3dDevice->CreateShaderResourceView(m_pd3dNormalMap.Get(), &srvDesc, srvCpuHandle);

    OutputDebugStringA("Normal map loaded successfully\n");
}

void GameObject::LoadHeightMap(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle)
{
    if (m_strHeightMapName.empty()) return;

    std::wstring wstrHeightMap(m_strHeightMapName.begin(), m_strHeightMapName.end());

    // Check if file exists
    if (GetFileAttributesW(wstrHeightMap.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        char buffer[256];
        sprintf_s(buffer, "Height map not found: %s\n", m_strHeightMapName.c_str());
        OutputDebugStringA(buffer);
        return;
    }

    // Load texture
    std::unique_ptr<uint8_t[]> decodedData;
    D3D12_SUBRESOURCE_DATA subresource;

    HRESULT hr = DirectX::LoadWICTextureFromFile(pd3dDevice, wstrHeightMap.c_str(), m_pd3dHeightMap.GetAddressOf(), decodedData, subresource);
    if (FAILED(hr))
    {
        char buffer[512];
        sprintf_s(buffer, "Failed to load height map: %ls\n", wstrHeightMap.c_str());
        OutputDebugStringA(buffer);
        return;
    }

    UINT64 nBytes = GetRequiredIntermediateSize(m_pd3dHeightMap.Get(), 0, 1);
    m_pd3dHeightMapUploadBuffer = CreateBufferResource(pd3dDevice, pd3dCommandList, NULL, nBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
    UpdateSubresources(pd3dCommandList, m_pd3dHeightMap.Get(), m_pd3dHeightMapUploadBuffer.Get(), 0, 0, 1, &subresource);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pd3dHeightMap.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pd3dCommandList->ResourceBarrier(1, &barrier);

    // Create SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = m_pd3dHeightMap->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = m_pd3dHeightMap->GetDesc().MipLevels;
    pd3dDevice->CreateShaderResourceView(m_pd3dHeightMap.Get(), &srvDesc, srvCpuHandle);

    OutputDebugStringA("Height map loaded successfully\n");
}

void GameObject::LoadAOMap(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle)
{
    if (m_strAOMapName.empty()) return;

    std::wstring wstrAOMap(m_strAOMapName.begin(), m_strAOMapName.end());

    if (GetFileAttributesW(wstrAOMap.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        char buffer[256];
        sprintf_s(buffer, "AO map not found: %s\n", m_strAOMapName.c_str());
        OutputDebugStringA(buffer);
        return;
    }

    std::unique_ptr<uint8_t[]> decodedData;
    D3D12_SUBRESOURCE_DATA subresource;

    HRESULT hr = DirectX::LoadWICTextureFromFile(pd3dDevice, wstrAOMap.c_str(), m_pd3dAOMap.GetAddressOf(), decodedData, subresource);
    if (FAILED(hr))
    {
        char buffer[512];
        sprintf_s(buffer, "Failed to load AO map: %ls\n", wstrAOMap.c_str());
        OutputDebugStringA(buffer);
        return;
    }

    UINT64 nBytes = GetRequiredIntermediateSize(m_pd3dAOMap.Get(), 0, 1);
    m_pd3dAOMapUploadBuffer = CreateBufferResource(pd3dDevice, pd3dCommandList, NULL, nBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
    UpdateSubresources(pd3dCommandList, m_pd3dAOMap.Get(), m_pd3dAOMapUploadBuffer.Get(), 0, 0, 1, &subresource);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pd3dAOMap.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pd3dCommandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = m_pd3dAOMap->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = m_pd3dAOMap->GetDesc().MipLevels;
    pd3dDevice->CreateShaderResourceView(m_pd3dAOMap.Get(), &srvDesc, srvCpuHandle);

    OutputDebugStringA("AO map loaded successfully\n");
}

void GameObject::LoadRoughnessMap(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle)
{
    if (m_strRoughnessMapName.empty()) return;

    std::wstring wstrRoughnessMap(m_strRoughnessMapName.begin(), m_strRoughnessMapName.end());

    if (GetFileAttributesW(wstrRoughnessMap.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        char buffer[256];
        sprintf_s(buffer, "Roughness map not found: %s\n", m_strRoughnessMapName.c_str());
        OutputDebugStringA(buffer);
        return;
    }

    std::unique_ptr<uint8_t[]> decodedData;
    D3D12_SUBRESOURCE_DATA subresource;

    HRESULT hr = DirectX::LoadWICTextureFromFile(pd3dDevice, wstrRoughnessMap.c_str(), m_pd3dRoughnessMap.GetAddressOf(), decodedData, subresource);
    if (FAILED(hr))
    {
        char buffer[512];
        sprintf_s(buffer, "Failed to load Roughness map: %ls\n", wstrRoughnessMap.c_str());
        OutputDebugStringA(buffer);
        return;
    }

    UINT64 nBytes = GetRequiredIntermediateSize(m_pd3dRoughnessMap.Get(), 0, 1);
    m_pd3dRoughnessMapUploadBuffer = CreateBufferResource(pd3dDevice, pd3dCommandList, NULL, nBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
    UpdateSubresources(pd3dCommandList, m_pd3dRoughnessMap.Get(), m_pd3dRoughnessMapUploadBuffer.Get(), 0, 0, 1, &subresource);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pd3dRoughnessMap.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pd3dCommandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = m_pd3dRoughnessMap->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = m_pd3dRoughnessMap->GetDesc().MipLevels;
    pd3dDevice->CreateShaderResourceView(m_pd3dRoughnessMap.Get(), &srvDesc, srvCpuHandle);

    OutputDebugStringA("Roughness map loaded successfully\n");
}
