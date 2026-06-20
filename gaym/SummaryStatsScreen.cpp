#include "stdafx.h"
#include "SummaryStatsScreen.h"
#include "InputSystem.h"
#include "Scene.h"
#include "NetworkManager.h"
#include "PlayerComponent.h"
#include "SkillComponent.h"
#include "SkillIconRenderer.h"
#include "Dx12App.h"
#include "GameObject.h"

using namespace DirectX;

namespace
{
    constexpr int kMaxCardsPerPage = 4;

    const wchar_t* ElementName(ElementType e)
    {
        switch (e)
        {
        case ElementType::Fire:  return L"FIRE";
        case ElementType::Water: return L"WATER";
        case ElementType::Wind:  return L"WIND";
        case ElementType::Earth: return L"EARTH";
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
                    for (int r = 0; r < 3; ++r)
                        row.runeIds[s][r] = pSkill->GetRuneSlot(static_cast<SkillSlot>(s), r).runeId;
            }
        }

        m_vRows.push_back(row);
    }

    std::sort(m_vRows.begin(), m_vRows.end(),
              [](const PlayerRow& a, const PlayerRow& b){
                  return a.totalDamageDealt > b.totalDamageDealt;
              });

    int rowCount = static_cast<int>(m_vRows.size());
    m_nMaxPage = (rowCount <= kMaxCardsPerPage) ? 0 : ((rowCount - 1) / kMaxCardsPerPage);
}

