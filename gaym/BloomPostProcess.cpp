#include "BloomPostProcess.h"
#include "d3dx12.h"

namespace
{
    constexpr UINT kCBAlign     = 256;
    constexpr UINT kCBSlotCount = 8;

    UINT AlignUp(UINT value, UINT alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }
}

void BloomPostProcess::Init(ID3D12Device* pDevice, UINT width, UINT height)
{
    m_width      = width;
    m_height     = height;
    m_halfWidth  = (width  / 2 > 0) ? width  / 2 : 1;
    m_halfHeight = (height / 2 > 0) ? height / 2 : 1;

    CreateRootSignature(pDevice);
    CreatePipelineStates(pDevice);
    CreateDescriptorHeaps(pDevice);
    CreateRenderTargets(pDevice, width, height);
    CreateViews(pDevice);
    CreateConstantBuffer(pDevice);
}

void BloomPostProcess::OnResize(ID3D12Device* pDevice, UINT width, UINT height)
{
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    m_width      = width;
    m_height     = height;
    m_halfWidth  = (width  / 2 > 0) ? width  / 2 : 1;
    m_halfHeight = (height / 2 > 0) ? height / 2 : 1;

    m_pCaptureRT.Reset();
    m_pBrightRT.Reset();
    m_pBlurA.Reset();
    m_pBlurB.Reset();

    m_captureState = D3D12_RESOURCE_STATE_COMMON;
    m_brightState  = D3D12_RESOURCE_STATE_COMMON;
    m_blurAState   = D3D12_RESOURCE_STATE_COMMON;
    m_blurBState   = D3D12_RESOURCE_STATE_COMMON;

    CreateRenderTargets(pDevice, width, height);
    CreateViews(pDevice);
}

void BloomPostProcess::CreateRootSignature(ID3D12Device* pDevice)
{
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        1,
        0,
        0,
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE
    );

    CD3DX12_ROOT_PARAMETER1 params[2];
    params[0].InitAsConstantBufferView(
        0,
        0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
        D3D12_SHADER_VISIBILITY_PIXEL
    );

    params[1].InitAsDescriptorTable(
        1,
        &srvRange,
        D3D12_SHADER_VISIBILITY_PIXEL
    );

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
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err);
    if (FAILED(hr))
    {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        CHECK_HR(hr);
    }
    CHECK_HR(pDevice->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          IID_PPV_ARGS(&m_pRootSig)));
    m_pRootSig->SetName(L"BloomRootSig");
}

void BloomPostProcess::CreatePipelineStates(ID3D12Device* pDevice)
{
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vs, psBright, psBlurH, psBlurV, psComposite, err;

    auto Compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out)
    {
        HRESULT hr = D3DCompileFromFile(L"bloom.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                        entry, target, compileFlags, 0, &out, &err);
        if (FAILED(hr))
        {
            if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
            CHECK_HR(hr);
        }
    };

    Compile("VS_Fullscreen", "vs_5_1", vs);
    Compile("PS_BrightPass", "ps_5_1", psBright);
    Compile("PS_BlurH",      "ps_5_1", psBlurH);
    Compile("PS_BlurV",      "ps_5_1", psBlurV);
    Compile("PS_Composite",  "ps_5_1", psComposite);

    auto BaseDesc = [&]() -> D3D12_GRAPHICS_PIPELINE_STATE_DESC
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_pRootSig.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable   = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.SampleDesc.Count = 1;
        desc.InputLayout = { nullptr, 0 };
        return desc;
    };

    // Bright / blur passes: opaque write, LDR format.
    {
        auto desc = BaseDesc();
        desc.PS = { psBright->GetBufferPointer(), psBright->GetBufferSize() };
        desc.RTVFormats[0] = kBufferFormat;
        CHECK_HR(pDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pPSOBright)));
    }
    {
        auto desc = BaseDesc();
        desc.PS = { psBlurH->GetBufferPointer(), psBlurH->GetBufferSize() };
        desc.RTVFormats[0] = kBufferFormat;
        CHECK_HR(pDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pPSOBlurH)));
    }
    {
        auto desc = BaseDesc();
        desc.PS = { psBlurV->GetBufferPointer(), psBlurV->GetBufferSize() };
        desc.RTVFormats[0] = kBufferFormat;
        CHECK_HR(pDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pPSOBlurV)));
    }
    // Composite pass: additive blend onto the swap-chain back buffer.
    {
        auto desc = BaseDesc();
        desc.PS = { psComposite->GetBufferPointer(), psComposite->GetBufferSize() };
        desc.RTVFormats[0] = kBufferFormat;

        desc.BlendState.RenderTarget[0].BlendEnable           = TRUE;
        desc.BlendState.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].DestBlend             = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ZERO;
        desc.BlendState.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        CHECK_HR(pDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pPSOComposite)));
    }

    m_pPSOBright   ->SetName(L"Bloom.BrightPSO");
    m_pPSOBlurH    ->SetName(L"Bloom.BlurHPSO");
    m_pPSOBlurV    ->SetName(L"Bloom.BlurVPSO");
    m_pPSOComposite->SetName(L"Bloom.CompositePSO");
}

