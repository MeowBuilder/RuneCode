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

    // nBaseDescriptorIndex: base(+0), fill(+1) 두 슬롯 사용.
    // nShieldDescriptorIndex: 보호막 fill(파란 바) 전용 슬롯 (base+1 과 겹치지 않는 별도 인덱스).
    void Initialize(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                    DirectX::DescriptorHeap* pDescriptorHeap, UINT nBaseDescriptorIndex,
                    UINT nShieldDescriptorIndex);

    void Render(DirectX::SpriteBatch* pSpriteBatch, float fHPRatio, float fShieldRatio,
                float fScreenWidth, float fScreenHeight);

    // 캐릭터 원소에 맞는 초상화. Dx12App 가 매 프레임 갱신.
    void SetAvatar(D3D12_GPU_DESCRIPTOR_HANDLE h, DirectX::XMUINT2 size)
    {
        m_hAvatarGPU = h;
        m_avatarSize = size;
    }

    // HP fill 바의 화면 사각형 (HP/보호막 숫자 텍스트 오버레이 정렬용)
    void GetFillRect(float& x, float& y, float& w, float& h) const
    {
        x = PADDING_LEFT + FILL_OFFSET_X * SCALE;
        y = PADDING_TOP  + FILL_OFFSET_Y * SCALE;
        w = static_cast<float>(m_nFillWidth)  * SCALE * FILL_SCALE_X;
        h = static_cast<float>(m_nFillHeight) * SCALE * FILL_SCALE_Y;
    }

private:
    ComPtr<ID3D12Resource> m_pBaseTexture;
    ComPtr<ID3D12Resource> m_pFillTexture;
    ComPtr<ID3D12Resource> m_pShieldTexture;   // 보호막 fill (small_bar.png, 파란 바)

    // Upload buffers (must stay alive until GPU upload completes)
    ComPtr<ID3D12Resource> m_pBaseUploadBuffer;
    ComPtr<ID3D12Resource> m_pFillUploadBuffer;
    ComPtr<ID3D12Resource> m_pShieldUploadBuffer;

    D3D12_GPU_DESCRIPTOR_HANDLE m_hBaseGPU;
    D3D12_GPU_DESCRIPTOR_HANDLE m_hFillGPU;
    D3D12_GPU_DESCRIPTOR_HANDLE m_hShieldGPU{};

    // 외부에서 주입되는 아바타 (Dx12App 의 UI 디스크립터 슬롯)
    D3D12_GPU_DESCRIPTOR_HANDLE m_hAvatarGPU = {};
    DirectX::XMUINT2            m_avatarSize = {};

    UINT m_nBaseWidth;
    UINT m_nBaseHeight;
    UINT m_nFillWidth;
    UINT m_nFillHeight;
    UINT m_nShieldWidth  = 1;
    UINT m_nShieldHeight = 1;

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
