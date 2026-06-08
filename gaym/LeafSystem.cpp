#include "stdafx.h"
#include "LeafSystem.h"
#include "DescriptorHeap.h"
#include "WICTextureLoader12.h"
#include "d3dx12.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
    constexpr UINT kCBAlign = 256;
    UINT AlignUp(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }

    // 결정론적 PRNG (frand) — 매 호출 다른 값.
    float frand01()
    {
        static uint32_t seed = 0xCAFEBABE;
        seed = seed * 1664525u + 1013904223u;
        return (float)(seed >> 8) / (float)(1u << 24);
    }
    float frand(float lo, float hi) { return lo + (hi - lo) * frand01(); }
}

LeafSystem::~LeafSystem() = default;

void LeafSystem::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
                      const std::vector<std::wstring>& leafTexturePaths)
{
    m_numTextures = (int)leafTexturePaths.size();
    if (m_numTextures <= 0) return;

    // 자체 SRV heap — slot 0 = instance, slot 1..N = textures
    UINT heapSize = (UINT)(1 + m_numTextures);
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.NumDescriptors = heapSize;
    hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK_HR(pDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_pSrvHeap)));
    m_srvIncr = pDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 텍스처 로드 — 자체 LoadWICTextureFromFile + UpdateSubresources (Dx12App::loadUITex 패턴)
    m_pLeafTexs.resize(m_numTextures);
    m_pLeafUploads.resize(m_numTextures);
    m_nSrvIndices.resize(m_numTextures);

    for (int i = 0; i < m_numTextures; ++i)
    {
        std::unique_ptr<uint8_t[]> decoded;
        D3D12_SUBRESOURCE_DATA sub{};
        HRESULT hr = DirectX::LoadWICTextureFromFile(pDevice,
            leafTexturePaths[i].c_str(),
            m_pLeafTexs[i].ReleaseAndGetAddressOf(),
            decoded, sub);
        if (FAILED(hr))
        {
            OutputDebugStringA("[LeafSystem] 텍스처 로드 실패: ");
            OutputDebugStringW(leafTexturePaths[i].c_str());
            OutputDebugStringA("\n");
            continue;
        }

        // Upload buffer
        UINT64 sz = GetRequiredIntermediateSize(m_pLeafTexs[i].Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(sz);
        CHECK_HR(pDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_pLeafUploads[i])));
        UpdateSubresources(pCmdList, m_pLeafTexs[i].Get(),
                           m_pLeafUploads[i].Get(), 0, 0, 1, &sub);
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pLeafTexs[i].Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        pCmdList->ResourceBarrier(1, &barrier);

        // 자체 heap 슬롯 1+i 에 SRV 등록
        UINT slot = 1 + (UINT)i;
        m_nSrvIndices[i] = slot;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = m_pLeafTexs[i]->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = m_pLeafTexs[i]->GetDesc().MipLevels;

        D3D12_CPU_DESCRIPTOR_HANDLE h = m_pSrvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(slot) * m_srvIncr;
        pDevice->CreateShaderResourceView(m_pLeafTexs[i].Get(), &srv, h);
    }

    CreateRootSignature(pDevice);
    CreatePipelineState(pDevice);
    CreateBuffers(pDevice);

    // Instance buffer SRV (slot 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format                  = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements      = kMaxLeaves;
        sd.Buffer.StructureByteStride = sizeof(LeafGPU);
        pDevice->CreateShaderResourceView(m_pParticleBuf.Get(), &sd,
            m_pSrvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // 잎 배열 초기 spawn
    m_leaves.resize(kMaxLeaves);
    m_gpuBuffer.resize(kMaxLeaves);
    for (auto& leaf : m_leaves)
        RespawnLeaf(leaf, true);
}

