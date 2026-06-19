#include "stdafx.h"
#include "ScreenSplitOverlay.h"
#include "d3dx12.h"

namespace
{
    constexpr UINT kCBAlign = 256;
    UINT AlignUp(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }
}

void ScreenSplitOverlay::Init(ID3D12Device* pDevice, UINT width, UINT height)
{
    m_width  = width;
    m_height = height;

    CreateRootSignature(pDevice);
    CreatePipelineState(pDevice);
    CreateDescriptorHeaps(pDevice);
    CreateCaptureRT(pDevice, width, height);
    CreateConstantBuffer(pDevice);
}

void ScreenSplitOverlay::OnResize(ID3D12Device* pDevice, UINT width, UINT height)
{
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;
    m_width  = width;
    m_height = height;
    m_pCaptureRT.Reset();
    m_captureState = D3D12_RESOURCE_STATE_COMMON;
    CreateCaptureRT(pDevice, width, height);
}

void ScreenSplitOverlay::CreateRootSignature(ID3D12Device* pDevice)
{
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                  D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);

    CD3DX12_ROOT_PARAMETER1 params[2];
    params[0].InitAsConstantBufferView(0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        D3D12_SHADER_VISIBILITY_PIXEL);
    params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(
        0,
        D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
    desc.Init_1_1(_countof(params), params, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&desc,
        D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); CHECK_HR(hr); }
    CHECK_HR(pDevice->CreateRootSignature(0,
        sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_pRootSig)));
    m_pRootSig->SetName(L"ScreenSplitRootSig");
}

void ScreenSplitOverlay::CreatePipelineState(ID3D12Device* pDevice)
{
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vs, ps, err;
    auto Compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out)
    {
        HRESULT hr = D3DCompileFromFile(L"screen_split.hlsl", nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
            compileFlags, 0, &out, &err);
        if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); CHECK_HR(hr); }
    };
    Compile("VS_Fullscreen", "vs_5_1", vs);
    Compile("PS_Split",      "ps_5_1", ps);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_pRootSig.Get();
    desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.DepthStencilState.DepthEnable   = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0]    = kRTFormat;
    desc.SampleDesc.Count = 1;
    desc.InputLayout = { nullptr, 0 };

    CHECK_HR(pDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pPSO)));
    m_pPSO->SetName(L"ScreenSplitPSO");
}

void ScreenSplitOverlay::CreateDescriptorHeaps(ID3D12Device* pDevice)
{
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = 1;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK_HR(pDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_pSrvHeap)));
    m_srvIncr = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void ScreenSplitOverlay::CreateCaptureRT(ID3D12Device* pDevice, UINT w, UINT h)
{
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width              = w;
    rd.Height             = h;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.Format             = kRTFormat;
    rd.SampleDesc.Count   = 1;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags              = D3D12_RESOURCE_FLAG_NONE;

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    CHECK_HR(pDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &rd, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&m_pCaptureRT)));
    m_pCaptureRT->SetName(L"ScreenSplitCaptureRT");
    m_captureState = D3D12_RESOURCE_STATE_COPY_DEST;

    // SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format                  = kRTFormat;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels     = 1;
    pDevice->CreateShaderResourceView(m_pCaptureRT.Get(), &srv,
        m_pSrvHeap->GetCPUDescriptorHandleForHeapStart());
}

void ScreenSplitOverlay::CreateConstantBuffer(ID3D12Device* pDevice)
{
    m_cbSlotBytes = AlignUp((UINT)sizeof(SplitCB), kCBAlign);
    UINT cbSize = m_cbSlotBytes * kCBSlotCount;

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
    CHECK_HR(pDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_pCB)));
    m_pCB->SetName(L"ScreenSplitCB");
    CD3DX12_RANGE noRead(0, 0);
    CHECK_HR(m_pCB->Map(0, &noRead, reinterpret_cast<void**>(&m_pCBMapped)));
}

void ScreenSplitOverlay::RequestSplit(float duration, float angleRad, float peakOffsetUV)
{
    if (duration <= 0.0f) return;
    m_duration   = duration;
    m_angleRad   = angleRad;
    m_peakOffset = peakOffsetUV;
    m_progress   = 0.001f;   // 활성 표시 (0.001 → IsActive true)
    m_elapsed    = 0.0f;
}