void BloomPostProcess::CreateDescriptorHeaps(ID3D12Device* pDevice)
{
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = kSrvHeapSize;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK_HR(pDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_pSrvHeap)));
    m_srvIncr = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 3;  // bright, blurA, blurB
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK_HR(pDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_pRtvHeap)));
    m_rtvIncr = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

void BloomPostProcess::CreateRenderTargets(ID3D12Device* pDevice, UINT width, UINT height)
{
    auto CreateRT = [&](UINT w, UINT h, bool needRTV, ComPtr<ID3D12Resource>& out, const wchar_t* name,
                        D3D12_RESOURCE_STATES initialState)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = w;
        desc.Height           = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = kBufferFormat;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = needRTV ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = kBufferFormat;
        clear.Color[0] = 0.0f;
        clear.Color[1] = 0.0f;
        clear.Color[2] = 0.0f;
        clear.Color[3] = 1.0f;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        CHECK_HR(pDevice->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            initialState, needRTV ? &clear : nullptr,
            IID_PPV_ARGS(&out)));
        out->SetName(name);
    };

    // Capture RT is a copy-dest / pixel-shader-resource. No RTV needed.
    CreateRT(width, height, /*needRTV*/ false, m_pCaptureRT, L"Bloom.CaptureRT",
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    CreateRT(m_halfWidth, m_halfHeight, true, m_pBrightRT, L"Bloom.BrightRT",
             D3D12_RESOURCE_STATE_RENDER_TARGET);
    CreateRT(m_halfWidth, m_halfHeight, true, m_pBlurA, L"Bloom.BlurA",
             D3D12_RESOURCE_STATE_RENDER_TARGET);
    CreateRT(m_halfWidth, m_halfHeight, true, m_pBlurB, L"Bloom.BlurB",
             D3D12_RESOURCE_STATE_RENDER_TARGET);

    m_captureState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_brightState  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_blurAState   = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_blurBState   = D3D12_RESOURCE_STATE_RENDER_TARGET;
}

void BloomPostProcess::CreateViews(ID3D12Device* pDevice)
{
    auto rtvBase = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
    auto RtvAt = [&](UINT i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvBase;
        h.ptr += SIZE_T(i) * m_rtvIncr;
        return h;
    };
    m_brightRTV = RtvAt(0);
    m_blurARTV  = RtvAt(1);
    m_blurBRTV  = RtvAt(2);

    auto MakeRTV = [&](ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE h)
    {
        D3D12_RENDER_TARGET_VIEW_DESC desc = {};
        desc.Format = kBufferFormat;
        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        pDevice->CreateRenderTargetView(res, &desc, h);
    };
    MakeRTV(m_pBrightRT.Get(), m_brightRTV);
    MakeRTV(m_pBlurA.Get(),    m_blurARTV);
    MakeRTV(m_pBlurB.Get(),    m_blurBRTV);

    auto srvBase = m_pSrvHeap->GetCPUDescriptorHandleForHeapStart();
    auto SrvAt = [&](UINT i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvBase;
        h.ptr += SIZE_T(i) * m_srvIncr;
        return h;
    };

    auto MakeSRV = [&](ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE h)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = kBufferFormat;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        pDevice->CreateShaderResourceView(res, &desc, h);
    };

    MakeSRV(m_pCaptureRT.Get(), SrvAt(kSrvCapture));
    MakeSRV(m_pBrightRT.Get(),  SrvAt(kSrvBright));
    MakeSRV(m_pBlurA.Get(),     SrvAt(kSrvBlurA));
    MakeSRV(m_pBlurB.Get(),     SrvAt(kSrvBlurB));
}

