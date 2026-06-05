#pragma once
#include "stdafx.h"
#include <SpriteBatch.h>
#include <SpriteFont.h>
#include <DirectXMath.h>

// 로딩 화면: 배경 + 회전 스피너 + "LOADING..." 텍스트.
// Dx12App가 m_fElapsed >= kMinShow 이후 InitSceneWithElement 호출 → AppState::Playing.
class LoadingScreen
{
public:
    void Initialize(D3D12_GPU_DESCRIPTOR_HANDLE hBg,      DirectX::XMUINT2 bgSize,
                    D3D12_GPU_DESCRIPTOR_HANDLE hSpinner, DirectX::XMUINT2 spinnerSize);

    void Update(float dt);
    void Render(DirectX::SpriteBatch* pBatch, DirectX::SpriteFont* pFont,
                float screenW, float screenH);

    float GetElapsed() const { return m_fElapsed; }
    void  Reset();

private:
    D3D12_GPU_DESCRIPTOR_HANDLE m_hBg{}, m_hSpinner{};
    DirectX::XMUINT2 m_szBg{}, m_szSpinner{};
    float m_fElapsed = 0.f;
    float m_fRot     = 0.f;
};