void ScreenSplitOverlay::Tick(float rawDt)
{
    if (m_progress <= 0.0f || m_duration <= 0.0f) return;
    m_elapsed += rawDt;
    m_progress = m_elapsed / m_duration;
    if (m_progress >= 1.0f) m_progress = 1.001f;  // 종료 표시
}

void ScreenSplitOverlay::Apply(ID3D12GraphicsCommandList* pCmd,
                                ID3D12Resource* pBackBuffer,
                                D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV,
                                UINT width, UINT height)
{
    if (m_progress <= 0.0f || m_progress >= 1.0f) return;
    if (!pBackBuffer) return;

    // ── 1) BackBuffer → Capture RT 복사 ────────────────────────────────────
    //   barrier 는 StateBefore == StateAfter 시 invalid → command list 무효화.
    //   각 자원별로 현재 상태가 목표와 다를 때만 transition.
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        UINT n = 0;
        // backbuffer: RENDER_TARGET → COPY_SOURCE (항상 필요 — Dx12App 가 RT 로 두고 호출)
        barriers[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[n].Transition.pResource   = pBackBuffer;
        barriers[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[n].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ++n;
        // capture RT: m_captureState → COPY_DEST (이미 COPY_DEST 면 skip)
        if (m_captureState != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            barriers[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[n].Transition.pResource   = m_pCaptureRT.Get();
            barriers[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[n].Transition.StateBefore = m_captureState;
            barriers[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            ++n;
            m_captureState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        if (n > 0) pCmd->ResourceBarrier(n, barriers);
    }
    pCmd->CopyResource(m_pCaptureRT.Get(), pBackBuffer);

    // ── 2) Capture RT → PIXEL_SHADER_RESOURCE, BackBuffer → RENDER_TARGET ──
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        UINT n = 0;
        if (m_captureState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        {
            barriers[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[n].Transition.pResource   = m_pCaptureRT.Get();
            barriers[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[n].Transition.StateBefore = m_captureState;
            barriers[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            ++n;
            m_captureState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        // backbuffer: COPY_SOURCE → RENDER_TARGET (방금 위에서 COPY_SOURCE 로 전환했음)
        barriers[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[n].Transition.pResource   = pBackBuffer;
        barriers[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[n].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ++n;
        if (n > 0) pCmd->ResourceBarrier(n, barriers);
    }

    // ── 3) CB 업로드 ────────────────────────────────────────────────────────
    SplitCB cb{};
    cb.params[0] = (m_progress > 1.0f) ? 1.0f : m_progress;   // ease 적용은 셰이더가 함
    cb.params[1] = m_peakOffset;
    cb.params[2] = m_angleRad;
    cb.params[3] = m_slitWidth;
    // Aspect (W/H) — 16:9 화면에서 UV 공간 -45° ≠ 화면 -45° 인 mismatch 보정.
    cb.params2[0] = (height > 0) ? (float)width / (float)height : 1.0f;
    cb.params2[1] = 0.0f;
    cb.params2[2] = 0.0f;
    cb.params2[3] = 0.0f;
    UINT slot = m_cbNextSlot;
    m_cbNextSlot = (m_cbNextSlot + 1) % kCBSlotCount;
    UINT8* dst = m_pCBMapped + slot * m_cbSlotBytes;
    memcpy(dst, &cb, sizeof(cb));
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_pCB->GetGPUVirtualAddress() + slot * m_cbSlotBytes;

    // ── 4) Split PSO 호출 — 풀스크린 삼각형, capture 를 input 으로 backbuffer 에 출력 ──
    pCmd->SetGraphicsRootSignature(m_pRootSig.Get());
    pCmd->SetPipelineState(m_pPSO.Get());
    ID3D12DescriptorHeap* heaps[] = { m_pSrvHeap.Get() };
    pCmd->SetDescriptorHeaps(1, heaps);
    pCmd->SetGraphicsRootConstantBufferView(0, cbAddr);
    pCmd->SetGraphicsRootDescriptorTable(1, m_pSrvHeap->GetGPUDescriptorHandleForHeapStart());

    pCmd->OMSetRenderTargets(1, &backBufferRTV, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)width, (float)height, 0, 1 };
    pCmd->RSSetViewports(1, &vp);
    D3D12_RECT sc = { 0, 0, (LONG)width, (LONG)height };
    pCmd->RSSetScissorRects(1, &sc);

    pCmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pCmd->IASetVertexBuffers(0, 0, nullptr);
    pCmd->IASetIndexBuffer(nullptr);
    pCmd->DrawInstanced(3, 1, 0, 0);
}
