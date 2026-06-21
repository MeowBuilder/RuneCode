#include "stdafx.h"
#include "SummaryStatsScreen.h"
#include "InputSystem.h"
#include "Scene.h"
#include "NetworkManager.h"
#include "PlayerComponent.h"
#include "SkillComponent.h"
#include "ISkillBehavior.h"
#include "SkillData.h"
#include "SkillIconRenderer.h"
#include "RuneRegistry.h"
#include "RuneDef.h"
#include "Dx12App.h"
#include "GameObject.h"
#include <sstream>
#include <iomanip>

using namespace DirectX;

namespace
{
    constexpr int   kMaxCardsPerPage = 2;
    // 룬 영역(카드 하단) 레이아웃 상수
    constexpr float kRuneAreaH       = 260.f;  // 룬 그리드 전체 높이
    constexpr float kRuneRowGap      = 6.f;
    constexpr float kRuneCardPadX    = 14.f;
    constexpr float kSkillIconColW   = 56.f;   // 좌측 스킬 아이콘 컬럼 너비
    constexpr float kSkillIconRuneGap = 10.f;

    const wchar_t* ElementName(ElementType e)
    {
        switch (e)
        {
        case ElementType::Fire:  return L"화염";
        case ElementType::Water: return L"물결";
        case ElementType::Wind:  return L"바람";
        case ElementType::Earth: return L"대지";
        default:                 return L"???";
        }
    }

    XMFLOAT4 ElementRGBA(ElementType e, float alpha = 1.0f)
    {
        switch (e)
        {
        case ElementType::Fire:  return { 1.00f, 0.55f, 0.20f, alpha };
        case ElementType::Water: return { 0.35f, 0.70f, 1.00f, alpha };
        case ElementType::Wind:  return { 0.65f, 1.00f, 0.55f, alpha };
        case ElementType::Earth: return { 0.90f, 0.75f, 0.30f, alpha };
        default:                 return { 1.00f, 1.00f, 1.00f, alpha };
        }
    }

    XMVECTORF32 ToVec(const XMFLOAT4& c)
    {
        return { c.x, c.y, c.z, c.w };
    }

    const wchar_t* SkillSlotLabel(int s)
    {
        switch (s) { case 0: return L"Q"; case 1: return L"E"; case 2: return L"R"; default: return L"RC"; }
    }

    // 원소 + 슬롯 → 스킬 이름 (Scene.cpp::Init 의 EquipSkill 매핑과 동일).
    // 원격 플레이어 SkillComponent 는 m_Skills 가 비어있어 GetSkill 이 nullptr 이라,
    // 원격 카드의 아이콘은 element + slot 으로 derive 한다.
    const char* SkillNameForSlot(ElementType e, int slot)
    {
        switch (e)
        {
        case ElementType::Fire:
            switch (slot) { case 0: return "WaveSlash";   case 1: return "FireBeam";    case 2: return "Meteor";     default: return "Fireball"; }
        case ElementType::Water:
            switch (slot) { case 0: return "WaterPuddle"; case 1: return "WaterVortex"; case 2: return "TidalWave";  default: return "WaterOrb"; }
        case ElementType::Wind:
            switch (slot) { case 0: return "WindCutter";  case 1: return "GaleRush";    case 2: return "Tornado";    default: return "WindShot"; }
        case ElementType::Earth:
            switch (slot) { case 0: return "StoneSpikes"; case 1: return "EarthArmor";  case 2: return "Earthquake"; default: return "EarthShard"; }
        default:
            return "";
        }
    }

    // 룬 등급별 색상 (SkillHudUI 와 동일 톤)
    XMFLOAT4 RuneGradeRGBA(RuneGrade g, float alpha)
    {
        switch (g)
        {
        case RuneGrade::Normal:    return { 0.65f, 0.65f, 0.70f, alpha }; // 회백
        case RuneGrade::Rare:      return { 0.40f, 0.65f, 1.00f, alpha }; // 파랑
        case RuneGrade::Epic:      return { 0.75f, 0.45f, 0.95f, alpha }; // 보라
        case RuneGrade::Unique:    return { 1.00f, 0.40f, 0.40f, alpha }; // 빨강
        case RuneGrade::Legendary: return { 1.00f, 0.85f, 0.25f, alpha }; // 금색
        default:                   return { 0.55f, 0.55f, 0.60f, alpha };
        }
    }

    XMVECTORF32 RuneGradeVec(RuneGrade g, float alpha)
    {
        XMFLOAT4 c = RuneGradeRGBA(g, alpha);
        return { c.x, c.y, c.z, c.w };
    }

    const wchar_t* RuneGradeText(RuneGrade g)
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

