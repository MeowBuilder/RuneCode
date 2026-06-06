#include "stdafx.h"
#include "Dx12App.h"
#include "DamageNumberManager.h"
#include "Camera.h"
#include "d3dx12.h"
#include "SkillComponent.h"
#include "ISkillBehavior.h"
#include "SkillData.h"
#include "DropItemComponent.h"
#include "RuneRegistry.h"
#include "SkillIconRenderer.h"
#include "SkillHudUI.h"
#include "RuneRewardUI.h"
#include "PlayerComponent.h"
#include "TransformComponent.h"
#include "Room.h"
#include "EnemyComponent.h"
#include <DescriptorHeap.h>  // DirectXTK12
#include <sstream>
#include <iomanip>
#include <algorithm>

// UTF-8(std::string) → UTF-16(std::wstring) 변환.
// 기존 std::wstring(s.begin(), s.end())은 UTF-8 바이트를 그대로 wchar_t에 복사해서
// 한글/이모지 등 멀티바이트 문자는 깨진 글리프가 되어 SpriteFont::MeasureString이 throw.
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return std::wstring();
    std::wstring w(wlen, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], wlen);
    return w;
}

// ─── 룬 UI 헬퍼 ──────────────────────────────────────────────────────────────
static const wchar_t* GetRuneGradeLabel(RuneGrade grade)
{
    switch (grade) {
    case RuneGrade::Normal:    return L"[노멀]";
    case RuneGrade::Rare:      return L"[레어]";
    case RuneGrade::Epic:      return L"[에픽]";
    case RuneGrade::Unique:    return L"[유니크]";
    case RuneGrade::Legendary: return L"[레전더리]";
    default:                   return L"";
    }
}

static XMVECTORF32 GetRuneGradeUIColor(RuneGrade grade)
{
    switch (grade) {
    case RuneGrade::Normal:    return DirectX::Colors::White;
    case RuneGrade::Rare:      return DirectX::Colors::CornflowerBlue;
    case RuneGrade::Epic:      return DirectX::Colors::MediumPurple;
    case RuneGrade::Unique:    return DirectX::Colors::Red;
    case RuneGrade::Legendary: return DirectX::Colors::Gold;
    default:                   return DirectX::Colors::White;
    }
}

static const wchar_t* GetElementName(ElementType e)
{
    switch (e) {
    case ElementType::Fire:    return L"화염";
    case ElementType::Water:   return L"물결";
    case ElementType::Wind:    return L"바람";
    case ElementType::Earth:   return L"대지";
    default:                   return L"";
    }
}

static std::wstring BuildRuneDesc(const RuneDef& def)
{
    std::wstringstream ss;
    auto pct = [](float m) { return (int)((m - 1.f) * 100.f + 0.5f); };

    if (!def.category.empty())
        ss << L"[" << Utf8ToWide(def.category) << L"] ";

    // 설명 텍스트 (description 필드)
    if (!def.description.empty())
        ss << Utf8ToWide(def.description) << L"  ";

    // 수치 스탯
    if (def.damageMult    != 1.f) ss << L"데미지 " << (pct(def.damageMult) >= 0 ? L"+" : L"") << pct(def.damageMult) << L"%  ";
    if (def.radiusMult    != 1.f) ss << L"범위 "   << (pct(def.radiusMult) >= 0 ? L"+" : L"") << pct(def.radiusMult) << L"%  ";
    if (def.cooldownMult  != 1.f) ss << L"쿨다운 " << (pct(def.cooldownMult) >= 0 ? L"+" : L"") << pct(def.cooldownMult) << L"%  ";
    if (def.castTimeMult  != 1.f) ss << L"시전 "   << (pct(def.castTimeMult) >= 0 ? L"+" : L"") << pct(def.castTimeMult) << L"%  ";
    if (def.durationMult  != 1.f) ss << L"지속 "   << (pct(def.durationMult) >= 0 ? L"+" : L"") << pct(def.durationMult) << L"%  ";
    if (def.knockbackMult != 1.f) ss << L"넉백 "   << (pct(def.knockbackMult) >= 0 ? L"+" : L"") << pct(def.knockbackMult) << L"%  ";
    if (def.statusDurationMult != 1.f) ss << L"상태지속 " << (pct(def.statusDurationMult) >= 0 ? L"+" : L"") << pct(def.statusDurationMult) << L"%  ";
    if (def.statusChanceMult   != 1.f) ss << L"상태확률 " << (pct(def.statusChanceMult) >= 0 ? L"+" : L"") << pct(def.statusChanceMult) << L"%  ";
    if (def.extraProjectiles > 0) ss << L"투사체 +" << def.extraProjectiles << L"  ";
    if (def.orbitalCount     > 0) ss << L"궤도탄 " << def.orbitalCount << L"  ";
    if (def.spawnOnHitCount  > 0) ss << L"반향 +"  << def.spawnOnHitCount << L"  ";
    if (def.lifestealRatio   > 0.f) ss << L"흡수 " << (int)(def.lifestealRatio * 100.f + 0.5f) << L"%  ";
    if (def.execDamageBonus  > 0.f) ss << L"처형 +" << (int)(def.execDamageBonus * 100.f + 0.5f) << L"%  ";
    if (def.cdResetChance    > 0.f) ss << L"무한 " << (int)(def.cdResetChance * 100.f + 0.5f) << L"%  ";
    if (def.piercing)    ss << L"관통  ";
    if (def.homing)      ss << L"유도  ";
    if (def.doublecast)  ss << L"쌍발  ";
    if (def.echoOnCast)  ss << L"잔상  ";
    if (def.randomElementOnCast) ss << L"원소무작위  ";
    if (def.activationOverride.has_value()) {
        switch (def.activationOverride.value()) {
        case ActivationType::Charge:  ss << L"차지형  ";  break;
        case ActivationType::Channel: ss << L"채널형  ";  break;
        case ActivationType::Place:   ss << L"설치형  ";  break;
        case ActivationType::Enhance: ss << L"버프형  ";  break;
        case ActivationType::Split:   ss << L"분열형  ";  break;
        default: break;
        }
    }
    std::wstring r = ss.str();
    if (r.empty()) r = L"효과 없음";
    return r;
}

// ─── Debug Rune Inspector constants ─────────────────────────────────────────
static constexpr int   kDebugVisibleRows = 12;
static constexpr float kDebugRowHeight   = 42.0f;
static constexpr float kDebugPanelLeft   = 30.0f;
static constexpr float kDebugPanelWidth  = 900.0f;
static constexpr float kDebugRowsStartY  = 158.0f;

Dx12App* Dx12App::s_pInstance = nullptr;

Dx12App::Dx12App()
{
    s_pInstance = this;
    m_nWndClientWidth = kWindowWidth;
    m_nWndClientHeight = kWindowHeight;
    m_nSwapChainBufferIndex = 0;
    m_nFenceValue = 0;
    m_bIsFullscreen = false;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

Dx12App::~Dx12App()
{
    CoUninitialize();
}

void Dx12App::OnCreate(HINSTANCE hInstance, HWND hMainWnd)
{
    m_hInstance = hInstance;
    m_hWnd = hMainWnd;

    CreateDirect3DDevice();
    CreateCommandQueueAndList();
    CreateSwapChain(hInstance, hMainWnd);
    CreateRtvAndDsvDescriptorHeaps();
    CreateRenderTargetViews();
    CreateDepthStencilView();
    CreateShadowMap();

    // HDR scene RT + bloom post-process (scene is rendered offscreen here, then
    // tonemap+composited to the LDR swap-chain back buffer).
    m_pBloom = std::make_unique<BloomPostProcess>();
    m_pBloom->Init(m_pd3dDevice.Get(), m_nWndClientWidth, m_nWndClientHeight);

    // 텍스트 렌더링 먼저 초기화 (캐릭터 선택 화면에서 사용)
    InitializeText();

    // 캐릭터 선택 화면 초기화
    //   폰트 디스크립터 힙 슬롯 3 = 흰 픽셀 텍스처 (배경 사각형용)
    //   슬롯 0=폰트, 1=HP바 base, 2=HP바 fill, 3=흰 픽셀
    {
        CHECK_HR(m_pd3dCommandAllocator->Reset());
        CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));

        m_pCharSelect = std::make_unique<CharacterSelectScreen>();
        m_pCharSelect->Initialize(m_pd3dDevice.Get(), m_pd3dCommandList.Get(),
                                   m_fontDescriptorHeap.get(), 3,
                                   m_pd3dCommandQueue.Get());

        CHECK_HR(m_pd3dCommandList->Close());
        ID3D12CommandList* lists[] = { m_pd3dCommandList.Get() };
        m_pd3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);
        WaitForGpuComplete();
    }

    // Title / Loading / GameOver / Ending 화면 초기화 (UI 텍스처 핸들/크기 주입)
    {
        auto texSize = [&](UISlot s) -> DirectX::XMUINT2 {
            auto d = m_pUITex[(UINT)s]->GetDesc();
            return { (UINT)d.Width, (UINT)d.Height };
        };
        m_pTitleScreen = std::make_unique<TitleScreen>();
        m_pTitleScreen->Initialize(
            m_hUI[(UINT)UISlot::TitleBg],   texSize(UISlot::TitleBg),
            m_hUI[(UINT)UISlot::TitleLogo], texSize(UISlot::TitleLogo),
            m_hUI[(UINT)UISlot::BtnNormal], texSize(UISlot::BtnNormal),
            m_hUI[(UINT)UISlot::BtnHover],  texSize(UISlot::BtnHover));

        m_pLoadingScreen = std::make_unique<LoadingScreen>();
        m_pLoadingScreen->Initialize(
            m_hUI[(UINT)UISlot::LoadingBg],      texSize(UISlot::LoadingBg),
            m_hUI[(UINT)UISlot::LoadingSpinner], texSize(UISlot::LoadingSpinner));

        m_pGameOverScreen = std::make_unique<GameOverScreen>();
        m_pGameOverScreen->Initialize(
            m_hUI[(UINT)UISlot::GameOverBg],    texSize(UISlot::GameOverBg),
            m_hUI[(UINT)UISlot::GameOverTitle], texSize(UISlot::GameOverTitle),
            m_hUI[(UINT)UISlot::BtnNormal],     texSize(UISlot::BtnNormal),
            m_hUI[(UINT)UISlot::BtnHover],      texSize(UISlot::BtnHover));

        m_pEndingScreen = std::make_unique<EndingScreen>();
        m_pEndingScreen->Initialize(
            m_hUI[(UINT)UISlot::EndingBg],    texSize(UISlot::EndingBg),
            m_hUI[(UINT)UISlot::EndingTitle], texSize(UISlot::EndingTitle),
            m_hUI[(UINT)UISlot::BtnNormal],   texSize(UISlot::BtnNormal),
            m_hUI[(UINT)UISlot::BtnHover],    texSize(UISlot::BtnHover));
    }

    m_eAppState = AppState::Title;

    // 네트워크 초기화
    InitializeNetwork();

    m_GameTimer.Reset();
}

void Dx12App::OnDestroy()
{
    // 네트워크 정리
    if (m_pNetworkManager)
    {
        m_pNetworkManager->Shutdown();
        delete m_pNetworkManager;
        m_pNetworkManager = nullptr;
    }

    WaitForGpuComplete();
    if (m_pdxgiSwapChain)
    {
        m_pdxgiSwapChain->SetFullscreenState(FALSE, NULL);
    }
    CloseHandle(m_hFenceEvent);
}

void Dx12App::CreateDirect3DDevice()
{
    UINT nDXGIFactoryFlags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> pd3dDebugController;
    D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void**)&pd3dDebugController);
    pd3dDebugController->EnableDebugLayer();
    nDXGIFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    CHECK_HR(CreateDXGIFactory2(nDXGIFactoryFlags, __uuidof(IDXGIFactory4), (void**)&m_pdxgiFactory));
    CHECK_HR(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&m_pd3dDevice));
}

void Dx12App::CreateCommandQueueAndList()
{
    D3D12_COMMAND_QUEUE_DESC d3dCommandQueueDesc;
    ::ZeroMemory(&d3dCommandQueueDesc, sizeof(D3D12_COMMAND_QUEUE_DESC));
    d3dCommandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    d3dCommandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK_HR(m_pd3dDevice->CreateCommandQueue(&d3dCommandQueueDesc, __uuidof(ID3D12CommandQueue), (void**)&m_pd3dCommandQueue));

    CHECK_HR(m_pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&m_pd3dCommandAllocator));
    CHECK_HR(m_pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pd3dCommandAllocator.Get(), NULL, __uuidof(ID3D12GraphicsCommandList), (void**)&m_pd3dCommandList));
    CHECK_HR(m_pd3dCommandList->Close());
}

