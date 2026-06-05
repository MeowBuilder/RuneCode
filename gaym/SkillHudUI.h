#pragma once

#include "stdafx.h"
#include <SpriteBatch.h>
#include <SpriteFont.h>
#include "SkillTypes.h"

class SkillIconRenderer;
class SkillComponent;
class InputSystem;

// ─────────────────────────────────────────────────────────────────────────────
// SkillHudUI
//   좌하단 스킬 HUD의 레이아웃/상호작용/렌더링을 담당. 아이콘은 SkillIconRenderer
//   (자체 PSO)로, 글자/숫자/툴팁은 SpriteBatch/SpriteFont 로 그린다.
//
//   Phase 1: 스킬 아이콘 + 조작키 + 방사형 쿨타임 + 장착 룬 아이콘.
//   Phase 2: TAB 홀드 시 확대(보간) + 마우스 호버 시 스킬/룬 툴팁.
//   Phase 3(예정): 드래그&드롭 룬 장착/교체.
// ─────────────────────────────────────────────────────────────────────────────
class SkillHudUI
{
public:
    SkillHudUI() = default;

    // 매 프레임 입력 처리(TAB 확대 보간 + 마우스 호버 판정). RenderText 가 그릴 정보를 갱신.
    void Update(InputSystem* pInput, SkillComponent* pSkill,
                float dt, float screenWidth, float screenHeight);

    // 아이콘 패스 — 원시 커맨드(자체 PSO). SpriteBatch::Begin 호출 전에 실행해야 한다.
    void RenderIcons(ID3D12GraphicsCommandList* pCommandList,
                     SkillIconRenderer* pIcons,
                     SkillComponent* pSkill,
                     float screenWidth, float screenHeight);

    // 텍스트/툴팁 패스 — SpriteBatch::Begin / End 사이에서 호출.
    //   whiteTexGPU: 폰트 디스크립터 힙의 1x1 흰 픽셀 핸들(툴팁 배경 사각형용).
    void RenderText(DirectX::SpriteBatch* pBatch,
                    DirectX::SpriteFont* pFont,
                    SkillComponent* pSkill,
                    D3D12_GPU_DESCRIPTOR_HANDLE whiteTexGPU,
                    float screenWidth, float screenHeight);

    bool IsZoomed() const { return m_tabZoom > 0.05f; }

private:
    struct HoverTarget
    {
        enum class Kind { None, Skill, Rune };
        Kind kind    = Kind::None;
        int  slot    = -1;   // 스킬 슬롯 인덱스 (0~3)
        int  runeIdx = -1;   // 룬 인덱스 (0~2), Rune 일 때만
    };

    // 현재 확대 배율(아이콘 크기 곱).
    float IconScale() const { return 1.0f + m_tabZoom * 0.95f; }

    // 슬롯 i 의 아이콘 박스(좌상단 x,y 와 한 변 크기) — 확대 반영.
    void ComputeSlotRect(int slotIndex, float screenW, float screenH,
                         float& outX, float& outY, float& outSize) const;
    // 슬롯 i, 룬 r 의 칸(좌상단 x,y 와 크기) — 확대 반영.
    void ComputeRuneRect(int slotIndex, int runeIdx, float screenW, float screenH,
                         float& outX, float& outY, float& outSize) const;

    // 마우스 좌표가 어떤 아이템 위인지 판정.
    HoverTarget HitTest(float mx, float my, float screenW, float screenH) const;

    // 드래그 놓을 때 이동/교체 수행 (m_drag 출발지 → m_hover 목적지).
    void DoDrop(SkillComponent* pSkill);

    // 레이아웃 상수 (확대 전 기준값)
    static constexpr float ICON_SIZE     = 64.0f;
    static constexpr float SLOT_GAP      = 16.0f;
    static constexpr float MARGIN_LEFT   = 24.0f;
    static constexpr float MARGIN_BOTTOM = 28.0f;
    static constexpr float RUNE_SIZE     = 20.0f;
    static constexpr float RUNE_GAP      = 4.0f;
    static constexpr float RUNE_ROW_GAP  = 6.0f;   // 아이콘과 룬 줄 사이 간격

    // 상태 (Phase 2)
    float       m_tabZoom = 0.0f;   // 0=평상시, 1=완전 확대
    HoverTarget m_hover;

    // 드래그&드롭 (Phase 3)
    struct DragState
    {
        bool        active   = false;
        int         fromSlot = -1;   // 출발 스킬 슬롯
        int         fromRune = -1;   // 출발 룬 인덱스
        std::string runeId;          // 집어든 룬 ID
        int         stack    = 1;    // 집어든 룬 스택
    };
    DragState        m_drag;
    DirectX::XMFLOAT2 m_mousePos{ 0.0f, 0.0f };
};
