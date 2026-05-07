#include "stdafx.h"
#include "CharacterSelectScreen.h"
#include "CharacterData.h"
#include "InputSystem.h"
#include "D3dx12.h"
#include <SpriteFont.h>
#include <ResourceUploadBatch.h>

// ─── 초기화 ─────────────────────────────────────────────────────────────────

void CharacterSelectScreen::Initialize(
    ID3D12Device*                pDevice,
    ID3D12GraphicsCommandList*   pCommandList,
    DirectX::DescriptorHeap*     pDescriptorHeap,
    UINT                         nWhitePixelSlot,
    ID3D12CommandQueue*          pCmdQueue)
{
    // 1×1 흰색 RGBA 텍스처 생성 (SpriteBatch 컬러 사각형용)
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = 1;
    texDesc.Height           = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc       = { 1, 0 };
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    pDevice->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_pWhiteTex));
    m_pWhiteTex->SetName(L"CharSelectWhitePixel");

    // 업로드 힙 (1픽셀 = 4바이트)
    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    pDevice->GetCopyableFootprints(&texDesc, 0, 1, 0, &layout, nullptr, nullptr, &uploadSize);

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadBuf  = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    pDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadBuf,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pWhiteUpload));

    // 픽셀 데이터: 불투명 흰색
    uint8_t white[4] = { 255, 255, 255, 255 };
    uint8_t* pMapped = nullptr;
    m_pWhiteUpload->Map(0, nullptr, reinterpret_cast<void**>(&pMapped));
    memcpy(pMapped + layout.Offset, white, 4);
    m_pWhiteUpload->Unmap(0, nullptr);

    // Copy upload → GPU texture
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource        = m_pWhiteTex.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource       = m_pWhiteUpload.Get();
    src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layout;

    pCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_pWhiteTex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    pCommandList->ResourceBarrier(1, &barrier);

    // SRV 생성
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels    = 1;

    pDevice->CreateShaderResourceView(
        m_pWhiteTex.Get(), &srvDesc,
        pDescriptorHeap->GetCpuHandle(nWhitePixelSlot));
    m_hWhiteGPU = pDescriptorHeap->GetGpuHandle(nWhitePixelSlot);
}

// ─── 레이아웃 계산 ───────────────────────────────────────────────────────────

void CharacterSelectScreen::ComputeCardLayout(float screenW, float screenH)
{
    m_fScreenW = screenW;
    m_fScreenH = screenH;

    const float cardW    = 220.f;
    const float cardH    = 340.f;
    const float gap      = 30.f;
    const float totalW   = kCharCount * cardW + (kCharCount - 1) * gap;
    const float startX   = (screenW - totalW) * 0.5f;
    const float cardY    = (screenH - cardH) * 0.5f - 20.f;

    for (int i = 0; i < kCharCount; ++i)
    {
        m_cards[i] = { startX + i * (cardW + gap), cardY, cardW, cardH };
    }
}

// ─── 업데이트 ────────────────────────────────────────────────────────────────

void CharacterSelectScreen::Update(InputSystem& input, float screenW, float screenH, float deltaTime)
{
    m_fFadeIn += deltaTime * 2.0f;
    if (m_fFadeIn > 1.0f) m_fFadeIn = 1.0f;
    m_fPulse += deltaTime * 3.0f;

    ComputeCardLayout(screenW, screenH);

    XMFLOAT2 mouse = input.GetMousePosition();
    m_nHoverIndex = -1;
    for (int i = 0; i < kCharCount; ++i)
    {
        const CardRect& c = m_cards[i];
        if (mouse.x >= c.x && mouse.x <= c.x + c.w &&
            mouse.y >= c.y && mouse.y <= c.y + c.h)
        {
            m_nHoverIndex = i;
        }
    }

    // 마우스 클릭으로 선택
    if (input.IsMouseButtonPressed(0) && m_nHoverIndex >= 0)
    {
        if (m_nHighlightIndex == m_nHoverIndex)
        {
            m_bConfirmed = true;  // 이미 선택된 카드를 다시 클릭하면 확정
        }
        else
        {
            m_nHighlightIndex = m_nHoverIndex;
        }
    }

    // 키보드: 좌우 화살표 (누른 채로 유지 시 0.2s 간격 반복)
    if (m_fKeyDebounce > 0.f) m_fKeyDebounce -= deltaTime;
    if (m_fKeyDebounce <= 0.f)
    {
        if (input.IsKeyDown(VK_LEFT))
        {
            m_nHighlightIndex = (m_nHighlightIndex - 1 + kCharCount) % kCharCount;
            m_fKeyDebounce = 0.20f;
        }
        else if (input.IsKeyDown(VK_RIGHT))
        {
            m_nHighlightIndex = (m_nHighlightIndex + 1) % kCharCount;
            m_fKeyDebounce = 0.20f;
        }
    }

    if (input.IsKeyPressed('1')) m_nHighlightIndex = 0;
    if (input.IsKeyPressed('2')) m_nHighlightIndex = 1;
    if (input.IsKeyPressed('3')) m_nHighlightIndex = 2;
    if (input.IsKeyPressed('4')) m_nHighlightIndex = 3;

    if (input.IsKeyPressed(VK_RETURN) || input.IsKeyPressed(VK_SPACE))
        m_bConfirmed = true;
}