void LeafSystem::CreateRootSignature(ID3D12Device* pDevice)
{
    // t0 : instance structured buffer (LeafGPU[])
    // t1 : leaf texture
    // s0 : sampler
    // b0 : pass CB
    CD3DX12_DESCRIPTOR_RANGE1 srvRangeInstance;
    srvRangeInstance.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                          D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
    CD3DX12_DESCRIPTOR_RANGE1 srvRangeTexture;
    srvRangeTexture.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0,
                         D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);

    CD3DX12_ROOT_PARAMETER1 params[4];
    params[0].InitAsConstantBufferView(0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        D3D12_SHADER_VISIBILITY_ALL);
    params[1].InitAsDescriptorTable(1, &srvRangeInstance, D3D12_SHADER_VISIBILITY_VERTEX);
    params[2].InitAsDescriptorTable(1, &srvRangeTexture,  D3D12_SHADER_VISIBILITY_PIXEL);
    // Root constants — baseInstance offset (per-draw)
    params[3].InitAsConstants(4, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);  // 4 int @ b1

    CD3DX12_STATIC_SAMPLER_DESC sampler(
        0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
    desc.Init_1_1(_countof(params), params, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&desc,
        D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); CHECK_HR(hr); }
    CHECK_HR(pDevice->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_pRootSig)));
    m_pRootSig->SetName(L"LeafRootSig");
}

void LeafSystem::CreatePipelineState(ID3D12Device* pDevice)
{
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vs, ps, err;
    auto Compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out)
    {
        HRESULT hr = D3DCompileFromFile(L"leaf_particle.hlsl", nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
            compileFlags, 0, &out, &err);
        if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); CHECK_HR(hr); }
    };
    Compile("VS_Leaf", "vs_5_1", vs);
    Compile("PS_Leaf", "ps_5_1", ps);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_pRootSig.Get();
    desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.BlendState.RenderTarget[0].BlendEnable    = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    desc.DepthStencilState.DepthEnable   = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;   // 깊이 쓰기 X (반투명)
    desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    desc.DepthStencilState.StencilEnable  = FALSE;
    desc.DSVFormat        = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleMask       = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0]    = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.InputLayout = { nullptr, 0 };

    CHECK_HR(pDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pPSO)));
    m_pPSO->SetName(L"LeafPSO");
}

void LeafSystem::CreateBuffers(ID3D12Device* pDevice)
{
    // Instance structured buffer (UPLOAD heap, shader visible SRV)
    UINT bufSize = sizeof(LeafGPU) * kMaxLeaves;
    CD3DX12_HEAP_PROPERTIES upload(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize);
    CHECK_HR(pDevice->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_pParticleBuf)));
    CD3DX12_RANGE noRead(0, 0);
    CHECK_HR(m_pParticleBuf->Map(0, &noRead, reinterpret_cast<void**>(&m_pParticleMapped)));

    // Pass CB (UPLOAD)
    UINT cbSize = AlignUp((UINT)sizeof(PassCB), kCBAlign);
    CD3DX12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
    CHECK_HR(pDevice->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE,
        &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_pPassCB)));
    CHECK_HR(m_pPassCB->Map(0, &noRead, reinterpret_cast<void**>(&m_pCBMapped)));

    // Instance SRV 는 Init() 에서 공용 heap 에 등록함.
}

void LeafSystem::SetSpawnArea(const XMFLOAT3& center, const XMFLOAT3& halfExtents)
{
    m_spawnCenter  = center;
    m_spawnHalfExt = halfExtents;
}

void LeafSystem::SetWind(const XMFLOAT3& dir, float strength)
{
    m_windDir = dir;
    m_windStrength = strength;
}

void LeafSystem::SetEnabled(bool b)
{
    if (m_bEnabled == b) return;
    m_bEnabled = b;
    if (b)
    {
        // 활성화 직후 모든 잎 즉시 재 spawn
        for (auto& leaf : m_leaves) RespawnLeaf(leaf, true);
    }
}

