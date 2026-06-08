#pragma once
#include "stdafx.h"
#include <DirectXMath.h>

// ─── WhiteFlashOverlay ───────────────────────────────────────────────────────
//
// 검기 임팩트용 풀스크린 알파-블렌딩 플래시. Bloom 직후, RenderText 직전에
// Apply() 호출. 활성 플래시가 없으면 즉시 return → 매 프레임 호출해도 안전.
//
//   m_pWhiteFlash->RequestFlash(0.55f, 0.12f);   // peak alpha 0.55, 0.12s fade-out
//   m_pWhiteFlash->Tick(rawDt);                  // 매 프레임 1회
//   m_pWhiteFlash->Apply(cmd, backBufferRTV, w, h);  // Bloom Apply 직후
//
// HLSL: whiteflash.hlsl (VS_Fullscreen / PS_Flash)
class WhiteFlashOverlay
{
public:
    WhiteFlashOverlay() = default;
    ~WhiteFlashOverlay() = default;

    void Init(ID3D12Device* pDevice);

    // 활성 플래시 요청. peakAlpha 0~1, duration 초.
    //   color 기본 흰색. 원소색 펄스로 활용 가능.
    void RequestFlash(float peakAlpha, float duration,
                      DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f });

    // 매 프레임 호출 (raw dt 사용 — hit-stop 영향 X)
    void Tick(float rawDt);

    // 풀스크린 알파 블렌딩 패스. 활성 플래시 없으면 즉시 return.
    //   pBackBufferRTV: 현재 swap-chain RTV (RENDER_TARGET 상태로 들어왔다 가정)
    void Apply(ID3D12GraphicsCommandList* pCmd,
               D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV,
               UINT width, UINT height);

    bool IsActive() const { return m_remaining > 0.0f; }
    void Cancel()         { m_remaining = 0.0f; }

private:
    struct FlashCB
    {
        float color[4];   // xyz=RGB, w=alpha
    };

    void CreateRootSignature(ID3D12Device* pDevice);
    void CreatePipelineState(ID3D12Device* pDevice);
    void CreateConstantBuffer(ID3D12Device* pDevice);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_pRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pPSO;

    // CB ring buffer (단순화: 8 슬롯 round-robin)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_pCB;
    UINT8* m_pCBMapped = nullptr;
    UINT   m_cbSlotBytes = 0;
    UINT   m_cbNextSlot  = 0;
    static constexpr UINT kCBSlotCount = 8;

    // 플래시 상태
    float            m_peakAlpha = 0.0f;
    float            m_duration  = 0.0f;
    float            m_remaining = 0.0f;
    DirectX::XMFLOAT3 m_color    = { 1.0f, 1.0f, 1.0f };

    static constexpr DXGI_FORMAT kRTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
};
