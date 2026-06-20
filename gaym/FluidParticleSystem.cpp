#include "stdafx.h"
#include "FluidParticleSystem.h"
#include "ScreenSpaceFluid.h"
#include "DescriptorHeap.h"
#include <cmath>
#include <cstring>
#include <algorithm>

// Static member definitions
ComPtr<ID3D12RootSignature> FluidParticleSystem::s_pRootSignature;
ComPtr<ID3D12PipelineState> FluidParticleSystem::s_pPSO;

// GPU SPH static members
ComPtr<ID3D12RootSignature> FluidParticleSystem::s_pSPHRootSig;
ComPtr<ID3D12PipelineState> FluidParticleSystem::s_pDensityPSO;
ComPtr<ID3D12PipelineState> FluidParticleSystem::s_pForcesPSO;
ComPtr<ID3D12PipelineState> FluidParticleSystem::s_pIntegratePSO;
ComPtr<ID3D12PipelineState> FluidParticleSystem::s_pHashClearPSO;
ComPtr<ID3D12PipelineState> FluidParticleSystem::s_pHashBuildPSO;

// Internal pass CB layout (matches cbFluidPass in inline shader)
struct FluidPassCB
{
    XMFLOAT4X4 viewProj;
    XMFLOAT3   cameraRight; float padR;
    XMFLOAT3   cameraUp;    float padU;
};
static_assert(sizeof(FluidPassCB) == 96, "FluidPassCB size mismatch");

// SSF depth pass CB (SphereDepthCB in shader)
struct FluidSphereDepthCB
{
    XMFLOAT4X4 viewProj;        // 64 bytes (전치됨)
    XMFLOAT4X4 view;            // 64 bytes (전치됨)
    XMFLOAT3   cameraRight;     // 12 bytes
    float      projA;           // 4 bytes
    XMFLOAT3   cameraUp;        // 12 bytes
    float      projB;           // 4 bytes
    float      smoothingRadius; // 4 bytes  (SSF 구체 최소 크기 보장)
    float      _pad[3];         // 12 bytes
    // total: 176 bytes
};

static constexpr float PI_F = 3.14159265358979323846f;

// Inline HLSL shader code for fluid particles
static const char* g_FluidShaderCode = R"(
    cbuffer cbFluidPass : register(b0)
    {
        matrix gViewProj;
        float3 gCameraRight; float gPadR;
        float3 gCameraUp;    float gPadU;
    };

    struct FluidParticleData
    {
        float3 position;
        float  size;
        float4 color;
    };
    StructuredBuffer<FluidParticleData> gParticles : register(t0);

    struct FluidVSOut
    {
        float4 pos   : SV_POSITION;
        float4 color : COLOR0;
        float2 uv    : TEXCOORD0;
    };

    // TRIANGLE_STRIP: 4 vertices per quad
    // 0=BL, 1=BR, 2=TL, 3=TR
    static const float2 kOffsets[4] = {
        { -0.5f, -0.5f },
        {  0.5f, -0.5f },
        { -0.5f,  0.5f },
        {  0.5f,  0.5f }
    };
    static const float2 kUVs[4] = {
        { 0.0f, 1.0f },
        { 1.0f, 1.0f },
        { 0.0f, 0.0f },
        { 1.0f, 0.0f }
    };

    // 빌보드 회전 + 모양 인덱스 패킹: VS 에서 instId hash → PS 로 전달.
    //   FluidVSOut 의 color.a 는 원본 알파가 필요해 따로 못 씀 → uv 의 z 슬롯 (X/Y 한 채널 추가 X)
    //   를 못 쓰므로 color.rgb 강도 유지 + alpha 보존 위해 별도 채널 필요.
    //   해결: FluidVSOut 에 uint shapeId 추가 (TEXCOORD1, nointerpolation).
    struct FluidVSOutEx
    {
        float4 pos      : SV_POSITION;
        float4 color    : COLOR0;
        float2 uv       : TEXCOORD0;
        nointerpolation uint shapeId : TEXCOORD1;
    };

    FluidVSOutEx VS_Fluid(uint vertId : SV_VertexID, uint instId : SV_InstanceID)
    {
        FluidParticleData p = gParticles[instId];

        float2 offset = kOffsets[vertId];
        float2 uv     = kUVs[vertId];

        // Hash instId → 회전각 + shape index. 각 파티클 독립적으로 다양한 모양/방향.
        uint h     = instId * 2654435761u;
        uint shape = (h >> 13) & 0x3;                     // 0~3: 원/잎/마름모/줄기
        float ang  = (float)((h >> 4) & 0xFF) / 255.0f * 6.28318530718f;
        float ca   = cos(ang);
        float sa   = sin(ang);
        float2 rotOff = float2(offset.x * ca - offset.y * sa,
                               offset.x * sa + offset.y * ca);

        float3 worldPos = p.position
            + gCameraRight * rotOff.x * p.size
            + gCameraUp    * rotOff.y * p.size;

        FluidVSOutEx output;
        output.pos     = mul(float4(worldPos, 1.0f), gViewProj);
        output.color   = p.color;
        output.uv      = uv;
        output.shapeId = shape;
        return output;
    }

    // 모양별 마스크 함수.
    float _shape_circle(float2 c) {
        float d = length(c);
        return 1.0f - smoothstep(0.4f, 1.0f, d);
    }
    float _shape_leaf(float2 c) {
        // Vesica piscis (두 원 교집합) → 잎 모양, 양쪽 끝 뾰족.
        float d1 = length(c - float2(0.0f,  0.55f)) - 0.78f;
        float d2 = length(c + float2(0.0f,  0.55f)) - 0.78f;
        float dL = max(d1, d2);
        return 1.0f - smoothstep(-0.15f, 0.05f, dL);
    }
    float _shape_diamond(float2 c) {
        float d = abs(c.x) + abs(c.y);
        return 1.0f - smoothstep(0.55f, 1.05f, d);
    }
    float _shape_streak(float2 c) {
        // 가로 긴 좁은 타원 — 모션 블러 줄기.
        float2 e = float2(c.x * 0.55f, c.y * 2.6f);
        float d = length(e);
        return 1.0f - smoothstep(0.45f, 1.0f, d);
    }

    float4 PS_Fluid(FluidVSOutEx input) : SV_TARGET
    {
        // UV to [-1, 1]
        float2 centered = input.uv * 2.0f - 1.0f;

        // 모양 분기 — VS 에서 결정된 shapeId.
        float alpha;
        if      (input.shapeId == 0) alpha = _shape_circle(centered);
        else if (input.shapeId == 1) alpha = _shape_leaf(centered);
        else if (input.shapeId == 2) alpha = _shape_diamond(centered);
        else                          alpha = _shape_streak(centered);

        clip(alpha - 0.01f);

        // Inner glow boost — 중심부 더 밝게 (기존 룩 유지).
        float d = length(centered);
        float glow = 1.0f - d;
        float3 col = input.color.rgb * (1.0f + glow * 1.5f);

        return float4(col, input.color.a * alpha);
    }
)";

// ============================================================================
// PRNG
// ============================================================================
float FluidParticleSystem::Rand01()
{
    m_Seed ^= m_Seed << 13;
    m_Seed ^= m_Seed >> 17;
    m_Seed ^= m_Seed << 5;
    return (float)(m_Seed & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

float FluidParticleSystem::RandRange(float lo, float hi)
{
    return lo + Rand01() * (hi - lo);
}

XMFLOAT3 FluidParticleSystem::RandInSphere(float radius)
{
    XMFLOAT3 v;
    float r;
    do {
        v = { RandRange(-1, 1), RandRange(-1, 1), RandRange(-1, 1) };
        r = v.x * v.x + v.y * v.y + v.z * v.z;
    } while (r > 1.0f || r < 0.0001f);
    float s = radius * Rand01() / sqrtf(r);
    return { v.x * s, v.y * s, v.z * s };
}

// ============================================================================
// Constructor / Destructor
// ============================================================================
FluidParticleSystem::FluidParticleSystem()
{
    memset(m_HashTable.data(), 0, sizeof(SpatialHashCell) * HASH_TABLE_SIZE);
}

FluidParticleSystem::~FluidParticleSystem()
{
    if (m_pParticleBuffer && m_pMappedParticles)
    {
        m_pParticleBuffer->Unmap(0, nullptr);
        m_pMappedParticles = nullptr;
    }
    if (m_pPassCB && m_pMappedPassCB)
    {
        m_pPassCB->Unmap(0, nullptr);
        m_pMappedPassCB = nullptr;
    }
    if (m_pDepthPassCB && m_pMappedDepthPassCB)
    {
        m_pDepthPassCB->Unmap(0, nullptr);
        m_pMappedDepthPassCB = nullptr;
    }
    // GPU SPH 리소스 해제
    if (m_pSPHCB && m_pMappedSPHCB)
    {
        m_pSPHCB->Unmap(0, nullptr);
        m_pMappedSPHCB = nullptr;
    }
    if (m_pCPBuffer && m_pMappedCPBuffer)
    {
        m_pCPBuffer->Unmap(0, nullptr);
        m_pMappedCPBuffer = nullptr;
    }
}

// ============================================================================
// Init
// ============================================================================
void FluidParticleSystem::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* /*pCommandList*/,
                               CDescriptorHeap* pDescriptorHeap, UINT nSrvDescriptorIndex)
{
    m_pDescriptorHeap     = pDescriptorHeap;
    m_nSrvDescriptorIndex = nSrvDescriptorIndex;

    // 1. Particle upload buffer (StructuredBuffer source)
    UINT64 bufferSize = sizeof(FluidParticleRenderData) * MAX_PARTICLES;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width              = bufferSize;
    bufDesc.Height             = 1;
    bufDesc.DepthOrArraySize   = 1;
    bufDesc.MipLevels          = 1;
    bufDesc.Format             = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count   = 1;
    bufDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = pDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_pParticleBuffer));
    if (FAILED(hr)) { OutputDebugStringA("[FluidPS] Failed to create particle buffer\n"); return; }

    m_pParticleBuffer->Map(0, nullptr, (void**)&m_pMappedParticles);

    // 2. SRV for StructuredBuffer
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                     = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement        = 0;
    srvDesc.Buffer.NumElements         = MAX_PARTICLES;
    srvDesc.Buffer.StructureByteStride = sizeof(FluidParticleRenderData);
    srvDesc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle = pDescriptorHeap->GetCPUHandle(nSrvDescriptorIndex);
    pDevice->CreateShaderResourceView(m_pParticleBuffer.Get(), &srvDesc, srvCpuHandle);

    // 3. Small pass CB (ViewProj + CameraRight + CameraUp) - 256 bytes aligned
    UINT passCBSize = (sizeof(FluidPassCB) + 255) & ~255;
    D3D12_RESOURCE_DESC cbDesc = bufDesc;
    cbDesc.Width = passCBSize;

    hr = pDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_pPassCB));
    if (FAILED(hr)) { OutputDebugStringA("[FluidPS] Failed to create pass CB\n"); return; }
    m_pPassCB->Map(0, nullptr, &m_pMappedPassCB);

    // 3b. SSF depth pass CB (FluidSphereDepthCB, 256바이트 정렬)
    {
        UINT depthCBSize = (sizeof(FluidSphereDepthCB) + 255) & ~255;
        D3D12_RESOURCE_DESC depthCBDesc = cbDesc;
        depthCBDesc.Width = depthCBSize;

        hr = pDevice->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &depthCBDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_pDepthPassCB));
        if (FAILED(hr)) { OutputDebugStringA("[FluidPS] SSF depth CB 생성 실패\n"); }
        else { m_pDepthPassCB->Map(0, nullptr, &m_pMappedDepthPassCB); }
    }

    // 4-6. Build shared pipeline (root signature, shaders, PSO) - once for all instances
    BuildSharedPipeline(pDevice);

    // ============================================================================
    // GPU SPH 리소스 생성
    // ============================================================================
    {
        D3D12_HEAP_PROPERTIES defaultHeapProps = {};
        defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        // m_pGPUStateBuffer: DEFAULT, UAV (GPUParticle x MAX_PARTICLES)
        {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width              = sizeof(GPUParticle) * MAX_PARTICLES;
            desc.Height             = 1;
            desc.DepthOrArraySize   = 1;
            desc.MipLevels          = 1;
            desc.Format             = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count   = 1;
            desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            hr = pDevice->CreateCommittedResource(
                &defaultHeapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(&m_pGPUStateBuffer));
            if (FAILED(hr)) { OutputDebugStringA("[FluidPS] GPU state buffer 생성 실패\n"); }
        }

        // m_pGPURenderBuffer: DEFAULT, UAV (FluidParticleRenderData x MAX_PARTICLES)
        {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width              = sizeof(FluidParticleRenderData) * MAX_PARTICLES;
            desc.Height             = 1;
            desc.DepthOrArraySize   = 1;
            desc.MipLevels          = 1;
            desc.Format             = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count   = 1;
            desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            hr = pDevice->CreateCommittedResource(
                &defaultHeapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(&m_pGPURenderBuffer));
            if (FAILED(hr)) { OutputDebugStringA("[FluidPS] GPU render buffer 생성 실패\n"); }
            m_eGPURenderBufferState = D3D12_RESOURCE_STATE_COMMON;
        }

        // m_pInitUpload: UPLOAD (GPUParticle x MAX_PARTICLES)
        {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width              = sizeof(GPUParticle) * MAX_PARTICLES;
            desc.Height             = 1;
            desc.DepthOrArraySize   = 1;
            desc.MipLevels          = 1;
            desc.Format             = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count   = 1;
            desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

            hr = pDevice->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&m_pInitUpload));
            if (FAILED(hr)) { OutputDebugStringA("[FluidPS] GPU init upload buffer 생성 실패\n"); }
        }

        // m_pSPHCB: UPLOAD, SPHConstants 크기에 맞게 256 bytes 정렬 (3 서브스텝 분)
        {
            const UINT64 spCBSize = 3 * ((sizeof(SPHConstants) + 255u) & ~255u);
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width              = spCBSize;
            desc.Height             = 1;
            desc.DepthOrArraySize   = 1;
            desc.MipLevels          = 1;
            desc.Format             = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count   = 1;
            desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

            hr = pDevice->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&m_pSPHCB));
            if (FAILED(hr)) { OutputDebugStringA("[FluidPS] SPH CB 생성 실패\n"); }
            else { m_pSPHCB->Map(0, nullptr, (void**)&m_pMappedSPHCB); }
        }

        // m_pCPBuffer: UPLOAD, MAX_GPU_CPS x 32 bytes
        {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width              = sizeof(GPUControlPoint) * MAX_GPU_CPS;
            desc.Height             = 1;
            desc.DepthOrArraySize   = 1;
            desc.MipLevels          = 1;
            desc.Format             = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count   = 1;
            desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

            hr = pDevice->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&m_pCPBuffer));
            if (FAILED(hr)) { OutputDebugStringA("[FluidPS] CP buffer 생성 실패\n"); }
            else { m_pCPBuffer->Map(0, nullptr, (void**)&m_pMappedCPBuffer); }
        }

        // m_pHashCountBuffer: DEFAULT, UAV (uint x GPU_HASH_TABLE_SIZE)
        {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width              = sizeof(uint32_t) * GPU_HASH_TABLE_SIZE;
            desc.Height             = 1;
            desc.DepthOrArraySize   = 1;
            desc.MipLevels          = 1;
            desc.Format             = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count   = 1;
            desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            hr = pDevice->CreateCommittedResource(
                &defaultHeapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr, IID_PPV_ARGS(&m_pHashCountBuffer));
            if (FAILED(hr)) { OutputDebugStringA("[FluidPS] Hash count buffer 생성 실패\n"); }
        }

        // m_pHashEntryBuffer: DEFAULT, UAV (uint x GPU_HASH_TABLE_SIZE * GPU_MAX_PER_CELL)
        {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width              = sizeof(uint32_t) * GPU_HASH_TABLE_SIZE * GPU_MAX_PER_CELL;
            desc.Height             = 1;
            desc.DepthOrArraySize   = 1;
            desc.MipLevels          = 1;
            desc.Format             = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count   = 1;
            desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            hr = pDevice->CreateCommittedResource(
                &defaultHeapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr, IID_PPV_ARGS(&m_pHashEntryBuffer));
            if (FAILED(hr)) { OutputDebugStringA("[FluidPS] Hash entry buffer 생성 실패\n"); }
        }

        // SRV를 m_pGPURenderBuffer에 등록 (기존 m_pParticleBuffer SRV를 덮어씀)
        if (m_pGPURenderBuffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDescGPU = {};
            srvDescGPU.Format                     = DXGI_FORMAT_UNKNOWN;
            srvDescGPU.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            srvDescGPU.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDescGPU.Buffer.FirstElement        = 0;
            srvDescGPU.Buffer.NumElements         = MAX_PARTICLES;
            srvDescGPU.Buffer.StructureByteStride = sizeof(FluidParticleRenderData);
            srvDescGPU.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

            D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle2 = pDescriptorHeap->GetCPUHandle(nSrvDescriptorIndex);
            pDevice->CreateShaderResourceView(m_pGPURenderBuffer.Get(), &srvDescGPU, srvCpuHandle2);
        }
    }

    // GPU SPH 파이프라인 빌드
    BuildSPHPipeline(pDevice);
    m_bGPUInited = true;

    OutputDebugStringA("[FluidParticleSystem] Initialized (GPU SPH enabled)\n");
}