void BloomPostProcess::CreateConstantBuffer(ID3D12Device* pDevice)
{
    m_cbSlotBytes = AlignUp(sizeof(BloomCB), kCBAlign);
    m_cbSlotCount = kCBSlotCount;

    CD3DX12_HEAP_PROPERTIES upload(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(SIZE_T(m_cbSlotBytes) * m_cbSlotCount);
    CHECK_HR(pDevice->CreateCommittedResource(
        &upload, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_pCB)));
    m_pCB->SetName(L"Bloom.CB");

    CD3DX12_RANGE range(0, 0);
    CHECK_HR(m_pCB->Map(0, &range, reinterpret_cast<void**>(&m_pCBMapped)));
    m_cbNextSlot = 0;
}

void BloomPostProcess::WriteCB(const BloomCB& data)
{
    memcpy(m_pCBMapped + m_cbNextSlot * m_cbSlotBytes, &data, sizeof(BloomCB));
}

void BloomPostProcess::Barrier(ID3D12GraphicsCommandList* pCmd,
                               ID3D12Resource* pResource,
                               D3D12_RESOURCE_STATES before,
                               D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    auto b = CD3DX12_RESOURCE_BARRIER::Transition(pResource, before, after);
    pCmd->ResourceBarrier(1, &b);
}

void BloomPostProcess::DrawFullscreen(ID3D12GraphicsCommandList* pCmd,
                                      ID3D12PipelineState* pPSO,
                                      D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                      UINT width, UINT height,
                                      D3D12_GPU_DESCRIPTOR_HANDLE srvTable,
                                      const BloomCB& cb)
{
    WriteCB(cb);

    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_pCB->GetGPUVirtualAddress() + SIZE_T(m_cbNextSlot) * m_cbSlotBytes;
    m_cbNextSlot = (m_cbNextSlot + 1) % m_cbSlotCount;

    pCmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (FLOAT)width, (FLOAT)height, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)width, (LONG)height };
    pCmd->RSSetViewports(1, &vp);
    pCmd->RSSetScissorRects(1, &sc);

    pCmd->SetGraphicsRootSignature(m_pRootSig.Get());
    pCmd->SetPipelineState(pPSO);
    pCmd->SetGraphicsRootConstantBufferView(0, cbAddr);
    pCmd->SetGraphicsRootDescriptorTable(1, srvTable);

    pCmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pCmd->IASetVertexBuffers(0, 0, nullptr);
    pCmd->IASetIndexBuffer(nullptr);
    pCmd->DrawInstanced(3, 1, 0, 0);
}

