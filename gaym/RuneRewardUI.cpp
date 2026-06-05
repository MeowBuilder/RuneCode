#include "stdafx.h"
#include "RuneRewardUI.h"
#include "SkillIconRenderer.h"
#include "SkillComponent.h"
#include "ISkillBehavior.h"
#include "SkillData.h"
#include "RuneRegistry.h"
#include <sstream>
#include <algorithm>

using namespace DirectX;

namespace
{
    XMFLOAT4 ElementColor(ElementType e)
    {
        switch (e)
        {
            case ElementType::Fire:  return XMFLOAT4(1.00f, 0.38f, 0.12f, 1.0f);
            case ElementType::Water: return XMFLOAT4(0.22f, 0.60f, 1.00f, 1.0f);
            case ElementType::Wind:  return XMFLOAT4(0.32f, 0.95f, 0.70f, 1.0f);
            case ElementType::Earth: return XMFLOAT4(0.85f, 0.65f, 0.28f, 1.0f);
            default:                 return XMFLOAT4(0.62f, 0.55f, 0.78f, 1.0f);
        }
    }

    std::wstring Utf8Wide(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        if (n <= 0) return std::wstring();
        std::wstring w(n, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }

    const wchar_t* GradeLabel(RuneGrade g)
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

    XMVECTORF32 GradeColor(RuneGrade g)
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

    std::wstring BuildRuneDesc(const RuneDef& def)
    {
        std::wstringstream ss;
        auto pct = [](float m) { return (int)((m - 1.f) * 100.f + 0.5f); };
        if (!def.category.empty())    ss << L"[" << Utf8Wide(def.category) << L"] ";
        if (!def.description.empty()) ss << Utf8Wide(def.description) << L"  ";
        if (def.damageMult    != 1.f) ss << L"피해 " << (pct(def.damageMult) >= 0 ? L"+" : L"") << pct(def.damageMult) << L"% ";
        if (def.radiusMult    != 1.f) ss << L"범위 " << (pct(def.radiusMult) >= 0 ? L"+" : L"") << pct(def.radiusMult) << L"% ";
        if (def.cooldownMult  != 1.f) ss << L"쿨 "   << (pct(def.cooldownMult) >= 0 ? L"+" : L"") << pct(def.cooldownMult) << L"% ";
        if (def.durationMult  != 1.f) ss << L"지속 " << (pct(def.durationMult) >= 0 ? L"+" : L"") << pct(def.durationMult) << L"% ";
        if (def.knockbackMult != 1.f) ss << L"넉백 " << (pct(def.knockbackMult) >= 0 ? L"+" : L"") << pct(def.knockbackMult) << L"% ";
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

    std::wstring WrapText(SpriteFont* font, const std::wstring& text, float maxWidth, float scale)
    {
        std::wstring result, line, word;
        std::wstringstream ss(text);
        while (ss >> word)
        {
            std::wstring trial = line.empty() ? word : (line + L" " + word);
            float wpx = XMVectorGetX(font->MeasureString(trial.c_str())) * scale;
            if (!line.empty() && wpx > maxWidth) { result += line + L"\n"; line = word; }
            else                                  { line = trial; }
        }
        if (!line.empty()) result += line;
        return result;
    }

    // 반투명 사각형 (SpriteBatch + 1x1 흰 픽셀)
    void FillRect(SpriteBatch* b, D3D12_GPU_DESCRIPTOR_HANDLE white,
                  float x, float y, float w, float h, FXMVECTOR tint)
    {
        if (!white.ptr) return;
        RECT r = { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
        b->Draw(white, XMUINT2(1, 1), r, tint);
    }

    void DrawCentered(SpriteFont* font, SpriteBatch* b, const wchar_t* text,
                      float cx, float y, FXMVECTOR color, float scale = 1.0f)
    {
        float tw = XMVectorGetX(font->MeasureString(text)) * scale;
        font->DrawString(b, text, XMFLOAT2(cx - tw * 0.5f, y), color, 0.0f, XMFLOAT2(0, 0), scale);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 레이아웃
// ─────────────────────────────────────────────────────────────────────────────
void RuneRewardUI::RuneCardRect(int i, float w, float h, float& x, float& y, float& cw, float& ch) const
{
    cw = CARD_W; ch = CARD_H;
    float totalW = 3 * CARD_W + 2 * CARD_GAP;
    float startX = w * 0.5f - totalW * 0.5f;
    x = startX + i * (CARD_W + CARD_GAP);
    y = h * 0.5f - CARD_H * 0.5f + 20.0f;
}

void RuneRewardUI::RuneCardIconRect(int i, float w, float h, float& x, float& y, float& sz) const
{
    float cx, cy, cw, ch;
    RuneCardRect(i, w, h, cx, cy, cw, ch);
    sz = CARD_ICON;
    x = cx + (cw - sz) * 0.5f;
    y = cy + 28.0f;
}

void RuneRewardUI::SkillIconRect(int skillIdx, float w, float h, float& x, float& y, float& sz) const
{
    sz = SK_ICON;
    float startY = h * 0.5f - 120.0f;
    x = w * 0.5f - 320.0f;
    y = startY + skillIdx * ROW_H;
}

void RuneRewardUI::SkillRuneRect(int skillIdx, int runeIdx, float w, float h, float& x, float& y, float& sz) const
{
    float ix, iy, isz;
    SkillIconRect(skillIdx, w, h, ix, iy, isz);
    sz = SK_RUNE;
    float runeStartX = w * 0.5f - 200.0f;
    x = runeStartX + runeIdx * (SK_RUNE + SK_RUNE_GAP);
    y = iy + (isz - sz) * 0.5f;
}

// ─────────────────────────────────────────────────────────────────────────────
// SelectingRune
// ─────────────────────────────────────────────────────────────────────────────
int RuneRewardUI::HitTestRuneOption(float mx, float my, float w, float h) const
{
    for (int i = 0; i < 3; ++i)
    {
        float x, y, cw, ch;
        RuneCardRect(i, w, h, x, y, cw, ch);
        if (mx >= x && mx <= x + cw && my >= y && my <= y + ch)
            return i;
    }
    return -1;
}

void RuneRewardUI::RenderRuneSelectIcons(ID3D12GraphicsCommandList* pCmd, SkillIconRenderer* pIcons,
                                         const std::array<EquippedRune, 3>& options,
                                         float mouseX, float mouseY, float w, float h)
{
    if (!pIcons) return;
    m_hoverOption = HitTestRuneOption(mouseX, mouseY, w, h);

    pIcons->Begin(pCmd, w, h);

    // 화면 어둡게 (모달 배경)
    pIcons->DrawSolid(pCmd, 0, 0, w, h, XMFLOAT4(0.0f, 0.0f, 0.0f, 0.55f));

    for (int i = 0; i < 3; ++i)
    {
        float cx, cy, cw, ch;
        RuneCardRect(i, w, h, cx, cy, cw, ch);

        bool hovered = (m_hoverOption == i);
        // 카드 배경 + 테두리
        XMFLOAT4 border = hovered ? XMFLOAT4(0.95f, 0.80f, 0.30f, 0.95f)
                                  : XMFLOAT4(0.30f, 0.30f, 0.38f, 0.9f);
        pIcons->DrawSolid(pCmd, cx - 3, cy - 3, cw + 6, ch + 6, border);
        pIcons->DrawSolid(pCmd, cx, cy, cw, ch, XMFLOAT4(0.08f, 0.08f, 0.11f, 0.96f));

        // 룬 아이콘
        const EquippedRune& er = options[i];
        const RuneDef* def = RuneRegistry::Get().Find(er.runeId);
        float ix, iy, isz;
        RuneCardIconRect(i, w, h, ix, iy, isz);
        XMFLOAT4 fb = def ? ElementColor(def->element) : ElementColor(ElementType::None);
        if (!er.IsEmpty())
            pIcons->DrawIcon(pCmd, er.runeId, ix, iy, isz, isz, fb, 1.0f, 1.0f);
        else
            pIcons->DrawSolid(pCmd, ix, iy, isz, isz, XMFLOAT4(0.12f, 0.12f, 0.15f, 1.0f));
    }
}

void RuneRewardUI::RenderRuneSelectText(SpriteBatch* pBatch, SpriteFont* pFont,
                                        D3D12_GPU_DESCRIPTOR_HANDLE whiteTexGPU,
                                        const std::array<EquippedRune, 3>& options, float w, float h)
{
    if (!pBatch || !pFont) return;

    DrawCentered(pFont, pBatch, L"룬 선택", w * 0.5f, h * 0.5f - CARD_H * 0.5f - 50.0f, Colors::Gold, 1.1f);

    const float nameScale = 0.7f;
    const float descScale = 0.58f;

    for (int i = 0; i < 3; ++i)
    {
        float cx, cy, cw, ch;
        RuneCardRect(i, w, h, cx, cy, cw, ch);
        const EquippedRune& er = options[i];
        const RuneDef* def = RuneRegistry::Get().Find(er.runeId);

        float textTop = cy + 28.0f + CARD_ICON + 16.0f;
        std::wstring name = def ? Utf8Wide(def->name) : Utf8Wide(er.runeId);
        DrawCentered(pFont, pBatch, name.c_str(), cx + cw * 0.5f, textTop,
                     def ? GradeColor(def->grade) : XMVECTORF32{ Colors::White }, nameScale);

        if (def)
        {
            DrawCentered(pFont, pBatch, GradeLabel(def->grade), cx + cw * 0.5f, textTop + 30.0f,
                         GradeColor(def->grade), descScale);

            std::wstring desc = WrapText(pFont, BuildRuneDesc(*def), cw - 28.0f, descScale);
            pFont->DrawString(pBatch, desc.c_str(), XMFLOAT2(cx + 14.0f, textTop + 60.0f),
                              XMVECTORF32{ 0.82f, 0.82f, 0.86f, 1.0f }, 0.0f, XMFLOAT2(0, 0), descScale);
        }

        // 번호 힌트
        wchar_t num[4]; swprintf_s(num, L"%d", i + 1);
        DrawCentered(pFont, pBatch, num, cx + cw * 0.5f, cy + 4.0f, Colors::Gray, 0.6f);
    }

    DrawCentered(pFont, pBatch, L"[클릭 또는 1/2/3] 선택   [ESC] 취소",
                 w * 0.5f, h * 0.5f + CARD_H * 0.5f + 30.0f, Colors::Gray, 0.6f);
}

// ─────────────────────────────────────────────────────────────────────────────
// SelectingSkill
// ─────────────────────────────────────────────────────────────────────────────
bool RuneRewardUI::HitTestSkillSlot(float mx, float my, float w, float h, int& outSkill, int& outRune) const
{
    for (int sk = 0; sk < static_cast<int>(SkillSlot::Count); ++sk)
    {
        for (int r = 0; r < RUNES_PER_SKILL; ++r)
        {
            float x, y, sz;
            SkillRuneRect(sk, r, w, h, x, y, sz);
            if (mx >= x && mx <= x + sz && my >= y && my <= y + sz)
            {
                outSkill = sk; outRune = r;
                return true;
            }
        }
    }
    return false;
}

void RuneRewardUI::RenderSkillSelectIcons(ID3D12GraphicsCommandList* pCmd, SkillIconRenderer* pIcons,
                                          const std::string& selectedRuneId, SkillComponent* pSkill,
                                          float mouseX, float mouseY, float w, float h)
{
    if (!pIcons) return;
    m_hoverSkill = -1; m_hoverRune = -1;
    HitTestSkillSlot(mouseX, mouseY, w, h, m_hoverSkill, m_hoverRune);

    pIcons->Begin(pCmd, w, h);
    pIcons->DrawSolid(pCmd, 0, 0, w, h, XMFLOAT4(0.0f, 0.0f, 0.0f, 0.55f));

    // 선택한 룬 아이콘 (상단 중앙)
    const RuneDef* selDef = RuneRegistry::Get().Find(selectedRuneId);
    XMFLOAT4 selFb = selDef ? ElementColor(selDef->element) : ElementColor(ElementType::None);
    float selSz = 96.0f;
    float selX = w * 0.5f - selSz * 0.5f;
    float selY = h * 0.5f - 120.0f - 132.0f;
    pIcons->DrawSolid(pCmd, selX - 4, selY - 4, selSz + 8, selSz + 8, XMFLOAT4(0.95f, 0.80f, 0.30f, 0.95f));
    if (!selectedRuneId.empty())
        pIcons->DrawIcon(pCmd, selectedRuneId, selX, selY, selSz, selSz, selFb, 1.0f, 1.0f);

    const XMFLOAT4 backing(0.06f, 0.06f, 0.09f, 0.92f);
    const XMFLOAT4 hover(0.95f, 0.80f, 0.30f, 0.95f);
    const XMFLOAT4 empty(0.12f, 0.12f, 0.16f, 1.0f);

    for (int sk = 0; sk < static_cast<int>(SkillSlot::Count); ++sk)
    {
        float ix, iy, isz;
        SkillIconRect(sk, w, h, ix, iy, isz);

        // 스킬 아이콘
        ISkillBehavior* pBeh = pSkill ? pSkill->GetSkill(static_cast<SkillSlot>(sk)) : nullptr;
        pIcons->DrawSolid(pCmd, ix - 3, iy - 3, isz + 6, isz + 6, backing);
        if (pBeh)
        {
            const SkillData& data = pBeh->GetSkillData();
            pIcons->DrawIcon(pCmd, data.name, ix, iy, isz, isz, ElementColor(data.element), 1.0f, 1.0f);
        }
        else
        {
            pIcons->DrawSolid(pCmd, ix, iy, isz, isz, empty);
        }

        // 룬 칸 3개
        for (int r = 0; r < RUNES_PER_SKILL; ++r)
        {
            float rx, ry, rsz;
            SkillRuneRect(sk, r, w, h, rx, ry, rsz);
            bool hov = (m_hoverSkill == sk && m_hoverRune == r);
            pIcons->DrawSolid(pCmd, rx - 3, ry - 3, rsz + 6, rsz + 6, hov ? hover : backing);

            EquippedRune er = pSkill ? pSkill->GetRuneSlot(static_cast<SkillSlot>(sk), r) : EquippedRune{};
            if (er.IsEmpty())
            {
                pIcons->DrawSolid(pCmd, rx, ry, rsz, rsz, empty);
            }
            else
            {
                const RuneDef* def = RuneRegistry::Get().Find(er.runeId);
                XMFLOAT4 fb = def ? ElementColor(def->element) : ElementColor(ElementType::None);
                pIcons->DrawIcon(pCmd, er.runeId, rx, ry, rsz, rsz, fb, 1.0f, 1.0f);
            }
        }
    }
}

void RuneRewardUI::RenderSkillSelectText(SpriteBatch* pBatch, SpriteFont* pFont,
                                         D3D12_GPU_DESCRIPTOR_HANDLE whiteTexGPU,
                                         const std::string& selectedRuneId, SkillComponent* pSkill,
                                         float w, float h)
{
    if (!pBatch || !pFont) return;

    const RuneDef* selDef = RuneRegistry::Get().Find(selectedRuneId);
    float titleY = h * 0.5f - 120.0f - 132.0f - 50.0f;
    DrawCentered(pFont, pBatch, L"장착할 룬 슬롯을 클릭하세요", w * 0.5f, titleY, Colors::Gold, 1.0f);

    // 선택한 룬 이름 (아이콘 아래)
    if (selDef)
    {
        std::wstring nm = Utf8Wide(selDef->name) + L" ";
        float selSz = 96.0f;
        float selY = h * 0.5f - 120.0f - 132.0f;
        std::wstring line = nm + GradeLabel(selDef->grade);
        DrawCentered(pFont, pBatch, line.c_str(), w * 0.5f, selY + selSz + 6.0f, GradeColor(selDef->grade), 0.62f);
    }

    const wchar_t* keyNames[] = { L"Q", L"E", L"R", L"M2" };
    for (int sk = 0; sk < static_cast<int>(SkillSlot::Count); ++sk)
    {
        float ix, iy, isz;
        SkillIconRect(sk, w, h, ix, iy, isz);
        pFont->DrawString(pBatch, keyNames[sk], XMFLOAT2(ix + 4.0f, iy + 2.0f),
                          Colors::White, 0.0f, XMFLOAT2(0, 0), 0.55f);

        // 스킬 이름 (아이콘 왼쪽)
        if (ISkillBehavior* pBeh = pSkill ? pSkill->GetSkill(static_cast<SkillSlot>(sk)) : nullptr)
        {
            const SkillData& data = pBeh->GetSkillData();
            std::wstring nm(data.name.begin(), data.name.end());
            float tw = XMVectorGetX(pFont->MeasureString(nm.c_str())) * 0.55f;
            pFont->DrawString(pBatch, nm.c_str(), XMFLOAT2(ix - 12.0f - tw, iy + isz * 0.5f - 12.0f),
                              XMVECTORF32{ 0.85f, 0.85f, 0.9f, 1.0f }, 0.0f, XMFLOAT2(0, 0), 0.55f);
        }
    }

    DrawCentered(pFont, pBatch, L"[클릭] 장착   [ESC] 취소",
                 w * 0.5f, h * 0.5f + 220.0f, Colors::Gray, 0.6f);

    // 호버한 룬 칸 툴팁
    if (m_hoverSkill >= 0 && m_hoverRune >= 0 && pSkill)
    {
        EquippedRune er = pSkill->GetRuneSlot(static_cast<SkillSlot>(m_hoverSkill), m_hoverRune);
        if (!er.IsEmpty())
        {
            const RuneDef* def = RuneRegistry::Get().Find(er.runeId);
            if (def)
            {
                float rx, ry, rsz;
                SkillRuneRect(m_hoverSkill, m_hoverRune, w, h, rx, ry, rsz);
                const float ts = 0.6f;
                std::wstring title = Utf8Wide(def->name) + L" " + GradeLabel(def->grade);
                std::wstring body  = WrapText(pFont, BuildRuneDesc(*def), 340.0f, ts);
                XMVECTOR tsz = pFont->MeasureString(title.c_str());
                XMVECTOR bsz = pFont->MeasureString(body.c_str());
                float tw = XMVectorGetX(tsz) * ts, th = XMVectorGetY(tsz) * ts;
                float bw = XMVectorGetX(bsz) * ts, bh = XMVectorGetY(bsz) * ts;
                float pad = 10.0f;
                float boxW = (std::max)(tw, bw) + pad * 2;
                float boxH = th + 4 + bh + pad * 2;
                float boxX = rx + rsz + 10.0f;
                float boxY = ry;
                if (boxX + boxW > w - 8) boxX = rx - boxW - 10.0f;
                FillRect(pBatch, whiteTexGPU, boxX, boxY, boxW, boxH, XMVECTORF32{ 0.05f, 0.05f, 0.07f, 0.92f });
                pFont->DrawString(pBatch, title.c_str(), XMFLOAT2(boxX + pad, boxY + pad),
                                  GradeColor(def->grade), 0.0f, XMFLOAT2(0, 0), ts);
                pFont->DrawString(pBatch, body.c_str(), XMFLOAT2(boxX + pad, boxY + pad + th + 4),
                                  XMVECTORF32{ 0.82f, 0.82f, 0.86f, 1.0f }, 0.0f, XMFLOAT2(0, 0), ts);
            }
        }
    }
}