void Dx12App::CreateSwapChain(HINSTANCE hInstance, HWND hMainWnd)
{
    DXGI_SWAP_CHAIN_DESC1 dxgiSwapChainDesc;
    ::ZeroMemory(&dxgiSwapChainDesc, sizeof(dxgiSwapChainDesc));
    dxgiSwapChainDesc.Width = m_nWndClientWidth;
    dxgiSwapChainDesc.Height = m_nWndClientHeight;
    dxgiSwapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dxgiSwapChainDesc.SampleDesc.Count = 1;
    dxgiSwapChainDesc.SampleDesc.Quality = 0;
    dxgiSwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    dxgiSwapChainDesc.BufferCount = kFrameCount;
    dxgiSwapChainDesc.Scaling = DXGI_SCALING_NONE;
    dxgiSwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    dxgiSwapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    dxgiSwapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    ComPtr<IDXGISwapChain1> pdxgiSwapChain1;
    CHECK_HR(m_pdxgiFactory->CreateSwapChainForHwnd(m_pd3dCommandQueue.Get(), hMainWnd, &dxgiSwapChainDesc, NULL, NULL, &pdxgiSwapChain1));
    CHECK_HR(pdxgiSwapChain1.As(&m_pdxgiSwapChain));
    m_nSwapChainBufferIndex = m_pdxgiSwapChain->GetCurrentBackBufferIndex();

    CHECK_HR(m_pdxgiFactory->MakeWindowAssociation(hMainWnd, DXGI_MWA_NO_ALT_ENTER));

    CHECK_HR(m_pd3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&m_pd3dFence));
    m_hFenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
}

void Dx12App::CreateRtvAndDsvDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC d3dDescriptorHeapDesc;
    ::ZeroMemory(&d3dDescriptorHeapDesc, sizeof(D3D12_DESCRIPTOR_HEAP_DESC));
    d3dDescriptorHeapDesc.NumDescriptors = kFrameCount;
    d3dDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    d3dDescriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    d3dDescriptorHeapDesc.NodeMask = 0;
    CHECK_HR(m_pd3dDevice->CreateDescriptorHeap(&d3dDescriptorHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)&m_pd3dRtvDescriptorHeap));
    m_nRtvDescriptorIncrementSize = m_pd3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    d3dDescriptorHeapDesc.NumDescriptors = 1;
    d3dDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    CHECK_HR(m_pd3dDevice->CreateDescriptorHeap(&d3dDescriptorHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)&m_pd3dDsvDescriptorHeap));
}

void Dx12App::CreateRenderTargetViews()
{
    D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle = m_pd3dRtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; i++)
    {
        CHECK_HR(m_pdxgiSwapChain->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&m_pd3dRenderTargetBuffers[i]));
        m_pd3dDevice->CreateRenderTargetView(m_pd3dRenderTargetBuffers[i].Get(), NULL, d3dRtvCPUDescriptorHandle);
        d3dRtvCPUDescriptorHandle.ptr += m_nRtvDescriptorIncrementSize;
    }
}

void Dx12App::CreateDepthStencilView()
{
    D3D12_RESOURCE_DESC d3dResourceDesc;
    d3dResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d3dResourceDesc.Alignment = 0;
    d3dResourceDesc.Width = m_nWndClientWidth;
    d3dResourceDesc.Height = m_nWndClientHeight;
    d3dResourceDesc.DepthOrArraySize = 1;
    d3dResourceDesc.MipLevels = 1;
    d3dResourceDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d3dResourceDesc.SampleDesc.Count = 1;
    d3dResourceDesc.SampleDesc.Quality = 0;
    d3dResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d3dResourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES d3dHeapProperties;
    ::ZeroMemory(&d3dHeapProperties, sizeof(D3D12_HEAP_PROPERTIES));
    d3dHeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    d3dHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    d3dHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    d3dHeapProperties.CreationNodeMask = 1;
    d3dHeapProperties.VisibleNodeMask = 1;

    D3D12_CLEAR_VALUE d3dClearValue;
    d3dClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d3dClearValue.DepthStencil.Depth = 1.0f;
    d3dClearValue.DepthStencil.Stencil = 0;

    CHECK_HR(m_pd3dDevice->CreateCommittedResource(&d3dHeapProperties, D3D12_HEAP_FLAG_NONE, &d3dResourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &d3dClearValue, __uuidof(ID3D12Resource), (void**)&m_pd3dDepthStencilBuffer));

    D3D12_DEPTH_STENCIL_VIEW_DESC d3dDepthStencilViewDesc;
    ::ZeroMemory(&d3dDepthStencilViewDesc, sizeof(D3D12_DEPTH_STENCIL_VIEW_DESC));
    d3dDepthStencilViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d3dDepthStencilViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    d3dDepthStencilViewDesc.Flags = D3D12_DSV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE d3dDsvCPUDescriptorHandle = m_pd3dDsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    m_pd3dDevice->CreateDepthStencilView(m_pd3dDepthStencilBuffer.Get(), &d3dDepthStencilViewDesc, d3dDsvCPUDescriptorHandle);
}

void Dx12App::CreateShadowMap()
{
    // Create Shadow Map texture resource
    D3D12_RESOURCE_DESC shadowMapDesc;
    shadowMapDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadowMapDesc.Alignment = 0;
    shadowMapDesc.Width = kShadowMapSize;
    shadowMapDesc.Height = kShadowMapSize;
    shadowMapDesc.DepthOrArraySize = 1;
    shadowMapDesc.MipLevels = 1;
    shadowMapDesc.Format = DXGI_FORMAT_R32_TYPELESS;  // Typeless for DSV/SRV compatibility
    shadowMapDesc.SampleDesc.Count = 1;
    shadowMapDesc.SampleDesc.Quality = 0;
    shadowMapDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    shadowMapDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES heapProperties;
    ::ZeroMemory(&heapProperties, sizeof(D3D12_HEAP_PROPERTIES));
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_CLEAR_VALUE clearValue;
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    CHECK_HR(m_pd3dDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &shadowMapDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        __uuidof(ID3D12Resource),
        (void**)&m_pd3dShadowMap));

    // Create Shadow DSV Heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
    ::ZeroMemory(&dsvHeapDesc, sizeof(D3D12_DESCRIPTOR_HEAP_DESC));
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK_HR(m_pd3dDevice->CreateDescriptorHeap(&dsvHeapDesc, __uuidof(ID3D12DescriptorHeap), (void**)&m_pd3dShadowDsvHeap));

    // Create Shadow DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
    ::ZeroMemory(&dsvDesc, sizeof(D3D12_DEPTH_STENCIL_VIEW_DESC));
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

    m_shadowDsvHandle = m_pd3dShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_pd3dDevice->CreateDepthStencilView(m_pd3dShadowMap.Get(), &dsvDesc, m_shadowDsvHandle);
}

void Dx12App::CreateShadowMapSRV()
{
    // Allocate descriptor from Scene's heap
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
    m_pScene->AllocateDescriptor(&srvCpuHandle, &m_shadowSrvGpuHandle);

    // Create Shadow SRV in Scene's descriptor heap
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ::ZeroMemory(&srvDesc, sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC));
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    srvDesc.Texture2D.PlaneSlice = 0;

    m_pd3dDevice->CreateShaderResourceView(m_pd3dShadowMap.Get(), &srvDesc, srvCpuHandle);
}



void Dx12App::WaitForGpuComplete()
{
    const UINT64 nFence = ++m_nFenceValue;
    CHECK_HR(m_pd3dCommandQueue->Signal(m_pd3dFence.Get(), nFence));

    if (m_pd3dFence->GetCompletedValue() < nFence)
    {
        CHECK_HR(m_pd3dFence->SetEventOnCompletion(nFence, m_hFenceEvent));
        WaitForSingleObject(m_hFenceEvent, INFINITE);
    }
}

void Dx12App::ToggleFullscreen()
{
    WaitForGpuComplete();

    m_bIsFullscreen = !m_bIsFullscreen;

    CHECK_HR(m_pdxgiSwapChain->SetFullscreenState(m_bIsFullscreen, NULL));

    // Release the old buffers
    for (int i = 0; i < kFrameCount; ++i)
        m_pd3dRenderTargetBuffers[i].Reset();
    m_pd3dDepthStencilBuffer.Reset();

    DXGI_SWAP_CHAIN_DESC1 dxgiSwapChainDesc;
    CHECK_HR(m_pdxgiSwapChain->GetDesc1(&dxgiSwapChainDesc));

    CHECK_HR(m_pdxgiSwapChain->ResizeBuffers(kFrameCount, 0, 0, dxgiSwapChainDesc.Format, dxgiSwapChainDesc.Flags));

    m_nSwapChainBufferIndex = m_pdxgiSwapChain->GetCurrentBackBufferIndex();

    CreateRenderTargetViews();
    CreateDepthStencilView();

    DXGI_SWAP_CHAIN_DESC1 newDesc;
    CHECK_HR(m_pdxgiSwapChain->GetDesc1(&newDesc));
    m_nWndClientWidth = newDesc.Width;
    m_nWndClientHeight = newDesc.Height;

    // Screen-Space Fluid 텍스처 리사이즈
    if (m_pScene)
        m_pScene->OnResizeSSF(m_nWndClientWidth, m_nWndClientHeight);
}

void Dx12App::UpdateFrameRate()
{
    WCHAR text[256];
    m_GameTimer.GetFrameRate(text, 256);
    ::SetWindowText(m_hWnd, text);
}

