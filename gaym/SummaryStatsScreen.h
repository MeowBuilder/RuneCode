#pragma once
#include "stdafx.h"
#include <SpriteBatch.h>
#include <SpriteFont.h>
#include <DirectXMath.h>
#include <vector>
#include "SkillTypes.h"

class InputSystem;
class Scene;
class SkillIconRenderer;

// 결산 화면: EndingScreen → SummaryStatsScreen → Title 순서로 진입.
//   DarkLord 클리어 시 NetworkManager 가 가진 누적 stat 을 페이지별로 표시.
//   페이지 1: 캐릭터별 카드 (이름/원소/데미지/받은데미지/스킬 사용 / 룬 슬롯)
//   페이지 2: 디테일 (단일 최대 / 명중 / 처치 / 사망 / 생존 시간)
//   좌우 화살표 또는 ← → 키 로 페이지 이동.
class SummaryStatsScreen
{
public:
    enum class Result { None, ToTitle, Quit };

    void Initialize(D3D12_GPU_DESCRIPTOR_HANDLE hBg, DirectX::XMUINT2 bgSize);

    // pScene 으로 GetAllPlayers + element 조회. NetworkManager 에서 stat 가져옴.
    void Update(InputSystem& input, Scene* pScene, float screenW, float screenH, float dt);

    // Icon 패스 (룬/스킬 아이콘) — SpriteBatch::Begin 호출 전에 실행.
    void RenderIcons(ID3D12GraphicsCommandList* pCmd, SkillIconRenderer* pIcons,
                     Scene* pScene, float screenW, float screenH);

    // 텍스트/배경/카드 백드롭 패스 — SpriteBatch::Begin/End 안에서 실행.
    void Render(DirectX::SpriteBatch* pBatch, DirectX::SpriteFont* pFont,
                D3D12_GPU_DESCRIPTOR_HANDLE whiteTexGPU, DirectX::XMUINT2 whiteTexSize,
                Scene* pScene, float screenW, float screenH);

    Result GetResult() const { return m_result; }
    void   Reset();

    // 외부에서 데이터 모음 트리거. AppState 전환 시 호출.
    void RebuildFromNetworkManager();

private:
    // 각 화살표/버튼 hit-rect
    struct BtnRect { float x, y, w, h; };

    struct PlayerRow
    {
        uint64       playerId        = 0;
        ElementType  element         = ElementType::Fire;
        float        totalDamageDealt = 0.0f;
        float        totalDamageTaken = 0.0f;
        float        maxSingleHit     = 0.0f;
        uint32_t     hitsLanded       = 0;
        uint32_t     deathCount       = 0;
        uint32_t     monstersKilled   = 0;
        bool         bossLastHit      = false;
        float        survivalTime     = 0.0f;
        uint32_t     skillUseCounts[4]= { 0,0,0,0 };
        // 슬롯[4] × 룬[3] = 12 runeId
        std::string  runeIds[4][3];
        // 슬롯별 장착 스킬 이름 (SkillIconRenderer 키)
        std::string  skillNames[4];
    };

    void DoLayout(float screenW, float screenH);
    void DrawCard(DirectX::SpriteBatch* pBatch, DirectX::SpriteFont* pFont,
                  D3D12_GPU_DESCRIPTOR_HANDLE whiteTexGPU, DirectX::XMUINT2 whiteTexSize,
                  const PlayerRow& row, int colIdx, float screenW, float screenH);

    // 룬 카드 안의 룬 셀/스킬 아이콘 위치 계산 — Update/RenderIcons/Render 공용.
    void ComputeCardRect(int cardIdx, int cardsThisPage, float screenW, float screenH,
                         float& outX, float& outY, float& outW, float& outH) const;
    void ComputeSkillIconRect(int cardIdx, int cardsThisPage, int slot,
                              float screenW, float screenH,
                              float& outX, float& outY, float& outSize) const;
    void ComputeRuneCellRect(int cardIdx, int cardsThisPage, int slot, int runeIdx,
                             float screenW, float screenH,
                             float& outX, float& outY, float& outW, float& outH) const;

    D3D12_GPU_DESCRIPTOR_HANDLE m_hBg{};
    DirectX::XMUINT2            m_szBg{};

    Result m_result    = Result::None;
    float  m_fFade     = 0.f;
    float  m_fInputCD  = 0.6f;
    int    m_nHoverBtn = -1;          // -1=none, 0=prev, 1=next, 2=continue

    // 페이지 인코딩: m_nPage = viewMode * m_nPlayerPages + playerSegment
    //   playerSegment = m_nPage % m_nPlayerPages    (어느 플레이어 묶음을 보일지)
    //   viewMode      = m_nPage / m_nPlayerPages    (0=OVERVIEW, 1=DETAILS)
    int    m_nPage         = 0;
    int    m_nMaxPage      = 1;
    int    m_nPlayerPages  = 1;

    BtnRect m_btnPrev{}, m_btnNext{}, m_btnContinue{};

    // 룬 셀 호버 (-1 = 호버 없음)
    int    m_nHoverCard = -1;
    int    m_nHoverSlot = -1;
    int    m_nHoverRune = -1;
    DirectX::XMFLOAT2 m_mousePos{};

    std::vector<PlayerRow> m_vRows;
};
