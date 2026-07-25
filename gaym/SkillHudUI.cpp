#include "stdafx.h"
#include "SkillHudUI.h"
#include "SkillIconRenderer.h"
#include "SkillComponent.h"
#include "ISkillBehavior.h"
#include "SkillData.h"
#include "RuneRegistry.h"
#include "RuneDef.h"
#include "InputSystem.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>

using namespace DirectX;

namespace
{
    // 원소별 폴백 타일 색 (PNG 아이콘이 없을 때 단색 표시용)
    XMFLOAT4 ElementColor(ElementType e)
    {
        switch (e)
        {
            case ElementType::Fire:  return XMFLOAT4(1.00f, 0.38f, 0.12f, 1.0f);
            case ElementType::Water: return XMFLOAT4(0.22f, 0.60f, 1.00f, 1.0f);
            case ElementType::Wind:  return XMFLOAT4(0.32f, 0.95f, 0.70f, 1.0f);
            case ElementType::Earth: return XMFLOAT4(0.85f, 0.65f, 0.28f, 1.0f);
            default:                 return XMFLOAT4(0.62f, 0.55f, 0.78f, 1.0f); // 범용 룬 = 보라기
        }
    }

    std::wstring Widen(const std::string& s)
    {
        return std::wstring(s.begin(), s.end());  // 스킬 이름/키는 ASCII
    }