void Dx12App::FrameAdvance()
{
    m_GameTimer.Tick();
    float deltaTime = m_GameTimer.GetTimeElapsed();

    // ── 타이틀 화면 ─────────────────────────────────────────────────────────
    if (m_eAppState == AppState::Title)
    {
        if (m_pTitleScreen)
            m_pTitleScreen->Update(m_inputSystem, (float)m_nWndClientWidth, (float)m_nWndClientHeight, deltaTime);

        WaitForGpuComplete();
        CHECK_HR(m_pd3dCommandAllocator->Reset());
        CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));

        if (m_pTitleScreen)
        {
            TitleScreen::Result r = m_pTitleScreen->GetResult();
            if (r == TitleScreen::Result::StartGame)
            {
                m_pTitleScreen->Reset();
                if (m_pCharSelect) m_pCharSelect->Reset();
                m_eAppState = AppState::CharacterSelect;
            }
            else if (r == TitleScreen::Result::Quit)
            {
                ::PostQuitMessage(0);
            }
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = m_pd3dRenderTargetBuffers[m_nSwapChainBufferIndex].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pd3dCommandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_pd3dRtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += m_nSwapChainBufferIndex * m_nRtvDescriptorIncrementSize;
        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        m_pd3dCommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        m_pd3dCommandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)m_nWndClientWidth, (float)m_nWndClientHeight, 0, 1 };
        m_pd3dCommandList->RSSetViewports(1, &vp);
        D3D12_RECT sc = { 0, 0, (LONG)m_nWndClientWidth, (LONG)m_nWndClientHeight };
        m_pd3dCommandList->RSSetScissorRects(1, &sc);

        if (m_pTitleScreen && m_spriteBatch && m_spriteFont)
        {
            ID3D12DescriptorHeap* heaps[] = { m_fontDescriptorHeap->Heap() };
            m_pd3dCommandList->SetDescriptorHeaps(1, heaps);
            m_spriteBatch->Begin(m_pd3dCommandList.Get());
            m_pTitleScreen->Render(m_spriteBatch.get(), m_spriteFont.get(),
                (float)m_nWndClientWidth, (float)m_nWndClientHeight);
            m_spriteBatch->End();
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        m_pd3dCommandList->ResourceBarrier(1, &barrier);

        CHECK_HR(m_pd3dCommandList->Close());
        ID3D12CommandList* lists[] = { m_pd3dCommandList.Get() };
        m_pd3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);
        m_pdxgiSwapChain->Present(1, 0);
        m_nSwapChainBufferIndex = m_pdxgiSwapChain->GetCurrentBackBufferIndex();

        if (m_graphicsMemory) m_graphicsMemory->Commit(m_pd3dCommandQueue.Get());
        UpdateFrameRate();
        return;
    }

    // ── 캐릭터 선택 화면 ────────────────────────────────────────────────────
    if (m_eAppState == AppState::CharacterSelect)
    {
        if (m_pCharSelect)
            m_pCharSelect->Update(m_inputSystem, (float)m_nWndClientWidth, (float)m_nWndClientHeight, deltaTime);

        WaitForGpuComplete();
        CHECK_HR(m_pd3dCommandAllocator->Reset());
        CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));

        // 선택 확정 → 즉시 init 하지 않고 Loading 화면 거치도록 전환
        if (m_pCharSelect && m_pCharSelect->IsConfirmed())
        {
            m_ePendingElement = m_pCharSelect->GetSelectedElement();
            if (m_pLoadingScreen) m_pLoadingScreen->Reset();
            m_eAppState = AppState::Loading;

            CHECK_HR(m_pd3dCommandList->Close());  // 빈 리스트 닫고 제출
            ID3D12CommandList* lists[] = { m_pd3dCommandList.Get() };
            m_pd3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);
            WaitForGpuComplete();
            return;
        }

        // 배경을 단색으로 지우고 캐릭터 선택 UI만 렌더
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = m_pd3dRenderTargetBuffers[m_nSwapChainBufferIndex].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pd3dCommandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_pd3dRtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += m_nSwapChainBufferIndex * m_nRtvDescriptorIncrementSize;
        float clearColor[4] = { 0.05f, 0.05f, 0.08f, 1.0f };
        m_pd3dCommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        m_pd3dCommandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)m_nWndClientWidth, (float)m_nWndClientHeight, 0, 1 };
        m_pd3dCommandList->RSSetViewports(1, &vp);
        D3D12_RECT sc = { 0, 0, (LONG)m_nWndClientWidth, (LONG)m_nWndClientHeight };
        m_pd3dCommandList->RSSetScissorRects(1, &sc);

        // SpriteBatch로 선택 UI 렌더
        if (m_pCharSelect && m_spriteBatch && m_spriteFont)
        {
            ID3D12DescriptorHeap* heaps[] = { m_fontDescriptorHeap->Heap() };
            m_pd3dCommandList->SetDescriptorHeaps(1, heaps);
            m_spriteBatch->Begin(m_pd3dCommandList.Get());
            m_pCharSelect->Render(m_spriteBatch.get(), m_spriteFont.get(),
                (float)m_nWndClientWidth, (float)m_nWndClientHeight);
            m_spriteBatch->End();
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        m_pd3dCommandList->ResourceBarrier(1, &barrier);

        CHECK_HR(m_pd3dCommandList->Close());
        ID3D12CommandList* lists[] = { m_pd3dCommandList.Get() };
        m_pd3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);
        m_pdxgiSwapChain->Present(1, 0);
        m_nSwapChainBufferIndex = m_pdxgiSwapChain->GetCurrentBackBufferIndex();

        if (m_graphicsMemory) m_graphicsMemory->Commit(m_pd3dCommandQueue.Get());
        UpdateFrameRate();
        return;
    }

    // ── 로딩 화면 ───────────────────────────────────────────────────────────
    if (m_eAppState == AppState::Loading)
    {
        if (m_pLoadingScreen)
            m_pLoadingScreen->Update(deltaTime);

        // 최소 노출 시간 경과 → InitScene → Playing (InitScene 내부에서 Reset/Close)
        if (m_pLoadingScreen && m_pLoadingScreen->GetElapsed() >= m_fLoadingMin
            && m_ePendingElement != ElementType::None)
        {
            ElementType e = m_ePendingElement;
            m_ePendingElement = ElementType::None;
            InitSceneWithElement(e);   // 끝에서 m_eAppState = AppState::Playing
            return;
        }

        WaitForGpuComplete();
        CHECK_HR(m_pd3dCommandAllocator->Reset());
        CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = m_pd3dRenderTargetBuffers[m_nSwapChainBufferIndex].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pd3dCommandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_pd3dRtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += m_nSwapChainBufferIndex * m_nRtvDescriptorIncrementSize;
        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        m_pd3dCommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        m_pd3dCommandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)m_nWndClientWidth, (float)m_nWndClientHeight, 0, 1 };
        m_pd3dCommandList->RSSetViewports(1, &vp);
        D3D12_RECT sc = { 0, 0, (LONG)m_nWndClientWidth, (LONG)m_nWndClientHeight };
        m_pd3dCommandList->RSSetScissorRects(1, &sc);

        if (m_pLoadingScreen && m_spriteBatch && m_spriteFont)
        {
            ID3D12DescriptorHeap* heaps[] = { m_fontDescriptorHeap->Heap() };
            m_pd3dCommandList->SetDescriptorHeaps(1, heaps);
            m_spriteBatch->Begin(m_pd3dCommandList.Get());
            m_pLoadingScreen->Render(m_spriteBatch.get(), m_spriteFont.get(),
                (float)m_nWndClientWidth, (float)m_nWndClientHeight);
            m_spriteBatch->End();
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        m_pd3dCommandList->ResourceBarrier(1, &barrier);

        CHECK_HR(m_pd3dCommandList->Close());
        ID3D12CommandList* lists[] = { m_pd3dCommandList.Get() };
        m_pd3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);
        m_pdxgiSwapChain->Present(1, 0);
        m_nSwapChainBufferIndex = m_pdxgiSwapChain->GetCurrentBackBufferIndex();

        if (m_graphicsMemory) m_graphicsMemory->Commit(m_pd3dCommandQueue.Get());
        UpdateFrameRate();
        return;
    }

    // ── 게임 오버 / 엔딩 ────────────────────────────────────────────────────
    if (m_eAppState == AppState::GameOver || m_eAppState == AppState::Ending)
    {
        if (m_eAppState == AppState::GameOver && m_pGameOverScreen)
            m_pGameOverScreen->Update(m_inputSystem, (float)m_nWndClientWidth, (float)m_nWndClientHeight, deltaTime);
        if (m_eAppState == AppState::Ending && m_pEndingScreen)
            m_pEndingScreen->Update(m_inputSystem, (float)m_nWndClientWidth, (float)m_nWndClientHeight, deltaTime);

        WaitForGpuComplete();
        CHECK_HR(m_pd3dCommandAllocator->Reset());
        CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));

        // 결과 처리 (이번 프레임에 어떤 화면을 그릴지는 renderAs 로 잠금)
        AppState renderAs = m_eAppState;
        if (m_eAppState == AppState::GameOver && m_pGameOverScreen)
        {
            auto r = m_pGameOverScreen->GetResult();
            if (r == GameOverScreen::Result::Retry)
            {
                m_pGameOverScreen->Reset();
                if (m_pCharSelect) m_ePendingElement = m_pCharSelect->GetSelectedElement();
                if (m_pLoadingScreen) m_pLoadingScreen->Reset();
                m_eAppState = AppState::Loading;
            }
            else if (r == GameOverScreen::Result::ToTitle)
            {
                m_pGameOverScreen->Reset();
                if (m_pTitleScreen) m_pTitleScreen->Reset();
                m_eAppState = AppState::Title;
            }
            else if (r == GameOverScreen::Result::Quit)
            {
                ::PostQuitMessage(0);
            }
        }
        else if (m_eAppState == AppState::Ending && m_pEndingScreen)
        {
            auto r = m_pEndingScreen->GetResult();
            if (r == EndingScreen::Result::ToTitle)
            {
                m_pEndingScreen->Reset();
                if (m_pTitleScreen) m_pTitleScreen->Reset();
                m_eAppState = AppState::Title;
            }
            else if (r == EndingScreen::Result::Quit)
            {
                ::PostQuitMessage(0);
            }
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = m_pd3dRenderTargetBuffers[m_nSwapChainBufferIndex].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pd3dCommandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_pd3dRtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += m_nSwapChainBufferIndex * m_nRtvDescriptorIncrementSize;
        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        m_pd3dCommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        m_pd3dCommandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)m_nWndClientWidth, (float)m_nWndClientHeight, 0, 1 };
        m_pd3dCommandList->RSSetViewports(1, &vp);
        D3D12_RECT sc = { 0, 0, (LONG)m_nWndClientWidth, (LONG)m_nWndClientHeight };
        m_pd3dCommandList->RSSetScissorRects(1, &sc);

        if (m_spriteBatch && m_spriteFont)
        {
            ID3D12DescriptorHeap* heaps[] = { m_fontDescriptorHeap->Heap() };
            m_pd3dCommandList->SetDescriptorHeaps(1, heaps);
            m_spriteBatch->Begin(m_pd3dCommandList.Get());
            if (renderAs == AppState::GameOver && m_pGameOverScreen)
                m_pGameOverScreen->Render(m_spriteBatch.get(), m_spriteFont.get(),
                    (float)m_nWndClientWidth, (float)m_nWndClientHeight);
            else if (renderAs == AppState::Ending && m_pEndingScreen)
                m_pEndingScreen->Render(m_spriteBatch.get(), m_spriteFont.get(),
                    (float)m_nWndClientWidth, (float)m_nWndClientHeight);
            m_spriteBatch->End();
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        m_pd3dCommandList->ResourceBarrier(1, &barrier);

        CHECK_HR(m_pd3dCommandList->Close());
        ID3D12CommandList* lists[] = { m_pd3dCommandList.Get() };
        m_pd3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);
        m_pdxgiSwapChain->Present(1, 0);
        m_nSwapChainBufferIndex = m_pdxgiSwapChain->GetCurrentBackBufferIndex();

        if (m_graphicsMemory) m_graphicsMemory->Commit(m_pd3dCommandQueue.Get());
        UpdateFrameRate();
        return;
    }

    // ── 게임 플레이 ─────────────────────────────────────────────────────────

    // 네트워크 업데이트 (GPU 대기 전에 수행)
    UpdateNetwork(deltaTime);

    WaitForGpuComplete();

    CHECK_HR(m_pd3dCommandAllocator->Reset());
    CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));

    // 네트워크 명령 처리 (메인 스레드에서 GameObject 생성/삭제)
    if (m_pNetworkManager)
    {
        m_pNetworkManager->Update(m_pScene.get(), m_pd3dDevice.Get(), m_pd3dCommandList.Get(), deltaTime);
    }

    // 네트워크 보스 / 일반 몬스터 공격 연출 업데이트
    if (m_pNetworkManager && m_pNetworkManager->IsConnected())
    {
        m_pNetworkManager->UpdateNetworkGolemBehaviors(deltaTime);
        m_pNetworkManager->UpdateNetworkDemonBehaviors(deltaTime);
        m_pNetworkManager->UpdateNetworkNormalMonsterBehaviors(deltaTime);
    }

    // Update scene first (calculates light matrices)
    m_pScene->Update(m_GameTimer.GetTimeElapsed(), &m_inputSystem);

    // DarkLord 처치 → 엔딩 화면으로 전환 (한 번만 트리거)
    if (m_pScene->IsGameClear() && m_eAppState == AppState::Playing)
    {
        if (m_pEndingScreen) m_pEndingScreen->Reset();
        m_eAppState = AppState::Ending;
        CHECK_HR(m_pd3dCommandList->Close());
        ID3D12CommandList* lists[] = { m_pd3dCommandList.Get() };
        m_pd3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);
        WaitForGpuComplete();
        return;
    }

    // 스킬 HUD (TAB 확대 보간 + 호버 판정)
    if (m_pSkillHud && m_pScene)
    {
        SkillComponent* pHudSkill = nullptr;
        if (GameObject* pHudPlayer = m_pScene->GetPlayer())
            pHudSkill = pHudPlayer->GetComponent<SkillComponent>();
        m_pSkillHud->Update(&m_inputSystem, pHudSkill, deltaTime,
                            (float)m_nWndClientWidth, (float)m_nWndClientHeight);
    }

    // Update damage number animations
    DamageNumberManager::Get().Update(m_GameTimer.GetTimeElapsed());
    VFXSpriteManager::Get().Update(m_GameTimer.GetTimeElapsed());

    // ========================================================================
    // Shadow Pass: Render depth from light's perspective
    // ========================================================================
    {
        // Set shadow map viewport
        D3D12_VIEWPORT shadowViewport = { 0, 0, (FLOAT)kShadowMapSize, (FLOAT)kShadowMapSize, 0.0f, 1.0f };
        m_pd3dCommandList->RSSetViewports(1, &shadowViewport);
        D3D12_RECT shadowScissorRect = { 0, 0, (LONG)kShadowMapSize, (LONG)kShadowMapSize };
        m_pd3dCommandList->RSSetScissorRects(1, &shadowScissorRect);

        // Clear shadow map
        m_pd3dCommandList->ClearDepthStencilView(m_shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);

        // Set shadow map as render target (no color target)
        m_pd3dCommandList->OMSetRenderTargets(0, nullptr, FALSE, &m_shadowDsvHandle);

        // Render shadow casters
        m_pScene->RenderShadowPass(m_pd3dCommandList.Get());

        // Transition shadow map from depth write to shader resource
        D3D12_RESOURCE_BARRIER shadowBarrier;
        shadowBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        shadowBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        shadowBarrier.Transition.pResource = m_pd3dShadowMap.Get();
        shadowBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        shadowBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        shadowBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pd3dCommandList->ResourceBarrier(1, &shadowBarrier);
    }

    // ========================================================================
    // Main Pass: Render scene with shadows
    // ========================================================================
    D3D12_VIEWPORT viewport = { 0, 0, (FLOAT)m_nWndClientWidth, (FLOAT)m_nWndClientHeight, 0.0f, 1.0f };
    m_pd3dCommandList->RSSetViewports(1, &viewport);
    D3D12_RECT scissorRect = { 0, 0, (LONG)m_nWndClientWidth, (LONG)m_nWndClientHeight };
    m_pd3dCommandList->RSSetScissorRects(1, &scissorRect);

    D3D12_RESOURCE_BARRIER d3dResourceBarrier;
    d3dResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    d3dResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    d3dResourceBarrier.Transition.pResource = m_pd3dRenderTargetBuffers[m_nSwapChainBufferIndex].Get();
    d3dResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    d3dResourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    d3dResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_pd3dCommandList->ResourceBarrier(1, &d3dResourceBarrier);

    D3D12_CPU_DESCRIPTOR_HANDLE d3dRtvCPUDescriptorHandle = m_pd3dRtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    d3dRtvCPUDescriptorHandle.ptr += (m_nSwapChainBufferIndex * m_nRtvDescriptorIncrementSize);

    m_pd3dCommandList->ClearRenderTargetView(d3dRtvCPUDescriptorHandle, m_fClearColor, 0, NULL);

    D3D12_CPU_DESCRIPTOR_HANDLE d3dDsvCPUDescriptorHandle = m_pd3dDsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    m_pd3dCommandList->ClearDepthStencilView(d3dDsvCPUDescriptorHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);

    m_pd3dCommandList->OMSetRenderTargets(1, &d3dRtvCPUDescriptorHandle, FALSE, &d3dDsvCPUDescriptorHandle);

    // Handle drop interaction state
    DropInteractionState dropState = m_pScene->GetDropInteractionState();

    // Block regular rune input when in any drop interaction state
    GameObject* pPlayer = m_pScene->GetPlayer();
    if (pPlayer)
    {
        SkillComponent* pSkill = pPlayer->GetComponent<SkillComponent>();
        if (pSkill)
        {
            pSkill->SetRuneInputBlocked(
                m_bShowPauseMenu ||
                dropState == DropInteractionState::SelectingRune ||
                dropState == DropInteractionState::SelectingSkill ||
                m_debugRuneState != DebugRuneUIState::None);
        }
    }

    if (m_bShowPauseMenu)
    {
        // ESC or clicking "계속하기" closes the menu
        if (m_inputSystem.IsKeyPressed(VK_ESCAPE))
        {
            m_bShowPauseMenu = false;
        }

        if (m_inputSystem.IsMouseButtonPressed(0))
        {
            XMFLOAT2 mp = m_inputSystem.GetMousePosition();
            float cx = (float)m_nWndClientWidth  / 2.0f;
            float cy = (float)m_nWndClientHeight / 2.0f;

            float btnW = 220.0f, btnH = 40.0f;
            float resumeY     = cy - 30.0f;
            float charSelectY = cy + 30.0f;
            float quitY       = cy + 90.0f;

            auto inBtn = [&](float btnY) {
                return mp.x >= cx - btnW/2.f && mp.x <= cx + btnW/2.f &&
                       mp.y >= btnY && mp.y <= btnY + btnH;
            };

            if (inBtn(resumeY))
            {
                m_bShowPauseMenu = false;
            }
            else if (inBtn(charSelectY))
            {
                m_bShowPauseMenu = false;
                if (m_pCharSelect) m_pCharSelect->Reset();
                m_eAppState = AppState::CharacterSelect;
            }
            else if (inBtn(quitY))
            {
                ::PostQuitMessage(0);
            }
        }
    }
    else if (dropState == DropInteractionState::SelectingRune)
    {
        // 아이콘 모달 — RuneRewardUI 의 레이아웃과 동일한 히트테스트로 카드 클릭 판정
        if (m_inputSystem.IsMouseButtonPressed(0) && m_pRuneRewardUI)  // Left click
        {
            XMFLOAT2 mousePos = m_inputSystem.GetMousePosition();
            int idx = m_pRuneRewardUI->HitTestRuneOption(mousePos.x, mousePos.y,
                                                         (float)m_nWndClientWidth, (float)m_nWndClientHeight);
            if (idx >= 0)
                m_pScene->SelectRuneByClick(idx);
        }

        // Also keep keyboard support
        if (m_inputSystem.IsKeyDown('1'))
        {
            m_pScene->SelectRuneByClick(0);
        }
        else if (m_inputSystem.IsKeyDown('2'))
        {
            m_pScene->SelectRuneByClick(1);
        }
        else if (m_inputSystem.IsKeyDown('3'))
        {
            m_pScene->SelectRuneByClick(2);
        }
        else if (m_inputSystem.IsKeyDown(VK_ESCAPE))
        {
            m_pScene->CancelDropInteraction();
        }
    }
    else if (dropState == DropInteractionState::SelectingSkill)
    {
        // 아이콘 모달 — RuneRewardUI 레이아웃과 동일한 히트테스트로 룬 칸 클릭 판정
        if (m_inputSystem.IsMouseButtonPressed(0) && m_pRuneRewardUI)  // Left click
        {
            XMFLOAT2 mousePos = m_inputSystem.GetMousePosition();
            int skillIdx = -1, runeIdx = -1;
            if (m_pRuneRewardUI->HitTestSkillSlot(mousePos.x, mousePos.y,
                    (float)m_nWndClientWidth, (float)m_nWndClientHeight, skillIdx, runeIdx))
            {
                m_pScene->SelectSkillSlot(static_cast<SkillSlot>(skillIdx), runeIdx);
            }
        }

        if (m_inputSystem.IsKeyDown(VK_ESCAPE))
        {
            m_pScene->CancelDropInteraction();
        }
    }
    else if (m_debugRuneState != DebugRuneUIState::None)
    {
        // I or ESC (from top-level list) closes the inspector
        if (m_inputSystem.IsKeyPressed('I'))
        {
            m_debugRuneState = DebugRuneUIState::None;
        }
        else if (m_debugRuneState == DebugRuneUIState::SelectingRune)
        {
            // Scroll with mouse wheel
            float wheel = m_inputSystem.GetMouseWheelDelta();
            if (wheel > 0.0f)
                m_debugRuneScrollOffset = max(0, m_debugRuneScrollOffset - 1);
            else if (wheel < 0.0f)
                m_debugRuneScrollOffset = min(
                    max(0, (int)m_debugRuneSortedIds.size() - kDebugVisibleRows),
                    m_debugRuneScrollOffset + 1);

            // Scroll with arrow keys
            if (m_inputSystem.IsKeyPressed(VK_UP))
                m_debugRuneScrollOffset = max(0, m_debugRuneScrollOffset - 1);
            if (m_inputSystem.IsKeyPressed(VK_DOWN))
                m_debugRuneScrollOffset = min(
                    max(0, (int)m_debugRuneSortedIds.size() - kDebugVisibleRows),
                    m_debugRuneScrollOffset + 1);

            // ESC closes
            if (m_inputSystem.IsKeyPressed(VK_ESCAPE))
                m_debugRuneState = DebugRuneUIState::None;

            // C = clear all equipped runes
            if (m_inputSystem.IsKeyPressed('C'))
            {
                GameObject* pDbgPlayer = m_pScene->GetPlayer();
                SkillComponent* pDbgSkill = pDbgPlayer
                    ? pDbgPlayer->GetComponent<SkillComponent>() : nullptr;
                if (pDbgSkill)
                {
                    for (int s = 0; s < static_cast<int>(SkillSlot::Count); ++s)
                        for (int r = 0; r < RUNES_PER_SKILL; ++r)
                            pDbgSkill->ClearRuneSlot(static_cast<SkillSlot>(s), r);
                }
            }

            // Left click = select rune → go to skill slot selection
            if (m_inputSystem.IsMouseButtonPressed(0))
            {
                XMFLOAT2 mousePos = m_inputSystem.GetMousePosition();
                for (int i = 0; i < kDebugVisibleRows; ++i)
                {
                    int runeIdx = m_debugRuneScrollOffset + i;
                    if (runeIdx >= (int)m_debugRuneSortedIds.size()) break;

                    float rowY = kDebugRowsStartY + i * kDebugRowHeight;
                    if (mousePos.x >= kDebugPanelLeft &&
                        mousePos.x <= kDebugPanelLeft + kDebugPanelWidth &&
                        mousePos.y >= rowY &&
                        mousePos.y < rowY + kDebugRowHeight)
                    {
                        m_debugSelectedRuneId = m_debugRuneSortedIds[runeIdx];
                        m_debugRuneState = DebugRuneUIState::SelectingSkill;
                        break;
                    }
                }
            }
        }
        else if (m_debugRuneState == DebugRuneUIState::SelectingSkill)
        {
            // ESC = back to rune list
            if (m_inputSystem.IsKeyPressed(VK_ESCAPE))
                m_debugRuneState = DebugRuneUIState::SelectingRune;

            // Left click = equip selected rune into clicked skill+rune slot
            if (m_inputSystem.IsMouseButtonPressed(0))
            {
                XMFLOAT2 mousePos = m_inputSystem.GetMousePosition();
                float screenCenterX = (float)m_nWndClientWidth / 2.0f;
                float screenCenterY = (float)m_nWndClientHeight / 2.0f;
                float slotStartY    = screenCenterY - 20.0f;
                float lineHeight    = 50.0f;

                GameObject* pDbgPlayer = m_pScene->GetPlayer();
                SkillComponent* pDbgSkill = pDbgPlayer
                    ? pDbgPlayer->GetComponent<SkillComponent>() : nullptr;

                bool equipped = false;
                for (int skillIdx = 0; skillIdx < static_cast<int>(SkillSlot::Count) && !equipped; ++skillIdx)
                {
                    float slotY = slotStartY + skillIdx * lineHeight;
                    for (int runeIdx = 0; runeIdx < RUNES_PER_SKILL && !equipped; ++runeIdx)
                    {
                        float runeX = screenCenterX - 140.0f + runeIdx * 140.0f;
                        if (mousePos.x >= runeX && mousePos.x <= runeX + 120.0f &&
                            mousePos.y >= slotY && mousePos.y <= slotY + 35.0f)
                        {
                            if (pDbgSkill)
                                pDbgSkill->SetRuneSlot(static_cast<SkillSlot>(skillIdx),
                                                       runeIdx, m_debugSelectedRuneId, 1);
                            m_debugRuneState = DebugRuneUIState::SelectingRune;
                            equipped = true;
                        }
                    }
                }
            }
        }
    }
    else
    {
        // Normal mode - check for F key interactions
        // ESC = open pause menu
        if (m_inputSystem.IsKeyPressed(VK_ESCAPE))
            m_bShowPauseMenu = true;

        // DEBUG: I key = open rune inspector
        if (m_inputSystem.IsKeyPressed('I'))
        {
            BuildDebugRuneList();
            m_debugRuneScrollOffset = 0;
            m_debugRuneState = DebugRuneUIState::SelectingRune;
        }

        // Priority: Drop item > Interaction cube
        if (m_inputSystem.IsKeyDown('F'))
        {
            if (m_pScene->IsNearDropItem())
            {
                m_pScene->StartDropInteraction();
            }
            else if (m_pScene->IsNearPortalCube())
            {
                m_pScene->TriggerPortalInteraction();
            }
            else if (m_pScene->IsNearInteractionCube())
            {
                m_pScene->TriggerInteraction();
            }
        }

        // DEBUG: T key = take 10 damage, Y key = heal 10
        GameObject* pPlayer = m_pScene->GetPlayer();
        if (pPlayer)
        {
            PlayerComponent* pPlayerComp = pPlayer->GetComponent<PlayerComponent>();
            if (pPlayerComp)
            {
                if (m_inputSystem.IsKeyPressed('T'))
                {
                    pPlayerComp->TakeDamage(10.0f);
                }
                if (m_inputSystem.IsKeyPressed('Y'))
                {
                    pPlayerComp->Heal(10.0f);
                }
            }
        }

        // DEBUG: G key = bloom(Glow) on/off toggle (before/after 비교)
        if (m_inputSystem.IsKeyPressed('G') && m_pBloom)
        {
            m_pBloom->ToggleEnabled();
            OutputDebugStringA(m_pBloom->IsEnabled() ? "[Bloom] ON\n" : "[Bloom] OFF\n");
        }

        // DEBUG: K key = 현재 방 적 전원 즉사 (포탈/드랍 테스트용)
        if (m_inputSystem.IsKeyPressed('K'))
        {
            CRoom* pRoom = m_pScene->GetCurrentRoom();
            if (pRoom)
            {
                int killed = 0;
                for (EnemyComponent* pEnemy : pRoom->GetEnemies())
                {
                    if (pEnemy && !pEnemy->IsDead())
                    {
                        pEnemy->TakeDamage(99999.0f, false);
                        ++killed;
                    }
                }
                wchar_t buf[64];
                swprintf_s(buf, L"[Debug] K: killed %d enemies\n", killed);
                OutputDebugString(buf);
            }
        }
    }

    // Render scene with shadow map (mainRTV + mainDSV 전달)
    m_pScene->Render(m_pd3dCommandList.Get(), m_shadowSrvGpuHandle,
                     d3dRtvCPUDescriptorHandle, d3dDsvCPUDescriptorHandle,
                     m_pd3dRenderTargetBuffers[m_nSwapChainBufferIndex].Get());

    // Transition shadow map back to depth write for next frame
    D3D12_RESOURCE_BARRIER shadowBarrierBack;
    shadowBarrierBack.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    shadowBarrierBack.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    shadowBarrierBack.Transition.pResource = m_pd3dShadowMap.Get();
    shadowBarrierBack.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    shadowBarrierBack.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    shadowBarrierBack.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_pd3dCommandList->ResourceBarrier(1, &shadowBarrierBack);

    // Bloom: capture LDR scene from the back buffer, extract bright pixels (threshold),
    // Gaussian blur, then additive-composite back onto the swap-chain back buffer.
    m_pBloom->Apply(m_pd3dCommandList.Get(),
                    m_pd3dRenderTargetBuffers[m_nSwapChainBufferIndex].Get(),
                    d3dRtvCPUDescriptorHandle,
                    m_nWndClientWidth, m_nWndClientHeight);

    // Text rendering (2D overlay on top of 3D scene + bloom)
    RenderText();

    d3dResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    d3dResourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_pd3dCommandList->ResourceBarrier(1, &d3dResourceBarrier);

    CHECK_HR(m_pd3dCommandList->Close());
    ID3D12CommandList* ppd3dCommandLists[] = { m_pd3dCommandList.Get() };
    m_pd3dCommandQueue->ExecuteCommandLists(_countof(ppd3dCommandLists), ppd3dCommandLists);

    // DirectXTK12 GPU 메모리 커밋
    m_graphicsMemory->Commit(m_pd3dCommandQueue.Get());

    CHECK_HR(m_pdxgiSwapChain->Present(1, 0));

    m_nSwapChainBufferIndex = m_pdxgiSwapChain->GetCurrentBackBufferIndex();

    UpdateFrameRate();

    m_inputSystem.Reset(); // Reset input deltas for the next frame
}