// ============================================================================
// BuildSharedPipeline (static) - 모든 인스턴스가 공유하는 파이프라인
// ============================================================================
void FluidParticleSystem::BuildSharedPipeline(ID3D12Device* pDevice)
{
    if (s_pRootSignature && s_pPSO) return;  // 이미 빌드됨

    HRESULT hr;

    // 4. Build Root Signature
    D3D12_ROOT_PARAMETER rootParams[2] = {};

    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;  // b0
    rootParams[0].Descriptor.RegisterSpace  = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0;  // t0
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = 2;
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
        return;
    }
    hr = pDevice->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                       IID_PPV_ARGS(&s_pRootSignature));
    if (FAILED(hr)) { OutputDebugStringA("[FluidPS] Failed to create root signature\n"); return; }

    // 5. Compile inline shaders
    ComPtr<ID3DBlob> vsBlob, psBlob;
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    hr = D3DCompile(g_FluidShaderCode, strlen(g_FluidShaderCode), "FluidParticleShader",
                     nullptr, nullptr, "VS_Fluid", "vs_5_1", compileFlags, 0, &vsBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
        OutputDebugStringA("[FluidPS] VS compile failed\n");
        return;
    }

    errBlob.Reset();
    hr = D3DCompile(g_FluidShaderCode, strlen(g_FluidShaderCode), "FluidParticleShader",
                     nullptr, nullptr, "PS_Fluid", "ps_5_1", compileFlags, 0, &psBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
        OutputDebugStringA("[FluidPS] PS compile failed\n");
        return;
    }

    // 6. Build PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = s_pRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

    psoDesc.InputLayout = { nullptr, 0 };

    psoDesc.BlendState.AlphaToCoverageEnable  = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    auto& rt = psoDesc.BlendState.RenderTarget[0];
    rt.BlendEnable           = TRUE;
    rt.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp               = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rt.DestBlendAlpha        = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    psoDesc.SampleMask             = UINT_MAX;
    psoDesc.PrimitiveTopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets       = 1;
    psoDesc.RTVFormats[0]          = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat              = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.SampleDesc.Count       = 1;

    hr = pDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&s_pPSO));
    if (FAILED(hr)) { OutputDebugStringA("[FluidPS] Failed to create PSO\n"); return; }

    OutputDebugStringA("[FluidParticleSystem] Shared pipeline built\n");
}

// ============================================================================
// Spawn
// ============================================================================
void FluidParticleSystem::Spawn(const XMFLOAT3& center, const FluidParticleConfig& config)
{
    // 슬롯 재사용 시 이전 시퀀스 상태 완전 초기화
    m_MotionMode          = ParticleMotionMode::ControlPoint;
    m_ConfinementBox      = ConfinementBoxDesc{};   // active=false
    m_BeamDesc            = BeamDesc{};
    m_GravityDesc         = GravityDesc{};
    m_GlobalGravityStrength = 0.f;
    m_ControlPoints.clear();
    m_vGPUPendingOffset    = {};
    m_vGPUPendingVelDelta  = {};
    // 보스 SPH 브레스 제트/원기둥 상태 초기화 (슬롯 재사용 시 오염 방지)
    m_JetActive     = false;
    m_JetConverge   = false;
    m_ObstacleCount = 0;

    m_Config = config;
    m_Colors = config.overrideColors
        ? FluidElementColor{ config.customCoreColor, config.customEdgeColor }
        : FluidElementColors::Get(config.element);

    int count = (std::min)(config.particleCount, MAX_PARTICLES);
    m_Particles.clear();
    m_Particles.resize(count);

    int nucleusCount = (int)(count * config.nucleusFraction);

    // 스폰 그룹별 파티클 수 합산 (핵 + 위성 그룹 + 나머지)
    int groupTotal = 0;
    for (const auto& g : config.spawnGroups) groupTotal += g.count;
    // 나머지 파티클은 spawnRadius 구체에 균일 스폰
    // (nucleusCount + groupTotal이 count 초과 시 clamp는 루프 내에서 처리)

    int pi = 0;

    // 1) 핵 파티클: center 근처 nucleusRadius 내, cpGroup=0 (핵 CP 전용)
    for (int k = 0; k < nucleusCount && pi < count; ++k, ++pi)
    {
        auto& p = m_Particles[pi];
        XMFLOAT3 offset = RandInSphere(config.nucleusRadius);
        p.position = { center.x + offset.x, center.y + offset.y, center.z + offset.z };
        p.velocity = { 0.f, 0.f, 0.f };  // 처음부터 중앙에 정지 스폰
        p.force = {}; p.density = config.restDensity; p.nearDensity = 0.f; p.mass = 1.f;
        p.active = true; p.cpGroup = 0;  // 핵 CP (master, gCPs[0])
    }

    // 2) 위성 CP 위치 스폰 그룹
    int satIndex = 0;
    for (const auto& g : config.spawnGroups)
    {
        ++satIndex;  // 위성 CP 인덱스 (cpGroup=-2일 때만 사용)
        // cpGroup: -2=satIndex 자동, -1=전체 CP, 0+=지정 CP
        int assignedCpGroup = (g.cpGroup == -2) ? satIndex : g.cpGroup;

        // inwardSpeed > 0이면 center(스폰 원점) 방향 초기 속도 부여
        XMFLOAT3 inwardVel = {};
        if (g.inwardSpeed > 0.f)
        {
            float dx = center.x - g.center.x;
            float dy = center.y - g.center.y;
            float dz = center.z - g.center.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist > 0.001f) {
                float inv = g.inwardSpeed / dist;
                inwardVel = { dx * inv, dy * inv, dz * inv };
            }
        }

        for (int k = 0; k < g.count && pi < count; ++k, ++pi)
        {
            auto& p = m_Particles[pi];
            XMFLOAT3 offset = RandInSphere(g.radius);
            p.position = { g.center.x + offset.x, g.center.y + offset.y, g.center.z + offset.z };
            if (g.inwardSpeed > 0.f)
                p.velocity = { inwardVel.x + RandRange(-1.f, 1.f),
                                inwardVel.y + RandRange(-0.5f, 0.5f),
                                inwardVel.z + RandRange(-1.f, 1.f) };
            else
                p.velocity = { RandRange(-1.f, 1.f), RandRange(-0.5f, 0.5f), RandRange(-1.f, 1.f) };
            p.force = {}; p.density = config.restDensity; p.nearDensity = 0.f; p.mass = 1.f;
            p.active = true; p.cpGroup = assignedCpGroup;
        }
    }

    // 3) 나머지: spawnRadius 구체에 균일 스폰, cpGroup=-1 (전체 CP)
    for (; pi < count; ++pi)
    {
        auto& p = m_Particles[pi];
        XMFLOAT3 offset = RandInSphere(config.spawnRadius);
        p.position = { center.x + offset.x, center.y + offset.y, center.z + offset.z };
        p.velocity = { RandRange(-1.5f, 1.5f), RandRange(-0.5f, 0.5f), RandRange(-1.5f, 1.5f) };
        p.force = {}; p.density = config.restDensity; p.nearDensity = 0.f; p.mass = 1.f;
        p.active = true; p.cpGroup = -1;  // 전체 CP 영향
    }

    // Set default control point at center if none set
    if (m_ControlPoints.empty())
    {
        FluidControlPoint cp;
        cp.position           = center;
        cp.attractionStrength = 15.0f;
        // SpawnGroup이 있으면 가장 먼 그룹까지 커버하도록 반경 확장
        float maxRadius = config.spawnRadius;
        for (const auto& g : config.spawnGroups) {
            float dx = g.center.x - center.x;
            float dy = g.center.y - center.y;
            float dz = g.center.z - center.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz) + g.radius;
            if (dist > maxRadius) maxRadius = dist;
        }
        cp.sphereRadius = maxRadius * 1.3f;
        m_ControlPoints.push_back(cp);
    }

    // GPU SPH: 초기 파티클 상태를 업로드 버퍼에 저장
    UploadInitialStateToGPU();

    OutputDebugStringA("[FluidPS] Spawned particles\n");
}

// 현재 m_Particles 상태(pos/vel/...)를 GPU 초기 업로드 버퍼에 기록.
// Spawn 직후, 그리고 SetSPHJet에서 입자를 제트 경로로 재배치한 뒤 호출.
void FluidParticleSystem::UploadInitialStateToGPU()
{
    if (!m_pInitUpload || !m_bGPUInited) return;

    int count = (int)m_Particles.size();
    GPUParticle* pMapped = nullptr;
    m_pInitUpload->Map(0, nullptr, (void**)&pMapped);
    if (pMapped)
    {
        memset(pMapped, 0, sizeof(GPUParticle) * MAX_PARTICLES);
        for (int i = 0; i < count; ++i)
        {
            pMapped[i].pos         = m_Particles[i].position;
            pMapped[i].density     = m_Particles[i].density;
            pMapped[i].vel         = m_Particles[i].velocity;
            pMapped[i].nearDensity = 0.f;
            pMapped[i].force       = { 0, 0, 0 };
            pMapped[i].mass        = m_Particles[i].mass;
            pMapped[i].active      = m_Particles[i].active ? 1 : 0;
            pMapped[i].cpGroup     = m_Particles[i].cpGroup;
            // _pad[0] = 제트 파티클 나이(age). 비제트 스폰은 beamT=0 → 0.
            //   제트는 SetSPHJet에서 경로상 위치에 비례한 초기 나이를 beamT에 실어 보냄
            //   → 재순환 시점이 분산되어 입에서 끊김 없이 계속 재방출.
            pMapped[i]._pad[0]     = m_Particles[i].beamT;
            pMapped[i]._pad[1]     = 0.f;
        }
        m_pInitUpload->Unmap(0, nullptr);
    }
    m_bNeedsUpload = true;
}

void FluidParticleSystem::Clear()
{
    m_Particles.clear();
    m_ControlPoints.clear();
    m_nActiveCount = 0;
    m_JetActive     = false;   // 제트/원기둥 상태도 해제 (슬롯 재사용 오염 방지)
    m_JetConverge   = false;
    m_ObstacleCount = 0;
}

void FluidParticleSystem::SetControlPoints(const std::vector<FluidControlPoint>& cps)
{
    m_ControlPoints = cps;
}

void FluidParticleSystem::AddControlPoint(const FluidControlPoint& cp)
{
    m_ControlPoints.push_back(cp);
}

void FluidParticleSystem::ClearControlPoints()
{
    m_ControlPoints.clear();
}

// ============================================================================
// SPH Kernel Functions (Clavet 2005 / Sebastian Lague dual-density relaxation)
// ============================================================================
float FluidParticleSystem::ViscLaplacian(float r, float h) const
{
    if (r >= h) return 0.0f;
    float h6 = h * h * h * h * h * h;
    return (45.0f / (PI_F * h6)) * (h - r);
}

// ============================================================================
// Spatial Hash
// ============================================================================
int FluidParticleSystem::HashCell(int cx, int cy, int cz) const
{
    unsigned int h = (unsigned int)(cx * 73856093) ^ (unsigned int)(cy * 19349663) ^ (unsigned int)(cz * 83492791);
    return (int)(h & (HASH_TABLE_SIZE - 1));
}

void FluidParticleSystem::CellFromPos(const XMFLOAT3& p, int& cx, int& cy, int& cz) const
{
    float invH = 1.0f / m_Config.smoothingRadius;
    cx = (int)floorf(p.x * invH);
    cy = (int)floorf(p.y * invH);
    cz = (int)floorf(p.z * invH);
}

void FluidParticleSystem::BuildSpatialHash()
{
    for (int i = 0; i < HASH_TABLE_SIZE; ++i)
        m_HashTable[i].count = 0;

    int n = (int)m_Particles.size();
    for (int i = 0; i < n; ++i)
    {
        if (!m_Particles[i].active) continue;
        int cx, cy, cz;
        CellFromPos(m_Particles[i].position, cx, cy, cz);
        int h = HashCell(cx, cy, cz);
        auto& cell = m_HashTable[h];
        if (cell.count < MAX_PER_CELL)
        {
            cell.particles[cell.count++] = i;
        }
    }
}

int FluidParticleSystem::GetNeighbors(int i, int* out, int maxOut) const
{
    int count = 0;
    int cx, cy, cz;
    CellFromPos(m_Particles[i].position, cx, cy, cz);

    float h2 = m_Config.smoothingRadius * m_Config.smoothingRadius;

    for (int dx = -1; dx <= 1; ++dx)
    {
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dz = -1; dz <= 1; ++dz)
            {
                int h = HashCell(cx + dx, cy + dy, cz + dz);
                const auto& cell = m_HashTable[h];
                for (int k = 0; k < cell.count; ++k)
                {
                    int j = cell.particles[k];
                    if (j == i) continue;

                    float rx = m_Particles[i].position.x - m_Particles[j].position.x;
                    float ry = m_Particles[i].position.y - m_Particles[j].position.y;
                    float rz = m_Particles[i].position.z - m_Particles[j].position.z;
                    float r2 = rx * rx + ry * ry + rz * rz;

                    if (r2 < h2)
                    {
                        if (count < maxOut)
                            out[count++] = j;
                    }
                }
            }
        }
    }
    return count;
}

// ============================================================================
// SPH Density — Clavet 2005 이중 밀도 완화 (GPU와 동일한 SpikyPow2 + SpikyPow3)
// ============================================================================
void FluidParticleSystem::ComputeDensityPressure()
{
    float h  = m_Config.smoothingRadius;
    float h2 = h * h;
    float h5 = h2 * h2 * h;
    float h6 = h5 * h;
    // 커널 상수 (GPU DispatchSPH의 kSpikyPow2/kSpikyPow3와 동일)
    float kPow2 = 15.0f / (2.0f * PI_F * h5);   // SpikyPow2 정규화
    float kPow3 = 15.0f / (PI_F * h6);           // SpikyPow3 정규화

    int neighbors[MAX_NEIGHBOR_SEARCH];
    int n = (int)m_Particles.size();

    for (int i = 0; i < n; ++i)
    {
        if (!m_Particles[i].active) continue;

        // 자기 기여: r=0 → SpikyPow2(0,h) = h² * kPow2 (GPU와 동일)
        float density     = h2 * kPow2;
        float nearDensity = 0.0f;

        int nCount = GetNeighbors(i, neighbors, MAX_NEIGHBOR_SEARCH);
        for (int k = 0; k < nCount; ++k)
        {
            int j = neighbors[k];
            float rx = m_Particles[i].position.x - m_Particles[j].position.x;
            float ry = m_Particles[i].position.y - m_Particles[j].position.y;
            float rz = m_Particles[i].position.z - m_Particles[j].position.z;
            float r2 = rx * rx + ry * ry + rz * rz;
            if (r2 >= h2) continue;

            float r = sqrtf(r2);
            float v = h - r;
            density     += v * v * kPow2;       // SpikyPow2: K*(h-r)^2
            nearDensity += v * v * v * kPow3;   // SpikyPow3: K*(h-r)^3
        }

        m_Particles[i].density     = (std::max)(density, 0.001f);
        m_Particles[i].nearDensity = (std::max)(nearDensity, 0.0f);
    }
}

