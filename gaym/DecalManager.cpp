#include "stdafx.h"
#include "d3dx12.h"
#include "DecalManager.h"
#include "GameObject.h"
#include "Mesh.h"
#include "Shader.h"
#include "DescriptorHeap.h"
#include "Dx12App.h"
#include "WICTextureLoader12.h"

using namespace DirectX;

static ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* pDevice, UINT64 byteSize)
{
    D3D12_HEAP_PROPERTIES hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC   rd = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
    ComPtr<ID3D12Resource> buf;
    pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
    return buf;
}

void DecalManager::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
                        CDescriptorHeap* pHeap, UINT& nNextIndex, Shader* pShader)
{
    m_pDescHeap = pHeap;
    m_pPSO      = pShader->GetWaterPSO();
    m_pRootSig  = pShader->GetRootSignature();

    // Constant buffer: MAX_DECALS slots × kCBStride bytes
    UINT64 cbTotalSize = (UINT64)MAX_DECALS * kCBStride;
    m_pCB = CreateUploadBuffer(pDevice, cbTotalSize);
    m_pCB->Map(0, nullptr, (void**)&m_pMappedCB);
    memset(m_pMappedCB, 0, cbTotalSize);

    // Create one CBV per slot
    m_nCBVStart = nNextIndex;
    for (int i = 0; i < MAX_DECALS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
        desc.BufferLocation = m_pCB->GetGPUVirtualAddress() + (UINT64)i * kCBStride;
        desc.SizeInBytes    = kCBStride;
        pDevice->CreateConstantBufferView(&desc, pHeap->GetCPUHandle(nNextIndex + i));
    }
    nNextIndex += MAX_DECALS;

    // Shared quad mesh (1×1 flat on XZ plane)
    m_pQuad = std::make_unique<QuadMesh>(pDevice, pCmdList);
}

void DecalManager::LoadTexture(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
                               CDescriptorHeap* pHeap, UINT& nNextIndex,
                               DecalTexture type, const wchar_t* pPath)
{
    int idx = (int)type;
    if (idx < 0 || idx >= 4) return;
    TexSlot& slot = m_texSlots[idx];

    std::unique_ptr<uint8_t[]> decodedData;
    D3D12_SUBRESOURCE_DATA subresource{};
    HRESULT hr = DirectX::LoadWICTextureFromFile(pDevice, pPath,
        slot.resource.GetAddressOf(), decodedData, subresource);
    if (FAILED(hr))
    {
        char buf[512];
        sprintf_s(buf, "[DecalManager] Texture not found: %ls\n", pPath);
        OutputDebugStringA(buf);
        return;
    }

    UINT64 nBytes = GetRequiredIntermediateSize(slot.resource.Get(), 0, 1);
    slot.upload = CreateUploadBuffer(pDevice, nBytes);
    UpdateSubresources(pCmdList, slot.resource.Get(), slot.upload.Get(), 0, 0, 1, &subresource);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        slot.resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pCmdList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = slot.resource->GetDesc().Format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = slot.resource->GetDesc().MipLevels;
    pDevice->CreateShaderResourceView(slot.resource.Get(), &srvDesc,
        pHeap->GetCPUHandle(nNextIndex));
    slot.srvGpu = pHeap->GetGPUHandle(nNextIndex);
    ++nNextIndex;

    slot.loaded = true;
}

void DecalManager::Spawn(DecalTexture tex, const XMFLOAT3& pos,
                         float size, float rotY, float lifetime)
{
    int target = -1;
    float oldest = FLT_MAX;

    // 빈 슬롯 우선, 없으면 가장 오래된 슬롯 교체
    for (int i = 0; i < MAX_DECALS; ++i)
    {
        if (!m_pool[i].active) { target = i; break; }
        if (m_pool[i].spawnTime < oldest) { oldest = m_pool[i].spawnTime; target = i; }
    }

    DecalEntry& d = m_pool[target];
    d.pos       = pos;
    d.size      = size;
    d.rotY      = rotY;
    d.lifeMax   = lifetime;
    d.lifeRemain= lifetime;
    d.spawnTime = m_totalTime;
    d.tex       = tex;
    d.active    = true;
}

void DecalManager::Update(float dt)
{
    m_totalTime += dt;
    for (auto& d : m_pool)
    {
        if (!d.active) continue;
        d.lifeRemain -= dt;
        if (d.lifeRemain <= 0.f) d.active = false;
    }
}

void DecalManager::Render(ID3D12GraphicsCommandList* pCmdList,
                          D3D12_GPU_VIRTUAL_ADDRESS passCBGpuAddr)
{
    if (!m_pPSO || !m_pRootSig) return;

    pCmdList->SetGraphicsRootSignature(m_pRootSig);
    pCmdList->SetPipelineState(m_pPSO);
    pCmdList->SetGraphicsRootConstantBufferView(1, passCBGpuAddr);

    for (int i = 0; i < MAX_DECALS; ++i)
    {
        const DecalEntry& d = m_pool[i];
        if (!d.active) continue;

        int texIdx = (int)d.tex;
        if (texIdx < 0 || texIdx >= 4 || !m_texSlots[texIdx].loaded) continue;

        // World matrix: Scale × RotY × Translate (decal slightly above ground)
        XMMATRIX world = XMMatrixScaling(d.size, 1.f, d.size)
                       * XMMatrixRotationY(d.rotY)
                       * XMMatrixTranslation(d.pos.x, d.pos.y + 0.1f, d.pos.z);

        float fade = d.lifeRemain / d.lifeMax;

        // Write to CB slot (only first 176 bytes; bone transforms already 0)
        BYTE* pSlot = m_pMappedCB + (UINT64)i * kCBStride;
        auto* pCB = reinterpret_cast<ObjectConstants*>(pSlot);

        XMStoreFloat4x4(&pCB->m_xmf4x4World, XMMatrixTranspose(world));
        pCB->m_nMaterialIndex        = 0;
        pCB->m_bIsSkinned            = 0;
        pCB->m_bHasTexture           = 1;
        pCB->m_bIsLava               = 0;
        pCB->m_bIsWater              = 0;
        pCB->m_bHasEmissiveTexture   = 0;
        pCB->m_fHitFlash             = 0.f;
        pCB->m_bIsRocky              = 0;
        pCB->m_bIsGrass              = 0;
        pCB->m_bIsPortal             = 0;
        pCB->m_bIsDecal              = 1;
        pCB->m_grassPad2             = 0;
        pCB->mMaterial.m_cAmbient    = { 0.f, 0.f, 0.f, 0.f };
        pCB->mMaterial.m_cDiffuse    = { 1.f, 1.f, 1.f, fade };
        pCB->mMaterial.m_cSpecular   = { 0.f, 0.f, 0.f, 0.f };
        pCB->mMaterial.m_cEmissive   = { 0.f, 0.f, 0.f, 0.f };

        pCmdList->SetGraphicsRootDescriptorTable(0, m_pDescHeap->GetGPUHandle(m_nCBVStart + i));
        pCmdList->SetGraphicsRootDescriptorTable(2, m_texSlots[texIdx].srvGpu);
        m_pQuad->Render(pCmdList, 0);
    }
}