    std::wstring Utf8Wide(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        if (wlen <= 0) return std::wstring();
        std::wstring w(wlen, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], wlen);
        return w;
    }

    // RuneDef → 사람이 읽는 효과 설명. SkillHudUI::BuildRuneDesc 의 축약본.
    std::wstring BuildRuneDesc(const RuneDef& def)
    {
        std::wstringstream ss;
        auto pct = [](float m) { return (int)((m - 1.f) * 100.f + 0.5f); };
        if (!def.category.empty())    ss << L"[" << Utf8Wide(def.category) << L"] ";
        if (!def.description.empty()) ss << Utf8Wide(def.description) << L"  ";
        if (def.damageMult    != 1.f) ss << L"피해 "   << (pct(def.damageMult) >= 0 ? L"+" : L"") << pct(def.damageMult) << L"% ";
        if (def.radiusMult    != 1.f) ss << L"범위 "   << (pct(def.radiusMult) >= 0 ? L"+" : L"") << pct(def.radiusMult) << L"% ";
        if (def.cooldownMult  != 1.f) ss << L"쿨 "     << (pct(def.cooldownMult) >= 0 ? L"+" : L"") << pct(def.cooldownMult) << L"% ";
        if (def.durationMult  != 1.f) ss << L"지속 "   << (pct(def.durationMult) >= 0 ? L"+" : L"") << pct(def.durationMult) << L"% ";
        if (def.knockbackMult != 1.f) ss << L"넉백 "   << (pct(def.knockbackMult) >= 0 ? L"+" : L"") << pct(def.knockbackMult) << L"% ";
        if (def.extraProjectiles > 0) ss << L"투사체 +" << def.extraProjectiles << L" ";
        if (def.orbitalCount     > 0) ss << L"궤도탄 "  << def.orbitalCount << L" ";
        if (def.spawnOnHitCount  > 0) ss << L"반향 +"   << def.spawnOnHitCount << L" ";
        if (def.lifestealRatio   > 0.f) ss << L"흡수 " << (int)(def.lifestealRatio * 100.f + 0.5f) << L"% ";
        if (def.execDamageBonus  > 0.f) ss << L"처형 +" << (int)(def.execDamageBonus * 100.f + 0.5f) << L"% ";
        if (def.piercing)   ss << L"관통 ";
        if (def.homing)     ss << L"유도 ";
        if (def.doublecast) ss << L"쌍발 ";
        if (def.echoOnCast) ss << L"잔상 ";
        if (def.randomElementOnCast) ss << L"원소무작위 ";
        std::wstring r = ss.str();
        if (r.empty()) r = L"효과 없음";
        return r;
    }

    // 너비에 맞춰 ... 으로 절단
    std::wstring TruncateToWidth(SpriteFont* pFont, const std::wstring& s, float maxW, float scale)
    {
        if (s.empty()) return s;
        XMVECTOR ts = pFont->MeasureString(s.c_str());
        if (XMVectorGetX(ts) * scale <= maxW) return s;
        std::wstring tail = L"..";
        for (size_t n = s.size(); n > 0; --n)
        {
            std::wstring trial = s.substr(0, n) + tail;
            float w = XMVectorGetX(pFont->MeasureString(trial.c_str())) * scale;
            if (w <= maxW) return trial;
        }
        return tail;
    }

    // 단순 word-wrap (공백 기준)
    std::wstring WrapText(SpriteFont* font, const std::wstring& text, float maxWidth, float scale)
    {
        std::wstring result, line;
        std::wstringstream ss(text);
        std::wstring word;
        while (ss >> word)
        {
            std::wstring trial = line.empty() ? word : (line + L" " + word);
            float w = XMVectorGetX(font->MeasureString(trial.c_str())) * scale;
            if (!line.empty() && w > maxWidth) { result += line + L"\n"; line = word; }
            else line = trial;
        }
        if (!line.empty()) result += line;
        return result;
    }
}

void SummaryStatsScreen::Initialize(D3D12_GPU_DESCRIPTOR_HANDLE hBg, XMUINT2 bgSize)
{
    m_hBg = hBg;
    m_szBg = bgSize;
}

void SummaryStatsScreen::Reset()
{
    m_result    = Result::None;
    m_fFade     = 0.f;
    m_fInputCD  = 0.6f;
    m_nHoverBtn = -1;
    m_nPage     = 0;
    m_vRows.clear();
}

void SummaryStatsScreen::RebuildFromNetworkManager()
{
    m_vRows.clear();

    NetworkManager* pNet = NetworkManager::GetInstance();
    if (!pNet) return;

    Dx12App* pApp = Dx12App::GetInstance();
    Scene* pScene = pApp ? pApp->GetScene() : nullptr;

    auto& mapStats = pNet->GetGameClearStats();
    for (const auto& kv : mapStats)
    {
        PlayerRow row;
        row.playerId         = kv.first;
        row.element          = pNet->GetPlayerElementPublic(kv.first);
        row.totalDamageDealt = kv.second.totalDamageDealt;
        row.totalDamageTaken = kv.second.totalDamageTaken;
        row.maxSingleHit     = kv.second.maxSingleHit;
        row.hitsLanded       = kv.second.hitsLanded;
        row.deathCount       = kv.second.deathCount;
        row.monstersKilled   = kv.second.monstersKilled;
        row.bossLastHit      = kv.second.bossLastHit;
        row.survivalTime     = kv.second.survivalTime;
        // 서버 권위 라운드 경과초가 있으면 모든 카드 동일값으로 덮어 클라간 정합성 확보
        if (uint32 srvSec = pNet->GetServerRoundElapsedSec(); srvSec > 0)
            row.survivalTime = static_cast<float>(srvSec);
        for (int i = 0; i < 4; ++i)
            row.skillUseCounts[i] = kv.second.skillUseCounts[i];

        GameObject* pPlayerObj = nullptr;
        if (pScene && pNet->GetLocalPlayerId() == row.playerId)
            pPlayerObj = pScene->GetPlayer();
        else
            pPlayerObj = pNet->GetRemotePlayer(row.playerId);

        if (pPlayerObj)
        {
            if (SkillComponent* pSkill = pPlayerObj->GetComponent<SkillComponent>())
            {
                for (int s = 0; s < 4; ++s)
                {
                    for (int r = 0; r < 3; ++r)
                        row.runeIds[s][r] = pSkill->GetRuneSlot(static_cast<SkillSlot>(s), r).runeId;

                    if (ISkillBehavior* pBehavior = pSkill->GetSkill(static_cast<SkillSlot>(s)))
                        row.skillNames[s] = pBehavior->GetSkillData().name;
                }
            }
        }

        // 원격은 SkillComponent::m_Skills 가 비어있어 위 루프에서 안 채워짐.
        // element + slot 매핑으로 보정한다 (로컬도 비어있을 가능성 대비해 fallback).
        for (int s = 0; s < 4; ++s)
        {
            if (row.skillNames[s].empty())
                row.skillNames[s] = SkillNameForSlot(row.element, s);
        }

        m_vRows.push_back(row);
    }

    std::sort(m_vRows.begin(), m_vRows.end(),
              [](const PlayerRow& a, const PlayerRow& b){
                  return a.totalDamageDealt > b.totalDamageDealt;
              });

    int rowCount = static_cast<int>(m_vRows.size());
    m_nPlayerPages = (std::max)(1, (rowCount + kMaxCardsPerPage - 1) / kMaxCardsPerPage);
    // 2 뷰(OVERVIEW + DETAILS) × 플레이어 묶음 페이지
    m_nMaxPage = m_nPlayerPages * 2 - 1;
}