// ============================================================================
// SPH Forces — Clavet 2005 이중 밀도 완화 (GPU와 동일한 압력 공식)
// ============================================================================
void FluidParticleSystem::ComputeForces()
{
    float h  = m_Config.smoothingRadius;
    float h2 = h * h;
    float h5 = h2 * h2 * h;
    float h6 = h5 * h;
    // DerivSpikyPow2: -2*K2*(h-r),  DerivSpikyPow3: -3*K3*(h-r)^2
    float kPow2Grad = 30.0f / (2.0f * PI_F * h5);  // GPU kSpikyPow2Grad
    float kPow3Grad = 45.0f / (PI_F * h6);          // GPU kSpikyPow3Grad

    int neighbors[MAX_NEIGHBOR_SEARCH];
    int n = (int)m_Particles.size();

    for (int i = 0; i < n; ++i)
    {
        if (!m_Particles[i].active) continue;

        float fx = 0.0f, fy = 0.0f, fz = 0.0f;

        float densityI      = m_Particles[i].density;
        float nearDensityI  = m_Particles[i].nearDensity;
        float pressI        = m_Config.stiffness * (densityI - m_Config.restDensity);
        float nearPressI    = m_Config.nearPressureMultiplier * nearDensityI;

        int nCount = GetNeighbors(i, neighbors, MAX_NEIGHBOR_SEARCH);
        for (int k = 0; k < nCount; ++k)
        {
            int j = neighbors[k];
            float rx = m_Particles[i].position.x - m_Particles[j].position.x;
            float ry = m_Particles[i].position.y - m_Particles[j].position.y;
            float rz = m_Particles[i].position.z - m_Particles[j].position.z;
            float rLen = sqrtf(rx * rx + ry * ry + rz * rz);
            if (rLen < 0.0001f || rLen >= h) continue;

            float densityJ      = (std::max)(m_Particles[j].density, 0.001f);
            float nearDensityJ  = (std::max)(m_Particles[j].nearDensity, 0.001f);
            float pressJ        = m_Config.stiffness * (densityJ - m_Config.restDensity);
            float nearPressJ    = m_Config.nearPressureMultiplier * nearDensityJ;

            float sharedPress     = (pressI + pressJ) * 0.5f;
            float sharedNearPress = (nearPressI + nearPressJ) * 0.5f;

            // 방향: j → i (GPU와 동일: diff = pos_i - pos_j)
            float dirX = rx / rLen;
            float dirY = ry / rLen;
            float dirZ = rz / rLen;

            // 압력 구배 커널 도함수 (음수값)
            float v = h - rLen;
            float dPress     = -v * kPow2Grad;        // DerivSpikyPow2
            float dNearPress = -v * v * kPow3Grad;    // DerivSpikyPow3

            // GPU와 동일: f -= dir * (dW*P/rho + dWnear*Pnear/rho_near) * mass_j
            float pForce = (dPress * sharedPress / densityJ
                          + dNearPress * sharedNearPress / nearDensityJ)
                          * m_Particles[j].mass;
            fx -= dirX * pForce;
            fy -= dirY * pForce;
            fz -= dirZ * pForce;

            // 점성 (기존 유지 - GPU와 동일)
            float viscLap = ViscLaplacian(rLen, h);
            float vScale = m_Config.viscosity * m_Particles[j].mass / densityJ * viscLap;
            fx += (m_Particles[j].velocity.x - m_Particles[i].velocity.x) * vScale;
            fy += (m_Particles[j].velocity.y - m_Particles[i].velocity.y) * vScale;
            fz += (m_Particles[j].velocity.z - m_Particles[i].velocity.z) * vScale;
        }

        // Gravity 모드: CP 인력 대신 중력 적용
        if (m_MotionMode == ParticleMotionMode::Gravity) {
            fx += m_GravityDesc.gravity.x * m_Particles[i].mass;
            fy += m_GravityDesc.gravity.y * m_Particles[i].mass;
            fz += m_GravityDesc.gravity.z * m_Particles[i].mass;
        }
        else {
            // ControlPoint / OrbitalCP 모드: 기존 CP 인력 + swirl
            for (const auto& cp : m_ControlPoints)
            {
                float dx = cp.position.x - m_Particles[i].position.x;
                float dy = cp.position.y - m_Particles[i].position.y;
                float dz = cp.position.z - m_Particles[i].position.z;
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);

                if (dist < 0.001f) continue;

                float invDist = 1.0f / dist;
                float ndx = dx * invDist;
                float ndy = dy * invDist;
                float ndz = dz * invDist;

                // Soft attraction (attenuated by distance)
                float attraction = cp.attractionStrength / (1.0f + dist * 0.5f);
                fx += ndx * attraction;
                fy += ndy * attraction;
                fz += ndz * attraction;

                // Gentle swirl: tangential force = cross(Y_up, toward_cp)
                // Creates horizontal rotation around the control point
                float swirlStrength = cp.attractionStrength * 0.35f;
                // tangent = cross((0,1,0), (ndx,ndy,ndz)) = (ndz, 0, -ndx)
                fx += ndz * swirlStrength;
                fz += -ndx * swirlStrength;

                // Boundary: strong inward force if outside sphere radius
                if (dist > cp.sphereRadius)
                {
                    float overshoot = dist - cp.sphereRadius;
                    float boundaryForce = m_Config.boundaryStiffness * overshoot;
                    fx += ndx * boundaryForce;
                    fy += ndy * boundaryForce;
                    fz += ndz * boundaryForce;
                }
            }
        }

        // 전역 중력 (모든 모드에서 선택적 적용)
        if (m_GlobalGravityStrength > 0.f) {
            fy -= m_GlobalGravityStrength * m_Particles[i].mass;
        }

        // ConfinementBox 경계력 (모든 모드에서 선택적 적용)
        if (m_ConfinementBox.active) {
            XMVECTOR toParticle = XMVectorSubtract(
                XMLoadFloat3(&m_Particles[i].position),
                XMLoadFloat3(&m_ConfinementBox.center));
            float localX = XMVectorGetX(XMVector3Dot(toParticle, XMLoadFloat3(&m_ConfinementBox.axisX)));
            float localY = XMVectorGetX(XMVector3Dot(toParticle, XMLoadFloat3(&m_ConfinementBox.axisY)));
            float localZ = XMVectorGetX(XMVector3Dot(toParticle, XMLoadFloat3(&m_ConfinementBox.axisZ)));

            XMVECTOR forceVec = XMVectorZero();
            float bStiff = m_Config.boundaryStiffness;

            auto applyAxisForce = [&](float localCoord, float halfExt, const XMFLOAT3& axis) {
                if (localCoord > halfExt) {
                    float excess = localCoord - halfExt;
                    XMVECTOR axisVec = XMLoadFloat3(&axis);
                    forceVec = XMVectorSubtract(forceVec, XMVectorScale(axisVec, bStiff * excess));
                } else if (localCoord < -halfExt) {
                    float excess = -halfExt - localCoord;
                    XMVECTOR axisVec = XMLoadFloat3(&axis);
                    forceVec = XMVectorAdd(forceVec, XMVectorScale(axisVec, bStiff * excess));
                }
            };

            applyAxisForce(localX, m_ConfinementBox.halfExtents.x, m_ConfinementBox.axisX);
            applyAxisForce(localY, m_ConfinementBox.halfExtents.y, m_ConfinementBox.axisY);
            applyAxisForce(localZ, m_ConfinementBox.halfExtents.z, m_ConfinementBox.axisZ);

            XMFLOAT3 boxForce;
            XMStoreFloat3(&boxForce, forceVec);
            fx += boxForce.x;
            fy += boxForce.y;
            fz += boxForce.z;
        }

        m_Particles[i].force = { fx, fy, fz };
    }
}

// ============================================================================
// Integration
// ============================================================================
void FluidParticleSystem::Integrate(float dt)
{
    const float DAMPING = 0.995f;
    const float MAX_SPEED = 12.0f;
    int n = (int)m_Particles.size();

    for (int i = 0; i < n; ++i)
    {
        if (!m_Particles[i].active) continue;

        float invDensity = 1.0f / (std::max)(m_Particles[i].density, 0.001f);

        // acceleration = force / density
        m_Particles[i].velocity.x += (m_Particles[i].force.x * invDensity) * dt;
        m_Particles[i].velocity.y += (m_Particles[i].force.y * invDensity) * dt;
        m_Particles[i].velocity.z += (m_Particles[i].force.z * invDensity) * dt;

        // Damping
        m_Particles[i].velocity.x *= DAMPING;
        m_Particles[i].velocity.y *= DAMPING;
        m_Particles[i].velocity.z *= DAMPING;

        // Speed clamping
        float speed2 = m_Particles[i].velocity.x * m_Particles[i].velocity.x
                      + m_Particles[i].velocity.y * m_Particles[i].velocity.y
                      + m_Particles[i].velocity.z * m_Particles[i].velocity.z;
        if (speed2 > MAX_SPEED * MAX_SPEED)
        {
            float scale = MAX_SPEED / sqrtf(speed2);
            m_Particles[i].velocity.x *= scale;
            m_Particles[i].velocity.y *= scale;
            m_Particles[i].velocity.z *= scale;
        }

        // Position update
        m_Particles[i].position.x += m_Particles[i].velocity.x * dt;
        m_Particles[i].position.y += m_Particles[i].velocity.y * dt;
        m_Particles[i].position.z += m_Particles[i].velocity.z * dt;
    }
}

void FluidParticleSystem::OffsetParticles(const XMFLOAT3& delta)
{
    for (auto& p : m_Particles)
    {
        if (!p.active) continue;
        p.position.x += delta.x;
        p.position.y += delta.y;
        p.position.z += delta.z;
    }
    // GPU 모드: 다음 DispatchSPH에서 GPU 버퍼에도 오프셋 적용
    if (m_bGPUInited)
    {
        m_vGPUPendingOffset.x += delta.x;
        m_vGPUPendingOffset.y += delta.y;
        m_vGPUPendingOffset.z += delta.z;
    }
}

// ============================================================================
// Update
// ============================================================================
void FluidParticleSystem::Update(float deltaTime)
{
    if (m_Particles.empty()) return;

    // Clamp dt for stability with large timesteps
    float dt = (std::min)(deltaTime, 0.016f);
    m_fElapsed += dt;

    // Beam 모드: 빔-로컬 좌표 방식 (방향 변경 시 모든 파티클 즉시 스냅)
    if (m_MotionMode == ParticleMotionMode::Beam) {
        XMVECTOR start     = XMLoadFloat3(&m_BeamDesc.startPos);
        XMVECTOR end       = XMLoadFloat3(&m_BeamDesc.endPos);
        XMVECTOR dir       = XMVector3Normalize(XMVectorSubtract(end, start));
        float    totalDist = XMVectorGetX(XMVector3Length(XMVectorSubtract(end, start)));
        XMStoreFloat3(&m_BeamDesc.prevDir, dir);

        // 수직 기저 벡터 (빔 축에 수직인 평면)
        XMVECTOR perpX = XMVector3Normalize(XMVectorSet(-XMVectorGetZ(dir), 0.f, XMVectorGetX(dir), 0.f));
        XMVECTOR perpY = XMVector3Cross(dir, perpX);

        for (auto& p : m_Particles) {
            if (!p.active) continue;

            // 빔 축 방향으로 전진 (enableFlow가 설정된 경우에만)
            if (m_BeamDesc.enableFlow) {
                p.beamT += p.beamSpeed * dt;

                // 끝 도달 또는 범위 이탈 → 시작으로 리셋
                if (p.beamT >= totalDist || p.beamT < 0.f) {
                    p.beamT = 0.f;
                    if (m_BeamDesc.swirlSpeed != 0.f || m_BeamDesc.swirlExpand) {
                        p.beamAngle = Rand01() * XM_2PI;
                        p.beamSizeScale = 1.f;
                    } else {
                        p.beamRx = (Rand01() - 0.5f) * 2.f * m_BeamDesc.spreadRadius;
                        p.beamRy = (Rand01() - 0.5f) * 2.f * (m_BeamDesc.spreadRadius * m_BeamDesc.verticalScale);
                    }
                    p.beamSpeed = m_BeamDesc.speedMin + Rand01() * (m_BeamDesc.speedMax - m_BeamDesc.speedMin);
                }
            }

            // swirl/expand: beamT 기반 반경 + 각도 공전
            if ((m_BeamDesc.swirlSpeed != 0.f || m_BeamDesc.swirlExpand) && totalDist > 0.f) {
                p.beamAngle += m_BeamDesc.swirlSpeed * dt;
                float t = p.beamT / totalDist;
                // baseT: 입 쪽 기본 폭 비율. 입에서 base*spread, 끝에서 spread 까지 선형
                float baseT = m_BeamDesc.swirlBaseT;
                float r = m_BeamDesc.swirlExpand
                    ? m_BeamDesc.spreadRadius * (baseT + (1.f - baseT) * t)  // 입=base, 끝=full
                    : m_BeamDesc.spreadRadius * (1.f - t);                   // 수렴
                p.beamRx = r * cosf(p.beamAngle);
                // swirl 모드에서도 verticalScale 적용 — 수직으로 납작한 타원형 cone
                p.beamRy = r * sinf(p.beamAngle) * m_BeamDesc.verticalScale;

                if (m_BeamDesc.swirlFadeEnd > 0.f) {
                    float ft = p.beamT / m_BeamDesc.swirlFadeEnd;
                    p.beamSizeScale = m_BeamDesc.swirlFadeInOut
                        ? (std::max)(0.f, 1.f - std::abs(2.f * ft - 1.f))  // 삼각파: 0→1→0
                        : (std::max)(0.f, 1.f - ft);                        // 페이드 아웃
                } else {
                    p.beamSizeScale = 1.f;
                }
            }

            // 빔-로컬 → 월드 좌표 변환: 방향이 바뀌면 즉시 반영, 늘어남 없음
            XMVECTOR pos = XMVectorAdd(
                XMVectorAdd(start, XMVectorScale(dir, p.beamT)),
                XMVectorAdd(XMVectorScale(perpX, p.beamRx), XMVectorScale(perpY, p.beamRy))
            );
            XMStoreFloat3(&p.position, pos);
        }
        return; // SPH 단계 스킵
    }

    // GPU SPH 모드: CPU 시뮬레이션 스킵 (DispatchSPH에서 처리)
    if (m_bGPUInited)
    {
        // Beam 모드가 아닌 경우 GPU에서 SPH 처리하므로 CPU 업데이트 불필요
        // UploadRenderData도 GPU에서 하므로 스킵
        return;
    }

    // GPU가 비활성인 경우 CPU fallback SPH 시뮬레이션
    BuildSpatialHash();
    ComputeDensityPressure();
    ComputeForces();
    Integrate(dt);
}