void Dx12App::OnResize(UINT nWidth, UINT nHeight)
{
    if ((m_nWndClientWidth == nWidth && m_nWndClientHeight == nHeight) || nWidth == 0 || nHeight == 0)
    {
        return;
    }

    WaitForGpuComplete();

    m_nWndClientWidth = nWidth;
    m_nWndClientHeight = nHeight;

    for (int i = 0; i < kFrameCount; ++i)
        m_pd3dRenderTargetBuffers[i].Reset();
    m_pd3dDepthStencilBuffer.Reset();

    CHECK_HR(m_pdxgiSwapChain->ResizeBuffers(kFrameCount, m_nWndClientWidth, m_nWndClientHeight, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

    m_nSwapChainBufferIndex = 0;

    CreateRenderTargetViews();
    CreateDepthStencilView();

    // HDR scene RT + bloom chain
    if (m_pBloom)
        m_pBloom->OnResize(m_pd3dDevice.Get(), m_nWndClientWidth, m_nWndClientHeight);

    // Screen-Space Fluid 텍스처 리사이즈
    if (m_pScene)
        m_pScene->OnResizeSSF(m_nWndClientWidth, m_nWndClientHeight);
}

ComPtr<ID3D12Resource> Dx12App::CreateBufferResource(const void* pData, UINT nBytes, D3D12_HEAP_TYPE d3dHeapType, D3D12_RESOURCE_STATES d3dResourceStates, ComPtr<ID3D12Resource>* ppd3dUploadBuffer)
{
    ComPtr<ID3D12Resource> pd3dBuffer;

    D3D12_HEAP_PROPERTIES d3dHeapProperties;
    ::ZeroMemory(&d3dHeapProperties, sizeof(D3D12_HEAP_PROPERTIES));
    d3dHeapProperties.Type = d3dHeapType;
    d3dHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    d3dHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    d3dHeapProperties.CreationNodeMask = 1;
    d3dHeapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC d3dResourceDesc;
    ::ZeroMemory(&d3dResourceDesc, sizeof(D3D12_RESOURCE_DESC));
    d3dResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d3dResourceDesc.Alignment = 0;
    d3dResourceDesc.Width = nBytes;
    d3dResourceDesc.Height = 1;
    d3dResourceDesc.DepthOrArraySize = 1;
    d3dResourceDesc.MipLevels = 1;
    d3dResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    d3dResourceDesc.SampleDesc.Count = 1;
    d3dResourceDesc.SampleDesc.Quality = 0;
    d3dResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d3dResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_RESOURCE_STATES d3dResourceInitialStates = D3D12_RESOURCE_STATE_COPY_DEST;
    if (d3dHeapType == D3D12_HEAP_TYPE_UPLOAD) d3dResourceInitialStates = D3D12_RESOURCE_STATE_GENERIC_READ;

    CHECK_HR(s_pInstance->m_pd3dDevice->CreateCommittedResource(&d3dHeapProperties, D3D12_HEAP_FLAG_NONE, &d3dResourceDesc, d3dResourceInitialStates, NULL, __uuidof(ID3D12Resource), (void**)&pd3dBuffer));

    if (pData)
    {
        if (d3dHeapType == D3D12_HEAP_TYPE_UPLOAD)
        {
            D3D12_RANGE d3dRange = { 0, 0 };
            UINT8* pBufferData = NULL;
            CHECK_HR(pd3dBuffer->Map(0, &d3dRange, (void**)&pBufferData));
            memcpy(pBufferData, pData, nBytes);
            pd3dBuffer->Unmap(0, NULL);
        }
        else
        {
            D3D12_HEAP_PROPERTIES d3dUploadHeapProperties;
            ::ZeroMemory(&d3dUploadHeapProperties, sizeof(D3D12_HEAP_PROPERTIES));
            d3dUploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
            d3dUploadHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            d3dUploadHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            d3dUploadHeapProperties.CreationNodeMask = 1;
            d3dUploadHeapProperties.VisibleNodeMask = 1;

            CHECK_HR(s_pInstance->m_pd3dDevice->CreateCommittedResource(&d3dUploadHeapProperties, D3D12_HEAP_FLAG_NONE, &d3dResourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, __uuidof(ID3D12Resource), (void**)ppd3dUploadBuffer));

            D3D12_RANGE d3dRange = { 0, 0 };
            UINT8* pBufferData = NULL;
            CHECK_HR((*ppd3dUploadBuffer)->Map(0, &d3dRange, (void**)&pBufferData));
            memcpy(pBufferData, pData, nBytes);
            (*ppd3dUploadBuffer)->Unmap(0, NULL);

            s_pInstance->m_pd3dCommandList->CopyResource(pd3dBuffer.Get(), (*ppd3dUploadBuffer).Get());

            D3D12_RESOURCE_BARRIER d3dResourceBarrier;
            ::ZeroMemory(&d3dResourceBarrier, sizeof(D3D12_RESOURCE_BARRIER));
            d3dResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            d3dResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            d3dResourceBarrier.Transition.pResource = pd3dBuffer.Get();
            d3dResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            d3dResourceBarrier.Transition.StateAfter = d3dResourceStates;
            d3dResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            s_pInstance->m_pd3dCommandList->ResourceBarrier(1, &d3dResourceBarrier);
        }
    }
    return pd3dBuffer;
}

void Dx12App::InitializeText()
{
    // GraphicsMemory 초기화
    m_graphicsMemory = std::make_unique<DirectX::GraphicsMemory>(m_pd3dDevice.Get());

    // 폰트용 디스크립터 힙 생성
    //   [0] 폰트, [1] HP바 base, [2] HP바 fill, [3] 캐릭터선택 흰픽셀
    //   [4] VFX magic_03, [5] VFX skull, [6] VFX star_08, [7] VFX twirl_01, [8] VFX fire_01, [9] VFX flare_01
    //   [10~26] UI 텍스처 (UISlot 매핑, kUIHeapBase=10)
    m_fontDescriptorHeap = std::make_unique<DirectX::DescriptorHeap>(
        m_pd3dDevice.Get(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        30
    );

    // 리소스 업로드 배치
    DirectX::ResourceUploadBatch resourceUpload(m_pd3dDevice.Get());
    resourceUpload.Begin();

    // SpriteBatch 생성 (알파 블렌딩 활성화)
    DirectX::RenderTargetState rtState(
        DXGI_FORMAT_R8G8B8A8_UNORM,      // 백버퍼 포맷
        DXGI_FORMAT_D24_UNORM_S8_UINT    // 깊이버퍼 포맷
    );

    // Non-premultiplied alpha blend state for PNG transparency
    CD3DX12_BLEND_DESC blendDesc(D3D12_DEFAULT);
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

    DirectX::SpriteBatchPipelineStateDescription pd(rtState, &blendDesc);
    m_spriteBatch = std::make_unique<DirectX::SpriteBatch>(m_pd3dDevice.Get(), resourceUpload, pd);

    // SpriteFont 로드
    m_spriteFont = std::make_unique<DirectX::SpriteFont>(
        m_pd3dDevice.Get(),
        resourceUpload,
        L"Fonts/myFont.spritefont",
        m_fontDescriptorHeap->GetCpuHandle(0),
        m_fontDescriptorHeap->GetGpuHandle(0)
    );
    // 폰트에 없는 문자(한글/이모지 등)가 들어와도 throw 안 하고 '?'로 대체해 렌더.
    // 없으면 MeasureString/DrawString이 std::runtime_error("Character not in font")를 던짐.
    m_spriteFont->SetDefaultCharacter(L'?');

    // 업로드 완료 대기
    auto uploadFinished = resourceUpload.End(m_pd3dCommandQueue.Get());
    uploadFinished.wait();

    // 뷰포트 설정
    D3D12_VIEWPORT viewport = { 0, 0, (float)m_nWndClientWidth, (float)m_nWndClientHeight, 0, 1 };
    m_spriteBatch->SetViewport(viewport);

    // HealthBarUI 초기화 (디스크립터 인덱스 1, 2 사용)
    // CommandList를 열어서 텍스처 업로드
    CHECK_HR(m_pd3dCommandAllocator->Reset());
    CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));

    m_pHealthBarUI = std::make_unique<HealthBarUI>();
    m_pHealthBarUI->Initialize(m_pd3dDevice.Get(), m_pd3dCommandList.Get(),
                                m_fontDescriptorHeap.get(), 1);

    // CommandList를 닫고 실행
    CHECK_HR(m_pd3dCommandList->Close());
    ID3D12CommandList* ppd3dCommandLists[] = { m_pd3dCommandList.Get() };
    m_pd3dCommandQueue->ExecuteCommandLists(_countof(ppd3dCommandLists), ppd3dCommandLists);
    WaitForGpuComplete();

    // VFX 스프라이트 텍스처 로드 (힙 슬롯 4, 5)
    CHECK_HR(m_pd3dCommandAllocator->Reset());
    CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));
    {
        auto loadVFXTex = [&](const wchar_t* path,
                               ComPtr<ID3D12Resource>& tex, ComPtr<ID3D12Resource>& upload,
                               UINT heapSlot, const std::string& regId)
        {
            std::unique_ptr<uint8_t[]> decoded;
            D3D12_SUBRESOURCE_DATA sub{};
            if (FAILED(DirectX::LoadWICTextureFromFile(m_pd3dDevice.Get(), path,
                                                        tex.ReleaseAndGetAddressOf(), decoded, sub)))
            {
                char buf[256];
                sprintf_s(buf, "[VFXSprite] 텍스처 로드 실패: %ls\n", path);
                OutputDebugStringA(buf);
                return;
            }
            UINT64 sz = GetRequiredIntermediateSize(tex.Get(), 0, 1);
            CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
            auto bd = CD3DX12_RESOURCE_DESC::Buffer(sz);
            m_pd3dDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
            UpdateSubresources(m_pd3dCommandList.Get(), tex.Get(), upload.Get(), 0, 0, 1, &sub);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(tex.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_pd3dCommandList->ResourceBarrier(1, &barrier);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format                  = tex->GetDesc().Format;
            srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels     = tex->GetDesc().MipLevels;
            m_pd3dDevice->CreateShaderResourceView(tex.Get(), &srvDesc,
                m_fontDescriptorHeap->GetCpuHandle(heapSlot));

            auto desc = tex->GetDesc();
            VFXSpriteManager::Get().RegisterTex(regId,
                m_fontDescriptorHeap->GetGpuHandle(heapSlot),
                (UINT)desc.Width, (UINT)desc.Height);
        };

        loadVFXTex(L"Assets/Textures/VFX/magic_03.png",   m_pMagicDecalTex, m_pMagicDecalUpload, 4, "magic3");
        loadVFXTex(L"Assets/Textures/VFX/human-skull.png", m_pSkullTex,      m_pSkullUpload,      5, "skull");
        loadVFXTex(L"Assets/Textures/VFX/star_08.png",     m_pStarTex,       m_pStarUpload,       6, "star_08");
        loadVFXTex(L"Assets/Textures/VFX/twirl_01.png",    m_pTwirlTex,      m_pTwirlUpload,      7, "twirl1");
        loadVFXTex(L"Assets/Textures/VFX/fire_01.png",     m_pFlameTex,      m_pFlameUpload,      8, "fire1");
        loadVFXTex(L"Assets/Textures/VFX/flare_01.png",    m_pFlareTex,      m_pFlareUpload,      9, "flare1");
    }
    CHECK_HR(m_pd3dCommandList->Close());
    ID3D12CommandList* vfxCmdLists[] = { m_pd3dCommandList.Get() };
    m_pd3dCommandQueue->ExecuteCommandLists(_countof(vfxCmdLists), vfxCmdLists);
    WaitForGpuComplete();

    // 스킬 아이콘 HUD 초기화 (자체 디스크립터힙/PSO + SkillIcons/RuneIcons PNG 일괄 로드)
    CHECK_HR(m_pd3dCommandAllocator->Reset());
    CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));
    m_pSkillIconRenderer = std::make_unique<SkillIconRenderer>();
    m_pSkillIconRenderer->Initialize(m_pd3dDevice.Get(), m_pd3dCommandList.Get());
    m_pSkillHud = std::make_unique<SkillHudUI>();
    m_pRuneRewardUI = std::make_unique<RuneRewardUI>();
    CHECK_HR(m_pd3dCommandList->Close());
    ID3D12CommandList* iconCmdLists[] = { m_pd3dCommandList.Get() };
    m_pd3dCommandQueue->ExecuteCommandLists(_countof(iconCmdLists), iconCmdLists);
    WaitForGpuComplete();

    // UI 텍스처 로드 (Title / Loading / Pause / GameOver / Ending / HUD / Avatars)
    CHECK_HR(m_pd3dCommandAllocator->Reset());
    CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));
    {
        auto loadUITex = [&](const wchar_t* path, UISlot slot)
        {
            UINT idx      = (UINT)slot;
            UINT heapSlot = kUIHeapBase + idx;

            std::unique_ptr<uint8_t[]> decoded;
            D3D12_SUBRESOURCE_DATA sub{};
            if (FAILED(DirectX::LoadWICTextureFromFile(m_pd3dDevice.Get(), path,
                m_pUITex[idx].ReleaseAndGetAddressOf(), decoded, sub)))
            {
                char buf[256];
                sprintf_s(buf, "[UI] 텍스처 로드 실패: %ls\n", path);
                OutputDebugStringA(buf);
                return;
            }
            UINT64 sz = GetRequiredIntermediateSize(m_pUITex[idx].Get(), 0, 1);
            CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
            auto bd = CD3DX12_RESOURCE_DESC::Buffer(sz);
            m_pd3dDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&m_pUIUpload[idx]));
            UpdateSubresources(m_pd3dCommandList.Get(), m_pUITex[idx].Get(),
                               m_pUIUpload[idx].Get(), 0, 0, 1, &sub);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pUITex[idx].Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_pd3dCommandList->ResourceBarrier(1, &barrier);

            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format                  = m_pUITex[idx]->GetDesc().Format;
            sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels     = m_pUITex[idx]->GetDesc().MipLevels;
            m_pd3dDevice->CreateShaderResourceView(m_pUITex[idx].Get(), &sd,
                m_fontDescriptorHeap->GetCpuHandle(heapSlot));
            m_hUI[idx] = m_fontDescriptorHeap->GetGpuHandle(heapSlot);
        };

        loadUITex(L"Assets/Textures/UI/title_bg.png",         UISlot::TitleBg);
        loadUITex(L"Assets/Textures/UI/title_logo.png",       UISlot::TitleLogo);
        loadUITex(L"Assets/Textures/UI/btn_normal.png",       UISlot::BtnNormal);
        loadUITex(L"Assets/Textures/UI/btn_hover.png",        UISlot::BtnHover);
        loadUITex(L"Assets/Textures/UI/Loading.png",          UISlot::LoadingBg);
        loadUITex(L"Assets/Textures/UI/loading_spinner.png",  UISlot::LoadingSpinner);
        loadUITex(L"Assets/Textures/UI/pause_panel.png",      UISlot::PausePanel);
        loadUITex(L"Assets/Textures/UI/gameover_bg.png",      UISlot::GameOverBg);
        loadUITex(L"Assets/Textures/UI/gameover_title.png",   UISlot::GameOverTitle);
        loadUITex(L"Assets/Textures/UI/ending_bg.png",        UISlot::EndingBg);
        loadUITex(L"Assets/Textures/UI/ending_title.png",     UISlot::EndingTitle);
        loadUITex(L"Assets/Textures/UI/hud_stage_badge.png",  UISlot::HudStageBadge);
        loadUITex(L"Assets/Textures/UI/hud_boss_bar.png",     UISlot::HudBossBar);
        loadUITex(L"Assets/Textures/UI/hud_boss_bar_fill.png",UISlot::HudBossBarFill);
        loadUITex(L"Assets/Textures/UI/avatar_fire.png",      UISlot::AvatarFire);
        loadUITex(L"Assets/Textures/UI/avatar_water.png",     UISlot::AvatarWater);
        loadUITex(L"Assets/Textures/UI/avatar_wind.png",      UISlot::AvatarWind);
        loadUITex(L"Assets/Textures/UI/avatar_earth.png",     UISlot::AvatarEarth);
    }
    CHECK_HR(m_pd3dCommandList->Close());
    ID3D12CommandList* uiCmdLists[] = { m_pd3dCommandList.Get() };
    m_pd3dCommandQueue->ExecuteCommandLists(_countof(uiCmdLists), uiCmdLists);
    WaitForGpuComplete();
}

