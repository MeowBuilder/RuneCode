#pragma once

#include "stdafx.h"

class RenderComponent;

class Shader
{
public:
    Shader();
    ~Shader();

    void Render(ID3D12GraphicsCommandList* pCommandList, D3D12_GPU_VIRTUAL_ADDRESS d3dPassCBVAddress, D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle,
                D3D12_GPU_DESCRIPTOR_HANDLE waterNormal2Handle = {}, D3D12_GPU_DESCRIPTOR_HANDLE waterHeight2Handle = {},
                D3D12_GPU_DESCRIPTOR_HANDLE foamOpacityHandle = {}, D3D12_GPU_DESCRIPTOR_HANDLE foamDiffuseHandle = {});
    void RenderShadowPass(ID3D12GraphicsCommandList* pCommandList, D3D12_GPU_VIRTUAL_ADDRESS d3dPassCBVAddress);

    void AddRenderComponent(RenderComponent* pRenderComponent);
    void RemoveRenderComponent(RenderComponent* pRenderComponent);
    void ClearRenderComponents() { m_vRenderComponents.clear(); }

    virtual void Build(ID3D12Device* pDevice);

    ID3D12RootSignature* GetRootSignature() const { return m_pd3dRootSignature.Get(); }

private:
    ComPtr<ID3D12RootSignature> m_pd3dRootSignature;
    ComPtr<ID3D12PipelineState> m_pd3dPipelineState;
    ComPtr<ID3D12PipelineState> m_pd3dShadowPSO;     // Shadow Pass PSO
    ComPtr<ID3D12PipelineState> m_pd3dWaterPSO;      // Water PSO (alpha blending)
    ComPtr<ID3D12PipelineState> m_pd3dIndicatorPSO;  // Overlay PSO (depth=ALWAYS, no depth write) — UI 느낌 인디케이터
    ComPtr<ID3D12PipelineState> m_pd3dOutlinePSO;    // Outline PSO (Cull=FRONT) — 원신풍 외곽선, g_ToonEnabled로 게이트

    std::vector<RenderComponent*> m_vRenderComponents;
};