void SummaryStatsScreen::DoLayout(float screenW, float screenH)
{
    const float btnW = 70.f, btnH = 70.f;
    m_btnPrev = { 30.f,             screenH * 0.5f - btnH * 0.5f, btnW, btnH };
    m_btnNext = { screenW - btnW - 30.f, screenH * 0.5f - btnH * 0.5f, btnW, btnH };

    const float contW = 260.f, contH = 64.f;
    m_btnContinue = { screenW * 0.5f - contW * 0.5f, screenH - contH - 28.f, contW, contH };
}

void SummaryStatsScreen::ComputeCardRect(int cardIdx, int cardsThisPage,
                                         float screenW, float screenH,
                                         float& outX, float& outY,
                                         float& outW, float& outH) const
{
    const float marginH = 110.f;
    const float marginTop = 130.f, marginBot = 140.f;
    const float cardAreaW = screenW - marginH * 2.f;
    const float gap = 18.f;
    const float cardW = (cardsThisPage > 0)
        ? (cardAreaW - gap * (cardsThisPage - 1)) / cardsThisPage : cardAreaW;
    const float cardH = screenH - marginTop - marginBot;
    outX = marginH + cardIdx * (cardW + gap);
    outY = marginTop;
    outW = cardW;
    outH = cardH;
}

void SummaryStatsScreen::ComputeSkillIconRect(int cardIdx, int cardsThisPage, int slot,
                                              float screenW, float screenH,
                                              float& outX, float& outY, float& outSize) const
{
    float cardX, cardY, cardW, cardH;
    ComputeCardRect(cardIdx, cardsThisPage, screenW, screenH, cardX, cardY, cardW, cardH);
    const float runeAreaY = cardY + cardH - kRuneAreaH;
    const float rowH = (kRuneAreaH - kRuneRowGap * 3.f) / 4.f;
    const float size = (std::min)(kSkillIconColW, rowH) - 4.f;
    outX = cardX + kRuneCardPadX;
    outY = runeAreaY + slot * (rowH + kRuneRowGap) + (rowH - size) * 0.5f;
    outSize = size;
}

void SummaryStatsScreen::ComputeRuneCellRect(int cardIdx, int cardsThisPage, int slot, int runeIdx,
                                             float screenW, float screenH,
                                             float& outX, float& outY,
                                             float& outW, float& outH) const
{
    float cardX, cardY, cardW, cardH;
    ComputeCardRect(cardIdx, cardsThisPage, screenW, screenH, cardX, cardY, cardW, cardH);
    const float runeAreaY = cardY + cardH - kRuneAreaH;
    const float rowH = (kRuneAreaH - kRuneRowGap * 3.f) / 4.f;
    const float runeAreaX = cardX + kRuneCardPadX + kSkillIconColW + kSkillIconRuneGap;
    const float runeAreaW = (cardX + cardW - kRuneCardPadX) - runeAreaX;
    const float cellGap = 6.f;
    const float cellW = (runeAreaW - cellGap * 2.f) / 3.f;
    outX = runeAreaX + runeIdx * (cellW + cellGap);
    outY = runeAreaY + slot * (rowH + kRuneRowGap);
    outW = cellW;
    outH = rowH;
}

