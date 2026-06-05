#include "stdafx.h"
#include "GameOverScreen.h"
#include "InputSystem.h"

using namespace DirectX;

void GameOverScreen::Initialize(
    D3D12_GPU_DESCRIPTOR_HANDLE hBg,        XMUINT2 bgSize,
    D3D12_GPU_DESCRIPTOR_HANDLE hTitle,     XMUINT2 titleSize,
    D3D12_GPU_DESCRIPTOR_HANDLE hBtnNormal, XMUINT2 btnNSize,
    D3D12_GPU_DESCRIPTOR_HANDLE hBtnHover,  XMUINT2 btnHSize)
{
    m_hBg = hBg;          m_szBg    = bgSize;
    m_hTitle = hTitle;    m_szTitle = titleSize;
    m_hBtnN = hBtnNormal; m_szBtnN  = btnNSize;
    m_hBtnH = hBtnHover;  m_szBtnH  = btnHSize;
}

void GameOverScreen::Reset()
{
    m_result    = Result::None;
    m_fFade     = 0.f;
    m_fInputCD  = 0.8f;
    m_nHoverBtn = -1;
}

void GameOverScreen::Update(InputSystem& input, float screenW, float screenH, float dt)
{
    if (m_result != Result::None) return;

    float fNext = m_fFade + dt * 1.3f;
    m_fFade = (fNext > 1.0f) ? 1.0f : fNext;
    if (m_fInputCD > 0.f) { m_fInputCD -= dt; return; }

    const float btnW = 320.f, btnH = 96.f, gap = 24.f;
    float cx = screenW * 0.5f;
    float by = screenH * 0.62f;
    m_btnRetry = { cx - btnW * 0.5f, by,                btnW, btnH };
    m_btnTitle = { cx - btnW * 0.5f, by + btnH + gap,   btnW, btnH };

    XMFLOAT2 mp = input.GetMousePosition();
    auto hit = [&](const BtnRect& r) {
        return mp.x >= r.x && mp.x <= r.x + r.w &&
               mp.y >= r.y && mp.y <= r.y + r.h;
    };
    m_nHoverBtn = hit(m_btnRetry) ? 0 : (hit(m_btnTitle) ? 1 : -1);

    if (input.IsMouseButtonPressed(0)) {
        if      (m_nHoverBtn == 0) m_result = Result::Retry;
        else if (m_nHoverBtn == 1) m_result = Result::ToTitle;
    }
    if (input.IsKeyPressed(VK_RETURN) || input.IsKeyPressed(VK_SPACE))
        m_result = Result::Retry;
    if (input.IsKeyPressed(VK_ESCAPE))
        m_result = Result::ToTitle;
}

void GameOverScreen::Render(SpriteBatch* pBatch, SpriteFont* pFont,
                             float screenW, float screenH)
{
    XMVECTORF32 tint = { 1.f, 1.f, 1.f, m_fFade };

    if (m_hBg.ptr) {
        RECT dst = { 0, 0, (LONG)screenW, (LONG)screenH };
        pBatch->Draw(m_hBg, m_szBg, dst, nullptr, tint);
    }
    if (m_hTitle.ptr) {
        float titleW = (m_szTitle.x < 1) ? 1.f : (float)m_szTitle.x;
        float scale = (screenW * 0.55f) / titleW;
        float w = m_szTitle.x * scale;
        XMFLOAT2 pos((screenW - w) * 0.5f, screenH * 0.16f);
        pBatch->Draw(m_hTitle, m_szTitle, pos, nullptr, tint,
                     0.f, XMFLOAT2(0.f, 0.f), XMFLOAT2(scale, scale));
    }

    auto drawBtn = [&](const BtnRect& r, const wchar_t* label, bool hover) {
        auto h  = hover ? m_hBtnH  : m_hBtnN;
        auto sz = hover ? m_szBtnH : m_szBtnN;
        RECT dst = { (LONG)r.x, (LONG)r.y, (LONG)(r.x + r.w), (LONG)(r.y + r.h) };
        pBatch->Draw(h, sz, dst, nullptr, tint);
        XMVECTOR ts = pFont->MeasureString(label);
        float tw = XMVectorGetX(ts), th = XMVectorGetY(ts);
        XMVECTORF32 col = hover ? XMVECTORF32{ 1.f, 0.95f, 0.55f, m_fFade }
                                : XMVECTORF32{ 1.f, 1.f,   1.f,   m_fFade };
        pFont->DrawString(pBatch, label,
            XMFLOAT2(r.x + (r.w - tw) * 0.5f, r.y + (r.h - th) * 0.5f), col);
    };
    drawBtn(m_btnRetry, L"RETRY", m_nHoverBtn == 0);
    drawBtn(m_btnTitle, L"TITLE", m_nHoverBtn == 1);
}