void BloomPostProcess::Apply(ID3D12GraphicsCommandList* pCmd,
                             ID3D12Resource* pBackBuffer,
                             D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV,
                             UINT targetWidth, UINT targetHeight)
{
    if (!m_enabled) return;

    // --- 0. Snapshot back buffer into CaptureRT ---
    Barrier(pCmd, pBackBuffer,         D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(pCmd, m_pCaptureRT.Get(),  m_captureState,                    D3D12_RESOURCE_STATE_COPY_DEST);

    pCmd->CopyResource(m_pCaptureRT.Get(), pBackBuffer);

    Barrier(pCmd, pBackBuffer,         D3D12_RESOURCE_STATE_COPY_SOURCE,  D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(pCmd, m_pCaptureRT.Get(),  D3D12_RESOURCE_STATE_COPY_DEST,    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_captureState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // Bind our shader-visible SRV heap for all bloom passes.
    ID3D12DescriptorHeap* heaps[] = { m_pSrvHeap.Get() };
    pCmd->SetDescriptorHeaps(_countof(heaps), heaps);

    auto GpuSrvAt = [&](UINT index)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_pSrvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(index) * m_srvIncr;
        return h;
    };

    // --- 1. Bright pass: capture -> bright ---
    Barrier(pCmd, m_pBrightRT.Get(), m_brightState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_brightState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    {
        BloomCB cb = {};
        cb.texelSizeX = 1.0f / (float)m_width;
        cb.texelSizeY = 1.0f / (float)m_height;
        cb.threshold  = m_threshold;
        cb.intensity  = m_intensity;
        cb.exposure   = 1.0f;
        DrawFullscreen(pCmd, m_pPSOBright.Get(), m_brightRTV,
                       m_halfWidth, m_halfHeight,
                       GpuSrvAt(kSrvCapture), cb);
    }

    // --- 2. Iterative H/V gaussian blur. Each iteration widens the radius.
    // Iteration 0: bright -> blurA (H), blurA -> blurB (V)
    // Iteration 1+: blurB -> blurA (H), blurA -> blurB (V)
    Barrier(pCmd, m_pBrightRT.Get(), m_brightState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_brightState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    const UINT iterations = (m_blurIterations == 0) ? 1 : m_blurIterations;
    for (UINT iter = 0; iter < iterations; ++iter)
    {
        UINT hSourceSrv = (iter == 0) ? kSrvBright : kSrvBlurB;

        // H pass: source -> blurA
        if (iter == 0)
        {
            Barrier(pCmd, m_pBlurA.Get(), m_blurAState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_blurAState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        else
        {
            Barrier(pCmd, m_pBlurB.Get(), m_blurBState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_blurBState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            Barrier(pCmd, m_pBlurA.Get(), m_blurAState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_blurAState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        {
            BloomCB cb = {};
            cb.texelSizeX = 1.0f / (float)m_halfWidth;
            cb.texelSizeY = 1.0f / (float)m_halfHeight;
            cb.threshold  = m_threshold;
            cb.intensity  = m_intensity;
            cb.exposure   = 1.0f;
            DrawFullscreen(pCmd, m_pPSOBlurH.Get(), m_blurARTV,
                           m_halfWidth, m_halfHeight,
                           GpuSrvAt(hSourceSrv), cb);
        }

        // V pass: blurA -> blurB
        Barrier(pCmd, m_pBlurA.Get(), m_blurAState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_blurAState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Barrier(pCmd, m_pBlurB.Get(), m_blurBState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_blurBState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        {
            BloomCB cb = {};
            cb.texelSizeX = 1.0f / (float)m_halfWidth;
            cb.texelSizeY = 1.0f / (float)m_halfHeight;
            cb.threshold  = m_threshold;
            cb.intensity  = m_intensity;
            cb.exposure   = 1.0f;
            DrawFullscreen(pCmd, m_pPSOBlurV.Get(), m_blurBRTV,
                           m_halfWidth, m_halfHeight,
                           GpuSrvAt(kSrvBlurA), cb);
        }
    }

    // --- 4. Additive composite: blurB -> back buffer (already RENDER_TARGET) ---
    Barrier(pCmd, m_pBlurB.Get(), m_blurBState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_blurBState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    {
        BloomCB cb = {};
        cb.texelSizeX = 1.0f / (float)targetWidth;
        cb.texelSizeY = 1.0f / (float)targetHeight;
        cb.threshold  = m_threshold;
        cb.intensity  = m_intensity;
        cb.exposure   = 1.0f;
        DrawFullscreen(pCmd, m_pPSOComposite.Get(), backBufferRTV,
                       targetWidth, targetHeight,
                       GpuSrvAt(kSrvBlurB), cb);
    }
}
