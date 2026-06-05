#pragma once
#include "stdafx.h"
#include <SpriteBatch.h>
#include <SpriteFont.h>
#include <DirectXMath.h>

class InputSystem;

// 게임오버 화면: 플레이어 사망 시 진입. 배경 + 타이틀 + 버튼 2개 (RETRY / TITLE).
// 트리거(PlayerComponent::TakeDamage HP<=0)는 후속 작업.
class GameOverScreen
{
public:
    enum class Result { None, Retry, ToTitle, Quit };

    void Initialize(D3D12_GPU_DESCRIPTOR_HANDLE hBg,        DirectX::XMUINT2 bgSize,
                    D3D12_GPU_DESCRIPTOR_HANDLE hTitle,     DirectX::XMUINT2 titleSize,
                    D3D12_GPU_DESCRIPTOR_HANDLE hBtnNormal, DirectX::XMUINT2 btnNSize,
                    D3D12_GPU_DESCRIPTOR_HANDLE hBtnHover,  DirectX::XMUINT2 btnHSize);

    void Update(InputSystem& input, float screenW, float screenH, float dt);
    void Render(DirectX::SpriteBatch* pBatch, DirectX::SpriteFont* pFont,
                float screenW, float screenH);

    Result GetResult() const { return m_result; }
    void   Reset();

private:
    D3D12_GPU_DESCRIPTOR_HANDLE m_hBg{}, m_hTitle{}, m_hBtnN{}, m_hBtnH{};
    DirectX::XMUINT2 m_szBg{}, m_szTitle{}, m_szBtnN{}, m_szBtnH{};

    Result m_result    = Result::None;
    float  m_fFade     = 0.f;
    float  m_fInputCD  = 0.8f;
    int    m_nHoverBtn = -1;

    struct BtnRect { float x, y, w, h; };
    BtnRect m_btnRetry{}, m_btnTitle{};
};