// ============================================================================
// Upload Render Data
// ============================================================================
void FluidParticleSystem::UploadRenderData()
{
    if (!m_pMappedParticles) return;

    int renderIdx = 0;
    int n = (int)m_Particles.size();

    // Beam 모드: 입→끝 그라디언트 및 cone 단면 증가 보정을 위해 길이/반경 사전 계산
    const bool bBeamMode = (m_MotionMode == ParticleMotionMode::Beam);
    float beamTotalDist = 0.f;
    float invBeamDist   = 0.f;
    float invMaxRadius  = 0.f;
    if (bBeamMode) {
        XMVECTOR startV = XMLoadFloat3(&m_BeamDesc.startPos);
        XMVECTOR endV   = XMLoadFloat3(&m_BeamDesc.endPos);
        beamTotalDist = XMVectorGetX(XMVector3Length(XMVectorSubtract(endV, startV)));
        if (beamTotalDist > 0.001f) invBeamDist = 1.0f / beamTotalDist;
        if (m_BeamDesc.spreadRadius > 0.001f) invMaxRadius = 1.0f / m_BeamDesc.spreadRadius;
    }

    for (int i = 0; i < n && renderIdx < MAX_PARTICLES; ++i)
    {
        if (!m_Particles[i].active) continue;

        float t;          // core↔edge 혼합 계수 (0=edge, 1=core)
        float brightness; // 추가 밝기 부스트

        if (bBeamMode)
        {
            // beamT 기반 입→끝 그라디언트 + 반경 기반 외곽 그라디언트
            float tBeam = (std::min)(m_Particles[i].beamT * invBeamDist, 1.0f);
            float rx    = m_Particles[i].beamRx;
            float ry    = m_Particles[i].beamRy;
            float rRatio = (std::min)(sqrtf(rx * rx + ry * ry) * invMaxRadius, 1.0f);

            // 파티클마다 조금씩 다르게 흔들기 (beamAngle 을 deterministic 시드로 사용)
            float jitter = sinf(m_Particles[i].beamAngle * 3.1f) * 0.12f
                         + cosf(m_Particles[i].beamAngle * 7.3f + tBeam * 4.0f) * 0.08f;

            // 입(0) = core 비율 높음, 끝(1) = edge 비율 높음. 반경 큰 것도 edge 쪽
            // 베이스 0.20 → 순수 코어 구간 축소 (흰/노랑 쏠림 방지)
            float edgeWeight = 0.20f + tBeam * 0.55f + rRatio * 0.45f + jitter * 0.5f;
            edgeWeight = (std::max)(0.f, (std::min)(edgeWeight, 1.0f));
            t = 1.0f - edgeWeight;  // core=1, edge=0

            // 밝기: 입 쪽도 과포화 안되게 억제, 끝은 그대로 어두움
            brightness = 1.05f - tBeam * 0.25f + jitter * 0.15f;
        }
        else
        {
            // 기타 모드: velocity 기반 기존 로직 유지
            const XMFLOAT3& vel = m_Particles[i].velocity;
            float speed    = sqrtf(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
            float maxSpd   = m_BeamDesc.speedMax > 0.f ? m_BeamDesc.speedMax : 18.f;
            float speedT   = (std::min)(speed / (maxSpd * 0.5f + 0.001f), 1.0f);
            t = (std::max)(0.0f, (std::min)(0.80f + speedT * 0.15f, 1.0f));
            brightness = 1.0f + speedT * 0.35f;
        }

        XMFLOAT4 color;
        color.x = m_Colors.edgeColor.x + (m_Colors.coreColor.x - m_Colors.edgeColor.x) * t;
        color.y = m_Colors.edgeColor.y + (m_Colors.coreColor.y - m_Colors.edgeColor.y) * t;
        color.z = m_Colors.edgeColor.z + (m_Colors.coreColor.z - m_Colors.edgeColor.z) * t;
        color.w = m_Colors.edgeColor.w + (m_Colors.coreColor.w - m_Colors.edgeColor.w) * t;

        color.x = (std::min)(color.x * brightness, 1.5f);
        color.y = (std::min)(color.y * brightness, 1.5f);
        color.z = (std::min)(color.z * brightness, 1.5f);

        float sizeScale = m_Particles[i].beamSizeScale;

        // Beam: cone 단면이 끝으로 갈수록 t² 증가 → 파티클 크기 보정으로 밀도 커버
        if (bBeamMode && invBeamDist > 0.f) {
            float tBeam = (std::min)(m_Particles[i].beamT * invBeamDist, 1.0f);
            sizeScale *= (1.0f + tBeam * 1.0f);  // 시작 1.0배 → 끝 2.0배
        }

        color.w *= sizeScale;
        m_pMappedParticles[renderIdx].position = m_Particles[i].position;
        m_pMappedParticles[renderIdx].size     = m_Config.particleSize * sizeScale;
        m_pMappedParticles[renderIdx].color    = color;

        renderIdx++;
    }

    m_nActiveCount = renderIdx;
}

// ============================================================================
// Render
// ============================================================================
void FluidParticleSystem::Render(ID3D12GraphicsCommandList* pCommandList,
                                 const XMFLOAT4X4& viewProj, const XMFLOAT3& cameraRight, const XMFLOAT3& cameraUp)
{
    if (m_Particles.empty() || !s_pPSO || !s_pRootSignature) return;

    // Update pass CB
    if (m_pMappedPassCB)
    {
        FluidPassCB* pCB = reinterpret_cast<FluidPassCB*>(m_pMappedPassCB);
        pCB->viewProj    = viewProj;
        pCB->cameraRight = cameraRight;
        pCB->padR        = 0.0f;
        pCB->cameraUp    = cameraUp;
        pCB->padU        = 0.0f;
    }

    // Upload render data
    UploadRenderData();
    if (m_nActiveCount == 0) return;

    // Set descriptor heap
    ID3D12DescriptorHeap* ppHeaps[] = { m_pDescriptorHeap->GetHeap() };
    pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    // Set pipeline state
    pCommandList->SetPipelineState(s_pPSO.Get());
    pCommandList->SetGraphicsRootSignature(s_pRootSignature.Get());

    // Bind root parameters
    pCommandList->SetGraphicsRootConstantBufferView(0, m_pPassCB->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle = m_pDescriptorHeap->GetGPUHandle(m_nSrvDescriptorIndex);
    pCommandList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

    // Set primitive topology
    pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // Instanced draw (4 vertices per quad, one instance per active particle)
    pCommandList->DrawInstanced(4, (UINT)m_nActiveCount, 0, 0);
}

// ============================================================================
// 운동 모드 관련 메서드
// ============================================================================
void FluidParticleSystem::SetMotionMode(ParticleMotionMode mode)
{
    m_MotionMode = mode;
}

void FluidParticleSystem::SetConfinementBox(const ConfinementBoxDesc& box)
{
    m_ConfinementBox = box;
}

void FluidParticleSystem::SetBeamDesc(const BeamDesc& beam)
{
    m_BeamDesc = beam;
}

void FluidParticleSystem::SetGravityDesc(const GravityDesc& grav)
{
    m_GravityDesc = grav;
}

void FluidParticleSystem::SetGlobalGravity(float strength)
{
    m_GlobalGravityStrength = strength;
}

void FluidParticleSystem::SetSPHJet(const XMFLOAT3& mouth, const XMFLOAT3& dir,
                                    float speed, float length, float spawnRadius, float spread,
                                    float lifetime, const XMFLOAT3& roomMin, const XMFLOAT3& roomMax,
                                    bool enable, bool converge)
{
    m_JetActive      = enable;
    m_JetConverge    = converge;
    m_JetMouth       = mouth;
    // 방향 정규화
    XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&dir));
    XMStoreFloat3(&m_JetDir, d);
    m_JetSpeed       = speed;
    m_JetLength      = length;
    m_JetSpawnRadius = spawnRadius;
    m_JetSpread      = spread;
    m_JetLifetime    = lifetime;
    m_JetRoomMin     = roomMin;
    m_JetRoomMax     = roomMax;

    // 활성화 순간: 입자를 입 앞 한 점이 아니라 제트 경로 전체에 분산 배치.
    //   → 첫 프레임부터 꽉 찬 흐름으로 보여 "초기 뭉침(클럼프)" 제거.
    //   각 입자: 축방향으로 균등 배치 + 부채꼴 측면 오프셋, 전방 속도 부여.
    if (enable && !m_Particles.empty())
    {
        XMVECTOR mouthV = XMLoadFloat3(&m_JetMouth);
        XMVECTOR dirV   = XMLoadFloat3(&m_JetDir);
        XMVECTOR upW    = XMVectorSet(0, 1, 0, 0);
        XMVECTOR rightV = XMVector3Normalize(XMVector3Cross(upW, dirV));
        float floorY    = m_JetRoomMin.y;

        int N = (int)m_Particles.size();
        for (int i = 0; i < N; ++i)
        {
            FluidParticle& p = m_Particles[i];
            if (!p.active) continue;

            // 초기 분포: 진행축 전역 × 측면 폭 × 높이에 흩뿌림.
            float along = (converge ? (0.25f + 0.75f * Rand01()) * m_JetLength
                                    : ((i + Rand01()) / (float)N) * m_JetLength);
            float latH  = (Rand01() * 2.f - 1.f) * m_JetSpread;        // 폭 전체 균일

            XMVECTOR pos = XMVectorAdd(
                XMVectorAdd(mouthV, XMVectorScale(dirV, along)),
                XMVectorScale(rightV, latH));
            XMFLOAT3 pos3; XMStoreFloat3(&pos3, pos);
            if (converge)
                // 수렴: 바닥~천장 부피형 높이 (수평 시트 일자 수축 방지)
                pos3.y = floorY + 1.f + Rand01() * (m_JetRoomMax.y - floorY - 1.f);
            else
                pos3.y += (Rand01() * 2.f - 1.f) * m_JetSpawnRadius;
            if (pos3.y < floorY) pos3.y = floorY;
            p.position = pos3;

            float spd = m_JetSpeed * (0.9f + 0.2f * Rand01());
            if (converge)
            {
                // 입 쪽으로 빨려드는 초기 속도 (맵 전역 → 입 수렴)
                XMVECTOR toMouth = XMVector3Normalize(XMVectorSubtract(mouthV, pos));
                XMStoreFloat3(&p.velocity, XMVectorScale(toMouth, spd));
            }
            else
            {
                // 평행 전방 속도 (측면 발산 거의 없음 → 커튼 형태 유지)
                XMVECTOR vel = XMVectorAdd(
                    XMVectorScale(dirV, spd),
                    XMVectorScale(rightV, (Rand01() * 2.f - 1.f) * spd * 0.03f));
                XMStoreFloat3(&p.velocity, vel);
            }

            // 초기 나이 — 재순환 시점 분산용 (분사: 경로상 위치 비례, 수렴: 랜덤)
            //   beamT를 age 운반 필드로 사용 (UploadInitialStateToGPU가 _pad[0]로 전달)
            p.beamT = converge ? (Rand01() * m_JetLifetime * 0.5f)
                               : (along / (std::max)(m_JetSpeed, 0.001f));
        }

        // 분산 배치한 초기 상태를 GPU로 업로드 (Spawn의 클럼프 업로드를 덮어씀)
        UploadInitialStateToGPU();
    }
}

void FluidParticleSystem::SetSPHObstacles(const XMFLOAT4* obstacles, int count)
{
    m_ObstacleCount = (count < 0) ? 0 : ((count > 4) ? 4 : count);
    for (int i = 0; i < m_ObstacleCount; ++i) m_Obstacles[i] = obstacles[i];
    for (int i = m_ObstacleCount; i < 4; ++i) m_Obstacles[i] = XMFLOAT4{ 0, 0, 0, 0 };
}

void FluidParticleSystem::InitBeamParticles()
{
    // Beam 모드용 파티클 초기화: startPos에서 endPos까지 전체 길이에 균등하게 배치
    XMVECTOR start = XMLoadFloat3(&m_BeamDesc.startPos);
    XMVECTOR end   = XMLoadFloat3(&m_BeamDesc.endPos);
    XMVECTOR dir   = XMVectorSubtract(end, start);
    float totalDist = XMVectorGetX(XMVector3Length(dir));
    XMVECTOR dirNorm = XMVector3Normalize(dir);

    // prevDir을 현재 방향으로 초기화 (첫 프레임에 불필요한 회전 방지)
    XMStoreFloat3(&m_BeamDesc.prevDir, dirNorm);

    XMVECTOR perpX = XMVector3Normalize(XMVectorSet(-XMVectorGetZ(dirNorm), 0, XMVectorGetX(dirNorm), 0));
    XMVECTOR perpY = XMVector3Cross(dirNorm, perpX);

    // 활성 파티클 수 계산
    int activeCount = 0;
    for (auto& p : m_Particles) if (p.active) activeCount++;
    if (activeCount == 0) return;

    int i = 0;
    for (auto& p : m_Particles) {
        if (!p.active) continue;

        // 빔 전체 길이에 균등하게 배치 (중간 끊김 방지)
        float t = (float)i / (float)activeCount;
        p.beamT     = t * totalDist;
        p.beamSpeed = m_BeamDesc.speedMin + Rand01() * (m_BeamDesc.speedMax - m_BeamDesc.speedMin);
        if (m_BeamDesc.swirlSpeed != 0.f || m_BeamDesc.swirlExpand) {
            p.beamAngle     = Rand01() * XM_2PI;
            p.beamSizeScale = 1.f;
            float t = (totalDist > 0.f) ? p.beamT / totalDist : 0.f;
            float baseT = m_BeamDesc.swirlBaseT;
            float r = m_BeamDesc.swirlExpand
                ? m_BeamDesc.spreadRadius * (baseT + (1.f - baseT) * t)
                : m_BeamDesc.spreadRadius * (1.f - t);
            p.beamRx = r * cosf(p.beamAngle);
            p.beamRy = r * sinf(p.beamAngle) * m_BeamDesc.verticalScale;
        } else {
            p.beamRx = (Rand01() - 0.5f) * 2.f * m_BeamDesc.spreadRadius;
            p.beamRy = (Rand01() - 0.5f) * 2.f * (m_BeamDesc.spreadRadius * m_BeamDesc.verticalScale);
        }

        // 초기 월드 좌표 설정
        XMVECTOR pos = XMVectorAdd(
            XMVectorAdd(start, XMVectorScale(dirNorm, p.beamT)),
            XMVectorAdd(XMVectorScale(perpX, p.beamRx), XMVectorScale(perpY, p.beamRy))
        );
        XMStoreFloat3(&p.position, pos);
        p.velocity = { 0, 0, 0 };
        i++;
    }
    m_bNeedsUpload = true;
}

void FluidParticleSystem::ApplyDirectionalForce(const XMFLOAT3& direction, float impulse)
{
    for (auto& p : m_Particles) {
        if (!p.active) continue;
        p.velocity.x += direction.x * impulse;
        p.velocity.y += direction.y * impulse;
        p.velocity.z += direction.z * impulse;
    }
    // GPU 모드: 다음 DispatchSPH에서 GPU 버퍼에도 속도 델타 적용
    if (m_bGPUInited)
    {
        m_vGPUPendingVelDelta.x += direction.x * impulse;
        m_vGPUPendingVelDelta.y += direction.y * impulse;
        m_vGPUPendingVelDelta.z += direction.z * impulse;
    }
}

void FluidParticleSystem::ApplyRadialBurst(XMFLOAT3 center, float minSpeed, float maxSpeed)
{
    for (auto& p : m_Particles) {
        if (!p.active) continue;
        XMVECTOR pDir = XMVector3Normalize(
            XMVectorSubtract(XMLoadFloat3(&p.position), XMLoadFloat3(&center))
        );
        // 방향이 영벡터인 경우 랜덤 방향
        if (XMVectorGetX(XMVector3Length(pDir)) < 0.001f) {
            pDir = XMVector3Normalize(XMVectorSet(
                Rand01() - 0.5f, Rand01() - 0.5f, Rand01() - 0.5f, 0));
        }
        float speed = minSpeed + Rand01() * (maxSpeed - minSpeed);
        XMFLOAT3 v; XMStoreFloat3(&v, XMVectorScale(pDir, speed));
        p.velocity.x += v.x;
        p.velocity.y += v.y;
        p.velocity.z += v.z;
    }
    if (m_bGPUInited)
    {
        m_uOpFlags |= 0x08u;
        m_vOpBurstCenter   = center;
        m_fOpBurstMinSpeed = minSpeed;
        m_fOpBurstMaxSpeed = maxSpeed;
        m_uOpFrameSeed    += 0x9E3779B9u;
    }
}

void FluidParticleSystem::SetColors(const FluidElementColor& colors)
{
    m_Colors = colors;
}

void FluidParticleSystem::ZeroAxisVelocity(const XMFLOAT3& worldAxis)
{
    XMVECTOR axis = XMVector3Normalize(XMLoadFloat3(&worldAxis));
    for (auto& p : m_Particles)
    {
        if (!p.active) continue;
        XMVECTOR vel    = XMLoadFloat3(&p.velocity);
        float    proj   = XMVectorGetX(XMVector3Dot(vel, axis));
        // 축 방향 성분 제거: vel -= proj * axis
        vel = XMVectorSubtract(vel, XMVectorScale(axis, proj));
        XMStoreFloat3(&p.velocity, vel);
    }
    if (m_bGPUInited)
    {
        m_uOpFlags |= 0x01u;
        XMStoreFloat3(&m_vOpZeroAxis, axis);
    }
}

