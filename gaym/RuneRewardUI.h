#pragma once

#include "stdafx.h"
#include <SpriteBatch.h>
#include <SpriteFont.h>
#include <array>
#include <string>
#include "RuneDef.h"     // EquippedRune
#include "SkillTypes.h"

class SkillIconRenderer;
class SkillComponent;

// ─────────────────────────────────────────────────────────────────────────────
// RuneRewardUI
//   방 클리어 후 룬 획득(드롭) 시의 중앙 모달 UI를 아이콘 기반으로 렌더링.
//   기존 텍스트 UI(Dx12App::RenderText 의 SelectingRune/SelectingSkill 블록)를 대체.
//   상태머신/장착 로직은 Scene 이 그대로 담당하고, 본 클래스는 레이아웃·렌더·히트테스트만.
//
//   레이아웃 함수를 렌더와 히트테스트가 공유하므로 클릭 좌표와 그림 좌표가 항상 일치한다.
// ─────────────────────────────────────────────────────────────────────────────
class RuneRewardUI
{
public:
    RuneRewardUI() = default;

    // ── SelectingRune (3개 룬 카드 중 선택) ─────────────────────────────────────
    void RenderRuneSelectIcons(ID3D12GraphicsCommandList* pCmd, SkillIconRenderer* pIcons,
                               const std::array<EquippedRune, 3>& options,
                               float mouseX, float mouseY, float w, float h);
    void RenderRuneSelectText(DirectX::SpriteBatch* pBatch, DirectX::SpriteFont* pFont,
                              D3D12_GPU_DESCRIPTOR_HANDLE whiteTexGPU,
                              const std::array<EquippedRune, 3>& options, float w, float h);
    // 마우스가 올라간 룬 카드 인덱스(0~2), 없으면 -1.
    int HitTestRuneOption(float mx, float my, float w, float h) const;

    // ── SelectingSkill (선택한 룬을 장착할 스킬/룬 칸 선택) ──────────────────────
    void RenderSkillSelectIcons(ID3D12GraphicsCommandList* pCmd, SkillIconRenderer* pIcons,
                                const std::string& selectedRuneId, SkillComponent* pSkill,
                                float mouseX, float mouseY, float w, float h);
    void RenderSkillSelectText(DirectX::SpriteBatch* pBatch, DirectX::SpriteFont* pFont,
                               D3D12_GPU_DESCRIPTOR_HANDLE whiteTexGPU,
                               const std::string& selectedRuneId, SkillComponent* pSkill,
                               float w, float h);
    // 마우스가 올라간 (스킬, 룬칸). 맞으면 true.
    bool HitTestSkillSlot(float mx, float my, float w, float h, int& outSkill, int& outRune) const;

private:
    // 레이아웃 (렌더/히트테스트 공용)
    void RuneCardRect(int i, float w, float h, float& x, float& y, float& cw, float& ch) const;
    void RuneCardIconRect(int i, float w, float h, float& x, float& y, float& sz) const;
    void SkillIconRect(int skillIdx, float w, float h, float& x, float& y, float& sz) const;
    void SkillRuneRect(int skillIdx, int runeIdx, float w, float h, float& x, float& y, float& sz) const;

    // 레이아웃 상수
    static constexpr float CARD_W   = 280.0f;
    static constexpr float CARD_H   = 360.0f;
    static constexpr float CARD_GAP = 30.0f;
    static constexpr float CARD_ICON = 128.0f;

    static constexpr float ROW_H        = 100.0f;
    static constexpr float SK_ICON      = 72.0f;
    static constexpr float SK_RUNE      = 64.0f;
    static constexpr float SK_RUNE_GAP  = 18.0f;

    // 호버 상태 (Icons 패스에서 계산 → Text 패스에서 강조/툴팁에 재사용)
    int m_hoverOption = -1;
    int m_hoverSkill  = -1;
    int m_hoverRune   = -1;
};
