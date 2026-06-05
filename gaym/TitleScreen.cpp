#include "stdafx.h"
#include "TitleScreen.h"
#include "InputSystem.h"

using namespace DirectX;

void TitleScreen::Initialize(
    D3D12_GPU_DESCRIPTOR_HANDLE hBg,        XMUINT2 bgSize,
    D3D12_GPU_DESCRIPTOR_HANDLE hLogo,      XMUINT2 logoSize,
    D3D12_GPU_DESCRIPTOR_HANDLE hBtnNormal, XMUINT2 btnNSize,
    D3D12_GPU_DESCRIPTOR_HANDLE hBtnHover,  XMUINT2 btnHSize)
{
    m_hBg = hBg;          m_szBg   = bgSize;
    m_hLogo = hLogo;      m_szLogo = logoSize;
    m_hBtnN = hBtnNormal; m_szBtnN = btnNSize;
    m_hBtnH = hBtnHover;  m_szBtnH = btnHSize;
}

void TitleScreen::Reset()
{
    m_result    = Result::None;
    m_fFade     = 0.f;
    m_fInputCD  = 0.3f;
    m_nHoverBtn = -1;
}

void TitleScreen::Update(InputSystem& input, float screenW, float screenH, float dt)
{
    if (m_result != Result::None) return;

    float fNext = m_fFade + dt * 1.8f;
    m_fFade = (fNext > 1.0f) ? 1.0f : fNext;
    if (m_fInputCD > 0.f) { m_fInputCD -= dt; return; }

    const float btnW = 320.f, btnH = 96.f, gap = 24.f;
    float cx = screenW * 0.5f;
    float by = screenH * 0.60f;
    m_btnStart = { cx - btnW * 0.5f, by,                btnW, btnH };
    m_btnQuit  = { cx - btnW * 0.5f, by + btnH + gap,   btnW, btnH };

    XMFLOAT2 mp = input.GetMousePosition();
    auto hit = [&](const BtnRect& r) {
        return mp.x >= r.x && mp.x <= r.x + r.w &&
               mp.y >= r.y && mp.y <= r.y + r.h;
    };
    m_nHoverBtn = hit(m_btnStart) ? 0 : (hit(m_btnQuit) ? 1 : -1);

    if (input.IsMouseButtonPressed(0)) {
        if      (m_nHoverBtn == 0) m_result = Result::StartGame;
        else if (m_nHoverBtn == 1) m_result = Result::Quit;
    }
    if (input.IsKeyPressed(VK_RETURN) || input.IsKeyPressed(VK_SPACE))
        m_result = Result::StartGame;
    if (input.IsKeyPressed(VK_ESCAPE))
        m_result = Result::Quit;
}

void TitleScreen::Render(SpriteBatch* pBatch, SpriteFont* pFont,
                          float screenW, float screenH)
{
    XMVECTORF32 tint = { 1.f, 1.f, 1.f, m_fFade };

    if (m_hBg.ptr) {
        RECT dst = { 0, 0, (LONG)screenW, (LONG)screenH };
        pBatch->Draw(m_hBg, m_szBg, dst, nullptr, tint);
    }
    if (m_hLogo.ptr) {
        float logoW = (m_szLogo.x < 1) ? 1.f : (float)m_szLogo.x;
        float scale = (screenW * 0.55f) / logoW;
        float w = m_szLogo.x * scale;
        XMFLOAT2 pos((screenW - w) * 0.5f, screenH * 0.10f);
        pBatch->Draw(m_hLogo, m_szLogo, pos, nullptr, tint,
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
    drawBtn(m_btnStart, L"START GAME", m_nHoverBtn == 0);
    drawBtn(m_btnQuit,  L"QUIT",       m_nHoverBtn == 1);
}