void Dx12App::RenderPauseMenu()
{
    if (!m_bShowPauseMenu) return;

    // 패널 배경 이미지 (RenderText 안에서 호출되므로 SpriteBatch begin 활성 상태)
    if (m_pUITex[(UINT)UISlot::PausePanel])
    {
        auto desc = m_pUITex[(UINT)UISlot::PausePanel]->GetDesc();
        DirectX::XMUINT2 panelSz = { (UINT)desc.Width, (UINT)desc.Height };
        float panelH = (float)m_nWndClientHeight * 0.80f;
        float panelScale = panelH / (float)panelSz.y;
        float panelW = panelSz.x * panelScale;
        float panelX = ((float)m_nWndClientWidth  - panelW) * 0.5f;
        float panelY = ((float)m_nWndClientHeight - panelH) * 0.5f;
        RECT panelDst = { (LONG)panelX, (LONG)panelY,
                          (LONG)(panelX + panelW), (LONG)(panelY + panelH) };
        m_spriteBatch->Draw(m_hUI[(UINT)UISlot::PausePanel], panelSz, panelDst);
    }

    float cx = (float)m_nWndClientWidth  / 2.0f;
    float cy = (float)m_nWndClientHeight / 2.0f;
    XMFLOAT2 mp = m_inputSystem.GetMousePosition();

    constexpr float btnW = 220.0f, btnH = 40.0f;
    float resumeY     = cy - 30.0f;
    float charSelectY = cy + 30.0f;
    float quitY       = cy + 90.0f;

    auto isHover = [&](float btnY) {
        return mp.x >= cx - btnW/2.f && mp.x <= cx + btnW/2.f &&
               mp.y >= btnY && mp.y <= btnY + btnH;
    };
    auto drawBtn = [&](const wchar_t* text, float btnY, DirectX::XMVECTORF32 normal, DirectX::XMVECTORF32 hovered) {
        XMVECTOR sz = m_spriteFont->MeasureString(text);
        m_spriteFont->DrawString(m_spriteBatch.get(), text,
            XMFLOAT2(cx - XMVectorGetX(sz) / 2.0f, btnY),
            isHover(btnY) ? hovered : normal);
    };

    // Title
    const wchar_t* title = L"===  일시정지  ===";
    XMVECTOR tsz = m_spriteFont->MeasureString(title);
    m_spriteFont->DrawString(m_spriteBatch.get(), title,
        XMFLOAT2(cx - XMVectorGetX(tsz) / 2.0f, cy - 90.0f),
        DirectX::Colors::White);

    drawBtn(L"계속하기",              resumeY,     DirectX::Colors::White,     DirectX::Colors::Yellow);
    drawBtn(L"캐릭터 선택 화면으로",   charSelectY, DirectX::Colors::LightBlue, DirectX::Colors::Cyan);
    drawBtn(L"게임 종료",             quitY,       DirectX::Colors::Gray,      DirectX::Colors::OrangeRed);

    // Hint
    const wchar_t* hint = L"[ESC] 계속하기";
    XMVECTOR hsz = m_spriteFont->MeasureString(hint);
    m_spriteFont->DrawString(m_spriteBatch.get(), hint,
        XMFLOAT2(cx - XMVectorGetX(hsz) / 2.0f, quitY + 60.0f),
        DirectX::Colors::DimGray);
}