void FluidParticleSystem::ApplyAxisSpreadForce(const XMFLOAT3& axisDir,
                                                const XMFLOAT3& originPoint,
                                                float           impulse)
{
    XMVECTOR axis   = XMVector3Normalize(XMLoadFloat3(&axisDir));
    XMVECTOR origin = XMLoadFloat3(&originPoint);
    for (auto& p : m_Particles)
    {
        if (!p.active) continue;
        XMVECTOR toP  = XMVectorSubtract(XMLoadFloat3(&p.position), origin);
        float    side = XMVectorGetX(XMVector3Dot(toP, axis));
        // 중심보다 오른쪽이면 +axis, 왼쪽이면 -axis
        float    sign = (side >= 0.f) ? 1.f : -1.f;
        p.velocity.x += XMVectorGetX(axis) * sign * impulse;
        p.velocity.y += XMVectorGetY(axis) * sign * impulse;
        p.velocity.z += XMVectorGetZ(axis) * sign * impulse;
    }
    if (m_bGPUInited)
    {
        m_uOpFlags |= 0x04u;
        XMStoreFloat3(&m_vOpSpreadAxis, axis);
        m_fOpSpreadImpulse = impulse;
        m_vOpSpreadOrigin  = originPoint;
    }
}

void FluidParticleSystem::SetWaveOscillation(float amplitude, float frequency, float waveNumber,
                                              const XMFLOAT3& fwdDir, const XMFLOAT3& upDir)
{
    m_waveOscAmplitude  = amplitude;
    m_waveOscFrequency  = frequency;
    m_waveOscWaveNumber = waveNumber;
    m_waveOscFwdDir     = fwdDir;
    m_waveOscUpDir      = upDir;
    m_waveOscEnabled    = (amplitude > 0.f);
}

void FluidParticleSystem::ClearWaveOscillation()
{
    m_waveOscEnabled   = false;
    m_waveOscAmplitude = 0.f;
}

void FluidParticleSystem::ApplyRandomSidewaysImpulse(const XMFLOAT3& worldAxis, float maxImpulse)
{
    XMVECTOR axis = XMVector3Normalize(XMLoadFloat3(&worldAxis));
    for (auto& p : m_Particles)
    {
        if (!p.active) continue;
        // -maxImpulse ~ +maxImpulse 균등 랜덤
        float impulse = (Rand01() * 2.f - 1.f) * maxImpulse;
        p.velocity.x += XMVectorGetX(axis) * impulse;
        p.velocity.y += XMVectorGetY(axis) * impulse;
        p.velocity.z += XMVectorGetZ(axis) * impulse;
    }
    if (m_bGPUInited)
    {
        m_uOpFlags |= 0x02u;
        XMStoreFloat3(&m_vOpSidewaysAxis, axis);
        m_fOpSidewaysImpulse = maxImpulse;
        m_uOpFrameSeed      += 0x9E3779B9u;
    }
}

