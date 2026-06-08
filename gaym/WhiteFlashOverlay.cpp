#include "stdafx.h"
#include "WhiteFlashOverlay.h"
#include "d3dx12.h"
#include <algorithm>

namespace
{
    constexpr UINT kCBAlign = 256;

    UINT AlignUp(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }
}

void WhiteFlashOverlay::Init(ID3D12Device* pDevice)
{
    CreateRootSignature(pDevice);
    CreatePipelineState(pDevice);
    CreateConstantBuffer(pDevice);
}

void WhiteFlashOverlay::CreateRootSignature(ID3D12Device* pDevice)
{
    CD3DX12_ROOT_PARAMETER1 params[1];
    params[0].InitAsConstantBufferView(0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
    desc.Init_1_1(_countof(params), params, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&desc,
        D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err);
    if (FAILED(hr))
    {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        CHECK_HR(hr);
    }
    CHECK_HR(pDevice->CreateRootSignature(0,
        sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_pRootSig)));
    m_pRootSig->SetName(L"WhiteFlashRootSig");
}

void WhiteFlashOverlay::CreatePipelineState(ID3D12Device* pDevice)
{
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vs, ps, err;

    auto Compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out)
    {
        HRESULT hr = D3DCompileFromFile(L"whiteflash.hlsl", nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
            compileFlags, 0, &out, &err);
        if (FAILED(hr))
        {
            if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
            CHECK_HR(hr);
        }
    };
    Compile("VS_Fullscreen", "vs_5_1", vs);
    Compile("PS_Flash",      "ps_5_1", ps);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_pRootSig.Get();
    desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    // 알파 블렌딩 ON: result = src.rgb * src.a + dst.rgb * (1 - src.a)
    desc.BlendState.RenderTarget[0].BlendEnable    = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    desc.DepthStencilState.DepthEnable   = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0]    = kRTVFormat;
    desc.SampleDesc.Count = 1;
    desc.InputLayout = { nullptr, 0 };

    CHECK_HR(pDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pPSO)));
    m_pPSO->SetName(L"WhiteFlashPSO");
}

void WhiteFlashOverlay::CreateConstantBuffer(ID3D12Device* pDevice)
{
    m_cbSlotBytes = AlignUp((UINT)sizeof(FlashCB), kCBAlign);
    UINT cbSize = m_cbSlotBytes * kCBSlotCount;

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
    CHECK_HR(pDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_pCB)));
    m_pCB->SetName(L"WhiteFlashCB");

    CD3DX12_RANGE noRead(0, 0);
    CHECK_HR(m_pCB->Map(0, &noRead, reinterpret_cast<void**>(&m_pCBMapped)));
}

void WhiteFlashOverlay::RequestFlash(float peakAlpha, float duration,
                                     DirectX::XMFLOAT3 color)
{
    if (peakAlpha <= 0.0f || duration <= 0.0f) return;

    // 이미 활성 중이면 더 강한/긴 쪽 우선
    if (peakAlpha > m_peakAlpha || duration > m_remaining)
    {
        m_peakAlpha = (std::max)(m_peakAlpha, peakAlpha);
        m_duration  = (std::max)(m_duration,  duration);
        m_remaining = (std::max)(m_remaining, duration);
        m_color     = color;
    }
}

void WhiteFlashOverlay::Tick(float rawDt)
{
    if (m_remaining <= 0.0f) return;
    m_remaining -= rawDt;
    if (m_remaining < 0.0f) m_remaining = 0.0f;
}

void WhiteFlashOverlay::Apply(ID3D12GraphicsCommandList* pCmd,
                              D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV,
                              UINT width, UINT height)
{
    if (m_remaining <= 0.0f || m_duration <= 0.0f) return;

    // 알파 envelope: ease-out (t*t) — 처음 1~2 프레임 peak 유지, 빠르게 감쇠
    //   "딱 번쩍" 느낌 → 선형보다 강한 임팩트.
    float t = m_remaining / m_duration;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    t = t * t;   // ease-out
    float alpha = m_peakAlpha * t;
    if (alpha < 0.001f) return;

    // CB 업로드
    FlashCB cb{};
    cb.color[0] = m_color.x;
    cb.color[1] = m_color.y;
    cb.color[2] = m_color.z;
    cb.color[3] = alpha;
    UINT slot = m_cbNextSlot;
    m_cbNextSlot = (m_cbNextSlot + 1) % kCBSlotCount;
    UINT8* dst = m_pCBMapped + slot * m_cbSlotBytes;
    memcpy(dst, &cb, sizeof(cb));
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_pCB->GetGPUVirtualAddress() + slot * m_cbSlotBytes;

    // PSO + RS
    pCmd->SetGraphicsRootSignature(m_pRootSig.Get());
    pCmd->SetPipelineState(m_pPSO.Get());
    pCmd->SetGraphicsRootConstantBufferView(0, cbAddr);

    pCmd->OMSetRenderTargets(1, &backBufferRTV, FALSE, nullptr);

    D3D12_VIEWPORT vp = { 0, 0, (float)width, (float)height, 0, 1 };
    pCmd->RSSetViewports(1, &vp);
    D3D12_RECT sc = { 0, 0, (LONG)width, (LONG)height };
    pCmd->RSSetScissorRects(1, &sc);

    pCmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pCmd->IASetVertexBuffers(0, 0, nullptr);
    pCmd->IASetIndexBuffer(nullptr);
    pCmd->DrawInstanced(3, 1, 0, 0);  // 풀스크린 삼각형
}