void Dx12App::BuildDebugRuneList()
{
    m_debugRuneSortedIds.clear();
    for (const auto& [id, def] : RuneRegistry::Get().GetAll())
        m_debugRuneSortedIds.push_back(id);

    auto gradeOrder = [](RuneGrade g) -> int {
        switch (g) {
        case RuneGrade::Legendary: return 0;
        case RuneGrade::Unique:    return 1;
        case RuneGrade::Epic:      return 2;
        case RuneGrade::Rare:      return 3;
        default:                   return 4;
        }
    };

    std::sort(m_debugRuneSortedIds.begin(), m_debugRuneSortedIds.end(),
        [&](const std::string& a, const std::string& b) {
            const RuneDef* da = RuneRegistry::Get().Find(a);
            const RuneDef* db = RuneRegistry::Get().Find(b);
            if (!da || !db) return a < b;
            int ga = gradeOrder(da->grade);
            int gb = gradeOrder(db->grade);
            return ga != gb ? ga < gb : a < b;
        });
}

void Dx12App::RenderDebugRuneUI()
{
    if (m_debugRuneState == DebugRuneUIState::None || !m_pScene) return;

    float screenCenterX = (float)m_nWndClientWidth / 2.0f;
    float screenCenterY = (float)m_nWndClientHeight / 2.0f;

    if (m_debugRuneState == DebugRuneUIState::SelectingRune)
    {
        // Header
        m_spriteFont->DrawString(m_spriteBatch.get(),
            L"=== [I] Debug Rune Inspector ===",
            XMFLOAT2(kDebugPanelLeft, 60.0f), DirectX::Colors::Gold);

        m_spriteFont->DrawString(m_spriteBatch.get(),
            L"[C] 룬 전체 초기화   [ESC/I] 닫기   [위아래 / 휠] 스크롤   [클릭] 선택",
            XMFLOAT2(kDebugPanelLeft, 92.0f), DirectX::Colors::Gray);

        m_spriteFont->DrawString(m_spriteBatch.get(),
            L"-----------------------------------------------------------------------",
            XMFLOAT2(kDebugPanelLeft, 118.0f), DirectX::Colors::Gray);

        m_spriteFont->DrawString(m_spriteBatch.get(),
            L"등급               이름                      설명",
            XMFLOAT2(kDebugPanelLeft, 133.0f), DirectX::Colors::DimGray);

        // Rune rows
        XMFLOAT2 mousePos = m_inputSystem.GetMousePosition();
        int total = (int)m_debugRuneSortedIds.size();

        for (int i = 0; i < kDebugVisibleRows; ++i)
        {
            int runeIdx = m_debugRuneScrollOffset + i;
            if (runeIdx >= total) break;

            const RuneDef* def = RuneRegistry::Get().Find(m_debugRuneSortedIds[runeIdx]);
            if (!def) continue;

            float rowY = kDebugRowsStartY + i * kDebugRowHeight;
            bool isHovered = (mousePos.x >= kDebugPanelLeft &&
                               mousePos.x <= kDebugPanelLeft + kDebugPanelWidth &&
                               mousePos.y >= rowY &&
                               mousePos.y < rowY + kDebugRowHeight);

            // Grade label
            const wchar_t* gradeLabel = GetRuneGradeLabel(def->grade);
            if (isHovered)
                m_spriteFont->DrawString(m_spriteBatch.get(), gradeLabel,
                    XMFLOAT2(kDebugPanelLeft, rowY), DirectX::Colors::Yellow);
            else
                m_spriteFont->DrawString(m_spriteBatch.get(), gradeLabel,
                    XMFLOAT2(kDebugPanelLeft, rowY), GetRuneGradeUIColor(def->grade));

            // Rune name
            std::wstring wname = Utf8ToWide(def->name);
            m_spriteFont->DrawString(m_spriteBatch.get(), wname.c_str(),
                XMFLOAT2(kDebugPanelLeft + 140.0f, rowY),
                isHovered ? DirectX::Colors::Yellow : DirectX::Colors::White);

            // Description
            std::wstring desc = BuildRuneDesc(*def);
            m_spriteFont->DrawString(m_spriteBatch.get(), desc.c_str(),
                XMFLOAT2(kDebugPanelLeft + 370.0f, rowY),
                isHovered ? DirectX::Colors::Yellow : DirectX::Colors::DimGray);
        }

        // Bottom separator and scroll status
        float sepY = kDebugRowsStartY + kDebugVisibleRows * kDebugRowHeight;
        m_spriteFont->DrawString(m_spriteBatch.get(),
            L"-----------------------------------------------------------------------",
            XMFLOAT2(kDebugPanelLeft, sepY), DirectX::Colors::Gray);

        int visEnd = min(m_debugRuneScrollOffset + kDebugVisibleRows, total);
        std::wstringstream statusSS;
        statusSS << (m_debugRuneScrollOffset + 1) << L" - " << visEnd << L" / " << total;
        if (m_debugRuneScrollOffset > 0)
            statusSS << L"   [▲ 위로 스크롤]";
        if (m_debugRuneScrollOffset + kDebugVisibleRows < total)
            statusSS << L"   [▼ 아래로 스크롤]";
        m_spriteFont->DrawString(m_spriteBatch.get(), statusSS.str().c_str(),
            XMFLOAT2(kDebugPanelLeft, sepY + 16.0f), DirectX::Colors::Gray);
    }
    else if (m_debugRuneState == DebugRuneUIState::SelectingSkill)
    {
        // Title
        const wchar_t* titleText = L"=== 디버그: 장착할 룬 슬롯 선택 ===";
        XMVECTOR titleSize = m_spriteFont->MeasureString(titleText);
        m_spriteFont->DrawString(m_spriteBatch.get(), titleText,
            XMFLOAT2(screenCenterX - XMVectorGetX(titleSize) / 2.0f, screenCenterY - 100.0f),
            DirectX::Colors::Gold);

        // Selected rune info
        const RuneDef* selDef = RuneRegistry::Get().Find(m_debugSelectedRuneId);
        std::wstring wselName = selDef ? Utf8ToWide(selDef->name)
                                       : Utf8ToWide(m_debugSelectedRuneId);
        std::wstring selectedText = L"선택한 룬: " + wselName;
        XMVECTOR selectedSize = m_spriteFont->MeasureString(selectedText.c_str());
        float selTextX = screenCenterX - XMVectorGetX(selectedSize) / 2.0f;
        m_spriteFont->DrawString(m_spriteBatch.get(), selectedText.c_str(),
            XMFLOAT2(selTextX, screenCenterY - 62.0f), DirectX::Colors::Cyan);
        if (selDef)
        {
            const wchar_t* selGrade = GetRuneGradeLabel(selDef->grade);
            m_spriteFont->DrawString(m_spriteBatch.get(), selGrade,
                XMFLOAT2(selTextX + XMVectorGetX(selectedSize) + 8.0f, screenCenterY - 62.0f),
                GetRuneGradeUIColor(selDef->grade));
        }

        // Skill slots × rune slots
        const wchar_t* slotNames[] = { L"Q", L"E", L"R", L"RMB" };
        GameObject* pPlayer = m_pScene->GetPlayer();
        SkillComponent* pSkill = pPlayer ? pPlayer->GetComponent<SkillComponent>() : nullptr;

        XMFLOAT2 mousePos = m_inputSystem.GetMousePosition();
        float slotStartY = screenCenterY - 20.0f;
        float lineHeight = 50.0f;

        for (int skillIdx = 0; skillIdx < static_cast<int>(SkillSlot::Count); ++skillIdx)
        {
            float slotY = slotStartY + skillIdx * lineHeight;
            std::wstring skillLabel = std::wstring(L"[") + slotNames[skillIdx] + L"] ";
            m_spriteFont->DrawString(m_spriteBatch.get(), skillLabel.c_str(),
                XMFLOAT2(screenCenterX - 250.0f, slotY), DirectX::Colors::White);

            for (int runeIdx = 0; runeIdx < RUNES_PER_SKILL; ++runeIdx)
            {
                float runeX = screenCenterX - 140.0f + runeIdx * 140.0f;
                constexpr float runeW = 120.0f, runeH = 35.0f;

                EquippedRune er = pSkill
                    ? pSkill->GetRuneSlot(static_cast<SkillSlot>(skillIdx), runeIdx)
                    : EquippedRune{};
                const RuneDef* rDef = RuneRegistry::Get().Find(er.runeId);
                std::wstring wRuneName = er.IsEmpty() ? L"[비어있음]"
                    : (rDef ? Utf8ToWide(rDef->name) : Utf8ToWide(er.runeId));

                bool isHovered = (mousePos.x >= runeX && mousePos.x <= runeX + runeW &&
                                   mousePos.y >= slotY && mousePos.y <= slotY + runeH);

                if (isHovered)
                    m_spriteFont->DrawString(m_spriteBatch.get(), wRuneName.c_str(),
                        XMFLOAT2(runeX, slotY), DirectX::Colors::Yellow);
                else if (er.IsEmpty())
                    m_spriteFont->DrawString(m_spriteBatch.get(), wRuneName.c_str(),
                        XMFLOAT2(runeX, slotY), DirectX::Colors::DarkGray);
                else if (rDef)
                    m_spriteFont->DrawString(m_spriteBatch.get(), wRuneName.c_str(),
                        XMFLOAT2(runeX, slotY), GetRuneGradeUIColor(rDef->grade));
                else
                    m_spriteFont->DrawString(m_spriteBatch.get(), wRuneName.c_str(),
                        XMFLOAT2(runeX, slotY), DirectX::Colors::Cyan);
            }
        }

        // Cancel hint
        const wchar_t* cancelText = L"[ESC] 목록으로 돌아가기";
        XMVECTOR cancelSize = m_spriteFont->MeasureString(cancelText);
        m_spriteFont->DrawString(m_spriteBatch.get(), cancelText,
            XMFLOAT2(screenCenterX - XMVectorGetX(cancelSize) / 2.0f, slotStartY + 220.0f),
            DirectX::Colors::Gray);
    }
}

