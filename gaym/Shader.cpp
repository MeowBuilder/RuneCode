#include "stdafx.h"
#include "Shader.h"
#include "RenderComponent.h"
#include <algorithm>

Shader::Shader()
{
}

Shader::~Shader()
{
}

void Shader::AddRenderComponent(RenderComponent* pRenderComponent)
{
    m_vRenderComponents.push_back(pRenderComponent);
    pRenderComponent->SetOwnerShader(this);
}

void Shader::RemoveRenderComponent(RenderComponent* pRenderComponent)
{
    m_vRenderComponents.erase(
        std::remove(m_vRenderComponents.begin(), m_vRenderComponents.end(), pRenderComponent),
        m_vRenderComponents.end());
}

void Shader::Render(ID3D12GraphicsCommandList* pCommandList, D3D12_GPU_VIRTUAL_ADDRESS d3dPassCBVAddress, D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle,
                     D3D12_GPU_DESCRIPTOR_HANDLE waterNormal2Handle, D3D12_GPU_DESCRIPTOR_HANDLE waterHeight2Handle,
                     D3D12_GPU_DESCRIPTOR_HANDLE foamOpacityHandle, D3D12_GPU_DESCRIPTOR_HANDLE foamDiffuseHandle)
{
    pCommandList->SetGraphicsRootSignature(m_pd3dRootSignature.Get());

    // Set the pass constant buffer view for root parameter 1
    pCommandList->SetGraphicsRootConstantBufferView(1, d3dPassCBVAddress);

    // Set the shadow map SRV for root parameter 3
    pCommandList->SetGraphicsRootDescriptorTable(3, shadowSrvHandle);

    // 0. Outline pass — 메인 패스 직전에 그려야 silhouette만 남음.
    //    g_ToonEnabled=0 이면 VS가 정점을 절두체 밖으로 보내 자동 무효화.
    pCommandList->SetPipelineState(m_pd3dOutlinePSO.Get());
    for (auto& pRenderComp : m_vRenderComponents)
    {
        if (!pRenderComp->IsTransparent() && !pRenderComp->IsOverlay())
            pRenderComp->Render(pCommandList);
    }

    // 1. Render opaque objects first (인디케이터/투명 제외)
    pCommandList->SetPipelineState(m_pd3dPipelineState.Get());
    for (auto& pRenderComp : m_vRenderComponents)
    {
        if (!pRenderComp->IsTransparent() && !pRenderComp->IsOverlay())
            pRenderComp->Render(pCommandList);
    }

    // 2. Render transparent objects (water) with alpha blending PSO
    pCommandList->SetPipelineState(m_pd3dWaterPSO.Get());

    // Bind additional water textures (t7~t10) if provided
    if (waterNormal2Handle.ptr != 0)
        pCommandList->SetGraphicsRootDescriptorTable(9, waterNormal2Handle);   // t7
    if (waterHeight2Handle.ptr != 0)
        pCommandList->SetGraphicsRootDescriptorTable(10, waterHeight2Handle);  // t8
    if (foamOpacityHandle.ptr != 0)
        pCommandList->SetGraphicsRootDescriptorTable(11, foamOpacityHandle);   // t9
    if (foamDiffuseHandle.ptr != 0)
        pCommandList->SetGraphicsRootDescriptorTable(12, foamDiffuseHandle);   // t10

    for (auto& pRenderComp : m_vRenderComponents)
    {
        if (pRenderComp->IsTransparent())
            pRenderComp->Render(pCommandList);
    }

    // 3. Render overlay objects (공격 인디케이터) — depth=ALWAYS, depth 미기록
    //    물/벽 뒤에 있어도 항상 맨 위에 그려져 UI 처럼 느낌
    pCommandList->SetPipelineState(m_pd3dIndicatorPSO.Get());
    for (auto& pRenderComp : m_vRenderComponents)
    {
        if (pRenderComp->IsOverlay())
            pRenderComp->Render(pCommandList);
    }
}

void Shader::RenderShadowPass(ID3D12GraphicsCommandList* pCommandList, D3D12_GPU_VIRTUAL_ADDRESS d3dPassCBVAddress)
{
    pCommandList->SetPipelineState(m_pd3dShadowPSO.Get());
    pCommandList->SetGraphicsRootSignature(m_pd3dRootSignature.Get());

    // Set the pass constant buffer view for root parameter 1
    pCommandList->SetGraphicsRootConstantBufferView(1, d3dPassCBVAddress);

    for (auto& pRenderComp : m_vRenderComponents)
    {
        // Only render objects that cast shadows
        if (pRenderComp->CastsShadow())
        {
            pRenderComp->Render(pCommandList);
        }
    }
}