void SummaryStatsScreen::Update(InputSystem& input, Scene* /*pScene*/,
                                 float screenW, float screenH, float dt)
{
    if (m_result != Result::None) return;

    float fNext = m_fFade + dt * 1.5f;
    m_fFade = (fNext > 1.0f) ? 1.0f : fNext;

    if (m_fInputCD > 0.f) { m_fInputCD -= dt; return; }

    DoLayout(screenW, screenH);

    XMFLOAT2 mp = input.GetMousePosition();
    m_mousePos = mp;
    auto hit = [&](const BtnRect& r){
        return mp.x >= r.x && mp.x <= r.x + r.w &&
               mp.y >= r.y && mp.y <= r.y + r.h;
    };
    m_nHoverBtn = -1;
    if (m_nPage > 0          && hit(m_btnPrev))    m_nHoverBtn = 0;
    else if (m_nPage < m_nMaxPage && hit(m_btnNext)) m_nHoverBtn = 1;
    else if (hit(m_btnContinue))                    m_nHoverBtn = 2;

    // ── 룬 셀 호버 (페이지 내 카드만 검사) ─────────────────────
    m_nHoverCard = m_nHoverSlot = m_nHoverRune = -1;
    int playerSeg = (m_nPlayerPages > 0) ? (m_nPage % m_nPlayerPages) : 0;
    int startIdx = playerSeg * kMaxCardsPerPage;
    int endIdx   = (std::min)(startIdx + kMaxCardsPerPage, (int)m_vRows.size());
    int cardsThisPage = endIdx - startIdx;
    for (int i = 0; i < cardsThisPage && m_nHoverRune < 0; ++i)
    {
        for (int s = 0; s < 4 && m_nHoverRune < 0; ++s)
        {
            for (int r = 0; r < 3; ++r)
            {
                float rx, ry, rw, rh;
                ComputeRuneCellRect(i, cardsThisPage, s, r, screenW, screenH, rx, ry, rw, rh);
                if (mp.x >= rx && mp.x <= rx + rw && mp.y >= ry && mp.y <= ry + rh)
                {
                    m_nHoverCard = i; m_nHoverSlot = s; m_nHoverRune = r;
                    break;
                }
            }
        }
    }

    if (input.IsMouseButtonPressed(0))
    {
        if (m_nHoverBtn == 0) { m_nPage = (std::max)(0, m_nPage - 1); m_fInputCD = 0.15f; }
        else if (m_nHoverBtn == 1) { m_nPage = (std::min)(m_nMaxPage, m_nPage + 1); m_fInputCD = 0.15f; }
        else if (m_nHoverBtn == 2) { m_result = Result::ToTitle; }
    }

    if (input.IsKeyPressed(VK_LEFT))  { m_nPage = (std::max)(0, m_nPage - 1); m_fInputCD = 0.15f; }
    if (input.IsKeyPressed(VK_RIGHT)) { m_nPage = (std::min)(m_nMaxPage, m_nPage + 1); m_fInputCD = 0.15f; }
    if (input.IsKeyPressed(VK_RETURN) || input.IsKeyPressed(VK_SPACE))
        m_result = Result::ToTitle;
    if (input.IsKeyPressed(VK_ESCAPE))
        m_result = Result::Quit;
}

