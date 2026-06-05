#include "stdafx.h"
#include "SkillIconRenderer.h"
#include "WICTextureLoader12.h"
#include "d3dx12.h"
#include <filesystem>

using namespace DirectX;

// ─────────────────────────────────────────────────────────────────────────────
// Initialize
// ─────────────────────────────────────────────────────────────────────────────
void SkillIconRenderer::Initialize(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList)
{
    CreatePipelineState(pDevice);

    // 디스크립터 힙 (shader-visible)
    m_pHeap = std::make_unique<DirectX::DescriptorHeap>(
        pDevice,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        HEAP_CAPACITY);

    // 슬롯 0 = 흰 텍스처
    CreateWhiteTexture(pDevice, pCommandList, m_nNextSlot);
    m_hWhiteGPU = m_pHeap->GetGpuHandle(m_nNextSlot);
    ++m_nNextSlot;

    // 스킬/룬 아이콘 폴더 일괄 스캔 (없으면 폴백으로 동작)
    LoadIconFolder(pDevice, pCommandList, L"Assets/Textures/SkillIcons");
    LoadIconFolder(pDevice, pCommandList, L"Assets/Textures/RuneIcons");
}

// ─────────────────────────────────────────────────────────────────────────────
// CreatePipelineState — 자체 루트시그/PSO/인라인 셰이더
// ─────────────────────────────────────────────────────────────────────────────
void SkillIconRenderer::CreatePipelineState(ID3D12Device* pDevice)
{
    // Root params:
    //   [0] 32bit constants b0 : rect(4) + tint(4) + ctl(4) = 12
    //   [1] descriptor table   : SRV t0 (icon texture)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0; // t0
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0; // b0
    rootParams[0].Constants.RegisterSpace  = 0;
    rootParams[0].Constants.Num32BitValues = 12;
    rootParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderRegister   = 0; // s0
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters     = 2;
    rootSigDesc.pParameters       = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers   = &samp;
    rootSigDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errorBlob;
    CHECK_HR(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob));
    CHECK_HR(pDevice->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_pRootSignature)));

    const char* shaderCode = R"(
        cbuffer IconCB : register(b0)
        {
            float4 gRect;   // NDC: x0(left), y0(top), x1(right), y1(bottom)
            float4 gTint;   // rgba multiply
            float4 gCtl;    // x=cooldownProgress(1=ready), y=hasTexture, z,w unused
        };
        Texture2D    gTex  : register(t0);
        SamplerState gSamp : register(s0);

        struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

        PSIn VSIcon(uint vid : SV_VertexID)
        {
            PSIn o;
            float2 uv = float2((vid == 1 || vid == 3) ? 1.0 : 0.0,
                               (vid >= 2)             ? 1.0 : 0.0);
            float x = lerp(gRect.x, gRect.z, uv.x);
            float y = lerp(gRect.y, gRect.w, uv.y);
            o.pos = float4(x, y, 0.0, 1.0);
            o.uv  = uv;
            return o;
        }

        float4 PSIcon(PSIn i) : SV_TARGET
        {
            float4 c = (gCtl.y > 0.5) ? gTex.Sample(gSamp, i.uv) : float4(1,1,1,1);
            c *= gTint;

            float prog = gCtl.x;
            if (prog < 0.999)
            {
                float2 d = i.uv - 0.5;
                float ang = atan2(d.x, -d.y);              // 0 at 12 o'clock, +clockwise, -pi..pi
                float a01 = ang / 6.2831853 + (ang < 0.0 ? 1.0 : 0.0); // 0..1 clockwise from top
                if (a01 > prog)
                {
                    float lum = dot(c.rgb, float3(0.299, 0.587, 0.114));
                    c.rgb = lum.xxx * 0.45;                // 어두운 회색 = 쿨다운 영역
                }
            }
            return c;
        }
    )";

    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> vsBlob, psBlob;
    CHECK_HR(D3DCompile(shaderCode, strlen(shaderCode), "SkillIconShader", nullptr, nullptr,
        "VSIcon", "vs_5_0", compileFlags, 0, &vsBlob, &errorBlob));
    CHECK_HR(D3DCompile(shaderCode, strlen(shaderCode), "SkillIconShader", nullptr, nullptr,
        "PSIcon", "ps_5_0", compileFlags, 0, &psBlob, &errorBlob));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_pRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

    psoDesc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // 비미리곱 알파 블렌딩 (PNG 투명)
    auto& rt0 = psoDesc.BlendState.RenderTarget[0];
    rt0.BlendEnable           = TRUE;
    rt0.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    rt0.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    rt0.BlendOp               = D3D12_BLEND_OP_ADD;
    rt0.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rt0.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    rt0.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable   = FALSE;  // UI 오버레이
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.DSVFormat              = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleMask             = UINT_MAX;
    psoDesc.PrimitiveTopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets       = 1;
    psoDesc.RTVFormats[0]          = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count       = 1;

    CHECK_HR(pDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pPipelineState)));
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateWhiteTexture — 1x1 흰 텍스처
// ─────────────────────────────────────────────────────────────────────────────
void SkillIconRenderer::CreateWhiteTexture(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, UINT heapSlot)
{
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
    CHECK_HR(pDevice->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_pWhiteTex)));

    const UINT64 uploadSize = GetRequiredIntermediateSize(m_pWhiteTex.Get(), 0, 1);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    ComPtr<ID3D12Resource> upload;
    CHECK_HR(pDevice->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    UINT32 whitePixel = 0xFFFFFFFF;
    D3D12_SUBRESOURCE_DATA sub = {};
    sub.pData      = &whitePixel;
    sub.RowPitch   = 4;
    sub.SlicePitch = 4;
    UpdateSubresources(pCommandList, m_pWhiteTex.Get(), upload.Get(), 0, 0, 1, &sub);

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_pWhiteTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pCommandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = 1;
    pDevice->CreateShaderResourceView(m_pWhiteTex.Get(), &srvDesc, m_pHeap->GetCpuHandle(heapSlot));

    m_uploads.push_back(upload);  // 수명 유지
}

