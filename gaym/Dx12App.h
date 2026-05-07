#include "stdafx.h"
#include "Timer.h"
#include "Scene.h"
#include "InputSystem.h" // Added InputSystem include
#include "HealthBarUI.h"
#include "NetworkManager.h" // Added NetworkManager include
#include "BloomPostProcess.h"
#include "CharacterSelectScreen.h"
#include <memory>
#include <string>
#include <vector>

// DirectXTK12 for text rendering
#include <SpriteFont.h>
#include <SpriteBatch.h>
#include <ResourceUploadBatch.h>
#include <GraphicsMemory.h>
#include <DescriptorHeap.h>

enum class AppState { CharacterSelect, Playing };

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

    InputSystem& GetInputSystem() { return m_inputSystem; } // Added getter for InputSystem
    ID3D12Device* GetDevice() const { return m_pd3dDevice.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return m_pd3dCommandList.Get(); }
    Scene* GetScene() const { return m_pScene.get(); }

    // 런타임 윈도우 크기 (NDC 변환용)
    UINT GetWindowWidth() const { return m_nWndClientWidth; }
    UINT GetWindowHeight() const { return m_nWndClientHeight; }

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

    CGameTimer m_GameTimer;
    bool m_bIsFullscreen;

    std::unique_ptr<Scene> m_pScene;
    InputSystem m_inputSystem; // Added InputSystem member

    // DirectXTK12 텍스트 렌더링
    std::unique_ptr<DirectX::GraphicsMemory> m_graphicsMemory;
    std::unique_ptr<DirectX::DescriptorHeap> m_fontDescriptorHeap;
    std::unique_ptr<DirectX::SpriteBatch> m_spriteBatch;
    std::unique_ptr<DirectX::SpriteFont> m_spriteFont;

    // Health Bar UI
    std::unique_ptr<HealthBarUI> m_pHealthBarUI;

    // Bloom post-process (owns HDR scene RT + blur chain + tonemap composite)
    std::unique_ptr<BloomPostProcess> m_pBloom;

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
    AppState m_eAppState = AppState::CharacterSelect;
    std::unique_ptr<CharacterSelectScreen> m_pCharSelect;
    void InitSceneWithElement(ElementType e);  // 선택 확정 후 씬 초기화

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