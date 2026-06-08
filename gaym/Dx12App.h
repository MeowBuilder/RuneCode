#include "stdafx.h"
#include "Timer.h"
#include "Scene.h"
#include "InputSystem.h" // Added InputSystem include
#include "HealthBarUI.h"
#include "VFXSpriteManager.h"
#include "NetworkManager.h" // Added NetworkManager include
#include "BloomPostProcess.h"
#include "CharacterSelectScreen.h"
#include "TitleScreen.h"
#include "EndingScreen.h"
#include "GameOverScreen.h"
#include "LoadingScreen.h"
#include "AudioManager.h"
#include <memory>
#include <string>
#include <vector>

// DirectXTK12 for text rendering
#include <SpriteFont.h>
#include <SpriteBatch.h>
#include <ResourceUploadBatch.h>
#include <GraphicsMemory.h>
#include <DescriptorHeap.h>

enum class AppState { Title, CharacterSelect, Loading, Playing, GameOver, Ending };

// UI 텍스처 슬롯 — m_fontDescriptorHeap 의 [kUIHeapBase + (UINT)UISlot::*]
enum class UISlot : UINT {
    TitleBg = 0, TitleLogo, BtnNormal, BtnHover,
    LoadingBg, LoadingSpinner,
    PausePanel, GameOverBg,
    GameOverTitle,
    EndingBg, EndingTitle,
    HudStageBadge, HudBossBar, HudBossBarFill,
    AvatarFire, AvatarWater, AvatarWind, AvatarEarth,
    IntroSlash,   // 최종 보스 입장 컷씬 — 화면 대각선 베기 스크린 오버레이
    Count
};

class Dx12App
{
public:
    Dx12App();
    ~Dx12App();

    static Dx12App* GetInstance() { return s_pInstance; }

    void OnCreate(HINSTANCE hInstance, HWND hMainWnd);
    void OnDestroy();
    void FrameAdvance();
    void ToggleFullscreen();
    void OnResize(UINT nWidth, UINT nHeight);
    void OnActivateApp(bool active);  // 포커스 손실/복귀 시 오디오 일시정지/재개

    InputSystem& GetInputSystem() { return m_inputSystem; } // Added getter for InputSystem
    ID3D12Device* GetDevice() const { return m_pd3dDevice.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return m_pd3dCommandList.Get(); }
    Scene* GetScene() const { return m_pScene.get(); }
    class AudioManager* GetAudio() const { return m_pAudio.get(); }
    class WhiteFlashOverlay* GetWhiteFlash() const { return m_pWhiteFlash.get(); }
    class ScreenSplitOverlay* GetScreenSplit() const { return m_pScreenSplit.get(); }

    // 런타임 윈도우 크기 (NDC 변환용)
    UINT GetWindowWidth() const { return m_nWndClientWidth; }
    UINT GetWindowHeight() const { return m_nWndClientHeight; }

    // Sky/clear color — 스테이지 테마별 톤 변경용
    void SetClearColor(float r, float g, float b)
    {
        m_fClearColor[0] = r; m_fClearColor[1] = g; m_fClearColor[2] = b;
    }

    static ComPtr<ID3D12Resource> CreateBufferResource(const void* pData, UINT nBytes, D3D12_HEAP_TYPE d3dHeapType = D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATES d3dResourceStates = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, ComPtr<ID3D12Resource>* ppd3dUploadBuffer = NULL);

private:
    static Dx12App* s_pInstance;

    void CreateDirect3DDevice();
    void CreateCommandQueueAndList();
    void CreateSwapChain(HINSTANCE hInstance, HWND hMainWnd);
    void CreateRtvAndDsvDescriptorHeaps();
    void CreateRenderTargetViews();
    void CreateDepthStencilView();
    void CreateShadowMap();
    
    void WaitForGpuComplete();
    void UpdateFrameRate();

    HINSTANCE m_hInstance;
    HWND m_hWnd;

    UINT m_nWndClientWidth;
    UINT m_nWndClientHeight;

    ComPtr<ID3D12Device> m_pd3dDevice;
    ComPtr<ID3D12CommandQueue> m_pd3dCommandQueue;
    ComPtr<IDXGIFactory4> m_pdxgiFactory;
    ComPtr<IDXGISwapChain3> m_pdxgiSwapChain;
    ComPtr<ID3D12Resource> m_pd3dRenderTargetBuffers[kFrameCount];
    ComPtr<ID3D12DescriptorHeap> m_pd3dRtvDescriptorHeap;
    UINT m_nRtvDescriptorIncrementSize;

    ComPtr<ID3D12Resource> m_pd3dDepthStencilBuffer;
    ComPtr<ID3D12DescriptorHeap> m_pd3dDsvDescriptorHeap;

    // Shadow Map resources
    static const UINT kShadowMapSize = 4096;
    ComPtr<ID3D12Resource> m_pd3dShadowMap;
    ComPtr<ID3D12DescriptorHeap> m_pd3dShadowDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_shadowDsvHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE m_shadowSrvGpuHandle;

    void CreateShadowMapSRV();  // Called after Scene init

    ComPtr<ID3D12CommandAllocator> m_pd3dCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_pd3dCommandList;

    ComPtr<ID3D12Fence> m_pd3dFence;
    UINT64 m_nFenceValue;
    HANDLE m_hFenceEvent;

    UINT m_nSwapChainBufferIndex;

    // 스테이지 테마별 sky/clear color (기본 짙은 회색)
    float m_fClearColor[4] = { 0.10f, 0.10f, 0.10f, 1.0f };