    // UTF-8(룬 이름/설명) → UTF-16
    std::wstring Utf8Wide(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        if (wlen <= 0) return std::wstring();
        std::wstring w(wlen, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], wlen);
        return w;
    }

    const wchar_t* KeyLabel(int slot)
    {
        switch (slot)
        {
            case 0:  return L"Q";
            case 1:  return L"E";
            case 2:  return L"R";
            default: return L"M2";  // RightClick
        }
    }

    const wchar_t* ElementName(ElementType e)
    {
        switch (e)
        {
            case ElementType::Fire:  return L"화염";
            case ElementType::Water: return L"물결";
            case ElementType::Wind:  return L"바람";
            case ElementType::Earth: return L"대지";
            default:                 return L"무속성";
        }
    }

    const wchar_t* ActivationName(ActivationType t)
    {
        switch (t)
        {
            case ActivationType::Charge:  return L"차지";
            case ActivationType::Channel: return L"채널";
            case ActivationType::Place:   return L"설치";
            case ActivationType::Enhance: return L"증강";
            case ActivationType::Split:   return L"분열";
            default:                      return L"즉발";
        }
    }

    const wchar_t* RuneGradeLabel(RuneGrade g)
    {
        switch (g)
        {
            case RuneGrade::Normal:    return L"[노멀]";
            case RuneGrade::Rare:      return L"[레어]";
            case RuneGrade::Epic:      return L"[에픽]";
            case RuneGrade::Unique:    return L"[유니크]";
            case RuneGrade::Legendary: return L"[전설]";
            default:                   return L"";
        }
    }

    XMVECTORF32 RuneGradeColor(RuneGrade g)
    {
        switch (g)
        {
            case RuneGrade::Normal:    return Colors::White;
            case RuneGrade::Rare:      return Colors::CornflowerBlue;
            case RuneGrade::Epic:      return Colors::MediumPurple;
            case RuneGrade::Unique:    return Colors::Red;
            case RuneGrade::Legendary: return Colors::Gold;
            default:                   return Colors::White;
        }
    }

    // 룬 설명 문자열 (Dx12App::BuildRuneDesc 와 동일 포맷)
    std::wstring BuildRuneDesc(const RuneDef& def)
    {
        std::wstringstream ss;
        auto pct = [](float m) { return (int)((m - 1.f) * 100.f + 0.5f); };

        if (!def.category.empty())    ss << L"[" << Utf8Wide(def.category) << L"] ";
        if (!def.description.empty()) ss << Utf8Wide(def.description) << L"  ";

        if (def.damageMult    != 1.f) ss << L"피해 "   << (pct(def.damageMult) >= 0 ? L"+" : L"") << pct(def.damageMult) << L"% ";
        if (def.radiusMult    != 1.f) ss << L"범위 "   << (pct(def.radiusMult) >= 0 ? L"+" : L"") << pct(def.radiusMult) << L"% ";
        if (def.cooldownMult  != 1.f) ss << L"쿨 "     << (pct(def.cooldownMult) >= 0 ? L"+" : L"") << pct(def.cooldownMult) << L"% ";
        if (def.castTimeMult  != 1.f) ss << L"시전 "   << (pct(def.castTimeMult) >= 0 ? L"+" : L"") << pct(def.castTimeMult) << L"% ";
        if (def.durationMult  != 1.f) ss << L"지속 "   << (pct(def.durationMult) >= 0 ? L"+" : L"") << pct(def.durationMult) << L"% ";
        if (def.knockbackMult != 1.f) ss << L"넉백 "   << (pct(def.knockbackMult) >= 0 ? L"+" : L"") << pct(def.knockbackMult) << L"% ";
        if (def.extraProjectiles > 0) ss << L"투사체 +" << def.extraProjectiles << L" ";
        if (def.orbitalCount     > 0) ss << L"궤도탄 "  << def.orbitalCount << L" ";
        if (def.spawnOnHitCount  > 0) ss << L"반향 +"   << def.spawnOnHitCount << L" ";
        if (def.lifestealRatio   > 0.f) ss << L"흡수 "  << (int)(def.lifestealRatio * 100.f + 0.5f) << L"% ";
        if (def.execDamageBonus  > 0.f) ss << L"처형 +" << (int)(def.execDamageBonus * 100.f + 0.5f) << L"% ";
        if (def.cdResetChance    > 0.f) ss << L"무한 "  << (int)(def.cdResetChance * 100.f + 0.5f) << L"% ";
        if (def.piercing)    ss << L"관통 ";
        if (def.homing)      ss << L"유도 ";
        if (def.doublecast)  ss << L"쌍발 ";
        if (def.echoOnCast)  ss << L"잔상 ";
        if (def.randomElementOnCast) ss << L"원소무작위 ";
        if (def.activationOverride.has_value())
            ss << ActivationName(def.activationOverride.value()) << L"형 ";

        std::wstring r = ss.str();
        if (r.empty()) r = L"효과 없음";
        return r;
    }

    // 공백 기준 단순 줄바꿈 (scale 적용 기준 maxWidth)
    std::wstring WrapText(DirectX::SpriteFont* font, const std::wstring& text, float maxWidth, float scale)
    {
        std::wstring result, line;
        std::wstringstream ss(text);
        std::wstring word;
        while (ss >> word)
        {
            std::wstring trial = line.empty() ? word : (line + L" " + word);
            float w = XMVectorGetX(font->MeasureString(trial.c_str())) * scale;
            if (!line.empty() && w > maxWidth)
            {
                result += line + L"\n";
                line = word;
            }
            else
            {
                line = trial;
            }
        }
        if (!line.empty()) result += line;
        return result;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 레이아웃 (확대 반영)
// ─────────────────────────────────────────────────────────────────────────────
void SkillHudUI::ComputeSlotRect(int slotIndex, float screenW, float screenH,
                                 float& outX, float& outY, float& outSize) const
{
    const float s = IconScale();
    outSize = ICON_SIZE * s;
    const float stride   = outSize + SLOT_GAP * s;
    const float runeRowH = RUNE_SIZE * s + RUNE_ROW_GAP * s;
    outX = MARGIN_LEFT + slotIndex * stride;
    outY = screenH - MARGIN_BOTTOM - runeRowH - outSize;
}

void SkillHudUI::ComputeRuneRect(int slotIndex, int runeIdx, float screenW, float screenH,
                                 float& outX, float& outY, float& outSize) const
{
    const float s = IconScale();
    float ix, iy, isz;
    ComputeSlotRect(slotIndex, screenW, screenH, ix, iy, isz);
    outSize = RUNE_SIZE * s;
    const float rgap = RUNE_GAP * s;
    outX = ix + runeIdx * (outSize + rgap);
    outY = iy + isz + RUNE_ROW_GAP * s;
}

SkillHudUI::HoverTarget SkillHudUI::HitTest(float mx, float my, float screenW, float screenH) const
{
    for (int i = 0; i < static_cast<int>(SkillSlot::Count); ++i)
    {
        float x, y, sz;
        ComputeSlotRect(i, screenW, screenH, x, y, sz);
        if (mx >= x && mx <= x + sz && my >= y && my <= y + sz)
            return HoverTarget{ HoverTarget::Kind::Skill, i, -1 };

        for (int r = 0; r < RUNES_PER_SKILL; ++r)
        {
            float rx, ry, rsz;
            ComputeRuneRect(i, r, screenW, screenH, rx, ry, rsz);
            if (mx >= rx && mx <= rx + rsz && my >= ry && my <= ry + rsz)
                return HoverTarget{ HoverTarget::Kind::Rune, i, r };
        }
    }
    return HoverTarget{};
}

// ─────────────────────────────────────────────────────────────────────────────
// Update — TAB 확대 보간 + 호버 판정
// ─────────────────────────────────────────────────────────────────────────────
void SkillHudUI::Update(InputSystem* pInput, SkillComponent* pSkill,
                        float dt, float screenWidth, float screenHeight)
{
    if (!pInput) return;

    float target = pInput->IsKeyDown(VK_TAB) ? 1.0f : 0.0f;
    float k = (std::min)(1.0f, dt * 12.0f);
    m_tabZoom += (target - m_tabZoom) * k;
    m_tabZoom = std::clamp(m_tabZoom, 0.0f, 1.0f);

    m_mousePos = pInput->GetMousePosition();

    // 호버 판정 (확대 상태에서만)
    m_hover = HoverTarget{};
    if (m_tabZoom > 0.05f)
        m_hover = HitTest(m_mousePos.x, m_mousePos.y, screenWidth, screenHeight);

    // ── 드래그&드롭 (확대 상태에서만 허용) ──────────────────────────────────────
    if (m_tabZoom <= 0.05f)
    {
        m_drag.active = false;   // 인벤토리 닫히면 드래그 취소
        return;
    }
    if (!pSkill) return;

    bool lmbDown    = pInput->IsMouseButtonDown(0);
    bool lmbPressed = pInput->IsMouseButtonPressed(0);

    if (!m_drag.active)
    {
        // 비어있지 않은 룬 칸을 누르면 드래그 시작
        if (lmbPressed && m_hover.kind == HoverTarget::Kind::Rune)
        {
            EquippedRune er = pSkill->GetRuneSlot(static_cast<SkillSlot>(m_hover.slot), m_hover.runeIdx);
            if (!er.IsEmpty())
            {
                m_drag.active   = true;
                m_drag.fromSlot = m_hover.slot;
                m_drag.fromRune = m_hover.runeIdx;
                m_drag.runeId   = er.runeId;
                m_drag.stack    = er.stackCount;
            }
        }
    }
    else
    {
        // 버튼을 떼면 드롭
        if (!lmbDown)
        {
            DoDrop(pSkill);
            m_drag.active = false;
        }
    }
}

// 드래그 놓기 — m_drag(출발) → m_hover(목적지) 이동/교체
void SkillHudUI::DoDrop(SkillComponent* pSkill)
{
    if (!pSkill || !m_drag.active) return;

    SkillSlot fromSlot = static_cast<SkillSlot>(m_drag.fromSlot);

    if (m_hover.kind == HoverTarget::Kind::Rune)
    {
        int toSlotI = m_hover.slot;
        int toRune  = m_hover.runeIdx;
        if (toSlotI == m_drag.fromSlot && toRune == m_drag.fromRune)
            return;  // 제자리

        SkillSlot toSlot = static_cast<SkillSlot>(toSlotI);
        EquippedRune tgt = pSkill->GetRuneSlot(toSlot, toRune);

        // 출발 룬을 목적지에 놓고, 목적지 룬을 출발지로 (교체). 목적지가 비었으면 단순 이동.
        pSkill->SetRuneSlot(toSlot, toRune, m_drag.runeId, m_drag.stack);
        if (tgt.IsEmpty())
            pSkill->ClearRuneSlot(fromSlot, m_drag.fromRune);
        else
            pSkill->SetRuneSlot(fromSlot, m_drag.fromRune, tgt.runeId, tgt.stackCount);
    }
    else if (m_hover.kind == HoverTarget::Kind::Skill)
    {
        // 스킬 아이콘 위에 놓으면 그 스킬의 첫 빈 룬 칸에 장착
        SkillSlot toSlot = static_cast<SkillSlot>(m_hover.slot);
        int dst = -1;
        for (int r = 0; r < RUNES_PER_SKILL; ++r)
        {
            if (pSkill->GetRuneSlot(toSlot, r).IsEmpty()) { dst = r; break; }
        }
        if (dst >= 0 && !(m_hover.slot == m_drag.fromSlot && dst == m_drag.fromRune))
        {
            pSkill->SetRuneSlot(toSlot, dst, m_drag.runeId, m_drag.stack);
            pSkill->ClearRuneSlot(fromSlot, m_drag.fromRune);
        }
        // 빈 칸 없으면 취소(원위치 유지)
    }
    // 그 외(영역 밖) → 취소: 출발 룬은 그대로 두었으므로 할 일 없음
}

// ─────────────────────────────────────────────────────────────────────────────
// RenderIcons — 아이콘 패스
// ─────────────────────────────────────────────────────────────────────────────
void SkillHudUI::RenderIcons(ID3D12GraphicsCommandList* pCommandList,
                             SkillIconRenderer* pIcons,
                             SkillComponent* pSkill,
                             float screenWidth, float screenHeight)
{
    if (!pIcons || !pSkill) return;

    pIcons->Begin(pCommandList, screenWidth, screenHeight);

    const XMFLOAT4 backingColor(0.04f, 0.04f, 0.06f, 0.62f);
    const XMFLOAT4 hoverColor(0.95f, 0.80f, 0.30f, 0.95f);   // 호버 강조 테두리
    const XMFLOAT4 emptySlotColor(0.10f, 0.10f, 0.13f, 0.55f);

    for (int i = 0; i < static_cast<int>(SkillSlot::Count); ++i)
    {
        SkillSlot slot = static_cast<SkillSlot>(i);
        float x, y, size;
        ComputeSlotRect(i, screenWidth, screenHeight, x, y, size);

        bool skillHovered = (m_hover.kind == HoverTarget::Kind::Skill && m_hover.slot == i);
        float pad = skillHovered ? 4.0f : 3.0f;
        pIcons->DrawSolid(pCommandList, x - pad, y - pad, size + pad * 2, size + pad * 2,
                          skillHovered ? hoverColor : backingColor);

        ISkillBehavior* pBehavior = pSkill->GetSkill(slot);
        if (pBehavior)
        {
            const SkillData& data = pBehavior->GetSkillData();
            float cdProgress = pSkill->GetCooldownProgress(slot);
            XMFLOAT4 fallback = ElementColor(data.element);
            pIcons->DrawIcon(pCommandList, data.name, x, y, size, size, fallback, cdProgress, 1.0f);
        }
        else
        {
            pIcons->DrawSolid(pCommandList, x, y, size, size, emptySlotColor);
        }

        // 장착 룬 3칸
        for (int r = 0; r < RUNES_PER_SKILL; ++r)
        {
            float rx, ry, rsz;
            ComputeRuneRect(i, r, screenWidth, screenHeight, rx, ry, rsz);

            EquippedRune er = pSkill->GetRuneSlot(slot, r);
            bool isDragSource = (m_drag.active && m_drag.fromSlot == i && m_drag.fromRune == r);
            const RuneDef* def = (!er.IsEmpty() && !isDragSource) ? RuneRegistry::Get().Find(er.runeId) : nullptr;

            bool runeHovered = (m_hover.kind == HoverTarget::Kind::Rune && m_hover.slot == i && m_hover.runeIdx == r);
            float rpad = runeHovered ? 2.5f : 1.5f;

            // 장착 룬 있으면 등급 색 테두리, 없으면 기본 테두리
            XMFLOAT4 borderCol = runeHovered ? hoverColor : backingColor;
            if (def && !runeHovered)
            {
                auto gc = RuneGradeColor(def->grade);
                borderCol = XMFLOAT4(gc.f[0], gc.f[1], gc.f[2], 0.85f);
            }
            pIcons->DrawSolid(pCommandList, rx - rpad, ry - rpad, rsz + rpad * 2, rsz + rpad * 2, borderCol);

            if (er.IsEmpty() || isDragSource)
            {
                pIcons->DrawSolid(pCommandList, rx, ry, rsz, rsz, emptySlotColor);
            }
            else
            {
                XMFLOAT4 elemCol = ElementColor(def ? def->element : ElementType::None);
                // 원소색 반투명 배경 (흑백 룬 아이콘이 잘 보이도록)
                pIcons->DrawSolid(pCommandList, rx, ry, rsz, rsz,
                    XMFLOAT4(elemCol.x * 0.45f, elemCol.y * 0.45f, elemCol.z * 0.45f, 0.8f));
                pIcons->DrawIcon(pCommandList, er.runeId, rx, ry, rsz, rsz, elemCol, 1.0f, 1.0f);
            }
        }
    }

    // 드래그 중인 룬 고스트 (마우스 따라다님, 반투명)
    if (m_drag.active)
    {
        const RuneDef* def = RuneRegistry::Get().Find(m_drag.runeId);
        XMFLOAT4 fallback = def ? ElementColor(def->element) : ElementColor(ElementType::None);
        float gsz = RUNE_SIZE * IconScale() * 1.25f;
        pIcons->DrawIcon(pCommandList, m_drag.runeId,
                         m_mousePos.x - gsz * 0.5f, m_mousePos.y - gsz * 0.5f,
                         gsz, gsz, fallback, 1.0f, 0.8f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RenderText — 글자/숫자/툴팁 패스
// ─────────────────────────────────────────────────────────────────────────────
void SkillHudUI::RenderText(DirectX::SpriteBatch* pBatch,
                            DirectX::SpriteFont* pFont,
                            SkillComponent* pSkill,
                            D3D12_GPU_DESCRIPTOR_HANDLE whiteTexGPU,
                            float screenWidth, float screenHeight)
{
    if (!pBatch || !pFont || !pSkill) return;

    const float s = IconScale();
    const float keyScale = 0.55f * s;

    for (int i = 0; i < static_cast<int>(SkillSlot::Count); ++i)
    {
        SkillSlot slot = static_cast<SkillSlot>(i);
        float x, y, size;
        ComputeSlotRect(i, screenWidth, screenHeight, x, y, size);

        // 조작키 라벨 (아이콘 좌상단)
        pFont->DrawString(pBatch, KeyLabel(i), XMFLOAT2(x + 4.0f, y + 2.0f),
                          Colors::White, 0.0f, XMFLOAT2(0, 0), keyScale);

        if (!pSkill->GetSkill(slot)) continue;

        // 쿨다운 남은 초 — 아이콘 중앙
        float remaining = pSkill->GetCooldownRemaining(slot);
        if (remaining > 0.05f)
        {
            wchar_t buf[16];
            swprintf_s(buf, L"%.1f", remaining);
            float cdScale = 0.8f * s;
            XMVECTOR sz = pFont->MeasureString(buf);
            float tw = XMVectorGetX(sz) * cdScale;
            float th = XMVectorGetY(sz) * cdScale;
            pFont->DrawString(pBatch, buf,
                              XMFLOAT2(x + size * 0.5f - tw * 0.5f, y + size * 0.5f - th * 0.5f),
                              Colors::White, 0.0f, XMFLOAT2(0, 0), cdScale);
        }
    }

    // ── 툴팁 ──────────────────────────────────────────────────────────────────
    if (m_hover.kind == HoverTarget::Kind::None) return;

    std::wstring titleText;
    XMVECTORF32  titleColor = Colors::White;
    std::wstring bodyText;

    if (m_hover.kind == HoverTarget::Kind::Skill)
    {
        ISkillBehavior* pBehavior = pSkill->GetSkill(static_cast<SkillSlot>(m_hover.slot));
        if (!pBehavior) return;
        const SkillData& data = pBehavior->GetSkillData();
        SkillStats stats = pSkill->BuildSkillStats(static_cast<SkillSlot>(m_hover.slot), data.activationType);

        int   dmg = (int)(data.damage * stats.damageMult);
        float cd  = data.cooldown * stats.cooldownMult;

        titleText = Widen(data.name);
        titleColor = (data.element == ElementType::Fire)  ? XMVECTORF32{1.0f,0.45f,0.2f,1.0f}
                   : (data.element == ElementType::Water) ? XMVECTORF32{0.4f,0.7f,1.0f,1.0f}
                   : (data.element == ElementType::Wind)  ? XMVECTORF32{0.4f,1.0f,0.8f,1.0f}
                   : (data.element == ElementType::Earth) ? XMVECTORF32{0.9f,0.75f,0.4f,1.0f}
                                                          : XMVECTORF32{Colors::White};

        std::wstringstream b;
        b << ElementName(data.element) << L" / " << ActivationName(stats.activationType) << L"\n";
        b << L"피해 " << dmg << L"   쿨 " << std::fixed << std::setprecision(1) << cd << L"s";
        bodyText = b.str();
    }
    else // Rune
    {
        EquippedRune er = pSkill->GetRuneSlot(static_cast<SkillSlot>(m_hover.slot), m_hover.runeIdx);
        if (er.IsEmpty())
        {
            titleText  = L"빈 룬 슬롯";
            titleColor = Colors::Gray;
            bodyText   = L"룬을 장착하면 스킬이 강화됩니다.";
        }
        else
        {
            const RuneDef* def = RuneRegistry::Get().Find(er.runeId);
            if (!def) return;
            titleText  = Utf8Wide(def->name);
            titleText += L" ";
            titleText += RuneGradeLabel(def->grade);
            titleColor = RuneGradeColor(def->grade);
            bodyText   = BuildRuneDesc(*def);
        }
    }

    // 본문 줄바꿈
    const float ttScale  = 0.62f;
    const float maxTextW = 360.0f;
    bodyText = WrapText(pFont, bodyText, maxTextW, ttScale);

    // 측정
    XMVECTOR titleSz = pFont->MeasureString(titleText.c_str());
    XMVECTOR bodySz  = pFont->MeasureString(bodyText.c_str());
    float titleW = XMVectorGetX(titleSz) * ttScale;
    float titleH = XMVectorGetY(titleSz) * ttScale;
    float bodyW  = XMVectorGetX(bodySz)  * ttScale;
    float bodyH  = XMVectorGetY(bodySz)  * ttScale;

    const float pad = 10.0f;
    const float lineGap = 4.0f;
    float boxW = (std::max)(titleW, bodyW) + pad * 2.0f;
    float boxH = titleH + lineGap + bodyH + pad * 2.0f;

    // 호버 대상 위치 기준으로 위쪽 배치
    float hx, hy, hsz;
    if (m_hover.kind == HoverTarget::Kind::Skill)
        ComputeSlotRect(m_hover.slot, screenWidth, screenHeight, hx, hy, hsz);
    else
        ComputeRuneRect(m_hover.slot, m_hover.runeIdx, screenWidth, screenHeight, hx, hy, hsz);

    float boxX = hx;
    float boxY = hy - boxH - 8.0f;
    if (boxX + boxW > screenWidth - 8.0f) boxX = screenWidth - 8.0f - boxW;
    if (boxX < 8.0f) boxX = 8.0f;
    if (boxY < 8.0f) boxY = hy + hsz + 8.0f;  // 위 공간 없으면 아래로

    // 배경 사각형 (흰 픽셀 텍스처 tint)
    if (whiteTexGPU.ptr)
    {
        RECT bg = { (LONG)boxX, (LONG)boxY, (LONG)(boxX + boxW), (LONG)(boxY + boxH) };
        pBatch->Draw(whiteTexGPU, XMUINT2(1, 1), bg, XMVECTORF32{ 0.05f, 0.05f, 0.07f, 0.9f });
    }

    // 제목 + 본문
    pFont->DrawString(pBatch, titleText.c_str(), XMFLOAT2(boxX + pad, boxY + pad),
                      titleColor, 0.0f, XMFLOAT2(0, 0), ttScale);
    pFont->DrawString(pBatch, bodyText.c_str(), XMFLOAT2(boxX + pad, boxY + pad + titleH + lineGap),
                      XMVECTORF32{ 0.82f, 0.82f, 0.86f, 1.0f }, 0.0f, XMFLOAT2(0, 0), ttScale);
}
