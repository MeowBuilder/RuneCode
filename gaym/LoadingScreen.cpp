#include "stdafx.h"
#include "LoadingScreen.h"

using namespace DirectX;

void LoadingScreen::Initialize(
    D3D12_GPU_DESCRIPTOR_HANDLE hBg,      XMUINT2 bgSize,
    D3D12_GPU_DESCRIPTOR_HANDLE hSpinner, XMUINT2 spinnerSize)
{
    m_hBg = hBg;           m_szBg      = bgSize;
    m_hSpinner = hSpinner; m_szSpinner = spinnerSize;
}

void LoadingScreen::Reset()
{
    m_fElapsed = 0.f;
    m_fRot     = 0.f;
}

void LoadingScreen::Update(float dt)
{
    m_fElapsed += dt;
    m_fRot     += dt * 3.0f;  // 3 rad/sec 회전
}

void LoadingScreen::Render(SpriteBatch* pBatch, SpriteFont* pFont,
                            float screenW, float screenH)
{
    if (m_hBg.ptr) {
        RECT dst = { 0, 0, (LONG)screenW, (LONG)screenH };
        pBatch->Draw(m_hBg, m_szBg, dst);
    }
    if (m_hSpinner.ptr) {
        float targetSz = screenH * 0.18f;
        float spW      = (m_szSpinner.x < 1) ? 1.f : (float)m_szSpinner.x;
        float scale    = targetSz / spW;
        XMFLOAT2 pos(screenW * 0.5f, screenH * 0.5f);
        XMFLOAT2 origin((float)m_szSpinner.x * 0.5f, (float)m_szSpinner.y * 0.5f);
        pBatch->Draw(m_hSpinner, m_szSpinner, pos, nullptr, Colors::White,
                     m_fRot, origin, XMFLOAT2(scale, scale));
    }

    const wchar_t* text = L"LOADING...";
    XMVECTOR ts = pFont->MeasureString(text);
    float tw = XMVectorGetX(ts);
    pFont->DrawString(pBatch, text,
        XMFLOAT2((screenW - tw) * 0.5f, screenH * 0.75f),
        Colors::White);
}