void Dx12App::RenderText()
{
    const float scrW = (float)m_nWndClientWidth;
    const float scrH = (float)m_nWndClientHeight;

    // 룬 획득 모달이 열려있는지 — 열려있으면 하단 스킬 HUD 는 숨긴다.
    DropInteractionState dropStateTop = m_pScene ? m_pScene->GetDropInteractionState()
                                                 : DropInteractionState::None;
    bool runeModalActive = (dropStateTop == DropInteractionState::SelectingRune ||
                            dropStateTop == DropInteractionState::SelectingSkill);

    // ========== 아이콘 패스 — 자체 PSO/디스크립터힙 (폰트 힙 바인딩 전에 먼저) ==========
    if (m_pScene && m_pSkillIconRenderer)
    {
        GameObject* pPlayer = m_pScene->GetPlayer();
        SkillComponent* pSkill = pPlayer ? pPlayer->GetComponent<SkillComponent>() : nullptr;

        // 하단 스킬 HUD (모달 중엔 숨김)
        if (!runeModalActive && m_pSkillHud && pSkill)
        {
            m_pSkillHud->RenderIcons(m_pd3dCommandList.Get(), m_pSkillIconRenderer.get(),
                                     pSkill, scrW, scrH);
        }

        // 룬 획득 모달 아이콘
        if (runeModalActive && m_pRuneRewardUI)
        {
            XMFLOAT2 mp = m_inputSystem.GetMousePosition();
            if (dropStateTop == DropInteractionState::SelectingRune)
            {
                CRoom* pRoom = m_pScene->GetCurrentRoom();
                GameObject* pDrop = pRoom ? pRoom->GetDropItem() : nullptr;
                DropItemComponent* pDropComp = pDrop ? pDrop->GetComponent<DropItemComponent>() : nullptr;
                if (pDropComp)
                {
                    m_pRuneRewardUI->RenderRuneSelectIcons(m_pd3dCommandList.Get(), m_pSkillIconRenderer.get(),
                                                           pDropComp->GetRuneOptions(), mp.x, mp.y, scrW, scrH);
                }
            }
            else // SelectingSkill
            {
                m_pRuneRewardUI->RenderSkillSelectIcons(m_pd3dCommandList.Get(), m_pSkillIconRenderer.get(),
                                                        m_pScene->GetSelectedRune(), pSkill, mp.x, mp.y, scrW, scrH);
            }
        }
    }

    // Bind descriptor heap
    ID3D12DescriptorHeap* heaps[] = { m_fontDescriptorHeap->Heap() };
    m_pd3dCommandList->SetDescriptorHeaps(1, heaps);

    m_spriteBatch->Begin(m_pd3dCommandList.Get());

    // ========== Player Health Bar ==========
    if (m_pScene && m_pHealthBarUI)
    {
        GameObject* pPlayer = m_pScene->GetPlayer();
        if (pPlayer)
        {
            PlayerComponent* pPlayerComp = pPlayer->GetComponent<PlayerComponent>();
            if (pPlayerComp)
            {
                // 선택한 원소에 맞는 아바타를 매 프레임 주입
                ElementType e = m_pScene->GetSelectedElement();
                UISlot avSlot = UISlot::AvatarWater;
                if      (e == ElementType::Fire)  avSlot = UISlot::AvatarFire;
                else if (e == ElementType::Water) avSlot = UISlot::AvatarWater;
                else if (e == ElementType::Wind)  avSlot = UISlot::AvatarWind;
                else if (e == ElementType::Earth) avSlot = UISlot::AvatarEarth;
                if (m_pUITex[(UINT)avSlot])
                {
                    auto avDesc = m_pUITex[(UINT)avSlot]->GetDesc();
                    m_pHealthBarUI->SetAvatar(m_hUI[(UINT)avSlot],
                        DirectX::XMUINT2((UINT)avDesc.Width, (UINT)avDesc.Height));
                }

                m_pHealthBarUI->Render(m_spriteBatch.get(), pPlayerComp->GetHPRatio(),
                                        (float)m_nWndClientWidth, (float)m_nWndClientHeight);
            }
        }
    }

    // Show interaction prompt when near the cube
    if (m_pScene && m_pScene->IsInteractionCubeActive() && m_pScene->IsNearInteractionCube())
    {
        const wchar_t* interactionText = L"[F] Interact";
        float screenCenterX = (float)m_nWndClientWidth / 2.0f;
        float screenCenterY = (float)m_nWndClientHeight / 2.0f + 100.0f;
        XMVECTOR textSize = m_spriteFont->MeasureString(interactionText);
        float textWidth = XMVectorGetX(textSize);

        m_spriteFont->DrawString(
            m_spriteBatch.get(),
            interactionText,
            XMFLOAT2(screenCenterX - textWidth / 2.0f, screenCenterY),
            DirectX::Colors::Yellow
        );
    }

    // ========== Drop Interaction UI ==========
    if (m_pScene)
    {
        DropInteractionState dropState = m_pScene->GetDropInteractionState();
        float screenCenterX = (float)m_nWndClientWidth / 2.0f;
        float screenCenterY = (float)m_nWndClientHeight / 2.0f;

        if (dropState == DropInteractionState::SelectingRune)
        {
            // 아이콘 기반 룬 선택 모달 (텍스트 패스). 아이콘은 위쪽 아이콘 패스에서 그렸다.
            CRoom* pRoom = m_pScene->GetCurrentRoom();
            GameObject* pDropItem = pRoom ? pRoom->GetDropItem() : nullptr;
            DropItemComponent* pDropComp = pDropItem ? pDropItem->GetComponent<DropItemComponent>() : nullptr;
            if (pDropComp && m_pRuneRewardUI)
            {
                m_pRuneRewardUI->RenderRuneSelectText(m_spriteBatch.get(), m_spriteFont.get(),
                    m_fontDescriptorHeap->GetGpuHandle(3), pDropComp->GetRuneOptions(), scrW, scrH);
            }
        }
        else if (dropState == DropInteractionState::SelectingSkill)
        {
            // 아이콘 기반 스킬/룬 슬롯 선택 모달 (텍스트 패스)
            GameObject* pSelPlayer = m_pScene->GetPlayer();
            SkillComponent* pSelSkill = pSelPlayer ? pSelPlayer->GetComponent<SkillComponent>() : nullptr;
            if (m_pRuneRewardUI)
            {
                m_pRuneRewardUI->RenderSkillSelectText(m_spriteBatch.get(), m_spriteFont.get(),
                    m_fontDescriptorHeap->GetGpuHandle(3), m_pScene->GetSelectedRune(), pSelSkill, scrW, scrH);
            }
        }
        else if (m_pScene->IsNearDropItem())
        {
            // Show pickup prompt
            const wchar_t* pickupText = L"[F] Pick up Rune";
            XMVECTOR textSize = m_spriteFont->MeasureString(pickupText);
            m_spriteFont->DrawString(m_spriteBatch.get(), pickupText,
                XMFLOAT2(screenCenterX - XMVectorGetX(textSize) / 2.0f, screenCenterY + 100.0f),
                DirectX::Colors::Cyan);
        }
        else if (m_pScene->IsNearPortalCube())
        {
            // Show portal prompt
            const wchar_t* portalText = L"[F] Enter Portal";
            XMVECTOR textSize = m_spriteFont->MeasureString(portalText);
            m_spriteFont->DrawString(m_spriteBatch.get(), portalText,
                XMFLOAT2(screenCenterX - XMVectorGetX(textSize) / 2.0f, screenCenterY + 100.0f),
                DirectX::Colors::DodgerBlue);
        }
    }

    // ========== Skill UI (아이콘 HUD 텍스트 오버레이) — 모달 중엔 숨김 ==========
    if (m_pScene && m_pSkillHud && !runeModalActive)
    {
        GameObject* pPlayer = m_pScene->GetPlayer();
        if (pPlayer)
        {
            SkillComponent* pSkill = pPlayer->GetComponent<SkillComponent>();
            if (pSkill)
            {
                // 아이콘 위 조작키 라벨 + 쿨다운 남은 초 + (확대 시) 호버 툴팁
                //   폰트 힙 슬롯 3 = 1x1 흰 픽셀 (툴팁 배경 사각형용)
                m_pSkillHud->RenderText(m_spriteBatch.get(), m_spriteFont.get(),
                                        pSkill, m_fontDescriptorHeap->GetGpuHandle(3),
                                        (float)m_nWndClientWidth, (float)m_nWndClientHeight);

                // 활성화 상태 표시 (차지/채널/강화) — 아이콘 줄 위
                float statusX = 24.0f;
                float statusY = (float)m_nWndClientHeight - 152.0f;
                if (pSkill->IsCharging())
                {
                    float chargeProgress = pSkill->GetChargeProgress();
                    float mult = 1.0f + chargeProgress * 2.0f;
                    std::wstringstream s;
                    s << L"CHARGING " << (int)(chargeProgress * 100) << L"% ("
                      << std::fixed << std::setprecision(1) << mult << L"x)";
                    m_spriteFont->DrawString(m_spriteBatch.get(), s.str().c_str(),
                        XMFLOAT2(statusX, statusY), DirectX::Colors::Orange);
                    statusY -= 30.0f;
                }
                if (pSkill->IsChanneling())
                {
                    float p = pSkill->GetChannelProgress();
                    std::wstringstream s;
                    s << L"CHANNELING " << (int)(p * 100) << L"%";
                    m_spriteFont->DrawString(m_spriteBatch.get(), s.str().c_str(),
                        XMFLOAT2(statusX, statusY), DirectX::Colors::LightBlue);
                    statusY -= 30.0f;
                }
                if (pSkill->IsEnhanced())
                {
                    std::wstringstream s;
                    s << L"ENHANCED 2x (" << std::fixed << std::setprecision(1)
                      << pSkill->GetEnhanceTimeRemaining() << L"s)";
                    m_spriteFont->DrawString(m_spriteBatch.get(), s.str().c_str(),
                        XMFLOAT2(statusX, statusY), DirectX::Colors::Gold);
                }
            }
        }
    }

    // Floating damage numbers (world → screen projection)
    if (m_pScene && m_pScene->GetCamera())
    {
        CCamera* pCam = m_pScene->GetCamera();
        XMMATRIX vp = XMLoadFloat4x4(&pCam->GetViewMatrix()) *
                      XMLoadFloat4x4(&pCam->GetProjectionMatrix());
        XMFLOAT4X4 vp4x4;
        XMStoreFloat4x4(&vp4x4, vp);
        DamageNumberManager::Get().Render(m_spriteBatch.get(), m_spriteFont.get(),
                                          vp4x4, (int)m_nWndClientWidth, (int)m_nWndClientHeight);
        VFXSpriteManager::Get().Render(m_spriteBatch.get(),
                                       vp4x4, (int)m_nWndClientWidth, (int)m_nWndClientHeight);
    }

    // ========== Debug Rune Inspector (overlay) ==========
    RenderDebugRuneUI();

    // ========== Flight Mode Crosshair + Hit Counter (4스테이지 바람 보스) ==========
    if (m_pScene && m_pScene->IsFlightHUDActive())
    {
        const wchar_t* crosshair = L"+";
        XMVECTOR sz = m_spriteFont->MeasureString(crosshair);
        float cx = (float)m_nWndClientWidth * 0.5f - XMVectorGetX(sz) * 0.5f;
        float cy = (float)m_nWndClientHeight * 0.5f - XMVectorGetY(sz) * 0.5f;
        m_spriteFont->DrawString(m_spriteBatch.get(), crosshair,
            XMFLOAT2(cx, cy), DirectX::Colors::White, 0.0f, XMFLOAT2(0,0), 1.6f);

        wchar_t hitBuf[64];
        swprintf_s(hitBuf, L"HITS: %d", m_pScene->GetFlightHitCount());
        m_spriteFont->DrawString(m_spriteBatch.get(), hitBuf,
            XMFLOAT2(20.0f, 20.0f), DirectX::Colors::Cyan);
    }

    // ========== Pause Menu (topmost overlay) ==========
    RenderPauseMenu();

    m_spriteBatch->End();
}