// ============================================================================
// RenderDepth (Screen-Space Fluid: 구체 깊이 렌더링)
// ============================================================================
void FluidParticleSystem::RenderDepth(
    ID3D12GraphicsCommandList* pCmdList,
    const XMFLOAT4X4& viewProjTransposed,
    const XMFLOAT4X4& viewTransposed,
    const XMFLOAT3& cameraRight,
    const XMFLOAT3& cameraUp,
    float projA, float projB,
    ScreenSpaceFluid* pSSF)
{
    if (!pSSF || !pSSF->IsInitialized()) return;
    if (m_Particles.empty()) return;
    if (!m_pDepthPassCB || !m_pMappedDepthPassCB) return;
    if (m_nActiveCount == 0) return;

    // GPU 렌더 버퍼 상태 전환: NON_PIXEL_SHADER_RESOURCE (SRV로 읽기)
    if (m_bGPUInited && m_pGPURenderBuffer &&
        m_eGPURenderBufferState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = m_pGPURenderBuffer.Get();
        barrier.Transition.StateBefore = m_eGPURenderBufferState;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pCmdList->ResourceBarrier(1, &barrier);
        m_eGPURenderBufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    // Depth pass CB 업데이트
    FluidSphereDepthCB cb;
    cb.viewProj         = viewProjTransposed;
    cb.view             = viewTransposed;
    cb.cameraRight      = cameraRight;
    cb.projA            = projA;
    cb.cameraUp         = cameraUp;
    cb.projB            = projB;
    // 폭발 페이드 중에는 SSF 구체 반경을 fadeMult로 축소 → 파티클이 시각적으로 줄어듦
    // 정상(m_explodeFade=2.0): 원래 반경 그대로
    // 폭발(m_explodeFade=1.0→0.0): 반경이 1.0→0.0으로 선형 축소
    {
        float displayRadius = m_Config.smoothingRadius;
        if (m_explodeFade < 1.5f) {
            float fadeMult = (std::max)(0.0f, (std::min)(1.0f, m_explodeFade));
            displayRadius *= fadeMult;
        }
        cb.smoothingRadius = displayRadius;
    }
    cb._pad[0] = cb._pad[1] = cb._pad[2] = 0.f;
    memcpy(m_pMappedDepthPassCB, &cb, sizeof(cb));

    // PSO + root sig (ScreenSpaceFluid에서 가져옴)
    pCmdList->SetGraphicsRootSignature(pSSF->GetDepthRootSignature());
    pCmdList->SetPipelineState(pSSF->GetDepthPSO());

    // 디스크립터 힙 (Scene의 마스터 힙)
    ID3D12DescriptorHeap* heaps[] = { m_pDescriptorHeap->GetHeap() };
    pCmdList->SetDescriptorHeaps(1, heaps);

    pCmdList->SetGraphicsRootConstantBufferView(0, m_pDepthPassCB->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle = m_pDescriptorHeap->GetGPUHandle(m_nSrvDescriptorIndex);
    pCmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

    pCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    pCmdList->DrawInstanced(4, (UINT)m_nActiveCount, 0, 0);
}

// ============================================================================
// RenderThicknessOnly (RenderDepth 이후 호출: 두께 전용, 깊이 테스트 없음)
// ============================================================================
void FluidParticleSystem::RenderThicknessOnly(
    ID3D12GraphicsCommandList* pCmdList,
    ScreenSpaceFluid* pSSF)
{
    if (!pSSF || !pSSF->IsInitialized()) return;
    if (m_Particles.empty()) return;
    if (!m_pDepthPassCB || !m_pMappedDepthPassCB) return;
    if (m_nActiveCount == 0) return;

    // GPU 렌더 버퍼는 RenderDepth에서 이미 NON_PIXEL_SHADER_RESOURCE 상태로 전환됨

    // 두께 전용 PSO 사용 (깊이 테스트 없음, 가산 블렌딩)
    // CB는 RenderDepth에서 업로드한 값 그대로 재사용
    pCmdList->SetGraphicsRootSignature(pSSF->GetDepthRootSignature());
    pCmdList->SetPipelineState(pSSF->GetThicknessPSO());

    ID3D12DescriptorHeap* heaps[] = { m_pDescriptorHeap->GetHeap() };
    pCmdList->SetDescriptorHeaps(1, heaps);

    pCmdList->SetGraphicsRootConstantBufferView(0, m_pDepthPassCB->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle = m_pDescriptorHeap->GetGPUHandle(m_nSrvDescriptorIndex);
    pCmdList->SetGraphicsRootDescriptorTable(1, srvGpuHandle);

    pCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    pCmdList->DrawInstanced(4, (UINT)m_nActiveCount, 0, 0);
}

// ============================================================================
// GPU SPH: HLSL Compute Shader
// ============================================================================
// g_SPHComputeShader: 두 부분으로 나뉜 인접 raw 문자열 리터럴 (MSVC 크기 제한 우회)
static const char* g_SPHComputeShader = R"_SPH_A_(
    static const float PI_H = 3.14159265f;

    struct GPUParticle {
        float3 pos;
        float  density;
        float3 vel;
        float  nearDensity;
        float3 force;
        float  mass;
        int    active;
        int    cpGroup;  // 담당 CP 인덱스 (-1=전체, 0=핵, 1+=위성)
        float2 pad;
    };

    struct FluidParticleRenderData {
        float3 position;
        float  size;
        float4 color;
    };

    struct GPUControlPoint {
        float3 position;
        float  attractionStrength;
        float  sphereRadius;
        float3 pad;
    };

    cbuffer SPHCB : register(b0)
    {
        int   gParticleCount;
        float gH;
        float gH2;
        float gRestDensity;
        float gStiffness;
        float gViscosity;
        float gDt;
        float gDamping;
        float gMaxSpeed;
        int   gMotionMode;
        float gGlobalGravity;
        int   gBoxActive;
        float gPad0;
        float3 gGravityVec;
        float  gPad1;
        float3 gBoxCenter;  float gPad2;
        float4 gBoxAxisXH;
        float4 gBoxAxisYH;
        float4 gBoxAxisZH;
        int   gCPCount;
        float3 gPad3;
        float4 gCoreColor;
        float4 gEdgeColor;
        float  gColorRestDensity;
        float3 gPad4;
        float3 gPosOffset;  float gPadPO;   // GPU pos 델타 (OffsetParticles)
        float3 gVelDelta;   float gPadVD;   // GPU vel 델타 (ApplyDirectionalForce)
        // one-shot 페이즈 조작
        int    gOpFlags;    uint  gOpFrameSeed; float2 _gOpR;
        float3 gOpZeroAxis;     float _gOpPadZ;
        float3 gOpSidewaysAxis; float gOpSidewaysImpulse;
        float3 gOpSpreadAxis;   float gOpSpreadImpulse;
        float3 gOpSpreadOrigin; float _gOpPadSO;
        float3 gOpBurstCenter;  float gOpBurstMinSpeed;
        float  gOpBurstMaxSpeed; float3 _gOpPadB;
        // foam / velocity color 파라미터
        float  gFoamThreshold;    float gFoamStrength;
        float  gVelocityColorBoost; float _gFoamPad;
        // Near-pressure + 커널 상수 (Sebastian Lague 이중 밀도 완화)
        float  gNearPressureMult;
        float  gKSpikyPow2;     // 15/(2*PI*h^5)  - SpikyPow2 정규화 상수
        float  gKSpikyPow3;     // 15/(PI*h^6)    - SpikyPow3 정규화 상수
        float  gKSpikyPow2Grad; // 30/(2*PI*h^5)  - SpikyPow2 미분 상수
        float  gKSpikyPow3Grad; // 45/(PI*h^6)    - SpikyPow3 미분 상수
        float  gElapsedTime;    // 박동 펄스용 누적 시간
        float  gExplodeFade;    // 폭발 페이드: 2.0=정상, 1.0..0.0=폭발 소멸
        float  gParticleSizeMult; // 파티클 크기 배율 (기본 1.0, Config.particleSize / 0.35 로 계산)
        // Traveling wave 수직 진동 (Q스킬 파도용)
        float  gWaveOscAmplitude; float gWaveOscFrequency; float gWaveOscWaveNumber; float gWaveOscEnabled;
        float3 gWaveOscFwdDir;  float _gWOPad0;
        float3 gWaveOscUpDir;   float _gWOPad1;
        // 보스 SPH 브레스: 제트 분사 + 원기둥 충돌 + 방 경계
        float3 gJetMouth;       float gJetSpeed;
        float3 gJetDir;         float gJetLength;
        float3 gJetRoomMin;     float gJetSpawnRadius;
        float3 gJetRoomMax;     float gJetSpread;
        float  gJetActive;      float gJetLifetime; uint gJetFrameSeed; int gObstacleCount;
        float  gJetConverge;    float3 _gJetPad2;   // 1=수렴(맵→입), 0=분사(입→맵)
        float4 gObstacles[4];   // xy=중심XZ, z=반경
    };

    RWStructuredBuffer<GPUParticle>             gParticles   : register(u0);
    RWStructuredBuffer<FluidParticleRenderData> gRenderData  : register(u1);
    StructuredBuffer<GPUControlPoint>           gCPs         : register(t0);
    RWStructuredBuffer<uint>                    gHashCount   : register(u2);  // [GPU_HASH_TABLE_SIZE]
    RWStructuredBuffer<uint>                    gHashEntries : register(u3);  // [GPU_HASH_TABLE_SIZE * GPU_MAX_PER_CELL]

    // ---- 공간 해시 상수 ----
    static const uint GPU_HASH_TABLE_SIZE = 8192u;
    static const uint GPU_MAX_PER_CELL    = 32u;

    uint CalcHashCell(int cx, int cy, int cz) {
        return (((uint)cx * 73856093u) ^ ((uint)cy * 19349663u) ^ ((uint)cz * 83492791u))
               & (GPU_HASH_TABLE_SIZE - 1u);
    }

    // ---- 이중 밀도 완화용 SPH 커널 (Sebastian Lague / Clavet 2005) ----
    // DensityKernel = SpikyPow2: K_Spiky2 * (h-r)^2
    float SpikyPow2(float r, float h) {
        if (r >= h) return 0.0f;
        float v = h - r;
        return v * v * gKSpikyPow2;
    }
    // NearDensityKernel = SpikyPow3: K_Spiky3 * (h-r)^3
    float SpikyPow3(float r, float h) {
        if (r >= h) return 0.0f;
        float v = h - r;
        return v * v * v * gKSpikyPow3;
    }
    // 압력 구배 커널 = SpikyPow2 도함수: -2*K_Spiky2*(h-r)
    float DerivSpikyPow2(float r, float h) {
        if (r >= h) return 0.0f;
        float v = h - r;
        return -v * gKSpikyPow2Grad;
    }
    // 근압력 구배 커널 = SpikyPow3 도함수: -3*K_Spiky3*(h-r)^2
    float DerivSpikyPow3(float r, float h) {
        if (r >= h) return 0.0f;
        float v = h - r;
        return -v * v * gKSpikyPow3Grad;
    }

    float ViscLap(float r, float h) {
        if (r >= h) return 0.0f;
        return (45.0f/(PI_H*h*h*h*h*h*h)) * (h - r);
    }

    // ---- CS_HashClear: 해시 카운트 버퍼 초기화 ----
    [numthreads(64,1,1)]
    void CS_HashClear(uint3 dtid : SV_DispatchThreadID)
    {
        uint idx = dtid.x;
        if (idx < GPU_HASH_TABLE_SIZE)
            gHashCount[idx] = 0u;
    }

    // ---- CS_HashBuild: 파티클 → 해시 셀 등록 ----
    [numthreads(64,1,1)]
    void CS_HashBuild(uint3 dtid : SV_DispatchThreadID)
    {
        uint i = dtid.x;
        if ((int)i >= gParticleCount) return;
        if (gParticles[i].active == 0) return;

        float invH = 1.0f / gH;
        int cx = (int)floor(gParticles[i].pos.x * invH);
        int cy = (int)floor(gParticles[i].pos.y * invH);
        int cz = (int)floor(gParticles[i].pos.z * invH);
        uint h = CalcHashCell(cx, cy, cz);

        uint slot;
        InterlockedAdd(gHashCount[h], 1u, slot);
        if (slot < GPU_MAX_PER_CELL)
            gHashEntries[h * GPU_MAX_PER_CELL + slot] = i;
    }

    // ---- CS_Density (이중 밀도 완화: SpikyPow2 + SpikyPow3, 공간 해시 이웃 탐색) ----
    [numthreads(64,1,1)]
    void CS_Density(uint3 dtid : SV_DispatchThreadID)
    {
        uint i = dtid.x;
        if ((int)i >= gParticleCount) return;
        if (gParticles[i].active == 0) return;

        float h  = gH;
        float h2 = gH2;
        float invH = 1.0f / h;

        // 자기 기여: r=0 → SpikyPow2(0,h) = h^2 * K
        float density     = h * h * gKSpikyPow2;
        float nearDensity = 0.0f;

        int cx0 = (int)floor(gParticles[i].pos.x * invH);
        int cy0 = (int)floor(gParticles[i].pos.y * invH);
        int cz0 = (int)floor(gParticles[i].pos.z * invH);

        for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
        for (int dz = -1; dz <= 1; dz++) {
            uint hc  = CalcHashCell(cx0+dx, cy0+dy, cz0+dz);
            uint cnt = min(gHashCount[hc], GPU_MAX_PER_CELL);
            for (uint k = 0u; k < cnt; k++) {
                uint j = gHashEntries[hc * GPU_MAX_PER_CELL + k];
                if (j == i) continue;
                if (gParticles[j].active == 0) continue;
                float3 diff = gParticles[i].pos - gParticles[j].pos;
                float r2 = dot(diff, diff);
                if (r2 >= h2) continue;
                float r = sqrt(r2);
                density     += SpikyPow2(r, h);
                nearDensity += SpikyPow3(r, h);
            }
        }}}

        gParticles[i].density     = max(density, 0.001f);
        gParticles[i].nearDensity = max(nearDensity, 0.0f);
    }
)_SPH_A_"
R"_SPH_B_(
    // ---- CS_Forces (이중 밀도 완화 압력, 공간 해시 이웃 탐색) ----
    [numthreads(64,1,1)]
    void CS_Forces(uint3 dtid : SV_DispatchThreadID)
    {
        uint i = dtid.x;
        if ((int)i >= gParticleCount) return;
        if (gParticles[i].active == 0) return;

        float3 f = float3(0,0,0);

        float densityI     = gParticles[i].density;
        float nearDensityI = gParticles[i].nearDensity;
        float pressI       = gStiffness * (densityI - gRestDensity);
        float nearPressI   = gNearPressureMult * nearDensityI;

        float invH = 1.0f / gH;
        int cx0 = (int)floor(gParticles[i].pos.x * invH);
        int cy0 = (int)floor(gParticles[i].pos.y * invH);
        int cz0 = (int)floor(gParticles[i].pos.z * invH);

        // SPH 이중 밀도 압력 + 점성 (공간 해시 이웃 탐색)
        for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
        for (int dz = -1; dz <= 1; dz++) {
            uint hc  = CalcHashCell(cx0+dx, cy0+dy, cz0+dz);
            uint cnt = min(gHashCount[hc], GPU_MAX_PER_CELL);
            for (uint k = 0u; k < cnt; k++) {
                uint j = gHashEntries[hc * GPU_MAX_PER_CELL + k];
                if (j == i) continue;
                if (gParticles[j].active == 0) continue;
                float3 diff = gParticles[i].pos - gParticles[j].pos;
                float rLen = length(diff);
                if (rLen < 0.0001f || rLen >= gH) continue;

                // 이중 밀도 완화 압력 (Sebastian Lague / Clavet 2005)
                float densityJ     = max(gParticles[j].density, 0.001f);
                float nearDensityJ = max(gParticles[j].nearDensity, 0.001f);
                float pressJ       = gStiffness * (densityJ - gRestDensity);
                float nearPressJ   = gNearPressureMult * nearDensityJ;

                float sharedPress     = (pressI + pressJ) * 0.5f;
                float sharedNearPress = (nearPressI + nearPressJ) * 0.5f;

                float3 dir = diff / rLen;  // j → i 방향
                float dPress     = DerivSpikyPow2(rLen, gH);
                float dNearPress = DerivSpikyPow3(rLen, gH);

                f -= dir * (dPress * sharedPress / densityJ + dNearPress * sharedNearPress / nearDensityJ) * gParticles[j].mass;

                // 점성
                float viscLap = ViscLap(rLen, gH);
                float vScale = gViscosity * gParticles[j].mass / densityJ * viscLap;
                f += (gParticles[j].vel - gParticles[i].vel) * vScale;
            }
        }}}

        // Gravity 모드: 중력만
        if (gMotionMode == 1) {
            f += gGravityVec * gParticles[i].mass;
        } else {
            // ControlPoint 모드: 개별 파티클 radial 진동 + swirl
            // 파티클마다 다른 위상 → 구체 형태가 뭉쳤다 흩어졌다 요동치는 효과
            float particlePhase = (float)(i * 1901u % 6283) * 0.001f; // 0 ~ 2PI 균등 분포
            float radialOsc = sin(gElapsedTime * 18.85f + particlePhase); // 3 Hz 개별 진동 -1..1

            for (int c = 0; c < gCPCount; ++c) {
                // cpGroup 필터: 담당 CP만 처리 (cpGroup < 0이면 전체)
                if (gParticles[i].cpGroup >= 0 && gParticles[i].cpGroup != c) continue;

                float3 toCP = gCPs[c].position - gParticles[i].pos;
                float dist = length(toCP);
                if (dist < 0.001f) continue;

                float3 nd = toCP / dist;  // toward CP

                // 개별 진동력: 파티클마다 위상이 달라 구체가 불규칙하게 무너짐
                // radialOsc > 0 → CP 쪽으로, < 0 → 바깥으로
                float oscForce = gCPs[c].attractionStrength * 4.0f * radialOsc;
                f += nd * oscForce;

                // soft 기본 인력: 너무 멀리 날아가지 않게 약하게 앵커
                float softAttr = gCPs[c].attractionStrength * 0.35f / (1.0f + dist * 0.15f);
                f += nd * softAttr;

                // 소용돌이 접선력 (swirl 강화)
                float swirlStr = gCPs[c].attractionStrength * 0.6f;
                f += float3(nd.z * swirlStr, 0.0f, -nd.x * swirlStr);

                // 경계 복원: 2.5배 반경 밖으로 나가면 강제로 당김
                if (dist > gCPs[c].sphereRadius * 2.5f) {
                    float overshoot = dist - gCPs[c].sphereRadius * 2.5f;
                    f += nd * (450.0f * overshoot);
                }
            }
        }

        // 전역 중력
        if (gGlobalGravity > 0.0f) {
            f.y -= gGlobalGravity * gParticles[i].mass;
        }

        // ConfinementBox
        if (gBoxActive != 0) {
            float3 toP = gParticles[i].pos - gBoxCenter;
            float localX = dot(toP, gBoxAxisXH.xyz);
            float localY = dot(toP, gBoxAxisYH.xyz);
            float localZ = dot(toP, gBoxAxisZH.xyz);
            float bStiff = 200.0f;

            if (localX > gBoxAxisXH.w)
                f -= gBoxAxisXH.xyz * bStiff * (localX - gBoxAxisXH.w);
            else if (localX < -gBoxAxisXH.w)
                f += gBoxAxisXH.xyz * bStiff * (-gBoxAxisXH.w - localX);

            if (localY > gBoxAxisYH.w)
                f -= gBoxAxisYH.xyz * bStiff * (localY - gBoxAxisYH.w);
            else if (localY < -gBoxAxisYH.w)
                f += gBoxAxisYH.xyz * bStiff * (-gBoxAxisYH.w - localY);

            if (localZ > gBoxAxisZH.w)
                f -= gBoxAxisZH.xyz * bStiff * (localZ - gBoxAxisZH.w);
            else if (localZ < -gBoxAxisZH.w)
                f += gBoxAxisZH.xyz * bStiff * (-gBoxAxisZH.w - localZ);
        }

        // ── 원기둥 소프트 유도: 충돌 직전부터 옆으로 흘려보냄 (정면 감속 최소화) ──
        //   순수 반경 반발은 입자를 머리부터 세워 정체 능선을 만든다 → 반경 반발은 약하게,
        //   접선(옆) 유도는 강하게 주어 기둥 앞에서 갈라져 매끄럽게 스쳐가게 한다.
        for (int ob = 0; ob < gObstacleCount; ++ob) {
            float2 toP = float2(gParticles[i].pos.x - gObstacles[ob].x,
                                gParticles[i].pos.z - gObstacles[ob].y);
            float R    = gObstacles[ob].z;
            float infl = R + gH * 1.5f;
            float d    = length(toP);
            if (R > 0.0f && d < infl && d > 1e-4f) {
                float2 n    = toP / d;
                float2 tang = float2(-n.y, n.x);
                float ramp  = (infl - d);
                // 입자가 이미 흐르는 접선 방향으로 유도 (정체점 좌우 갈라짐 유지)
                float vt   = gParticles[i].vel.x * tang.x + gParticles[i].vel.z * tang.y;
                float side = (vt >= 0.0f) ? 1.0f : -1.0f;
                float radialPush = ramp * 35.0f;   // 약한 반경 반발 (파고들기만 방지)
                float tangPush   = ramp * 70.0f;   // 강한 접선 유도 (옆으로 흘려보냄)
                f.x += n.x * radialPush + tang.x * side * tangPush;
                f.z += n.y * radialPush + tang.y * side * tangPush;
            }
        }

        gParticles[i].force = f;
    }
)_SPH_B_"
R"_SPH_C_(
    // ---- CS_Integrate ----
    [numthreads(64,1,1)]
    void CS_Integrate(uint3 dtid : SV_DispatchThreadID)
    {
        uint i = dtid.x;
        if ((int)i >= gParticleCount) return;
        if (gParticles[i].active == 0) return;

        // CPU에서 전달한 per-frame 포지션/속도 델타 먼저 적용
        gParticles[i].pos += gPosOffset;
        gParticles[i].vel += gVelDelta;

        // ── one-shot 페이즈 조작 ──
        if (gOpFlags != 0) {
            // GPU PRNG (particle-index + frameSeed 기반)
            uint _s = (uint)i * 2654435761u ^ gOpFrameSeed;
            #define _RNG(s) (s = s * 1664525u + 1013904223u)
            #define _FRAND(s) ((float)(_RNG(s) >> 1) / 1073741823.0f)

            // ZeroAxisVelocity
            if (gOpFlags & 1) {
                float proj = dot(gParticles[i].vel, gOpZeroAxis);
                gParticles[i].vel -= gOpZeroAxis * proj;
            }
            // ApplyRandomSidewaysImpulse
            if (gOpFlags & 2) {
                float r = (_FRAND(_s) * 2.0f - 1.0f) * gOpSidewaysImpulse;
                gParticles[i].vel += gOpSidewaysAxis * r;
            }
            // ApplyAxisSpreadForce
            if (gOpFlags & 4) {
                float3 toP = gParticles[i].pos - gOpSpreadOrigin;
                float  signV = (dot(toP, gOpSpreadAxis) >= 0.0f) ? 1.0f : -1.0f;
                gParticles[i].vel += gOpSpreadAxis * (signV * gOpSpreadImpulse);
            }
            // ApplyRadialBurst
            if (gOpFlags & 8) {
                float3 bDir = gParticles[i].pos - gOpBurstCenter;
                float  bLen = length(bDir);
                bDir = (bLen > 0.001f) ? (bDir / bLen) : normalize(float3(_FRAND(_s)*2.0f-1.0f, _FRAND(_s)*2.0f-1.0f, _FRAND(_s)*2.0f-1.0f));
                float  spd  = gOpBurstMinSpeed + _FRAND(_s) * (gOpBurstMaxSpeed - gOpBurstMinSpeed);
                gParticles[i].vel += bDir * spd;
            }
        }

        // Traveling wave 수직 진동: F_y = A * sin(k * dot(pos, fwd) - ω * t)
        // 파티클마다 전진 방향 위치에 따라 다른 위상 → 물결처럼 굽이치는 효과
        if (gWaveOscEnabled > 0.5f) {
            float fwdProj  = dot(gParticles[i].pos, gWaveOscFwdDir);
            float phase    = gWaveOscWaveNumber * fwdProj - gWaveOscFrequency * gElapsedTime;
            float oscForce = gWaveOscAmplitude * sin(phase);
            gParticles[i].vel += gWaveOscUpDir * (oscForce * gDt);
        }

        float invDensity = 1.0f / max(gParticles[i].density, 0.001f);

        gParticles[i].vel += (gParticles[i].force * invDensity) * gDt;
        gParticles[i].vel *= gDamping;

        float speed2 = dot(gParticles[i].vel, gParticles[i].vel);
        if (speed2 > gMaxSpeed * gMaxSpeed) {
            gParticles[i].vel *= gMaxSpeed / sqrt(speed2);
        }

        gParticles[i].pos += gParticles[i].vel * gDt;

        // ── 보스 SPH 브레스: 바닥/원기둥 충돌 + 방 경계 + 제트 재순환 ──────────
        if (gJetActive > 0.5f && gJetConverge > 0.5f) {
            // ════ 수렴 모드: 맵 전역에 흩어진 입자가 입(gJetMouth)으로 빨려듦 (Windup 예고) ════
            gParticles[i].pad.x += gDt;
            float3 upC    = float3(0, 1, 0);
            float3 rightC = normalize(cross(upC, gJetDir));

            // 입 방향으로 조향 — SPH 속도와 블렌드해 곧게가 아니라 곡선형으로 빨려듦
            float3 toMouth = gJetMouth - gParticles[i].pos;
            float  distM   = length(toMouth);
            float3 dirM    = toMouth / max(distM, 0.001f);
            gParticles[i].vel = lerp(gParticles[i].vel, dirM * gJetSpeed, 0.30f);

            // 바닥 아래 금지 + 방 경계 클램프
            gParticles[i].pos.y = max(gParticles[i].pos.y, gJetRoomMin.y);
            gParticles[i].pos.x = clamp(gParticles[i].pos.x, gJetRoomMin.x, gJetRoomMax.x);
            gParticles[i].pos.z = clamp(gParticles[i].pos.z, gJetRoomMin.z, gJetRoomMax.z);

            // 입 도달(빨려 들어감) 또는 수명 초과 → 맵 전역 먼 곳에서 재생성
            //   수렴점에 쌓이기 전(5u)에 재순환시켜 입 근처 뭉침 방지
            float lifeJitterC = 0.55f + 0.9f * frac(sin((float)i * 12.9898f) * 43758.5453f);
            if (distM < 5.0f || gParticles[i].pad.x > gJetLifetime * lifeJitterC) {
                uint js = (uint)i * 2654435761u ^ gJetFrameSeed;
                js = js * 1664525u + 1013904223u; float ra = (float)(js >> 1) / 1073741823.0f;
                js = js * 1664525u + 1013904223u; float rb = (float)(js >> 1) / 1073741823.0f;
                js = js * 1664525u + 1013904223u; float rc = (float)(js >> 1) / 1073741823.0f;
                js = js * 1664525u + 1013904223u; float rd = (float)(js >> 1) / 1073741823.0f;

                // 진행축 [0.25,1.0]×길이 (맵 전역) + 측면 폭 + 바닥~천장 부피 높이.
                //   높이를 바닥~천장으로 펼쳐 "수평 시트가 일자로 수축" 현상 방지 → 사방에서 모임.
                float spawnAlong = (0.25f + 0.75f * ra) * gJetLength;
                gParticles[i].pos = gJetMouth
                                  + gJetDir * spawnAlong
                                  + rightC  * ((rb * 2.0f - 1.0f) * gJetSpread);
                gParticles[i].pos.y = lerp(gJetRoomMin.y + 1.0f, gJetRoomMax.y, rc); // 부피형 높이
                // 입 쪽으로 초기 속도
                float3 toM = normalize(gJetMouth - gParticles[i].pos);
                gParticles[i].vel = toM * (gJetSpeed * (0.9f + 0.2f * rd));
                gParticles[i].pad.x = 0.0f;
                gParticles[i].density = gRestDensity;
            }
        }
        else if (gJetActive > 0.5f) {
            // 파티클 수명 누적 (pad.x = age)
            gParticles[i].pad.x += gDt;

            // 가장 가까운 기둥 표면까지 거리 + 중심 + 반경 — 근처면 측면 비킴 처리.
            float  nearObs = 1e9f;
            float2 nearCtr = float2(0, 0);
            float  nearR   = 0.0f;
            for (int ob = 0; ob < gObstacleCount; ++ob) {
                float2 toC = float2(gParticles[i].pos.x - gObstacles[ob].x,
                                    gParticles[i].pos.z - gObstacles[ob].y);
                float dEdge = length(toC) - gObstacles[ob].z;
                if (dEdge < nearObs) { nearObs = dEdge; nearCtr = float2(gObstacles[ob].x, gObstacles[ob].y); nearR = gObstacles[ob].z; }
            }
            {  // 스코프 격리 — 지역 변수가 뒤쪽 재순환 코드의 동명 변수와 충돌하지 않게
            // ── 원기둥 주위 포텐셜 흐름(potential flow) 속도장 ──────────────────────────
            //   이상유체가 원기둥을 지날 때의 해석적 속도장으로 입자를 부드럽게 유도한다.
            //   앞에서 정체점→옆으로 가속하며 자연스럽게 갈라지고, 뒤에서 스스로 대칭 재합류한다.
            //   보정항이 R²/r² 로 거리와 함께 사라져 멀리 커튼은 안 건드림 → 쐐기/빈 밴드 없음.
            float2 fxz      = normalize(float2(gJetDir.x, gJetDir.z) + 1e-6f);
            float2 perpAxis = float2(-fxz.y, fxz.x);            // 진행축에 수직 (XZ)
            float  U = max(length(gParticles[i].vel.xz), gJetSpeed * 0.9f);

            float2 rel = float2(gParticles[i].pos.x - nearCtr.x,
                                gParticles[i].pos.z - nearCtr.y);
            float  px = dot(rel, fxz);          // 진행축 좌표 (<0 앞, >0 뒤)
            float  py = dot(rel, perpAxis);     // 측면 좌표 (부호 있음)
            float  r2 = px*px + py*py;

            if (nearR > 0.0f && r2 > nearR*nearR) {
                float R2 = nearR * nearR;
                float r4 = r2 * r2;
                // 균일류 U(진행축) 기준 원기둥 포텐셜 흐름 (직교좌표):
                //   u_along = U[1 - R²(x²-y²)/r⁴],  u_perp = U[-R²·2xy/r⁴]
                //   뒤쪽(px>0)에서 u_perp 가 축 방향(-py)으로 작용 → 갈라진 흐름이 다시 모인다.
                float uAlong = U * (1.0f - R2 * (px*px - py*py) / r4);
                float uPerp  = U * (-R2 * 2.0f * px * py / r4);
                // 비대칭 조정: 기둥 뒤(px>0)는 재합류(안쪽 수렴) 더 약화 → 그림자 포켓 크게.
                //   기둥 앞/옆(px<=0)은 바깥 비킴 강화 → 부딪힌 입자가 더 멀리 갈라짐.
                if (px > 0.0f) uPerp *= 1.2f;   // 뒤: 재합류 확실하게
                else           uPerp *= 3.0f;   // 앞/옆: 더 강하게 밀려 안전 포켓 넓게
                float2 potVel = fxz * uAlong + perpAxis * uPerp;
                // 근처일수록 강하게 따르고 멀면 0 (R²/r² 자체가 감쇠하지만 명시 게이트로 깔끔히)
                float infl = saturate((nearR * 7.5f - sqrt(r2)) / (nearR * 6.5f));
                float2 newV = lerp(gParticles[i].vel.xz, potVel, 0.65f * infl);
                gParticles[i].vel.x = newV.x;
                gParticles[i].vel.z = newV.y;
            }

            // 진행축 최소 속도 유지 — 전역 댐핑/정체로 느려져 벽·바닥에 쌓이는 것 방지.
            //   계속 앞으로 흐르게 해 맵 끝 뭉침을 줄인다. (역방향 "반대로 날아가는" 입자도 함께 전진 보정)
            float vAlongNow = dot(gParticles[i].vel, gJetDir);
            float vMin = gJetSpeed * 0.6f;
            if (vAlongNow < vMin) gParticles[i].vel += gJetDir * (vMin - vAlongNow);
            }  // 스코프 격리 끝

            // 바닥 충돌: 바닥 아래로 못 내려감, 하향 속도 약하게 반사 + 수평 마찰
            float floorY = gJetRoomMin.y;
            if (gParticles[i].pos.y < floorY) {
                gParticles[i].pos.y = floorY;
                if (gParticles[i].vel.y < 0.0f) gParticles[i].vel.y *= -0.15f;
                gParticles[i].vel.xz *= 0.99f;  // 바닥 마찰 약하게 → 계속 앞으로 미끄러져 맵 끝까지 커버
            }

            // 원기둥 하드 충돌: 위치만 표면 밖으로 밀어냄 (속도는 위 슬립 로직이 전담).
            //   반경 진입 속도만 제거해 파고들기 방지 — 접선/측면 방향은 슬립이 정한 대로 유지.
            for (int ob = 0; ob < gObstacleCount; ++ob) {
                float2 toP = float2(gParticles[i].pos.x - gObstacles[ob].x,
                                    gParticles[i].pos.z - gObstacles[ob].y);
                float R  = gObstacles[ob].z;
                float d2 = dot(toP, toP);
                if (R > 0.0f && d2 < R * R) {
                    float d = sqrt(max(d2, 1e-6f));
                    float2 n = toP / d;                 // 바깥 반경 방향
                    gParticles[i].pos.x = gObstacles[ob].x + n.x * R;
                    gParticles[i].pos.z = gObstacles[ob].y + n.y * R;
                    float vn = gParticles[i].vel.x * n.x + gParticles[i].vel.z * n.y;
                    if (vn < 0.0f) {            // 안으로 파고드는 속도만 제거
                        gParticles[i].vel.x -= n.x * vn;
                        gParticles[i].vel.z -= n.y * vn;
                    }
                }
            }

            // 방 경계 클램프 (벽 밖으로 새지 않게) + 화염 홍수 높이 상한 (너무 높이 안 솟게)
            gParticles[i].pos.y = min(gParticles[i].pos.y, gJetRoomMin.y + 5.0f);
)_SPH_C_"
R"_SPH_D_(
            gParticles[i].pos.x = clamp(gParticles[i].pos.x, gJetRoomMin.x, gJetRoomMax.x);
            gParticles[i].pos.z = clamp(gParticles[i].pos.z, gJetRoomMin.z, gJetRoomMax.z);

            // 제트 재순환: 수명 초과 또는 축방향 사거리 도달 → 입에서 재분사
            //   수명은 파티클별로 흩뜨려(0.55~1.45배) 동시 재순환(맥동) 방지
            float lifeJitter = 0.55f + 0.9f * frac(sin((float)i * 12.9898f) * 43758.5453f);
            float along = dot(gParticles[i].pos - gJetMouth, gJetDir);
            // 하류 벽 도달 시 즉시(투명) 재순환 — 벽에 핀처럼 박혀 쌓이는 "맵 끝 뭉침" 방지.
            //   진행 방향 성분으로만 벽 거리 측정 → 측면 벽(커튼 가장자리)은 안 건드림.
            float remX = (gJetDir.x > 0.0f) ? (gJetRoomMax.x - gParticles[i].pos.x)
                                            : (gParticles[i].pos.x - gJetRoomMin.x);
            float remZ = (gJetDir.z > 0.0f) ? (gJetRoomMax.z - gParticles[i].pos.z)
                                            : (gParticles[i].pos.z - gJetRoomMin.z);
            float wallAhead = remX * abs(gJetDir.x) + remZ * abs(gJetDir.z);
            bool atFarWall = (wallAhead < 4.0f) && (along > 30.0f);
            if (gParticles[i].pad.x > gJetLifetime * lifeJitter || along > gJetLength || atFarWall) {
                uint js = (uint)i * 2654435761u ^ gJetFrameSeed;
                js = js * 1664525u + 1013904223u; float ra = (float)(js >> 1) / 1073741823.0f;
                js = js * 1664525u + 1013904223u; float rb = (float)(js >> 1) / 1073741823.0f;
                js = js * 1664525u + 1013904223u; float rc = (float)(js >> 1) / 1073741823.0f;
                js = js * 1664525u + 1013904223u; float rd = (float)(js >> 1) / 1073741823.0f;

                // 평행 커튼: 진행축에 수직인 폭(gJetSpread=반폭) 라인 위에서 스폰,
                //   전부 같은 진행 방향으로 평행 분사 → 부채꼴 발산 없이 균일한 불의 장막.
                float3 up    = float3(0, 1, 0);
                float3 right = normalize(cross(up, gJetDir));
                gParticles[i].pos = gJetMouth
                                  + right * ((ra * 2.0f - 1.0f) * gJetSpread)
                                  + up    * ((rb * 2.0f - 1.0f) * gJetSpawnRadius);
                float speed = gJetSpeed * (0.9f + 0.2f * rd);
                // 평행 전방 속도 + 아주 작은 jitter (사방 발산 방지)
                gParticles[i].vel = gJetDir * speed
                                  + right * ((rc * 2.0f - 1.0f) * speed * 0.03f)
                                  + up    * (speed * 0.04f);
                gParticles[i].pad.x = 0.0f;
                gParticles[i].density = gRestDensity;
            }
        }

        // ── 속성색 + foam + 속도 기반 색상 ──────────────────────
        float rho    = gParticles[i].density / max(gColorRestDensity, 0.001f);
        // 제트 재분사 직후 파티클은 주변 밀도가 낮아 진한 edgeColor로 깜빡임 →
        //   age(pad.x)가 작은 동안 색 전용 rho를 1로 끌어올려 페이드인 (물리 밀도는 불변)
        if (gJetActive > 0.5f) {
            float ageFade = saturate(gParticles[i].pad.x / max(gJetLifetime * 0.25f, 0.001f));
            rho = lerp(1.0f, rho, ageFade);
        }
        float speed  = length(gParticles[i].vel);
        float speedT = saturate(speed / max(gMaxSpeed * 0.4f, 0.001f));

        // density curve: rho=1 -> t=0.85 (coreColor 지배, 속성색이 강하게 나옴)
        float t = saturate(rho * 0.85f);

        // 폭발 페이드: gExplodeFade 2.0=정상, 1.0..0.0=폭발 소멸
        // isExplodingF=1 이면 폭발 모드 (coreColor 강제 + 크기 축소)
        float isExplodingF = 1.0f - step(1.5f, gExplodeFade);
        float fadeMult = saturate(gExplodeFade); // 2.0→1.0, 1.0→1.0, 0.0→0.0

        // 색상: 정상=밀도 기반, 폭발=원소 고유색 70% 유지 (92% 강제 → 과밝아짐 방지)
        float tFinal = lerp(t, 0.7f, isExplodingF);
        float4 baseColor = lerp(gEdgeColor, gCoreColor, tFinal);

        // foam: density > threshold 일 때 coreColor 오버드라이브 (파도 거품)
        float foamT  = saturate((rho - gFoamThreshold) * 2.5f) * gFoamStrength;
        float3 foamRGB = baseColor.rgb * (1.0f + foamT * 1.2f)
                       + float3(0.25f, 0.25f, 0.25f) * foamT;

        // 속도 기반 brightness boost: 폭발 모드에서는 비활성 (고속 + 빌보드 glow → 채도 포화/흰색 방지)
        float3 finalRGB = foamRGB * (1.0f + speedT * gVelocityColorBoost * (1.0f - isExplodingF));

        // 알파: 정상=밀도 기반, 폭발=coreColor 알파로 고정 후 fadeMult로 소멸
        float normalAlpha = baseColor.a + foamT * (1.0f - baseColor.a) * 0.6f;
        float explodeAlpha = gCoreColor.a * fadeMult;
        float finalA = lerp(normalAlpha, explodeAlpha, isExplodingF);

        // 제트 재순환 페이드인: 재사용된 입자(age≈0)는 투명 상태로 입에 복귀 후
        //   서서히 나타남 → "끝→입으로 되돌아가는" 가시적 팝 제거.
        if (gJetActive > 0.5f) {
            finalA *= saturate(gParticles[i].pad.x / 0.18f);
        }
        // 수렴(차지): 입에 가까워질수록 페이드아웃 → 수렴점에 겹쳐 쌓여 하얗게 뭉치는
        //   블로우아웃 제거. 입자가 입 근처에서 사라지며 빨려드는 것처럼 보임.
        if (gJetActive > 0.5f && gJetConverge > 0.5f) {
            float dm = length(gJetMouth - gParticles[i].pos);
            finalA *= saturate((dm - 4.0f) / 14.0f);  // 입 18u 이내부터, 4u에서 완전 투명
        }

        // 속도에 따른 동적 크기
        float spd = length(gParticles[i].vel);
        float dynSize = lerp(0.42f, 0.25f, saturate(spd / max(gMaxSpeed * 0.6f, 0.001f)));
        // 폭발 모드: 0.65 기준 크기에서 시작 → 제곱으로 빠르게 소멸 (원본 스킬 크기감 유지)
        // 일반 모드: fadeMult 선형 (소멸 시 일반 축소)
        float explodeBase = lerp(dynSize, 0.65f, isExplodingF);
        float sizeScale   = lerp(fadeMult, fadeMult * fadeMult * 0.8f, isExplodingF);
        dynSize = explodeBase * sizeScale * gParticleSizeMult;

        gRenderData[i].position = gParticles[i].pos;
        gRenderData[i].size     = dynSize;
        gRenderData[i].color    = float4(finalRGB, finalA);
    }
)_SPH_D_";