// ─── 렌더링 헬퍼 ────────────────────────────────────────────────────────────

void CharacterSelectScreen::DrawRect(DirectX::SpriteBatch* pBatch,
    float x, float y, float w, float h, XMFLOAT4 col)
{
    if (!m_hWhiteGPU.ptr) return;
    RECT destRect = {
        static_cast<LONG>(x), static_cast<LONG>(y),
        static_cast<LONG>(x + w), static_cast<LONG>(y + h)
    };
    XMVECTORF32 tint = { col.x, col.y, col.z, col.w };
    pBatch->Draw(m_hWhiteGPU, XMUINT2(1, 1), destRect, tint);
}

// ─── 렌더링 ─────────────────────────────────────────────────────────────────

void CharacterSelectScreen::Render(DirectX::SpriteBatch* pBatch, DirectX::SpriteFont* pFont,
    float screenW, float screenH)
{
    const float fade = m_fFadeIn;

    // 배경 어둡게
    DrawRect(pBatch, 0, 0, screenW, screenH, { 0.05f, 0.05f, 0.08f, 0.92f * fade });

    // 타이틀
    {
        const wchar_t* title = L"캐릭터 선택";
        XMVECTOR sz = pFont->MeasureString(title);
        float tx = (screenW - XMVectorGetX(sz)) * 0.5f;
        float ty = m_cards[0].y - 90.f;
        // 그림자
        pFont->DrawString(pBatch, title, XMFLOAT2(tx + 2, ty + 2),
            XMVECTORF32{ 0, 0, 0, 0.6f * fade });
        pFont->DrawString(pBatch, title, XMFLOAT2(tx, ty),
            XMVECTORF32{ 1.0f, 0.9f, 0.6f, fade });
    }

    // 조작 안내
    {
        const wchar_t* hint = L"[1-4] or 마우스로 선택  |  [Enter/Space] or 더블클릭으로 확정";
        XMVECTOR sz = pFont->MeasureString(hint);
        float tx = (screenW - XMVectorGetX(sz)) * 0.5f;
        float ty = m_cards[0].y + m_cards[0].h + 24.f;
        pFont->DrawString(pBatch, hint, XMFLOAT2(tx, ty),
            XMVECTORF32{ 0.7f, 0.7f, 0.7f, 0.8f * fade });
    }

    int count = 0;
    const CharacterData* table = GetAllCharacterData(&count);

    for (int i = 0; i < count; ++i)
    {
        const CharacterData& cd = table[i];
        const CardRect& c = m_cards[i];

        bool isSelected = (i == m_nHighlightIndex);
        bool isHovered  = (i == m_nHoverIndex);

        // 선택된 카드는 살짝 위로 올림
        float yOff = isSelected ? -12.f : (isHovered ? -5.f : 0.f);

        // 카드 배경 외곽선
        if (isSelected)
        {
            float pulse = 0.5f + 0.5f * sinf(m_fPulse);
            XMFLOAT4 border = { cd.cardTextColor.x, cd.cardTextColor.y,
                                 cd.cardTextColor.z, (0.6f + 0.4f * pulse) * fade };
            DrawRect(pBatch, c.x - 3, c.y + yOff - 3, c.w + 6, c.h + 6, border);
        }
        else if (isHovered)
        {
            DrawRect(pBatch, c.x - 2, c.y + yOff - 2, c.w + 4, c.h + 4,
                { 0.7f, 0.7f, 0.7f, 0.35f * fade });
        }

        // 카드 배경
        XMFLOAT4 bg = cd.cardBgColor;
        bg.w *= fade;
        DrawRect(pBatch, c.x, c.y + yOff, c.w, c.h, bg);

        // 원소 색상 헤더 띠
        XMFLOAT4 hdr = cd.cardTextColor;
        hdr.w = 0.75f * fade;
        DrawRect(pBatch, c.x, c.y + yOff, c.w, 8.f, hdr);

        // 캐릭터 번호 인덱스
        {
            wchar_t numBuf[4];
            swprintf_s(numBuf, L"%d", i + 1);
            XMVECTOR sz = pFont->MeasureString(numBuf);
            float nx = c.x + (c.w - XMVectorGetX(sz)) * 0.5f;
            float ny = c.y + yOff + 20.f;
            XMFLOAT4 tc = cd.cardTextColor;
            pFont->DrawString(pBatch, numBuf, XMFLOAT2(nx, ny),
                XMVECTORF32{ tc.x, tc.y, tc.z, 0.5f * fade });
        }

        // 캐릭터 이름
        {
            XMVECTOR sz = pFont->MeasureString(cd.name);
            float nx = c.x + (c.w - XMVectorGetX(sz)) * 0.5f;
            float ny = c.y + yOff + 60.f;
            XMFLOAT4 tc = isSelected ? cd.cardTextColor : XMFLOAT4{ 0.9f, 0.9f, 0.9f, 1.f };
            pFont->DrawString(pBatch, cd.name, XMFLOAT2(nx, ny),
                XMVECTORF32{ tc.x, tc.y, tc.z, fade });
        }

        // 구분선
        DrawRect(pBatch, c.x + 16, c.y + yOff + 105.f, c.w - 32, 1.f,
            { 0.5f, 0.5f, 0.5f, 0.4f * fade });

        // 속성 레이블
        {
            const wchar_t* elemLabel = L"";
            switch (cd.element) {
            case ElementType::Fire:  elemLabel = L"[불꽃]";  break;
            case ElementType::Water: elemLabel = L"[물결]";  break;
            case ElementType::Wind:  elemLabel = L"[바람]";  break;
            case ElementType::Earth: elemLabel = L"[대지]";  break;
            default: break;
            }
            XMVECTOR sz = pFont->MeasureString(elemLabel);
            float ex = c.x + (c.w - XMVectorGetX(sz)) * 0.5f;
            float ey = c.y + yOff + 115.f;
            XMFLOAT4 tc = cd.cardTextColor;
            pFont->DrawString(pBatch, elemLabel, XMFLOAT2(ex, ey),
                XMVECTORF32{ tc.x, tc.y, tc.z, fade });
        }

        // 선택 상태 표시
        if (isSelected)
        {
            const wchar_t* selText = L"[ 선택됨 ]";
            XMVECTOR sz = pFont->MeasureString(selText);
            float sx = c.x + (c.w - XMVectorGetX(sz)) * 0.5f;
            float sy = c.y + yOff + c.h - 40.f;
            float pulse = 0.7f + 0.3f * sinf(m_fPulse);
            XMFLOAT4 tc = cd.cardTextColor;
            pFont->DrawString(pBatch, selText, XMFLOAT2(sx, sy),
                XMVECTORF32{ tc.x, tc.y, tc.z, pulse * fade });
        }
    }

    // 하단: 게임 시작 안내
    if (m_nHighlightIndex >= 0)
    {
        const wchar_t* startMsg = L"Enter / Space — 게임 시작";
        XMVECTOR sz = pFont->MeasureString(startMsg);
        float mx = (screenW - XMVectorGetX(sz)) * 0.5f;
        float my = m_cards[0].y + m_cards[0].h + 60.f;
        float pulse = 0.5f + 0.5f * sinf(m_fPulse * 0.8f);
        pFont->DrawString(pBatch, startMsg, XMFLOAT2(mx, my),
            XMVECTORF32{ 1.0f, 1.0f, 1.0f, pulse * fade });
    }
}

// ─── 게터 ────────────────────────────────────────────────────────────────────

ElementType CharacterSelectScreen::GetSelectedElement() const
{
    int count = 0;
    const CharacterData* table = GetAllCharacterData(&count);
    if (m_nHighlightIndex >= 0 && m_nHighlightIndex < count)
        return table[m_nHighlightIndex].element;
    return ElementType::Water;
}

void CharacterSelectScreen::Reset()
{
    m_bConfirmed      = false;
    m_nHighlightIndex = 1;
    m_fFadeIn         = 0.f;
    m_fPulse          = 0.f;
    m_nHoverIndex     = -1;
    m_fKeyDebounce    = 0.f;
}
