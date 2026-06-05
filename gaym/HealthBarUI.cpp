#include "stdafx.h"
#include "HealthBarUI.h"
#include "d3dx12.h"

using namespace DirectX;

HealthBarUI::HealthBarUI()
    : m_nBaseWidth(0)
    , m_nBaseHeight(0)
    , m_nFillWidth(0)
    , m_nFillHeight(0)
{
    m_hBaseGPU = {};
    m_hFillGPU = {};
}

HealthBarUI::~HealthBarUI()
{
}

void HealthBarUI::Initialize(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                              DirectX::DescriptorHeap* pDescriptorHeap, UINT nBaseDescriptorIndex)
{
    // Load base texture
    std::unique_ptr<uint8_t[]> baseData;
    D3D12_SUBRESOURCE_DATA baseSubresource = {};
    CHECK_HR(LoadWICTextureFromFile(
        pDevice,
        L"Assets/Textures/UI/base.png",
        m_pBaseTexture.ReleaseAndGetAddressOf(),
        baseData,
        baseSubresource
    ));

    // Load fill texture
    std::unique_ptr<uint8_t[]> fillData;
    D3D12_SUBRESOURCE_DATA fillSubresource = {};
    CHECK_HR(LoadWICTextureFromFile(
        pDevice,
        L"Assets/Textures/UI/large_bar.png",
        m_pFillTexture.ReleaseAndGetAddressOf(),
        fillData,
        fillSubresource
    ));

    // Create upload buffers and copy data (kept as member variables until GPU completes)

    // Upload base texture
    {
        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(m_pBaseTexture.Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);

        CHECK_HR(pDevice->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_pBaseUploadBuffer)
        ));

        UpdateSubresources(pCommandList, m_pBaseTexture.Get(), m_pBaseUploadBuffer.Get(), 0, 0, 1, &baseSubresource);

        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_pBaseTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        );
        pCommandList->ResourceBarrier(1, &barrier);
    }

    // Upload fill texture
    {
        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(m_pFillTexture.Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);

        CHECK_HR(pDevice->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_pFillUploadBuffer)
        ));

        UpdateSubresources(pCommandList, m_pFillTexture.Get(), m_pFillUploadBuffer.Get(), 0, 0, 1, &fillSubresource);

        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_pFillTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        );
        pCommandList->ResourceBarrier(1, &barrier);
    }

    // Get texture dimensions
    D3D12_RESOURCE_DESC baseDesc = m_pBaseTexture->GetDesc();
    m_nBaseWidth = static_cast<UINT>(baseDesc.Width);
    m_nBaseHeight = static_cast<UINT>(baseDesc.Height);

    D3D12_RESOURCE_DESC fillDesc = m_pFillTexture->GetDesc();
    m_nFillWidth = static_cast<UINT>(fillDesc.Width);
    m_nFillHeight = static_cast<UINT>(fillDesc.Height);

    // Create SRVs
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    // Base texture SRV (index nBaseDescriptorIndex)
    srvDesc.Format = baseDesc.Format;
    pDevice->CreateShaderResourceView(
        m_pBaseTexture.Get(),
        &srvDesc,
        pDescriptorHeap->GetCpuHandle(nBaseDescriptorIndex)
    );
    m_hBaseGPU = pDescriptorHeap->GetGpuHandle(nBaseDescriptorIndex);

    // Fill texture SRV (index nBaseDescriptorIndex + 1)
    srvDesc.Format = fillDesc.Format;
    pDevice->CreateShaderResourceView(
        m_pFillTexture.Get(),
        &srvDesc,
        pDescriptorHeap->GetCpuHandle(nBaseDescriptorIndex + 1)
    );
    m_hFillGPU = pDescriptorHeap->GetGpuHandle(nBaseDescriptorIndex + 1);
}

void HealthBarUI::Render(DirectX::SpriteBatch* pSpriteBatch, float fHPRatio,
                          float fScreenWidth, float fScreenHeight)
{
    // Clamp HP ratio
    if (fHPRatio < 0.0f) fHPRatio = 0.0f;
    if (fHPRatio > 1.0f) fHPRatio = 1.0f;

    // Calculate position (top-left of screen)
    XMFLOAT2 basePos;
    basePos.x = PADDING_LEFT;
    basePos.y = PADDING_TOP;

    // 1. Avatar — base 프레임 뒤에 (초승달 테두리가 위에 보이도록)
    if (m_hAvatarGPU.ptr && m_avatarSize.x > 0)
    {
        XMFLOAT2 avatarCenter = {
            basePos.x + AVATAR_CENTER_X * SCALE,
            basePos.y + AVATAR_CENTER_Y * SCALE
        };
        float displayDiameter = AVATAR_RADIUS * 2.0f * SCALE;
        float srcDim = (m_avatarSize.x < 1) ? 1.0f : (float)m_avatarSize.x;
        float avatarScale = displayDiameter / srcDim;
        XMFLOAT2 origin(
            (float)m_avatarSize.x * 0.5f,
            (float)m_avatarSize.y * 0.5f
        );
        pSpriteBatch->Draw(
            m_hAvatarGPU,
            m_avatarSize,
            avatarCenter,
            nullptr,
            Colors::White,
            0.0f,
            origin,
            XMFLOAT2(avatarScale, avatarScale)
        );
    }

    // 2. Base frame
    XMFLOAT2 baseScale = { SCALE, SCALE };
    pSpriteBatch->Draw(
        m_hBaseGPU,
        XMUINT2(m_nBaseWidth, m_nBaseHeight),
        basePos,
        nullptr,
        Colors::White,
        0.0f,
        XMFLOAT2(0.0f, 0.0f),
        baseScale
    );

    // 3. HP Fill bar — base 의 HP 슬롯 위에 (위에서 그려야 슬롯 안쪽 검정 위로 빨강이 보임)
    if (fHPRatio > 0.0f)
    {
        LONG fillWidthClipped = static_cast<LONG>(m_nFillWidth * fHPRatio);
        RECT srcRect = { 0, 0, fillWidthClipped, static_cast<LONG>(m_nFillHeight) };

        XMFLOAT2 fillPos;
        fillPos.x = basePos.x + FILL_OFFSET_X * SCALE;
        fillPos.y = basePos.y + FILL_OFFSET_Y * SCALE;

        XMFLOAT2 fillScale = { SCALE * FILL_SCALE_X, SCALE * FILL_SCALE_Y };

        pSpriteBatch->Draw(
            m_hFillGPU,
            XMUINT2(m_nFillWidth, m_nFillHeight),
            fillPos,
            &srcRect,
            Colors::White,
            0.0f,
            XMFLOAT2(0.0f, 0.0f),
            fillScale
        );
    }
}