// ============================================================================
// BuildSPHPipeline (static) - GPU SPH 파이프라인 빌드
// ============================================================================
void FluidParticleSystem::BuildSPHPipeline(ID3D12Device* pDevice)
{
    if (s_pSPHRootSig && s_pDensityPSO && s_pForcesPSO && s_pIntegratePSO
        && s_pHashClearPSO && s_pHashBuildPSO)
        return; // 이미 빌드됨

    HRESULT hr;
    ComPtr<ID3DBlob> errBlob;

    // Root Signature: [0] CBV b0, [1] UAV u0, [2] UAV u1, [3] SRV t0, [4] UAV u2, [5] UAV u3
    D3D12_ROOT_PARAMETER rootParams[6] = {};

    // [0] CBV b0 (root descriptor)
    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace  = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [1] UAV u0 - 파티클 상태 (root descriptor)
    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[1].Descriptor.ShaderRegister = 0;
    rootParams[1].Descriptor.RegisterSpace  = 0;
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [2] UAV u1 - 렌더 데이터 (root descriptor)
    rootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[2].Descriptor.ShaderRegister = 1;
    rootParams[2].Descriptor.RegisterSpace  = 0;
    rootParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [3] SRV t0 - 제어점 (root descriptor)
    rootParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[3].Descriptor.ShaderRegister = 0;
    rootParams[3].Descriptor.RegisterSpace  = 0;
    rootParams[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [4] UAV u2 - 공간 해시 카운트 (root descriptor)
    rootParams[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[4].Descriptor.ShaderRegister = 2;
    rootParams[4].Descriptor.RegisterSpace  = 0;
    rootParams[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [5] UAV u3 - 공간 해시 엔트리 (root descriptor)
    rootParams[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[5].Descriptor.ShaderRegister = 3;
    rootParams[5].Descriptor.RegisterSpace  = 0;
    rootParams[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = 6;
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob;
    hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
        OutputDebugStringA("[FluidPS] SPH root sig 직렬화 실패\n");
        return;
    }
    hr = pDevice->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                       IID_PPV_ARGS(&s_pSPHRootSig));
    if (FAILED(hr)) { OutputDebugStringA("[FluidPS] SPH root sig 생성 실패\n"); return; }

    // Compile 3 compute shaders
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto CompileCS = [&](const char* entryPoint, ComPtr<ID3D12PipelineState>& outPSO) -> bool
    {
        ComPtr<ID3DBlob> csBlob;
        errBlob.Reset();
        hr = D3DCompile(g_SPHComputeShader, strlen(g_SPHComputeShader), "SPHCompute",
                         nullptr, nullptr, entryPoint, "cs_5_1", compileFlags, 0, &csBlob, &errBlob);
        if (FAILED(hr))
        {
            if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
            char msg[128];
            sprintf_s(msg, "[FluidPS] CS %s 컴파일 실패\n", entryPoint);
            OutputDebugStringA(msg);
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = s_pSPHRootSig.Get();
        psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };

        hr = pDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPSO));
        if (FAILED(hr))
        {
            char msg[128];
            sprintf_s(msg, "[FluidPS] CS %s PSO 생성 실패\n", entryPoint);
            OutputDebugStringA(msg);
            return false;
        }
        return true;
    };

    if (!CompileCS("CS_HashClear", s_pHashClearPSO)) return;
    if (!CompileCS("CS_HashBuild", s_pHashBuildPSO)) return;
    if (!CompileCS("CS_Density",   s_pDensityPSO))   return;
    if (!CompileCS("CS_Forces",    s_pForcesPSO))    return;
    if (!CompileCS("CS_Integrate", s_pIntegratePSO)) return;

    OutputDebugStringA("[FluidPS] GPU SPH 파이프라인 빌드 완료 (공간 해싱 포함)\n");
}

void FluidParticleSystem::EnsureRenderPipeline(ID3D12Device* pDevice) {
    BuildSharedPipeline(pDevice);
}

// ============================================================================
// DispatchSPH - GPU 기반 SPH 시뮬레이션 실행
// ============================================================================
void FluidParticleSystem::DispatchSPH(ID3D12GraphicsCommandList* pCmdList, float dt)
{
    if (!m_bGPUInited || m_Particles.empty()) return;
    if (!s_pSPHRootSig || !s_pDensityPSO || !s_pForcesPSO || !s_pIntegratePSO) return;
    if (!s_pHashClearPSO || !s_pHashBuildPSO) return;
    if (!m_pHashCountBuffer || !m_pHashEntryBuffer) return;

    // Beam 모드: CPU에서 처리 후 GPU 렌더 버퍼로 복사
    if (m_MotionMode == ParticleMotionMode::Beam)
    {
        CopyBeamRenderDataToGPU(pCmdList);
        return;
    }

    int N = (int)m_Particles.size();

    // 1. 초기 업로드 처리
    if (m_bNeedsUpload && m_pInitUpload && m_pGPUStateBuffer)
    {
        // State buffer: COMMON -> COPY_DEST
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = m_pGPUStateBuffer.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            pCmdList->ResourceBarrier(1, &barrier);
        }

        pCmdList->CopyBufferRegion(m_pGPUStateBuffer.Get(), 0,
                                    m_pInitUpload.Get(), 0,
                                    sizeof(GPUParticle) * MAX_PARTICLES);

        // COPY_DEST -> UAV
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = m_pGPUStateBuffer.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            pCmdList->ResourceBarrier(1, &barrier);
        }

        // Render buffer도 UAV 상태로
        if (m_eGPURenderBufferState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = m_pGPURenderBuffer.Get();
            barrier.Transition.StateBefore = m_eGPURenderBufferState;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            pCmdList->ResourceBarrier(1, &barrier);
            m_eGPURenderBufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        m_bNeedsUpload = false;
    }
    else
    {
        // State buffer가 이미 UAV 상태인지 확인 (첫 프레임 이후는 UAV)
        // Render buffer도 UAV 상태로 전환
        if (m_eGPURenderBufferState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = m_pGPURenderBuffer.Get();
            barrier.Transition.StateBefore = m_eGPURenderBufferState;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            pCmdList->ResourceBarrier(1, &barrier);
            m_eGPURenderBufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
    }

    // 2. SPH CB 업데이트
    if (m_pMappedSPHCB)
    {
        SPHConstants cb = {};
        cb.particleCount = N;
        cb.h             = m_Config.smoothingRadius;
        cb.h2            = m_Config.smoothingRadius * m_Config.smoothingRadius;
        cb.restDensity   = m_Config.restDensity;
        cb.stiffness     = m_Config.stiffness;
        cb.viscosity     = m_Config.viscosity;
        cb.dt            = (std::min)(dt, 0.016f);
        // 제트(보스 브레스)는 댐핑을 거의 1로 → 속도 감소폭 최소화, 맵 끝까지 빠르게 도달
        cb.damping       = m_JetActive ? 0.9996f : 0.995f;
        cb.maxSpeed      = m_Config.maxParticleSpeed;
        cb.motionMode    = (m_MotionMode == ParticleMotionMode::Gravity) ? 1 : 0;
        cb.globalGravity = m_GlobalGravityStrength;
        cb.boxActive     = m_ConfinementBox.active ? 1 : 0;
        cb.pad0          = 0.f;
        cb.gravityVec    = m_GravityDesc.gravity;
        cb.pad1          = 0.f;
        cb.boxCenter     = m_ConfinementBox.center;
        cb.pad2          = 0.f;
        cb.boxAxisXH     = { m_ConfinementBox.axisX.x, m_ConfinementBox.axisX.y, m_ConfinementBox.axisX.z, m_ConfinementBox.halfExtents.x };
        cb.boxAxisYH     = { m_ConfinementBox.axisY.x, m_ConfinementBox.axisY.y, m_ConfinementBox.axisY.z, m_ConfinementBox.halfExtents.y };
        cb.boxAxisZH     = { m_ConfinementBox.axisZ.x, m_ConfinementBox.axisZ.y, m_ConfinementBox.axisZ.z, m_ConfinementBox.halfExtents.z };

        int cpCount = (std::min)((int)m_ControlPoints.size(), MAX_GPU_CPS);
        cb.cpCount  = cpCount;
        cb.pad3     = { 0, 0, 0 };

        cb.coreColor        = m_Colors.coreColor;
        cb.edgeColor        = m_Colors.edgeColor;
        cb.colorRestDensity = m_Config.restDensity;
        cb.pad4             = { 0, 0, 0 };

        // foam / velocity color 파라미터
        cb.foamThreshold      = 1.1f;   // restDensity 대비 1.1배 이상에서 거품 시작 (더 자주 발동)
        cb.foamStrength       = 0.75f;  // 최대 foam 강도
        cb.velocityColorBoost = 0.40f;  // 속도 기반 밝기 계수
        cb._foamPad           = 0.f;

        // Near-pressure + 커널 상수 (Sebastian Lague 이중 밀도 완화)
        {
            float h = m_Config.smoothingRadius;
            static const float PI_K = 3.14159265f;
            cb.nearPressureMult = m_Config.nearPressureMultiplier;
            cb.kSpikyPow2       = 15.0f / (2.0f * PI_K * powf(h, 5.0f));
            cb.kSpikyPow3       = 15.0f / (PI_K * powf(h, 6.0f));
            cb.kSpikyPow2Grad   = 30.0f / (2.0f * PI_K * powf(h, 5.0f));  // 2 * kSpikyPow2
            cb.kSpikyPow3Grad   = 45.0f / (PI_K * powf(h, 6.0f));         // 3 * kSpikyPow3
            cb.elapsedTime      = m_fElapsed;
            cb.explodeFade      = m_explodeFade;
            // 기본 baseline = 0.35 (FluidSkillVFXDef 기본 particleSize 기준)
            cb.particleSizeMult = (m_Config.particleSize > 0.0f) ? (m_Config.particleSize / 0.35f) : 1.0f;
        }

        // Traveling wave 수직 진동
        cb.waveOscAmplitude  = m_waveOscAmplitude;
        cb.waveOscFrequency  = m_waveOscFrequency;
        cb.waveOscWaveNumber = m_waveOscWaveNumber;
        cb.waveOscEnabled    = m_waveOscEnabled ? 1.f : 0.f;
        cb.waveOscFwdDir     = m_waveOscFwdDir;
        cb._woPad0           = 0.f;
        cb.waveOscUpDir      = m_waveOscUpDir;
        cb._woPad1           = 0.f;

        // 프레임별 CPU -> GPU 델타 (OffsetParticles / ApplyDirectionalForce 누적값)
        cb.posOffset = m_vGPUPendingOffset;
        cb._padPO    = 0.f;
        cb.velDelta  = m_vGPUPendingVelDelta;
        cb._padVD    = 0.f;

        // one-shot 페이즈 조작 누적값 -> CB
        cb.opFlags           = (int)m_uOpFlags;
        cb.opFrameSeed       = m_uOpFrameSeed;
        cb.opZeroAxis        = m_vOpZeroAxis;
        cb.opSidewaysAxis    = m_vOpSidewaysAxis;
        cb.opSidewaysImpulse = m_fOpSidewaysImpulse;
        cb.opSpreadAxis      = m_vOpSpreadAxis;
        cb.opSpreadImpulse   = m_fOpSpreadImpulse;
        cb.opSpreadOrigin    = m_vOpSpreadOrigin;
        cb.opBurstCenter     = m_vOpBurstCenter;
        cb.opBurstMinSpeed   = m_fOpBurstMinSpeed;
        cb.opBurstMaxSpeed   = m_fOpBurstMaxSpeed;

        // ── 보스 SPH 브레스: 제트 분사 + 원기둥 충돌 + 방 경계 ──
        cb.jetMouth       = m_JetMouth;
        cb.jetSpeed       = m_JetSpeed;
        cb.jetDir         = m_JetDir;
        cb.jetLength      = m_JetLength;
        cb.jetRoomMin     = m_JetRoomMin;
        cb.jetSpawnRadius = m_JetSpawnRadius;
        cb.jetRoomMax     = m_JetRoomMax;
        cb.jetSpread      = m_JetSpread;
        cb.jetActive      = m_JetActive ? 1.f : 0.f;
        cb.jetConverge    = m_JetConverge ? 1.f : 0.f;
        cb.jetLifetime    = m_JetLifetime;
        cb.jetFrameSeed   = (++m_JetFrameCounter) * 2654435761u + 12345u;
        cb.obstacleCount  = m_ObstacleCount;
        for (int o = 0; o < 4; ++o) cb.obstacles[o] = m_Obstacles[o];

        // ── 3 서브스텝 CB 준비 ──
        const UINT64 cbStride = ((sizeof(SPHConstants) + 255u) & ~255u);
        float subDt = cb.dt / 3.0f;

        // Substep 0: 전체 CB (opFlags, posOffset, velDelta 포함), dt를 subDt로
        cb.dt = subDt;
        memcpy(m_pMappedSPHCB + 0, &cb, sizeof(cb));

        // Substep 1, 2: one-shot 필드 초기화 (dt = subDt), jet 시드는 서브스텝마다 다르게
        SPHConstants cbSub = cb;
        cbSub.opFlags         = 0;
        cbSub.opFrameSeed     = 0;
        cbSub.posOffset       = { 0, 0, 0 };
        cbSub._padPO          = 0.f;
        cbSub.velDelta        = { 0, 0, 0 };
        cbSub._padVD          = 0.f;
        cbSub.jetFrameSeed    = cb.jetFrameSeed ^ 0x9E3779B9u;
        memcpy(m_pMappedSPHCB + cbStride,     &cbSub, sizeof(cbSub));
        cbSub.jetFrameSeed    = cb.jetFrameSeed ^ 0x85EBCA6Bu;
        memcpy(m_pMappedSPHCB + cbStride * 2, &cbSub, sizeof(cbSub));

        // GPU에 전달 완료 -> 초기화
        m_vGPUPendingOffset    = {};
        m_vGPUPendingVelDelta  = {};
        m_uOpFlags             = 0;
        m_uOpFrameSeed         = 0;
    }

    // 3. CP 버퍼 업데이트
    if (m_pMappedCPBuffer)
    {
        int cpCount = (std::min)((int)m_ControlPoints.size(), MAX_GPU_CPS);
        for (int c = 0; c < cpCount; ++c)
        {
            m_pMappedCPBuffer[c].position           = m_ControlPoints[c].position;
            m_pMappedCPBuffer[c].attractionStrength  = m_ControlPoints[c].attractionStrength;
            m_pMappedCPBuffer[c].sphereRadius        = m_ControlPoints[c].sphereRadius;
            m_pMappedCPBuffer[c].pad                 = { 0, 0, 0 };
        }
    }

    // 4. Compute shader dispatch (3 서브스텝 - Sebastian Lague 방식)
    UINT numGroups = ((UINT)N + 63) / 64;
    const UINT64 cbStride = ((sizeof(SPHConstants) + 255u) & ~255u);
    D3D12_GPU_VIRTUAL_ADDRESS cbBase = m_pSPHCB->GetGPUVirtualAddress();

    pCmdList->SetComputeRootSignature(s_pSPHRootSig.Get());
    pCmdList->SetComputeRootUnorderedAccessView(1, m_pGPUStateBuffer->GetGPUVirtualAddress());
    pCmdList->SetComputeRootUnorderedAccessView(2, m_pGPURenderBuffer->GetGPUVirtualAddress());
    pCmdList->SetComputeRootShaderResourceView(3,  m_pCPBuffer->GetGPUVirtualAddress());
    pCmdList->SetComputeRootUnorderedAccessView(4, m_pHashCountBuffer->GetGPUVirtualAddress());
    pCmdList->SetComputeRootUnorderedAccessView(5, m_pHashEntryBuffer->GetGPUVirtualAddress());

    // 전역 UAV 배리어 (모든 UAV 쓰기 완료 대기)
    auto GlobalUAVBarrier = [&]() {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;  // nullptr = 모든 UAV
        pCmdList->ResourceBarrier(1, &barrier);
    };

    // 파티클 상태 버퍼 전용 UAV 배리어 (기존 이름 유지)
    auto DispatchUAVBarrier = [&]() {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_pGPUStateBuffer.Get();
        pCmdList->ResourceBarrier(1, &barrier);
    };

    // 해시 클리어 dispatch 그룹 수
    UINT hashClearGroups = (GPU_HASH_TABLE_SIZE + 63) / 64;

    for (int sub = 0; sub < 3; sub++)
    {
        pCmdList->SetComputeRootConstantBufferView(0, cbBase + cbStride * sub);

        // ── 공간 해시 구축 ──────────────────────────────────────────
        // 1. HashClear: 카운트 버퍼 초기화
        pCmdList->SetPipelineState(s_pHashClearPSO.Get());
        pCmdList->Dispatch(hashClearGroups, 1, 1);
        GlobalUAVBarrier();  // HashCount 쓰기 완료 보장

        // 2. HashBuild: 파티클 위치 → 해시 셀에 등록
        pCmdList->SetPipelineState(s_pHashBuildPSO.Get());
        pCmdList->Dispatch(numGroups, 1, 1);
        GlobalUAVBarrier();  // HashCount + HashEntries 쓰기 완료 보장

        // ── SPH 패스 (공간 해시 이웃 탐색) ─────────────────────────
        // Density pass
        pCmdList->SetPipelineState(s_pDensityPSO.Get());
        pCmdList->Dispatch(numGroups, 1, 1);
        DispatchUAVBarrier();

        // Forces pass
        pCmdList->SetPipelineState(s_pForcesPSO.Get());
        pCmdList->Dispatch(numGroups, 1, 1);
        DispatchUAVBarrier();

        // Integrate pass
        pCmdList->SetPipelineState(s_pIntegratePSO.Get());
        pCmdList->Dispatch(numGroups, 1, 1);

        // UAV barrier (다음 서브스텝 전에 필요)
        if (sub < 2) DispatchUAVBarrier();
    }

    // UAV barrier (렌더 버퍼)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = m_pGPURenderBuffer.Get();
        pCmdList->ResourceBarrier(1, &barrier);
    }

    // 렌더 버퍼: UAV -> NON_PIXEL_SHADER_RESOURCE (VS에서 SRV로 읽기)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = m_pGPURenderBuffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pCmdList->ResourceBarrier(1, &barrier);
        m_eGPURenderBufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    m_nActiveCount = N;
}