void LeafSystem::RespawnLeaf(Leaf& leaf, bool initialSpawn)
{
    leaf.pos.x = m_spawnCenter.x + frand(-m_spawnHalfExt.x, m_spawnHalfExt.x);
    if (initialSpawn)
        leaf.pos.y = m_spawnCenter.y + frand(0.0f, m_spawnHalfExt.y * 2.0f);
    else
        leaf.pos.y = m_spawnCenter.y + m_spawnHalfExt.y * 1.5f;
    leaf.pos.z = m_spawnCenter.z + frand(-m_spawnHalfExt.z, m_spawnHalfExt.z);

    // 기본 drift — 약한 바람 따라가는 속도. swirl 이 진짜 움직임 만듦.
    leaf.baseVel.x = m_windDir.x * m_windStrength * 0.5f;
    leaf.baseVel.y = -frand(2.5f, 4.5f);                           // 1.5~2.8 → 2.5~4.5 (살짝 더 빠르게)
    leaf.baseVel.z = m_windDir.z * m_windStrength * 0.5f;

    leaf.size        = frand(0.9f, 2.0f);
    leaf.maxLifetime = frand(8.0f, 14.0f);
    leaf.lifetime    = leaf.maxLifetime;
    leaf.color       = { 1.0f, 1.0f, 1.0f, 1.0f };
    leaf.texIdx      = (m_numTextures > 0) ? (int)(frand01() * m_numTextures) % m_numTextures : 0;

    // ── 입자별 swirl/회전 파라미터 — 다양성 ────────────────────────────────
    leaf.swirlPhase   = frand(0.0f, 6.2832f);
    leaf.swirlFreqXZ  = frand(0.6f, 1.4f);                         // 좌우 회오리 주기 다양
    leaf.swirlFreqY   = frand(1.3f, 2.4f);                         // 위/아래 떨림 빠르게
    leaf.swirlAmpXZ   = frand(3.5f, 6.5f);                         // 좌우 진폭 — 더 격하게 (사용자 요청)
    leaf.yawAng       = frand(0.0f, 6.2832f);
    leaf.yawVel       = frand(-3.0f, 3.0f);                         // 입자별 다른 회전 속도 (±)
    leaf.pitchPhase   = frand(0.0f, 6.2832f);
}

void LeafSystem::Update(float rawDt)
{
    if (!m_bEnabled) return;
    if (m_leaves.empty()) return;

    for (auto& leaf : m_leaves)
    {
        // ── 자연스러운 swirl ─────────────────────────────────────────────────
        //   base drift + perpendicular sin 진동 (좌우/위아래 살랑살랑) + 살짝 노이즈.
        leaf.swirlPhase += rawDt;

        float swirlX = std::sin(leaf.swirlPhase * leaf.swirlFreqXZ) * leaf.swirlAmpXZ;
        float swirlZ = std::cos(leaf.swirlPhase * leaf.swirlFreqXZ * 0.85f + 1.7f) * leaf.swirlAmpXZ;
        // 위/아래 떨림 — 낙하 속도에 변동 줘서 일정하지 않게
        float wobbleY = std::sin(leaf.swirlPhase * leaf.swirlFreqY) * 0.8f;

        leaf.pos.x += (leaf.baseVel.x + swirlX) * rawDt;
        leaf.pos.y += (leaf.baseVel.y + wobbleY) * rawDt;
        leaf.pos.z += (leaf.baseVel.z + swirlZ) * rawDt;

        // ── 회전 누적 ────────────────────────────────────────────────────────
        // yaw 는 일정 angular vel 누적. swirl 진폭에 따라 살짝 가속/감속 — 휙휙 회전.
        float yawAccel = 0.6f * std::cos(leaf.swirlPhase * 1.7f);   // ±0.6 rad/s² 변동
        leaf.yawAng += (leaf.yawVel + yawAccel) * rawDt;

        leaf.lifetime -= rawDt;

        float groundY = m_spawnCenter.y - m_spawnHalfExt.y;
        bool outOfRange = (leaf.lifetime <= 0.0f) ||
                          (leaf.pos.y < groundY) ||
                          (std::abs(leaf.pos.x - m_spawnCenter.x) > m_spawnHalfExt.x * 1.3f) ||
                          (std::abs(leaf.pos.z - m_spawnCenter.z) > m_spawnHalfExt.z * 1.3f);
        if (outOfRange)
            RespawnLeaf(leaf, false);
    }
}

