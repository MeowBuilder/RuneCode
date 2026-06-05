#pragma once

#include "stdafx.h"
#include <SpriteBatch.h>
#include "WICTextureLoader12.h"  // Use local version
#include <DescriptorHeap.h>

class HealthBarUI
{
public:
    HealthBarUI();
    ~HealthBarUI();

    void Initialize(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                    DirectX::DescriptorHeap* pDescriptorHeap, UINT nBaseDescriptorIndex);

    void Render(DirectX::SpriteBatch* pSpriteBatch, float fHPRatio,
                float fScreenWidth, float fScreenHeight);

    // 캐릭터 원소에 맞는 초상화. Dx12App 가 매 프레임 갱신.
    void SetAvatar(D3D12_GPU_DESCRIPTOR_HANDLE h, DirectX::XMUINT2 size)
    {
        m_hAvatarGPU = h;
        m_avatarSize = size;
    }

private:
    ComPtr<ID3D12Resource> m_pBaseTexture;
    ComPtr<ID3D12Resource> m_pFillTexture;

    // Upload buffers (must stay alive until GPU upload completes)
    ComPtr<ID3D12Resource> m_pBaseUploadBuffer;
    ComPtr<ID3D12Resource> m_pFillUploadBuffer;

    D3D12_GPU_DESCRIPTOR_HANDLE m_hBaseGPU;
    D3D12_GPU_DESCRIPTOR_HANDLE m_hFillGPU;

    // 외부에서 주입되는 아바타 (Dx12App 의 UI 디스크립터 슬롯)
    D3D12_GPU_DESCRIPTOR_HANDLE m_hAvatarGPU = {};
    DirectX::XMUINT2            m_avatarSize = {};

    UINT m_nBaseWidth;
    UINT m_nBaseHeight;
    UINT m_nFillWidth;
    UINT m_nFillHeight;

    // Layout constants — 새 base.png: 2172x724, large_bar(crop): ~2013x307
    static constexpr float PADDING_LEFT = 20.0f;
    static constexpr float PADDING_TOP  = 20.0f;
    static constexpr float SCALE        = 0.28f;  // base 2172px → 표시 ~608px

    // HP 슬롯 좌상단 in base.png 픽셀 좌표 (스크린샷 측정 기반)
    static constexpr float FILL_OFFSET_X = 625.0f;
    static constexpr float FILL_OFFSET_Y = 175.0f;

    // HP 슬롯에 fill (2013x307) 을 맞추는 스케일
    static constexpr float FILL_SCALE_X = 0.64f;
    static constexpr float FILL_SCALE_Y = 0.58f;

    // 아바타 초승달 안쪽 중심/반경 (base 픽셀)
    static constexpr float AVATAR_CENTER_X = 450.0f;
    static constexpr float AVATAR_CENTER_Y = 380.0f;
    static constexpr float AVATAR_RADIUS   = 230.0f;
};