// ============================================================================
// CopyBeamRenderDataToGPU - Beam 모드 CPU 렌더 데이터를 GPU 렌더 버퍼로 복사
// ============================================================================
void FluidParticleSystem::CopyBeamRenderDataToGPU(ID3D12GraphicsCommandList* pCmdList)
{
    if (!m_pMappedParticles || !m_pGPURenderBuffer || !m_pParticleBuffer) return;

    // CPU에서 렌더 데이터 업로드 (기존 UPLOAD 버퍼에)
    UploadRenderData();
    if (m_nActiveCount == 0) return;

    // GPU 렌더 버퍼를 COPY_DEST로 전환
    if (m_eGPURenderBufferState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = m_pGPURenderBuffer.Get();
        barrier.Transition.StateBefore = m_eGPURenderBufferState;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pCmdList->ResourceBarrier(1, &barrier);
    }

    // UPLOAD -> DEFAULT 복사
    pCmdList->CopyBufferRegion(m_pGPURenderBuffer.Get(), 0,
                                m_pParticleBuffer.Get(), 0,
                                sizeof(FluidParticleRenderData) * m_nActiveCount);

    // COPY_DEST -> NON_PIXEL_SHADER_RESOURCE
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = m_pGPURenderBuffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pCmdList->ResourceBarrier(1, &barrier);
        m_eGPURenderBufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
}