void LeafSystem::Render(ID3D12GraphicsCommandList* pCmdList,
                         const XMFLOAT4X4& viewProj,
                         const XMFLOAT3& camRight,
                         const XMFLOAT3& camUp,
                         float time)
{
    if (!m_bEnabled || m_leaves.empty() || m_numTextures <= 0) return;

    // ── 한 buffer 에 텍스처별 입자 stack 저장 + offset 으로 그리기 ─────────
    //   이전 방식 (buffer 마다 memcpy 후 draw) 은 GPU batch 실행 시 마지막 데이터만
    //   사용되어 잎이 겹쳐 보임. 모든 텍스처 입자를 한 번에 memcpy 후
    //   StartInstanceLocation 으로 sub-range 그리기.
    int countByTex[8] = {};
    int offsetByTex[8] = {};
    int totalCount = 0;

    // 1) 텍스처별로 count 계산 → offset 결정
    for (int t = 0; t < m_numTextures; ++t)
    {
        offsetByTex[t] = totalCount;
        int n = 0;
        for (auto& leaf : m_leaves)
            if (leaf.texIdx == t) ++n;
        if (totalCount + n > kMaxLeaves) n = kMaxLeaves - totalCount;
        countByTex[t] = n;
        totalCount += n;
    }
    if (totalCount == 0) return;

    // 2) buffer 채우기 — 텍스처별 순차 stack
    int writeIdx = 0;
    for (int t = 0; t < m_numTextures; ++t)
    {
        for (auto& leaf : m_leaves)
        {
            if (leaf.texIdx != t) continue;
            if (writeIdx >= kMaxLeaves) break;
            LeafGPU& g = m_gpuBuffer[writeIdx];
            g.position = leaf.pos;
            g.size     = leaf.size;
            g.color    = leaf.color;
            g.yawAng   = leaf.yawAng;
            g.pitchAng = std::sin(leaf.swirlPhase * 1.3f + leaf.pitchPhase) * 0.45f;
            g.pad0 = g.pad1 = 0;
            ++writeIdx;
        }
    }
    memcpy(m_pParticleMapped, m_gpuBuffer.data(), sizeof(LeafGPU) * totalCount);

    // 3) pass CB (한 번만)
    PassCB cb{};
    cb.viewProj = viewProj;
    cb.camRight = camRight;
    cb.camUp    = camUp;
    cb.time     = time;
    cb.rotSpeed = 2.4f;
    cb.swayAmp  = 0.40f;
    cb.swayFreq = 1.8f;
    memcpy(m_pCBMapped, &cb, sizeof(cb));

    // 4) 공통 bindings (한 번만)
    pCmdList->SetGraphicsRootSignature(m_pRootSig.Get());
    pCmdList->SetPipelineState(m_pPSO.Get());
    ID3D12DescriptorHeap* heaps[] = { m_pSrvHeap.Get() };
    pCmdList->SetDescriptorHeaps(1, heaps);
    pCmdList->SetGraphicsRootConstantBufferView(0, m_pPassCB->GetGPUVirtualAddress());
    pCmdList->SetGraphicsRootDescriptorTable(1,
        m_pSrvHeap->GetGPUDescriptorHandleForHeapStart());
    pCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // 5) 텍스처별 draw — root constant 로 baseInstance offset 전달
    for (int t = 0; t < m_numTextures; ++t)
    {
        if (countByTex[t] == 0) continue;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuTex =
            m_pSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpuTex.ptr += SIZE_T(m_nSrvIndices[t]) * m_srvIncr;
        pCmdList->SetGraphicsRootDescriptorTable(2, gpuTex);
        int baseInst = offsetByTex[t];
        pCmdList->SetGraphicsRoot32BitConstants(3, 1, &baseInst, 0);
        pCmdList->DrawInstanced(4, (UINT)countByTex[t], 0, 0);
    }
}