void SummaryStatsScreen::RenderIcons(ID3D12GraphicsCommandList* pCmd,
                                      SkillIconRenderer* pIcons,
                                      Scene* /*pScene*/, float screenW, float screenH)
{
    if (!pIcons || !pCmd) return;

    pIcons->Begin(pCmd, screenW, screenH);

    DoLayout(screenW, screenH);

    // ─── 풀스크린 어두운 오버레이 (가독성 ↑) ───────────────────────────
    pIcons->DrawSolid(pCmd, 0, 0, screenW, screenH,
                      XMFLOAT4(0.05f, 0.06f, 0.10f, 0.92f * m_fFade));

    // 상단 타이틀 바
    pIcons->DrawSolid(pCmd, 0, 0, screenW, 110.f,
                      XMFLOAT4(0.10f, 0.10f, 0.18f, 0.95f * m_fFade));
    pIcons->DrawSolid(pCmd, 0, 108.f, screenW, 2.f,
                      XMFLOAT4(0.55f, 0.65f, 0.95f, m_fFade));

    // 하단 바 (CONTINUE 영역)
    float footerY = screenH - 110.f;
    pIcons->DrawSolid(pCmd, 0, footerY, screenW, 110.f,
                      XMFLOAT4(0.10f, 0.10f, 0.18f, 0.95f * m_fFade));
    pIcons->DrawSolid(pCmd, 0, footerY, screenW, 2.f,
                      XMFLOAT4(0.55f, 0.65f, 0.95f, m_fFade));

    if (m_vRows.empty())
    {
        // 데이터 없을 때도 continue 버튼은 그림
        XMFLOAT4 bc = (m_nHoverBtn == 2)
            ? XMFLOAT4(0.35f, 0.35f, 0.50f, 0.95f * m_fFade)
            : XMFLOAT4(0.18f, 0.20f, 0.32f, 0.85f * m_fFade);
        pIcons->DrawSolid(pCmd, m_btnContinue.x, m_btnContinue.y,
                          m_btnContinue.w, m_btnContinue.h, bc);
        return;
    }

    int playerSeg = (m_nPlayerPages > 0) ? (m_nPage % m_nPlayerPages) : 0;
    int startIdx = playerSeg * kMaxCardsPerPage;
    int endIdx   = (std::min)(startIdx + kMaxCardsPerPage, (int)m_vRows.size());
    int cardsThisPage = endIdx - startIdx;

    for (int i = 0; i < cardsThisPage; ++i)
    {
        const PlayerRow& row = m_vRows[startIdx + i];

        float cardX, cardY, cardW, cardH;
        ComputeCardRect(i, cardsThisPage, screenW, screenH, cardX, cardY, cardW, cardH);

        XMFLOAT4 elemCol = ElementRGBA(row.element);

        // 카드 본체 배경 (어두운 베이스 + 원소 색조 미세)
        pIcons->DrawSolid(pCmd, cardX, cardY, cardW, cardH,
                          XMFLOAT4(0.08f, 0.09f, 0.14f, 0.92f * m_fFade));
        // 원소 색조 헤더 (상단 90px)
        pIcons->DrawSolid(pCmd, cardX, cardY, cardW, 90.f,
                          XMFLOAT4(elemCol.x * 0.30f, elemCol.y * 0.30f, elemCol.z * 0.30f, 0.95f * m_fFade));
        // 헤더 하단 라인
        pIcons->DrawSolid(pCmd, cardX, cardY + 88.f, cardW, 3.f,
                          XMFLOAT4(elemCol.x, elemCol.y, elemCol.z, m_fFade));

        // 외곽 테두리 (3px)
        XMFLOAT4 borderCol{ elemCol.x, elemCol.y, elemCol.z, m_fFade };
        pIcons->DrawSolid(pCmd, cardX,             cardY,             cardW, 3.f, borderCol);
        pIcons->DrawSolid(pCmd, cardX,             cardY + cardH - 3, cardW, 3.f, borderCol);
        pIcons->DrawSolid(pCmd, cardX,             cardY,             3.f,   cardH, borderCol);
        pIcons->DrawSolid(pCmd, cardX + cardW - 3, cardY,             3.f,   cardH, borderCol);

        // stat 구분선 (룬 그리드 시작 바로 위)
        float runeAreaY = cardY + cardH - kRuneAreaH;
        pIcons->DrawSolid(pCmd, cardX + 14.f, runeAreaY - 14.f, cardW - 28.f, 1.5f,
                          XMFLOAT4(elemCol.x * 0.6f, elemCol.y * 0.6f, elemCol.z * 0.6f, 0.5f * m_fFade));

        // ── 룬 그리드: [스킬아이콘 | 룬1 | 룬2 | 룬3] × 4행 ──
        for (int s = 0; s < 4; ++s)
        {
            float sx, sy, ssz;
            ComputeSkillIconRect(i, cardsThisPage, s, screenW, screenH, sx, sy, ssz);

            // 행 전체 백드롭 (살짝 어두운 띠)
            const float rowH = (kRuneAreaH - kRuneRowGap * 3.f) / 4.f;
            const float rowY = runeAreaY + s * (rowH + kRuneRowGap);
            pIcons->DrawSolid(pCmd, cardX + 10.f, rowY, cardW - 20.f, rowH,
                              XMFLOAT4(0.0f, 0.0f, 0.0f, 0.28f * m_fFade));

            // 스킬 아이콘 배경 + 아이콘
            pIcons->DrawSolid(pCmd, sx - 2.f, sy - 2.f, ssz + 4.f, ssz + 4.f,
                              XMFLOAT4(elemCol.x * 0.4f, elemCol.y * 0.4f, elemCol.z * 0.4f, 0.85f * m_fFade));
            if (!row.skillNames[s].empty())
            {
                XMFLOAT4 tint(1.0f, 1.0f, 1.0f, m_fFade);
                pIcons->DrawIcon(pCmd, row.skillNames[s], sx, sy, ssz, ssz,
                                 ElementRGBA(row.element, m_fFade), 1.0f, 1.0f);
            }
            else
            {
                pIcons->DrawSolid(pCmd, sx, sy, ssz, ssz,
                                  XMFLOAT4(0.1f, 0.1f, 0.13f, 0.85f * m_fFade));
            }

            // 룬 셀 3개
            for (int r = 0; r < 3; ++r)
            {
                float cx, cy, cw, ch;
                ComputeRuneCellRect(i, cardsThisPage, s, r, screenW, screenH, cx, cy, cw, ch);

                const std::string& runeId = row.runeIds[s][r];
                const RuneDef* def = runeId.empty()
                    ? nullptr : RuneRegistry::Get().Find(runeId);

                bool isHovered = (m_nHoverCard == i && m_nHoverSlot == s && m_nHoverRune == r);

                // 셀 배경
                if (def)
                {
                    XMFLOAT4 base = RuneGradeRGBA(def->grade, 0.32f * m_fFade);
                    pIcons->DrawSolid(pCmd, cx, cy, cw, ch, base);
                    // 등급색 좌측 strip (4px) — 빠른 등급 식별
                    XMFLOAT4 strip = RuneGradeRGBA(def->grade, 0.95f * m_fFade);
                    pIcons->DrawSolid(pCmd, cx, cy, 4.f, ch, strip);
                }
                else
                {
                    pIcons->DrawSolid(pCmd, cx, cy, cw, ch,
                                      XMFLOAT4(0.0f, 0.0f, 0.0f, 0.45f * m_fFade));
                }

                // 호버 강조 (테두리)
                if (isHovered)
                {
                    XMFLOAT4 hcol{ 1.0f, 0.85f, 0.30f, 0.95f * m_fFade };
                    pIcons->DrawSolid(pCmd, cx,              cy,              cw, 2.f, hcol);
                    pIcons->DrawSolid(pCmd, cx,              cy + ch - 2.f,   cw, 2.f, hcol);
                    pIcons->DrawSolid(pCmd, cx,              cy,              2.f, ch, hcol);
                    pIcons->DrawSolid(pCmd, cx + cw - 2.f,   cy,              2.f, ch, hcol);
                }
            }
        }

        // BOSS LAST HIT 마크 — 우측 상단 작은 박스
        if (row.bossLastHit)
        {
            float badgeW = 80.f, badgeH = 24.f;
            float bx = cardX + cardW - badgeW - 8.f;
            float by = cardY + 8.f;
            pIcons->DrawSolid(pCmd, bx, by, badgeW, badgeH,
                              XMFLOAT4(1.0f, 0.85f, 0.30f, 0.95f * m_fFade));
        }
    }

    // ─── 화살표 / CONTINUE 버튼 배경 ───────────────────────────────
    if (m_nPage > 0)
    {
        XMFLOAT4 c = (m_nHoverBtn == 0)
            ? XMFLOAT4(0.40f, 0.40f, 0.55f, 0.95f * m_fFade)
            : XMFLOAT4(0.18f, 0.20f, 0.30f, 0.80f * m_fFade);
        pIcons->DrawSolid(pCmd, m_btnPrev.x, m_btnPrev.y, m_btnPrev.w, m_btnPrev.h, c);
    }
    if (m_nPage < m_nMaxPage)
    {
        XMFLOAT4 c = (m_nHoverBtn == 1)
            ? XMFLOAT4(0.40f, 0.40f, 0.55f, 0.95f * m_fFade)
            : XMFLOAT4(0.18f, 0.20f, 0.30f, 0.80f * m_fFade);
        pIcons->DrawSolid(pCmd, m_btnNext.x, m_btnNext.y, m_btnNext.w, m_btnNext.h, c);
    }

    XMFLOAT4 bc = (m_nHoverBtn == 2)
        ? XMFLOAT4(0.40f, 0.40f, 0.55f, 0.95f * m_fFade)
        : XMFLOAT4(0.18f, 0.20f, 0.32f, 0.85f * m_fFade);
    pIcons->DrawSolid(pCmd, m_btnContinue.x, m_btnContinue.y,
                      m_btnContinue.w, m_btnContinue.h, bc);
    // CONTINUE 버튼 하단 강조 라인
    pIcons->DrawSolid(pCmd, m_btnContinue.x, m_btnContinue.y + m_btnContinue.h - 3.f,
                      m_btnContinue.w, 3.f,
                      XMFLOAT4(1.0f, 0.85f, 0.30f, m_fFade));
}

