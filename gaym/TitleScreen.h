#pragma once
#include "stdafx.h"
#include <SpriteBatch.h>
#include <SpriteFont.h>
#include <DirectXMath.h>

class InputSystem;

// 메인 타이틀 화면: 배경 + 로고 이미지 + START/QUIT 버튼.
// 텍스처는 Dx12App가 슬롯/크기 형태로 주입한다 (m_fontDescriptorHeap 슬롯 핸들).
class TitleScreen
{
public:
    enum class Result { None, StartGame, Quit };

    void Initialize(D3D12_GPU_DESCRIPTOR_HANDLE hBg,        DirectX::XMUINT2 bgSize,
                    D3D12_GPU_DESCRIPTOR_HANDLE hLogo,      DirectX::XMUINT2 logoSize,
                    D3D12_GPU_DESCRIPTOR_HANDLE hBtnNormal, DirectX::XMUINT2 btnNSize,
                    D3D12_GPU_DESCRIPTOR_HANDLE hBtnHover,  DirectX::XMUINT2 btnHSize);

    void Update(InputSystem& input, float screenW, float screenH, float dt);
    void Render(DirectX::SpriteBatch* pBatch, DirectX::SpriteFont* pFont,
                float screenW, float screenH);

    Result GetResult() const { return m_result; }
    void   Reset();

private:
    D3D12_GPU_DESCRIPTOR_HANDLE m_hBg{}, m_hLogo{}, m_hBtnN{}, m_hBtnH{};
    DirectX::XMUINT2 m_szBg{}, m_szLogo{}, m_szBtnN{}, m_szBtnH{};

    Result m_result    = Result::None;
    float  m_fFade     = 0.f;
    float  m_fInputCD  = 0.3f;
    int    m_nHoverBtn = -1;

    struct BtnRect { float x, y, w, h; };
    BtnRect m_btnStart{}, m_btnQuit{};
};