void Dx12App::InitSceneWithElement(ElementType e)
{
    WaitForGpuComplete();
    CHECK_HR(m_pd3dCommandAllocator->Reset());
    CHECK_HR(m_pd3dCommandList->Reset(m_pd3dCommandAllocator.Get(), NULL));

    m_pScene = std::make_unique<Scene>();
    m_pScene->SetSelectedElement(e);
    m_pScene->Init(m_pd3dDevice.Get(), m_pd3dCommandList.Get());

    CreateShadowMapSRV();
    m_pScene->UpdatePersistentDescriptorEnd();

    // 로딩 화면 동안 인게임 hitch 방지 워밍업 — vector capacity 예약 등.
    m_pScene->PerformWarmup();

    CHECK_HR(m_pd3dCommandList->Close());
    ID3D12CommandList* ppd3dCommandLists[] = { m_pd3dCommandList.Get() };
    m_pd3dCommandQueue->ExecuteCommandLists(_countof(ppd3dCommandLists), ppd3dCommandLists);
    WaitForGpuComplete();

    // 서버에 선택한 원소를 알린다. playerIndex 0~3 = Fire/Water/Wind/Earth 슬롯.
    // ElementType 은 None=0,Fire=1,...Earth=4 이므로 -1 보정.
    // (S_LOGIN 단계에서 자동 ENTER_GAME 을 보내지 않도록 바꿨으므로 여기서 송신해야 LocalPlayerId 가 발급됨.)
    // SendEnterGame 내부에서 연결 상태 체크하므로 IsConnected()로 게이트하지 않는다
    // (IsConnected()는 LocalPlayerId 발급 후에만 true 라서 첫 호출엔 false).
    if (m_pNetworkManager)
    {
        int slot = static_cast<int>(e) - 1; // Fire→0, Water→1, Wind→2, Earth→3
        if (slot < 0) slot = 0;
        m_pNetworkManager->SendEnterGame(slot);
    }

    m_eAppState = AppState::Playing;
}

void Dx12App::InitializeNetwork()
{
    // NetworkManager 초기화
    m_pNetworkManager = NetworkManager::GetInstance();

    if (!m_pNetworkManager->Initialize())
    {
        OutputDebugString(L"[Network] Failed to initialize NetworkManager\n");
        return;
    }

    // 서버에 연결 (127.0.0.1:7777)
    if (!m_pNetworkManager->Connect(L"127.0.0.1", 7777))
    {
        OutputDebugString(L"[Network] Failed to connect to server\n");
        // 연결 실패해도 게임은 계속 진행 (싱글 플레이)
    }
    else
    {
        OutputDebugString(L"[Network] Connecting to server...\n");
    }
}

void Dx12App::UpdateNetwork(float deltaTime)
{
    if (!m_pNetworkManager || !m_pNetworkManager->IsConnected())
        return;

    // 원격 플레이어 idle 전환 체크 (항상 실행)
    m_pNetworkManager->CheckRemotePlayerIdle(deltaTime);

    // 원격 플레이어 VFX 타임아웃 체크
    m_pNetworkManager->CheckRemotePlayerVFXTimeout(m_pScene.get(), deltaTime);

    // 활성화 룬(차지/증강) VFX 가 원격 플레이어 발 아래 따라가도록 위치 추적
    m_pNetworkManager->UpdateRemoteActivationRuneVFX(m_pScene.get());

    // 궤도 룬(TRF_ORB) deferred 발사 큐 tick
    m_pNetworkManager->UpdatePendingOrbitals(m_pScene.get(), deltaTime);

    // 서버 몬스터 idle 전환 체크
    m_pNetworkManager->CheckServerMonsterIdle(deltaTime);

    // 서버 보스 인디케이터 fill 진행도 갱신 (windup 동안 0→1 차오름)
    m_pNetworkManager->UpdateServerMonsterIndicators(deltaTime);

    // 보스 VFX 지연 스폰 처리 (windup 후/동안 staggered 발사)
    m_pNetworkManager->UpdatePendingMonsterVFX(m_pScene.get(), deltaTime);

	// 서버 몬스터 행동 업데이트 — Dx12App::Update 내부의 cmd list Reset 이후에 호출되어야 함.
	// SpawnRocks 가 MeshLoader 로 vertex/index/texture upload 커맨드를 기록하는데,
	// 여기서 호출하면 Reset 에 의해 그 upload 가 다 폐기되어 GPU 에 데이터가 없는
	// 상태로 draw → 화면에 안 보임. → Dx12App::Update 에서 따로 호출하므로 여기선 빼둔다.
	// m_pNetworkManager->UpdateNetworkGolemBehaviors(deltaTime);  // moved to Dx12App::Update after Reset

    // 서버 몬스터 위치/회전 보간 (MOVE 패킷 간격 사이 부드럽게 이동)
    m_pNetworkManager->InterpolateServerMonsters(deltaTime);

    // 보스 인트로 컷신 yOffset 적용 — 보간 직후 호출해 yOffset 이 매 프레임 덮이도록.
    m_pNetworkManager->UpdateServerBossIntros(m_pScene.get(), deltaTime);

    // 보스 MegaBreath 엄폐물(기둥 4개) 컷신 — 잔여 시간 감소 + 종료 시 정리
    m_pNetworkManager->UpdateServerMegaBreathCutscenes(m_pScene.get(), deltaTime);

    // 보스 액션 yOffset (Jump/Flying) — 보간된 위치에 점프/비행 곡선 덧붙임
    m_pNetworkManager->UpdateServerBossActions(m_pScene.get(), deltaTime);

    // 이동 패킷 전송 간격 체크
    m_fNetworkSendTimer += deltaTime;
    if (m_fNetworkSendTimer < m_fNetworkSendInterval)
        return;

    m_fNetworkSendTimer = 0.0f;

    // 로컬 플레이어 위치 가져오기
    if (!m_pScene)
        return;

    GameObject* pPlayer = m_pScene->GetPlayer();
    if (!pPlayer)
        return;

    TransformComponent* pTransform = pPlayer->GetTransform();
    if (!pTransform)
        return;

    const XMFLOAT3& currentPos = pTransform->GetPosition();

    // 위치가 변경되었는지 확인 (오차 범위 0.01)
    float dx = currentPos.x - m_lastSentPosition.x;
    float dy = currentPos.y - m_lastSentPosition.y;
    float dz = currentPos.z - m_lastSentPosition.z;
    float distSq = dx * dx + dy * dy + dz * dz;

    if (distSq > 0.0001f)  // 0.01 squared
    {
        // 방향 벡터 가져오기 (Look 방향)
        XMVECTOR lookVec = pTransform->GetLook();
        XMFLOAT3 lookDir;
        XMStoreFloat3(&lookDir, lookVec);

        // 위치와 방향 전송
        m_pNetworkManager->SendMove(currentPos.x, currentPos.y, currentPos.z,
                                    lookDir.x, lookDir.y, lookDir.z);
        m_lastSentPosition = currentPos;
    }
}