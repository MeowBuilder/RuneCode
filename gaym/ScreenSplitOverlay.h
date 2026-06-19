#pragma once
#include "stdafx.h"
#include <DirectXMath.h>

// ─── ScreenSplitOverlay ──────────────────────────────────────────────────────
//
// 화면 베기 직후 backbuffer 를 2 조각으로 분리해 슬라이드 시키는 풀스크린 후처리.
// BloomPostProcess 패턴: backbuffer → capture RT 복사 → split shader 가 capture
// 를 input 으로 화면을 두 조각으로 그려서 다시 backbuffer 에 출력.
//
//   m_pSplit->RequestSplit(0.6f, -0.785f);   // 0.6s, -45° (베기 각도)
//   m_pSplit->Tick(rawDt);                   // 매 프레임
//   m_pSplit->Apply(cmd, backBuffer, rtv, w, h);  // Bloom 직후
//
// HLSL: screen_split.hlsl (VS_Fullscreen / PS_Split)
class ScreenSplitOverlay
{
public:
    ScreenSplitOverlay() = default;
    ~ScreenSplitOverlay() = default;

    void Init(ID3D12Device* pDevice, UINT width, UINT height);
    void OnResize(ID3D12Device* pDevice, UINT width, UINT height);

    // 분리 시작 요청.
    //   duration  : 전체 분리 진행 시간 (초)
    //   angleRad  : 베기 라인 각도 (라디안). 화면 좌표계 -PI/4 = -45°
    //   peakOffsetUV : UV 공간 최대 분리 폭 (보통 0.15~0.30)
    void RequestSplit(float duration, float angleRad, float peakOffsetUV = 0.20f);

    // 매 프레임 호출 — progress 진행.
    void Tick(float rawDt);

    // 분리 후처리 패스 적용. Bloom 직후, WhiteFlash 전 호출.
    void Apply(ID3D12GraphicsCommandList* pCmd,
               ID3D12Resource* pBackBuffer,
               D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV,
               UINT width, UINT height);

    bool IsActive() const { return m_progress > 0.0f && m_progress < 1.0f; }
    void Cancel()         { m_progress = 0.0f; m_duration = 0.0f; }

private:
    struct SplitCB
    {
        float params[4];    // x=progress, y=peakOffset, z=angle, w=slitWidth
        float params2[4];   // x=aspect (W/H), y/z/w=reserved
    };

    void CreateRootSignature(ID3D12Device* pDevice);
    void CreatePipelineState(ID3D12Device* pDevice);
    void CreateDescriptorHeaps(ID3D12Device* pDevice);
    void CreateCaptureRT(ID3D12Device* pDevice, UINT w, UINT h);
    void CreateConstantBuffer(ID3D12Device* pDevice);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_pRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pPSO;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_pSrvHeap;
    UINT m_srvIncr = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_pCaptureRT;
    D3D12_RESOURCE_STATES m_captureState = D3D12_RESOURCE_STATE_COMMON;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_pCB;
    UINT8* m_pCBMapped = nullptr;
    UINT   m_cbSlotBytes = 0;
    UINT   m_cbNextSlot  = 0;
    static constexpr UINT kCBSlotCount = 8;

    // 분리 상태
    float m_progress       = 0.0f;
    float m_duration       = 0.0f;
    float m_elapsed        = 0.0f;
    float m_angleRad       = -0.7853982f;   // -45°
    float m_peakOffset     = 0.20f;         // UV 공간 최대 분리
    float m_slitWidth      = 0.005f;        // 가운데 슬릿 base 폭

    UINT m_width  = 0;
    UINT m_height = 0;

    static constexpr DXGI_FORMAT kRTFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
};