void Shader::Build(ID3D12Device* pDevice)
{
    // Create a root signature with 9 parameters: Object CBV, Pass CBV, Albedo SRV(t0), Shadow SRV(t1), Normal SRV(t2), Height SRV(t3), Emissive SRV(t4), AO SRV(t5), Roughness SRV(t6)
    D3D12_ROOT_PARAMETER d3dRootParameters[13];  // 9 + 4 (t7~t10)

    // Parameter 0: Descriptor table for the per-object constant buffer (b0)
    D3D12_DESCRIPTOR_RANGE d3dDescriptorRange;
    d3dDescriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    d3dDescriptorRange.NumDescriptors = 1;
    d3dDescriptorRange.BaseShaderRegister = 0; // b0
    d3dDescriptorRange.RegisterSpace = 0;
    d3dDescriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[0].DescriptorTable.pDescriptorRanges = &d3dDescriptorRange;
    d3dRootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1: Root CBV for the per-pass constant buffer (b1)
    d3dRootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    d3dRootParameters[1].Descriptor.ShaderRegister = 1; // b1
    d3dRootParameters[1].Descriptor.RegisterSpace = 0;
    d3dRootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 2: Descriptor table for the albedo texture SRV (t0)
    D3D12_DESCRIPTOR_RANGE d3dTextureRange;
    d3dTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dTextureRange.NumDescriptors = 1;
    d3dTextureRange.BaseShaderRegister = 0; // t0
    d3dTextureRange.RegisterSpace = 0;
    d3dTextureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[2].DescriptorTable.pDescriptorRanges = &d3dTextureRange;
    d3dRootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Parameter 3: Descriptor table for the shadow map SRV (t1)
    D3D12_DESCRIPTOR_RANGE d3dShadowRange;
    d3dShadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dShadowRange.NumDescriptors = 1;
    d3dShadowRange.BaseShaderRegister = 1; // t1
    d3dShadowRange.RegisterSpace = 0;
    d3dShadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[3].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[3].DescriptorTable.pDescriptorRanges = &d3dShadowRange;
    d3dRootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Parameter 4: Descriptor table for the normal map SRV (t2)
    D3D12_DESCRIPTOR_RANGE d3dNormalMapRange;
    d3dNormalMapRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dNormalMapRange.NumDescriptors = 1;
    d3dNormalMapRange.BaseShaderRegister = 2; // t2
    d3dNormalMapRange.RegisterSpace = 0;
    d3dNormalMapRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[4].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[4].DescriptorTable.pDescriptorRanges = &d3dNormalMapRange;
    d3dRootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Parameter 5: Descriptor table for the height map SRV (t3)
    D3D12_DESCRIPTOR_RANGE d3dHeightMapRange;
    d3dHeightMapRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dHeightMapRange.NumDescriptors = 1;
    d3dHeightMapRange.BaseShaderRegister = 3; // t3
    d3dHeightMapRange.RegisterSpace = 0;
    d3dHeightMapRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[5].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[5].DescriptorTable.pDescriptorRanges = &d3dHeightMapRange;
    d3dRootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // VS에서도 접근 가능 (정점 변위용)

    // Static Samplers
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};

    // Sampler 0: Albedo texture sampler (s0)
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MipLODBias = 0;
    samplers[0].MaxAnisotropy = 1;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samplers[0].MinLOD = 0.0f;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // VS에서도 텍스처 샘플링 필요 (물 정점 변위)

    // Sampler 1: Shadow map comparison sampler (s1)
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].MipLODBias = 0;
    samplers[1].MaxAnisotropy = 1;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;  // Outside = lit
    samplers[1].MinLOD = 0.0f;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;
    samplers[1].RegisterSpace = 0;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Parameter 6: Descriptor table for the emissive texture SRV (t4)
    D3D12_DESCRIPTOR_RANGE d3dEmissiveRange;
    d3dEmissiveRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dEmissiveRange.NumDescriptors = 1;
    d3dEmissiveRange.BaseShaderRegister = 4; // t4
    d3dEmissiveRange.RegisterSpace = 0;
    d3dEmissiveRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[6].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[6].DescriptorTable.pDescriptorRanges = &d3dEmissiveRange;
    d3dRootParameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Parameter 7: Descriptor table for the AO map SRV (t5)
    D3D12_DESCRIPTOR_RANGE d3dAORange;
    d3dAORange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dAORange.NumDescriptors = 1;
    d3dAORange.BaseShaderRegister = 5; // t5
    d3dAORange.RegisterSpace = 0;
    d3dAORange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[7].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[7].DescriptorTable.pDescriptorRanges = &d3dAORange;
    d3dRootParameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Parameter 8: Descriptor table for the Roughness map SRV (t6)
    D3D12_DESCRIPTOR_RANGE d3dRoughnessRange;
    d3dRoughnessRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dRoughnessRange.NumDescriptors = 1;
    d3dRoughnessRange.BaseShaderRegister = 6; // t6
    d3dRoughnessRange.RegisterSpace = 0;
    d3dRoughnessRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[8].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[8].DescriptorTable.pDescriptorRanges = &d3dRoughnessRange;
    d3dRootParameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Parameter 9: Descriptor table for the second Normal map SRV (t7) - Water_6
    D3D12_DESCRIPTOR_RANGE d3dNormalMap2Range;
    d3dNormalMap2Range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dNormalMap2Range.NumDescriptors = 1;
    d3dNormalMap2Range.BaseShaderRegister = 7; // t7
    d3dNormalMap2Range.RegisterSpace = 0;
    d3dNormalMap2Range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[9].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[9].DescriptorTable.pDescriptorRanges = &d3dNormalMap2Range;
    d3dRootParameters[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Parameter 10: Descriptor table for the second Height map SRV (t8) - Water_6
    D3D12_DESCRIPTOR_RANGE d3dHeightMap2Range;
    d3dHeightMap2Range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dHeightMap2Range.NumDescriptors = 1;
    d3dHeightMap2Range.BaseShaderRegister = 8; // t8
    d3dHeightMap2Range.RegisterSpace = 0;
    d3dHeightMap2Range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[10].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[10].DescriptorTable.pDescriptorRanges = &d3dHeightMap2Range;
    d3dRootParameters[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;  // VS에서도 사용 (heightmap displacement)

    // Parameter 11: Descriptor table for the foam opacity SRV (t9) - foam4
    D3D12_DESCRIPTOR_RANGE d3dFoamOpacityRange;
    d3dFoamOpacityRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dFoamOpacityRange.NumDescriptors = 1;
    d3dFoamOpacityRange.BaseShaderRegister = 9; // t9
    d3dFoamOpacityRange.RegisterSpace = 0;
    d3dFoamOpacityRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[11].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[11].DescriptorTable.pDescriptorRanges = &d3dFoamOpacityRange;
    d3dRootParameters[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Parameter 12: Descriptor table for the foam diffuse SRV (t10) - foam4
    D3D12_DESCRIPTOR_RANGE d3dFoamDiffuseRange;
    d3dFoamDiffuseRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    d3dFoamDiffuseRange.NumDescriptors = 1;
    d3dFoamDiffuseRange.BaseShaderRegister = 10; // t10
    d3dFoamDiffuseRange.RegisterSpace = 0;
    d3dFoamDiffuseRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    d3dRootParameters[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    d3dRootParameters[12].DescriptorTable.NumDescriptorRanges = 1;
    d3dRootParameters[12].DescriptorTable.pDescriptorRanges = &d3dFoamDiffuseRange;
    d3dRootParameters[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;  // VS에서도 사용 (water_height_02 displacement)

    D3D12_ROOT_SIGNATURE_DESC d3dRootSignatureDesc;
    d3dRootSignatureDesc.NumParameters = 13;  // 9 -> 13
    d3dRootSignatureDesc.pParameters = d3dRootParameters;
    d3dRootSignatureDesc.NumStaticSamplers = 2;
    d3dRootSignatureDesc.pStaticSamplers = samplers;
    d3dRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> pd3dSignatureBlob;
    ComPtr<ID3DBlob> pd3dErrorBlob;
    CHECK_HR(D3D12SerializeRootSignature(&d3dRootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pd3dSignatureBlob, &pd3dErrorBlob));
    CHECK_HR(pDevice->CreateRootSignature(0, pd3dSignatureBlob->GetBufferPointer(), pd3dSignatureBlob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&m_pd3dRootSignature));

    ComPtr<ID3DBlob> vsBlob, psBlob, vsShadowBlob, vsOutlineBlob, psOutlineBlob;
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    CHECK_HR(D3DCompileFromFile(L"shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS", "vs_5_1", compileFlags, 0, &vsBlob, &pd3dErrorBlob));
    CHECK_HR(D3DCompileFromFile(L"shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS", "ps_5_1", compileFlags, 0, &psBlob, &pd3dErrorBlob));
    CHECK_HR(D3DCompileFromFile(L"shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS_Shadow", "vs_5_1", compileFlags, 0, &vsShadowBlob, &pd3dErrorBlob));
    CHECK_HR(D3DCompileFromFile(L"shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VS_Outline", "vs_5_1", compileFlags, 0, &vsOutlineBlob, &pd3dErrorBlob));
    CHECK_HR(D3DCompileFromFile(L"shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PS_Outline", "ps_5_1", compileFlags, 0, &psOutlineBlob, &pd3dErrorBlob));

    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BONEINDICES", 0, DXGI_FORMAT_R32G32B32A32_SINT, 3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BONEWEIGHTS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Main PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = m_pd3dRootSignature.Get();
    psoDesc.VS = { reinterpret_cast<BYTE*>(vsBlob->GetBufferPointer()), vsBlob->GetBufferSize() };
    psoDesc.PS = { reinterpret_cast<BYTE*>(psBlob->GetBufferPointer()), psBlob->GetBufferSize() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    CHECK_HR(pDevice->CreateGraphicsPipelineState(&psoDesc, __uuidof(ID3D12PipelineState), (void**)&m_pd3dPipelineState));

    // Shadow PSO (depth only, no pixel shader)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPsoDesc = psoDesc;
    shadowPsoDesc.VS = { reinterpret_cast<BYTE*>(vsShadowBlob->GetBufferPointer()), vsShadowBlob->GetBufferSize() };
    shadowPsoDesc.PS = { nullptr, 0 };  // No pixel shader
    shadowPsoDesc.NumRenderTargets = 0;
    shadowPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    shadowPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;  // Shadow map uses 32-bit depth
    shadowPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // No culling: handles negative-scale (mirrored) objects correctly
    shadowPsoDesc.RasterizerState.DepthBias = 10000;
    shadowPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    shadowPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;

    CHECK_HR(pDevice->CreateGraphicsPipelineState(&shadowPsoDesc, __uuidof(ID3D12PipelineState), (void**)&m_pd3dShadowPSO));

    // Water PSO (alpha blending enabled)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC waterPsoDesc = psoDesc;
    waterPsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    waterPsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    waterPsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    waterPsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    waterPsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    waterPsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    waterPsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    waterPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;  // Don't write to depth for transparent

    CHECK_HR(pDevice->CreateGraphicsPipelineState(&waterPsoDesc, __uuidof(ID3D12PipelineState), (void**)&m_pd3dWaterPSO));

    // Indicator PSO — water 위에 그리되 보스/벽은 정상 occlude (depth=LESS 유지)
    // DepthWriteMask=ZERO 로 depth 기록만 생략 → 뒤에 그려지는 다른 게 인디케이터 때문에 가려지지 않음.
    // 알파 블렌딩 — 인디케이터 가장자리 soft fade. 알파=1 인 기존 오브젝트는 변화 없음.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC indicatorPsoDesc = psoDesc;
    indicatorPsoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    indicatorPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    indicatorPsoDesc.BlendState.RenderTarget[0].BlendEnable    = TRUE;
    indicatorPsoDesc.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    indicatorPsoDesc.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    indicatorPsoDesc.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    indicatorPsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    indicatorPsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    indicatorPsoDesc.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    CHECK_HR(pDevice->CreateGraphicsPipelineState(&indicatorPsoDesc, __uuidof(ID3D12PipelineState), (void**)&m_pd3dIndicatorPSO));

    // Outline PSO — Inverted Hull (월드스페이스 normal 푸시). Cull=FRONT 로
    // 카메라 뒤쪽 면만 그려 silhouette ring 형성. 스킨드 메시 한정 적용
    // (VS에서 비-스킨드는 절두체 밖으로 보냄).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC outlinePsoDesc = psoDesc;
    outlinePsoDesc.VS = { reinterpret_cast<BYTE*>(vsOutlineBlob->GetBufferPointer()), vsOutlineBlob->GetBufferSize() };
    outlinePsoDesc.PS = { reinterpret_cast<BYTE*>(psOutlineBlob->GetBufferPointer()), psOutlineBlob->GetBufferSize() };
    outlinePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    CHECK_HR(pDevice->CreateGraphicsPipelineState(&outlinePsoDesc, __uuidof(ID3D12PipelineState), (void**)&m_pd3dOutlinePSO));
}