void SummaryStatsScreen::Render(SpriteBatch* pBatch, SpriteFont* pFont,
                                 D3D12_GPU_DESCRIPTOR_HANDLE whiteTexGPU, XMUINT2 /*whiteTexSize*/,
                                 Scene* /*pScene*/, float screenW, float screenH)
{
    DoLayout(screenW, screenH);

    XMVECTORF32 white   = { 1.0f, 1.0f, 1.0f, m_fFade };
    XMVECTORF32 dim     = { 0.70f, 0.72f, 0.78f, m_fFade };
    XMVECTORF32 gold    = { 1.0f, 0.88f, 0.35f, m_fFade };
    XMVECTORF32 goldDk  = { 0.85f, 0.55f, 0.10f, m_fFade };

    auto drawText = [&](const wchar_t* text, float x, float y, XMVECTORF32 col,
                        float scale = 1.0f, bool centerX = false) {
        XMVECTOR ts = pFont->MeasureString(text);
        float tw = XMVectorGetX(ts) * scale;
        float dx = centerX ? (x - tw * 0.5f) : x;
        pFont->DrawString(pBatch, text, XMFLOAT2(dx, y), col, 0.f,
                          XMFLOAT2(0.f, 0.f), XMFLOAT2(scale, scale));
    };
    auto drawShadowedText = [&](const wchar_t* text, float x, float y, XMVECTORF32 col,
                                 float scale = 1.0f, bool centerX = false) {
        XMVECTORF32 shadow = { 0.f, 0.f, 0.f, 0.65f * m_fFade };
        XMVECTOR ts = pFont->MeasureString(text);
        float tw = XMVectorGetX(ts) * scale;
        float dx = centerX ? (x - tw * 0.5f) : x;
        pFont->DrawString(pBatch, text, XMFLOAT2(dx + 2.f, y + 2.f), shadow, 0.f,
                          XMFLOAT2(0.f, 0.f), XMFLOAT2(scale, scale));
        pFont->DrawString(pBatch, text, XMFLOAT2(dx, y), col, 0.f,
                          XMFLOAT2(0.f, 0.f), XMFLOAT2(scale, scale));
    };

    // 타이틀
    drawShadowedText(L"게임 클리어 - 결산", screenW * 0.5f, 24.f, gold, 1.5f, true);

    int playerSeg = (m_nPlayerPages > 0) ? (m_nPage % m_nPlayerPages) : 0;
    int viewMode  = (m_nPlayerPages > 0) ? (m_nPage / m_nPlayerPages) : 0;
    int pStart    = playerSeg * kMaxCardsPerPage + 1;
    int pEnd      = (std::min)(playerSeg * kMaxCardsPerPage + kMaxCardsPerPage, (int)m_vRows.size());

    wchar_t pgBuf[96];
    swprintf_s(pgBuf, L"페이지  %d / %d    %s    플레이어 %d-%d",
               m_nPage + 1, m_nMaxPage + 1,
               (viewMode == 0 ? L"요약" : L"세부"),
               pStart, (std::max)(pStart, pEnd));
    drawText(pgBuf, screenW * 0.5f, 72.f, dim, 0.9f, true);

    if (m_vRows.empty())
    {
        drawShadowedText(L"수집된 기록 없음", screenW * 0.5f, screenH * 0.5f - 20.f, dim, 1.2f, true);
        // 계속 텍스트
        {
            XMVECTOR ts = pFont->MeasureString(L"계속");
            float tw = XMVectorGetX(ts) * 1.0f, th = XMVectorGetY(ts) * 1.0f;
            XMVECTORF32 col = (m_nHoverBtn == 2) ? gold : white;
            pFont->DrawString(pBatch, L"계속",
                XMFLOAT2(m_btnContinue.x + (m_btnContinue.w - tw) * 0.5f,
                         m_btnContinue.y + (m_btnContinue.h - th) * 0.5f), col);
        }
        return;
    }

    int startIdx = playerSeg * kMaxCardsPerPage;
    int endIdx   = (std::min)(startIdx + kMaxCardsPerPage, (int)m_vRows.size());
    int cardsThisPage = endIdx - startIdx;

    // 카드별 텍스트
    for (int i = 0; i < cardsThisPage; ++i)
    {
        const PlayerRow& row = m_vRows[startIdx + i];
        float cardX, cardY, cardW, cardH;
        ComputeCardRect(i, cardsThisPage, screenW, screenH, cardX, cardY, cardW, cardH);

        XMFLOAT4 elemRGBA = ElementRGBA(row.element);
        XMVECTORF32 elemCol = ToVec(elemRGBA);
        elemCol.f[3] = m_fFade;

        // ── 헤더: 원소명 큰 글씨 ─────────────────────────────
        drawShadowedText(ElementName(row.element), cardX + 18.f, cardY + 14.f, elemCol, 1.3f);
        wchar_t pid[32];
        swprintf_s(pid, L"PID %llu", (unsigned long long)(row.playerId % 100000));
        drawText(pid, cardX + 18.f, cardY + 56.f, dim, 0.65f);

        // BOSS LAST HIT 라벨 (badge 위)
        if (row.bossLastHit)
        {
            XMVECTORF32 black = { 0.05f, 0.05f, 0.05f, m_fFade };
            float badgeW = 80.f;
            float bx = cardX + cardW - badgeW - 8.f;
            float by = cardY + 8.f;
            drawText(L"막타", bx + 8.f, by + 5.f, black, 0.55f);
        }

        // ── stat 본문 ─────────────────────────────────────
        float tx = cardX + 18.f;
        float sy = cardY + 110.f;
        float lineH = 30.f;

        wchar_t buf[160];

        auto drawStatLine = [&](const wchar_t* label, const wchar_t* valueText, XMVECTORF32 valCol) {
            drawText(label, tx, sy, dim, 0.7f);
            // 값 우측 정렬
            XMVECTOR ts = pFont->MeasureString(valueText);
            float vw = XMVectorGetX(ts) * 0.95f;
            float vx = cardX + cardW - 18.f - vw;
            drawShadowedText(valueText, vx, sy - 4.f, valCol, 0.95f);
            sy += lineH;
        };

        if (viewMode == 0)
        {
            swprintf_s(buf, L"%.0f", row.totalDamageDealt);
            drawStatLine(L"가한 피해", buf, white);

            swprintf_s(buf, L"%.0f", row.totalDamageTaken);
            drawStatLine(L"받은 피해", buf, dim);

            swprintf_s(buf, L"%u", row.monstersKilled);
            drawStatLine(L"처치",     buf, white);

            swprintf_s(buf, L"%u", row.deathCount);
            drawStatLine(L"사망",     buf, dim);
        }
        else
        {
            swprintf_s(buf, L"%.0f", row.maxSingleHit);
            drawStatLine(L"최대 단일 피해", buf, white);

            swprintf_s(buf, L"%u", row.hitsLanded);
            drawStatLine(L"명중 횟수",       buf, white);

            float dps = (row.survivalTime > 1.0f) ? (row.totalDamageDealt / row.survivalTime) : 0.0f;
            swprintf_s(buf, L"%.1f", dps);
            drawStatLine(L"평균 DPS",        buf, white);

            int totalSec = (int)row.survivalTime;
            swprintf_s(buf, L"%d:%02d", totalSec / 60, totalSec % 60);
            drawStatLine(L"생존 시간",        buf, white);
        }

        // ── 룬 영역 라벨 (그리드 바로 위에 고정) ──────────────
        float runeAreaY = cardY + cardH - kRuneAreaH;
        drawText(L"장착 룬", cardX + kRuneCardPadX, runeAreaY - 24.f,
                 elemCol, 0.72f);

        // ── 룬 그리드: 슬롯 라벨 + 사용 횟수 + 룬 이름 ──────
        for (int s = 0; s < 4; ++s)
        {
            float sx, sy2, ssz;
            ComputeSkillIconRect(i, cardsThisPage, s, screenW, screenH, sx, sy2, ssz);

            // Q/E/R/RC 라벨 (스킬 아이콘 좌상단 모서리)
            drawShadowedText(SkillSlotLabel(s), sx + 4.f, sy2 + 2.f, white, 0.55f);

            // 사용 횟수 (스킬 아이콘 우하단)
            wchar_t cnt[16];
            swprintf_s(cnt, L"x%u", row.skillUseCounts[s]);
            XMVECTOR cz = pFont->MeasureString(cnt);
            float cnw = XMVectorGetX(cz) * 0.50f;
            float cnh = XMVectorGetY(cz) * 0.50f;
            drawShadowedText(cnt, sx + ssz - cnw - 3.f, sy2 + ssz - cnh - 2.f, gold, 0.50f);

            // 룬 이름 — 셀 안에 표시
            for (int r = 0; r < 3; ++r)
            {
                float cx, cy, cw, ch;
                ComputeRuneCellRect(i, cardsThisPage, s, r, screenW, screenH, cx, cy, cw, ch);

                const std::string& runeId = row.runeIds[s][r];
                if (runeId.empty())
                {
                    drawText(L"-", cx + cw * 0.5f, cy + ch * 0.5f - 8.f,
                             dim, 0.7f, true);
                    continue;
                }

                const RuneDef* def = RuneRegistry::Get().Find(runeId);
                std::wstring nameW = def ? Utf8Wide(def->name) : Utf8Wide(runeId);
                const float nameScale = 0.55f;
                const float maxNameW = cw - 12.f;       // 좌측 strip + 패딩
                nameW = TruncateToWidth(pFont, nameW, maxNameW, nameScale);

                XMVECTORF32 nameCol = def ? RuneGradeVec(def->grade, m_fFade) : white;
                XMVECTOR nsz = pFont->MeasureString(nameW.c_str());
                float nw = XMVectorGetX(nsz) * nameScale;
                float nh = XMVectorGetY(nsz) * nameScale;
                drawShadowedText(nameW.c_str(),
                                 cx + 8.f + ((cw - 8.f) - nw) * 0.5f,
                                 cy + (ch - nh) * 0.5f,
                                 nameCol, nameScale);
            }
        }
    }

    // ─── 화살표 / CONTINUE 텍스트 ───────────────────────────
    if (m_nPage > 0)
    {
        XMVECTORF32 c = (m_nHoverBtn == 0) ? gold : white;
        XMVECTOR ts = pFont->MeasureString(L"<");
        float tw = XMVectorGetX(ts) * 1.5f, th = XMVectorGetY(ts) * 1.5f;
        pFont->DrawString(pBatch, L"<",
            XMFLOAT2(m_btnPrev.x + (m_btnPrev.w - tw) * 0.5f,
                     m_btnPrev.y + (m_btnPrev.h - th) * 0.5f),
            c, 0.f, XMFLOAT2(0,0), XMFLOAT2(1.5f, 1.5f));
    }
    if (m_nPage < m_nMaxPage)
    {
        XMVECTORF32 c = (m_nHoverBtn == 1) ? gold : white;
        XMVECTOR ts = pFont->MeasureString(L">");
        float tw = XMVectorGetX(ts) * 1.5f, th = XMVectorGetY(ts) * 1.5f;
        pFont->DrawString(pBatch, L">",
            XMFLOAT2(m_btnNext.x + (m_btnNext.w - tw) * 0.5f,
                     m_btnNext.y + (m_btnNext.h - th) * 0.5f),
            c, 0.f, XMFLOAT2(0,0), XMFLOAT2(1.5f, 1.5f));
    }
    {
        XMVECTORF32 c = (m_nHoverBtn == 2) ? gold : white;
        XMVECTOR ts = pFont->MeasureString(L"계속");
        float tw = XMVectorGetX(ts) * 1.0f, th = XMVectorGetY(ts) * 1.0f;
        pFont->DrawString(pBatch, L"계속",
            XMFLOAT2(m_btnContinue.x + (m_btnContinue.w - tw) * 0.5f,
                     m_btnContinue.y + (m_btnContinue.h - th) * 0.5f), c);
    }

    // ─── 룬 호버 툴팁 ─────────────────────────────────────────
    if (m_nHoverCard >= 0 && m_nHoverSlot >= 0 && m_nHoverRune >= 0 &&
        startIdx + m_nHoverCard < (int)m_vRows.size())
    {
        const PlayerRow& hr = m_vRows[startIdx + m_nHoverCard];
        const std::string& runeId = hr.runeIds[m_nHoverSlot][m_nHoverRune];
        if (!runeId.empty())
        {
            const RuneDef* def = RuneRegistry::Get().Find(runeId);
            std::wstring title;
            XMVECTORF32  titleCol = white;
            std::wstring body;
            if (def)
            {
                title = Utf8Wide(def->name);
                title += L" ";
                title += RuneGradeText(def->grade);
                titleCol = RuneGradeVec(def->grade, m_fFade);
                body = BuildRuneDesc(*def);
            }
            else
            {
                title = Utf8Wide(runeId);
                body  = L"등록되지 않은 룬";
            }

            const float ttScale = 0.62f;
            const float maxBodyW = 360.f;
            body = WrapText(pFont, body, maxBodyW, ttScale);

            XMVECTOR titleSz = pFont->MeasureString(title.c_str());
            XMVECTOR bodySz  = pFont->MeasureString(body.c_str());
            float titleW = XMVectorGetX(titleSz) * ttScale;
            float titleH = XMVectorGetY(titleSz) * ttScale;
            float bodyW  = XMVectorGetX(bodySz)  * ttScale;
            float bodyH  = XMVectorGetY(bodySz)  * ttScale;

            const float pad = 10.f, lineGap = 4.f;
            float boxW = (std::max)(titleW, bodyW) + pad * 2.f;
            float boxH = titleH + lineGap + bodyH + pad * 2.f;

            float hx, hy, hw, hh;
            ComputeRuneCellRect(m_nHoverCard, cardsThisPage,
                                m_nHoverSlot, m_nHoverRune,
                                screenW, screenH, hx, hy, hw, hh);
            float boxX = hx;
            float boxY = hy - boxH - 8.f;
            if (boxX + boxW > screenW - 8.f) boxX = screenW - 8.f - boxW;
            if (boxX < 8.f) boxX = 8.f;
            if (boxY < 8.f) boxY = hy + hh + 8.f;

            // 배경
            if (whiteTexGPU.ptr)
            {
                RECT bg = { (LONG)boxX, (LONG)boxY,
                            (LONG)(boxX + boxW), (LONG)(boxY + boxH) };
                pBatch->Draw(whiteTexGPU, XMUINT2(1, 1), bg,
                             XMVECTORF32{ 0.05f, 0.05f, 0.07f, 0.93f * m_fFade });
            }
            // 제목 + 본문
            pFont->DrawString(pBatch, title.c_str(),
                              XMFLOAT2(boxX + pad, boxY + pad),
                              titleCol, 0.f, XMFLOAT2(0, 0),
                              XMFLOAT2(ttScale, ttScale));
            pFont->DrawString(pBatch, body.c_str(),
                              XMFLOAT2(boxX + pad, boxY + pad + titleH + lineGap),
                              XMVECTORF32{ 0.82f, 0.82f, 0.86f, m_fFade },
                              0.f, XMFLOAT2(0, 0), XMFLOAT2(ttScale, ttScale));
        }
    }
}

void SummaryStatsScreen::DrawCard(SpriteBatch* /*pBatch*/, SpriteFont* /*pFont*/,
                                   D3D12_GPU_DESCRIPTOR_HANDLE /*whiteTexGPU*/, XMUINT2 /*whiteTexSize*/,
                                   const PlayerRow& /*row*/, int /*colIdx*/,
                                   float /*screenW*/, float /*screenH*/)
{
    // unused — Render() 안에서 직접 처리
}