    CGameTimer m_GameTimer;
    bool m_bIsFullscreen;

    std::unique_ptr<Scene> m_pScene;
    InputSystem m_inputSystem; // Added InputSystem member

    // 오디오 (DirectXTK12 / XAudio2) — BGM 재생
    std::unique_ptr<AudioManager> m_pAudio;
    void UpdateBGMForState();  // m_eAppState 에 맞는 BGM 선택 (매 프레임, 중복 호출 안전)

    // DirectXTK12 텍스트 렌더링
    std::unique_ptr<DirectX::GraphicsMemory> m_graphicsMemory;
    std::unique_ptr<DirectX::DescriptorHeap> m_fontDescriptorHeap;
    std::unique_ptr<DirectX::SpriteBatch> m_spriteBatch;
    std::unique_ptr<DirectX::SpriteFont> m_spriteFont;

    // Health Bar UI
    std::unique_ptr<HealthBarUI> m_pHealthBarUI;

    // 스킬 아이콘 HUD (아이콘 렌더러 + 레이아웃)
    std::unique_ptr<class SkillIconRenderer> m_pSkillIconRenderer;
    std::unique_ptr<class SkillHudUI>         m_pSkillHud;
    std::unique_ptr<class RuneRewardUI>       m_pRuneRewardUI;  // 룬 획득 모달

    // VFX 스프라이트 텍스처 (힙 슬롯 4: magic_03, 5: skull, 6: star_08, 7: twirl_01, 8: flame_04, 9: flare_01)
    ComPtr<ID3D12Resource> m_pMagicDecalTex;
    ComPtr<ID3D12Resource> m_pMagicDecalUpload;
    ComPtr<ID3D12Resource> m_pSkullTex;
    ComPtr<ID3D12Resource> m_pSkullUpload;
    ComPtr<ID3D12Resource> m_pStarTex;
    ComPtr<ID3D12Resource> m_pStarUpload;
    ComPtr<ID3D12Resource> m_pTwirlTex;
    ComPtr<ID3D12Resource> m_pTwirlUpload;
    ComPtr<ID3D12Resource> m_pFlameTex;
    ComPtr<ID3D12Resource> m_pFlameUpload;
    ComPtr<ID3D12Resource> m_pFlareTex;
    ComPtr<ID3D12Resource> m_pFlareUpload;
    // 프로시저럴 생성 룬 VFX (힙 슬롯 30: clock 시간역행, 31: fang 흡혈)
    ComPtr<ID3D12Resource> m_pClockTex;
    ComPtr<ID3D12Resource> m_pClockUpload;
    ComPtr<ID3D12Resource> m_pFangTex;
    ComPtr<ID3D12Resource> m_pFangUpload;

    // UI 텍스처 (Title / Loading / Pause / GameOver / Ending / HUD / Avatars)
    static constexpr UINT kUIHeapBase = 10;
    ComPtr<ID3D12Resource>      m_pUITex[(UINT)UISlot::Count]{};
    ComPtr<ID3D12Resource>      m_pUIUpload[(UINT)UISlot::Count]{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_hUI[(UINT)UISlot::Count]{};

    // Bloom post-process (owns HDR scene RT + blur chain + tonemap composite)
    std::unique_ptr<BloomPostProcess> m_pBloom;

    // 검기 임팩트용 풀스크린 화이트 플래시 (Bloom 직후 알파-블렌딩 패스)
    std::unique_ptr<class WhiteFlashOverlay> m_pWhiteFlash;

    // 화면 베기 후 두 조각 분리 슬라이드 (DarkLord Sever 페이즈)
    std::unique_ptr<class ScreenSplitOverlay> m_pScreenSplit;

    // Network Manager
    NetworkManager* m_pNetworkManager = nullptr;
    XMFLOAT3 m_lastSentPosition = { 0.0f, 0.0f, 0.0f };
    float m_fNetworkSendInterval = 0.05f;  // 50ms (20 packets/sec)
    float m_fNetworkSendTimer = 0.0f;

    void InitializeText();
    void InitializeNetwork();
    void RenderText();
    void UpdateNetwork(float deltaTime);

    // 앱 상태 (캐릭터 선택 → 게임 플레이)
    AppState m_eAppState = AppState::Title;
    std::unique_ptr<CharacterSelectScreen> m_pCharSelect;
    std::unique_ptr<TitleScreen>           m_pTitleScreen;
    std::unique_ptr<LoadingScreen>         m_pLoadingScreen;
    std::unique_ptr<GameOverScreen>        m_pGameOverScreen;
    std::unique_ptr<EndingScreen>          m_pEndingScreen;

    ElementType m_ePendingElement = ElementType::None;  // Loading 대기 중 선택 원소
    float       m_fLoadingMin     = 0.7f;                // 최소 로딩 노출 시간(초)

    void InitSceneWithElement(ElementType e);  // 선택 확정 후 씬 초기화
    void RenderTitleScreen();                  // FrameAdvance 의 Title 분기에서 호출

    // Pause menu
    bool m_bShowPauseMenu = false;
    void RenderPauseMenu();

    // Debug rune inspector
    enum class DebugRuneUIState { None, SelectingRune, SelectingSkill };
    DebugRuneUIState         m_debugRuneState        = DebugRuneUIState::None;
    int                      m_debugRuneScrollOffset  = 0;
    std::string              m_debugSelectedRuneId;
    std::vector<std::string> m_debugRuneSortedIds;

    void BuildDebugRuneList();
    void RenderDebugRuneUI();
};