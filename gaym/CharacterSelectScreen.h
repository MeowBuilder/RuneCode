#pragma once
#include "stdafx.h"
#include <SpriteBatch.h>
#include <SpriteFont.h>
#include <DescriptorHeap.h>
#include "SkillTypes.h"

class InputSystem;

class CharacterSelectScreen
{
public:
    CharacterSelectScreen() = default;

    // pDescriptorHeap: 공유 폰트 디스크립터 힙, nWhitePixelSlot: 흰 픽셀 텍스처 슬롯 번호
    void Initialize(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                    DirectX::DescriptorHeap* pDescriptorHeap, UINT nWhitePixelSlot,
                    ID3D12CommandQueue* pCmdQueue);

    void Update(InputSystem& input, float screenW, float screenH, float deltaTime);
    void Render(DirectX::SpriteBatch* pBatch, DirectX::SpriteFont* pFont,
                float screenW, float screenH);

    bool        IsConfirmed()        const { return m_bConfirmed; }
    ElementType GetSelectedElement() const;
    void        Reset();

    // 다른 모듈(Dx12App 의 컷씬 fullscreen flash 등) 이 같은 1×1 흰 픽셀을 재사용하기 위한 접근자.
    D3D12_GPU_DESCRIPTOR_HANDLE GetWhitePixelGpuHandle() const { return m_hWhiteGPU; }

private:
    static constexpr int kCharCount = 4;

    int   m_nHighlightIndex = 1;  // 기본 선택: Water
    bool  m_bConfirmed      = false;
    float m_fFadeIn         = 0.f;
    float m_fPulse          = 0.f;
    int   m_nHoverIndex     = -1;
    float m_fKeyDebounce    = 0.f;

    // 흰 픽셀 텍스처 — 색칠된 사각형을 SpriteBatch로 그릴 때 사용
    ComPtr<ID3D12Resource>      m_pWhiteTex;
    ComPtr<ID3D12Resource>      m_pWhiteUpload;
    D3D12_GPU_DESCRIPTOR_HANDLE m_hWhiteGPU = {};

    // 카드 레이아웃 캐시
    struct CardRect { float x, y, w, h; };
    CardRect m_cards[kCharCount] = {};
    float    m_fScreenW = 1280.f;
    float    m_fScreenH = 720.f;

    void ComputeCardLayout(float screenW, float screenH);
    void DrawRect(DirectX::SpriteBatch* pBatch, float x, float y, float w, float h,
                  XMFLOAT4 color);
};