// ─────────────────────────────────────────────────────────────────────────────
// LoadIconFile / LoadIconFolder
// ─────────────────────────────────────────────────────────────────────────────
bool SkillIconRenderer::LoadIconFile(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                                     const std::wstring& path, const std::string& key, UINT heapSlot)
{
    ComPtr<ID3D12Resource> tex;
    std::unique_ptr<uint8_t[]> decoded;
    D3D12_SUBRESOURCE_DATA sub = {};
    if (FAILED(LoadWICTextureFromFile(pDevice, path.c_str(),
                                      tex.ReleaseAndGetAddressOf(), decoded, sub)))
    {
        return false;
    }

    const UINT64 uploadSize = GetRequiredIntermediateSize(tex.Get(), 0, 1);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    ComPtr<ID3D12Resource> upload;
    CHECK_HR(pDevice->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    UpdateSubresources(pCommandList, tex.Get(), upload.Get(), 0, 0, 1, &sub);
    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pCommandList->ResourceBarrier(1, &barrier);

    D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = desc.Format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = desc.MipLevels;
    pDevice->CreateShaderResourceView(tex.Get(), &srvDesc, m_pHeap->GetCpuHandle(heapSlot));

    IconTex it;
    it.gpu    = m_pHeap->GetGpuHandle(heapSlot);
    it.width  = (UINT)desc.Width;
    it.height = (UINT)desc.Height;
    m_icons[key] = it;

    m_textures.push_back(tex);
    m_uploads.push_back(upload);
    return true;
}

void SkillIconRenderer::LoadIconFolder(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                                       const std::wstring& folderW)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path folder(folderW);
    if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec))
        return;

    for (const auto& entry : fs::directory_iterator(folder, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const fs::path& p = entry.path();
        std::wstring ext = p.extension().wstring();
        for (auto& ch : ext) ch = (wchar_t)towlower(ch);
        if (ext != L".png") continue;

        if (m_nNextSlot >= HEAP_CAPACITY)
        {
            OutputDebugStringA("[SkillIconRenderer] 디스크립터 힙 용량 초과 — 일부 아이콘 미로드\n");
            break;
        }

        std::string key = p.stem().string();  // 파일명(확장자 제외)
        if (LoadIconFile(pDevice, pCommandList, p.wstring(), key, m_nNextSlot))
            ++m_nNextSlot;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Begin / DrawIcon / DrawSolid / DrawQuad
// ─────────────────────────────────────────────────────────────────────────────
void SkillIconRenderer::Begin(ID3D12GraphicsCommandList* pCommandList, float screenWidth, float screenHeight)
{
    m_screenW = screenWidth;
    m_screenH = screenHeight;

    pCommandList->SetPipelineState(m_pPipelineState.Get());
    pCommandList->SetGraphicsRootSignature(m_pRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { m_pHeap->Heap() };
    pCommandList->SetDescriptorHeaps(1, heaps);

    pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, screenWidth, screenHeight, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)screenWidth, (LONG)screenHeight };
    pCommandList->RSSetViewports(1, &vp);
    pCommandList->RSSetScissorRects(1, &sc);
}

void SkillIconRenderer::DrawQuad(ID3D12GraphicsCommandList* pCommandList,
                                 D3D12_GPU_DESCRIPTOR_HANDLE gpu, bool hasTexture,
                                 float x, float y, float w, float h,
                                 const XMFLOAT4& tint, float cooldownProgress)
{
    // 픽셀 → NDC
    float x0 = x / m_screenW * 2.0f - 1.0f;
    float x1 = (x + w) / m_screenW * 2.0f - 1.0f;
    float yTop = 1.0f - y / m_screenH * 2.0f;
    float yBot = 1.0f - (y + h) / m_screenH * 2.0f;

    float consts[12] = {
        x0, yTop, x1, yBot,                          // gRect
        tint.x, tint.y, tint.z, tint.w,              // gTint
        cooldownProgress, hasTexture ? 1.0f : 0.0f, 0.0f, 0.0f  // gCtl
    };

    pCommandList->SetGraphicsRootDescriptorTable(1, gpu);
    pCommandList->SetGraphicsRoot32BitConstants(0, 12, consts, 0);
    pCommandList->DrawInstanced(4, 1, 0, 0);
}

void SkillIconRenderer::DrawIcon(ID3D12GraphicsCommandList* pCommandList,
                                 const std::string& key,
                                 float x, float y, float w, float h,
                                 const XMFLOAT4& fallbackColor,
                                 float cooldownProgress, float alpha)
{
    auto it = m_icons.find(key);
    if (it != m_icons.end())
    {
        XMFLOAT4 tint(1, 1, 1, alpha);
        DrawQuad(pCommandList, it->second.gpu, true, x, y, w, h, tint, cooldownProgress);
    }
    else
    {
        XMFLOAT4 tint(fallbackColor.x, fallbackColor.y, fallbackColor.z, fallbackColor.w * alpha);
        DrawQuad(pCommandList, m_hWhiteGPU, false, x, y, w, h, tint, cooldownProgress);
    }
}

void SkillIconRenderer::DrawSolid(ID3D12GraphicsCommandList* pCommandList,
                                  float x, float y, float w, float h,
                                  const XMFLOAT4& color)
{
    DrawQuad(pCommandList, m_hWhiteGPU, false, x, y, w, h, color, 1.0f);
}