void SummaryStatsScreen::DoLayout(float screenW, float screenH)
{
    const float btnW = 70.f, btnH = 70.f;
    m_btnPrev = { 30.f,             screenH * 0.5f - btnH * 0.5f, btnW, btnH };
    m_btnNext = { screenW - btnW - 30.f, screenH * 0.5f - btnH * 0.5f, btnW, btnH };

    const float contW = 260.f, contH = 64.f;
    m_btnContinue = { screenW * 0.5f - contW * 0.5f, screenH - contH - 28.f, contW, contH };
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
    auto hit = [&](const BtnRect& r){
        return mp.x >= r.x && mp.x <= r.x + r.w &&
               mp.y >= r.y && mp.y <= r.y + r.h;
    };
    m_nHoverBtn = -1;
    if (m_nPage > 0          && hit(m_btnPrev))    m_nHoverBtn = 0;
    else if (m_nPage < m_nMaxPage && hit(m_btnNext)) m_nHoverBtn = 1;
    else if (hit(m_btnContinue))                    m_nHoverBtn = 2;

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

    int startIdx = m_nPage * kMaxCardsPerPage;
    int endIdx   = (std::min)(startIdx + kMaxCardsPerPage, (int)m_vRows.size());
    int cardsThisPage = endIdx - startIdx;

    // 카드 영역
    float marginH = 110.f;
    float marginTop = 130.f, marginBot = 140.f;
    float cardAreaW = screenW - marginH * 2.f;
    float gap = 18.f;
    float cardW = (cardAreaW - gap * (cardsThisPage - 1)) / cardsThisPage;
    float cardH = screenH - marginTop - marginBot;

    for (int i = 0; i < cardsThisPage; ++i)
    {
        const PlayerRow& row = m_vRows[startIdx + i];
        float cardX = marginH + i * (cardW + gap);
        float cardY = marginTop;

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

        // stat 구분선 (헤더 아래 200px 부근, 룬 그리드 시작 전)
        float runeAreaY = cardY + cardH - 250.f;
        pIcons->DrawSolid(pCmd, cardX + 14.f, runeAreaY - 14.f, cardW - 28.f, 1.5f,
                          XMFLOAT4(elemCol.x * 0.6f, elemCol.y * 0.6f, elemCol.z * 0.6f, 0.5f * m_fFade));

        // 룬 grid 4행 × 3열 (실제 아이콘)
        float runeAreaH = 230.f;
        float runeCellW = (cardW - 32.f) / 3.f;
        float runeCellH = runeAreaH / 4.f;
        float runeIconSize = (std::min)(runeCellW, runeCellH) * 0.78f;

        // 슬롯 라벨 박스 (좌측 작은 표시)
        for (int s = 0; s < 4; ++s)
        {
            float rowY = runeAreaY + s * runeCellH;
            // 슬롯 행 배경 (살짝 어둡게)
            pIcons->DrawSolid(pCmd, cardX + 10.f, rowY + 2.f, cardW - 20.f, runeCellH - 4.f,
                              XMFLOAT4(0.0f, 0.0f, 0.0f, 0.25f * m_fFade));

            for (int r = 0; r < 3; ++r)
            {
                const std::string& runeId = row.runeIds[s][r];
                float cellCenterX = cardX + 16.f + r * runeCellW + runeCellW * 0.5f;
                float cellCenterY = rowY + runeCellH * 0.5f;
                float iconX = cellCenterX - runeIconSize * 0.5f;
                float iconY = cellCenterY - runeIconSize * 0.5f;

                // 빈 슬롯도 자리 표시 (점선 박스 대신 어두운 fill)
                pIcons->DrawSolid(pCmd, iconX, iconY, runeIconSize, runeIconSize,
                                  XMFLOAT4(0.0f, 0.0f, 0.0f, 0.4f * m_fFade));

                if (!runeId.empty())
                {
                    XMFLOAT4 iconTint(1.0f, 1.0f, 1.0f, m_fFade);
                    pIcons->DrawIcon(pCmd, runeId, iconX, iconY, runeIconSize, runeIconSize,
                                     iconTint, 1.0f, 1.0f);
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
                                 D3D12_GPU_DESCRIPTOR_HANDLE /*whiteTexGPU*/, XMUINT2 /*whiteTexSize*/,
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
    drawShadowedText(L"GAME CLEAR — SUMMARY", screenW * 0.5f, 24.f, gold, 1.5f, true);

    wchar_t pgBuf[64];
    swprintf_s(pgBuf, L"PAGE  %d / %d   (%s)",
               m_nPage + 1, m_nMaxPage + 1,
               (m_nPage == 0 ? L"OVERVIEW" : L"DETAILS"));
    drawText(pgBuf, screenW * 0.5f, 72.f, dim, 0.9f, true);

    if (m_vRows.empty())
    {
        drawShadowedText(L"NO STATS COLLECTED", screenW * 0.5f, screenH * 0.5f - 20.f, dim, 1.2f, true);
        // CONTINUE 텍스트
        {
            XMVECTOR ts = pFont->MeasureString(L"CONTINUE");
            float tw = XMVectorGetX(ts) * 1.0f, th = XMVectorGetY(ts) * 1.0f;
            XMVECTORF32 col = (m_nHoverBtn == 2) ? gold : white;
            pFont->DrawString(pBatch, L"CONTINUE",
                XMFLOAT2(m_btnContinue.x + (m_btnContinue.w - tw) * 0.5f,
                         m_btnContinue.y + (m_btnContinue.h - th) * 0.5f), col);
        }
        return;
    }

    int startIdx = m_nPage * kMaxCardsPerPage;
    int endIdx   = (std::min)(startIdx + kMaxCardsPerPage, (int)m_vRows.size());
    int cardsThisPage = endIdx - startIdx;

    float marginH = 110.f;
    float marginTop = 130.f, marginBot = 140.f;
    float cardAreaW = screenW - marginH * 2.f;
    float gap = 18.f;
    float cardW = (cardAreaW - gap * (cardsThisPage - 1)) / cardsThisPage;
    float cardH = screenH - marginTop - marginBot;

    // 카드별 텍스트
    for (int i = 0; i < cardsThisPage; ++i)
    {
        const PlayerRow& row = m_vRows[startIdx + i];
        float cardX = marginH + i * (cardW + gap);
        float cardY = marginTop;

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
            drawText(L"FINISHER", bx + 8.f, by + 5.f, black, 0.55f);
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

        if (m_nPage == 0)
        {
            swprintf_s(buf, L"%.0f", row.totalDamageDealt);
            drawStatLine(L"DAMAGE DEALT", buf, white);

            swprintf_s(buf, L"%.0f", row.totalDamageTaken);
            drawStatLine(L"DAMAGE TAKEN", buf, dim);

            swprintf_s(buf, L"%u", row.monstersKilled);
            drawStatLine(L"KILLS",        buf, white);

            swprintf_s(buf, L"%u", row.deathCount);
            drawStatLine(L"DEATHS",       buf, dim);

            sy += 8.f;
            drawText(L"SKILLS USED", tx, sy, elemCol, 0.7f);
            sy += 26.f;
            swprintf_s(buf, L"Q  %u    E  %u    R  %u    RC  %u",
                       row.skillUseCounts[0], row.skillUseCounts[1],
                       row.skillUseCounts[2], row.skillUseCounts[3]);
            drawShadowedText(buf, tx, sy, white, 0.78f);
            sy += 30.f;

            drawText(L"EQUIPPED RUNES", tx, sy, elemCol, 0.7f);
        }
        else
        {
            swprintf_s(buf, L"%.0f", row.maxSingleHit);
            drawStatLine(L"MAX SINGLE HIT", buf, white);

            swprintf_s(buf, L"%u", row.hitsLanded);
            drawStatLine(L"HITS LANDED",    buf, white);

            float dps = (row.survivalTime > 1.0f) ? (row.totalDamageDealt / row.survivalTime) : 0.0f;
            swprintf_s(buf, L"%.1f", dps);
            drawStatLine(L"AVG DPS",        buf, white);

            int totalSec = (int)row.survivalTime;
            swprintf_s(buf, L"%d:%02d", totalSec / 60, totalSec % 60);
            drawStatLine(L"SURVIVAL TIME",  buf, white);

            sy += 8.f;
            drawText(L"EQUIPPED RUNES", tx, sy, elemCol, 0.7f);
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
        XMVECTOR ts = pFont->MeasureString(L"CONTINUE");
        float tw = XMVectorGetX(ts) * 1.0f, th = XMVectorGetY(ts) * 1.0f;
        pFont->DrawString(pBatch, L"CONTINUE",
            XMFLOAT2(m_btnContinue.x + (m_btnContinue.w - tw) * 0.5f,
                     m_btnContinue.y + (m_btnContinue.h - th) * 0.5f), c);
    }
}

void SummaryStatsScreen::DrawCard(SpriteBatch* /*pBatch*/, SpriteFont* /*pFont*/,
                                   D3D12_GPU_DESCRIPTOR_HANDLE /*whiteTexGPU*/, XMUINT2 /*whiteTexSize*/,
                                   const PlayerRow& /*row*/, int /*colIdx*/,
                                   float /*screenW*/, float /*screenH*/)
{
    // unused — Render() 안에서 직접 처리
}
