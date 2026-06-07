#pragma once

#include <vector>
#include <memory>
#include <unordered_map>
#include "GameObject.h"
#include "SkillTypes.h"  // For ActivationType, ElementType
#include "CharacterData.h"
#include "Shader.h"
#include "Mesh.h"
#include "DescriptorHeap.h"
#include "InputSystem.h"
#include "Camera.h"
#include "Room.h" // Added Room.h include
#include "CollisionManager.h" // Added CollisionManager include
#include "EnemySpawner.h" // Added EnemySpawner include
#include "EnemyComponent.h" // For BossIntroPhase, EnemyComponent*
#include "ProjectileManager.h" // Added ProjectileManager include
#include "FluidParticleSystem.h" // Added FluidParticleSystem include (FluidSkillVFXManager 내부에서 SPH 슬롯용으로 사용)
#include "FluidSkillVFXManager.h" // Added FluidSkillVFXManager include
#include "VFXManager.h"     // 통합 VFX 파사드
#include "ScreenSpaceFluid.h" // Screen-Space Fluid Renderer
#include "DebugRenderer.h" // Added DebugRenderer include
#include "Terrain.h"       // Decorative terrain
#include "DecalManager.h"  // 지면 데칼 (회전 마법진 등)

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

// Stage theme for different environments
enum class StageTheme
{
    Fire,   // Volcanic/lava theme (default)
    Water,  // Ocean/water theme
    Earth,  // Ground/rock theme
    Grass,  // Wind/forest theme
    Dark    // Final boss arena
};

// StageTheme → 적 메쉬 색 접미사 후보 (우선순위순). 첫 번째 존재 파일을 사용.
inline const char* const* StageThemeColorCandidates(StageTheme theme, int& outCount)
{
    static const char* kFire [] = { "Rd", "Or", "Ye" };
    static const char* kWater[] = { "Bl", "Cn" };
    static const char* kEarth[] = { "Pe", "Gr", "Br", "Bk" };
    static const char* kGrass[] = { "Gn", "Ye" };
    switch (theme)
    {
    case StageTheme::Water: outCount = 2; return kWater;
    case StageTheme::Earth: outCount = 4; return kEarth;
    case StageTheme::Grass: outCount = 2; return kGrass;
    case StageTheme::Fire:
    default:                outCount = 3; return kFire;
    }
}

// Drop interaction state machine
enum class DropInteractionState
{
    None,           // No drop nearby
    NearDrop,       // Near a drop, can press F
    SelectingRune,  // Choosing from 3 runes (click one)
    SelectingSkill  // Choosing which skill slot to assign rune to
};

struct SpotLight
{
    XMFLOAT4 m_xmf4SpotLightColor;
    XMFLOAT3 m_xmf3SpotLightPosition; float m_fSpotLightRange;
    XMFLOAT3 m_xmf3SpotLightDirection; float m_fSpotLightInnerCone;
    float m_fSpotLightOuterCone; float m_fPad5; float m_fPad6; float m_fPad7;
};

// Torch point light data (for GPU constant buffer)
static constexpr int MAX_TORCH_LIGHTS = 8;

struct TorchLight
{
    XMFLOAT3 m_xmf3Position; float m_fRange;
    XMFLOAT3 m_xmf3Color;    float m_fIntensity;
};

// Gerstner Wave Parameters
struct WaveParams
{
    float m_fWavelength;
    float m_fAmplitude;
    float m_fSteepness;
    float m_fSpeed;
    XMFLOAT2 m_xmf2Direction;
    float m_fFadeSpeed;
    float m_fPad;
};

struct PassConstants
{
    XMFLOAT4X4 m_xmf4x4ViewProj;
    XMFLOAT4X4 m_xmf4x4LightViewProj;  // Shadow Map용 Light View-Projection
    XMFLOAT4 m_xmf4LightColor;
    XMFLOAT3 m_xmf3LightDirection; float m_fPad0;
    XMFLOAT4 m_xmf4PointLightColor;
    XMFLOAT3 m_xmf3PointLightPosition; float m_fPad1;
    float m_fPointLightRange; float m_fPad2; float m_fPad3; float m_fPad4;
    XMFLOAT4 m_xmf4AmbientLight;
    XMFLOAT3 m_xmf3CameraPosition; float m_fPadCam; // Camera World Position
    SpotLight m_SpotLight;
    float m_fTime; float m_fTimePad1; float m_fTimePad2; float m_fTimePad3; // 게임 시간 (용암 애니메이션용)

    // Torch lights array
    TorchLight m_TorchLights[MAX_TORCH_LIGHTS];
    int m_nActiveTorchLights; int m_nTorchPad1; int m_nTorchPad2; int m_nTorchPad3;

    // Gerstner Waves (5 waves for ocean simulation)
    WaveParams m_Waves[5];

    // 스테이지 테마: 0=Fire, 1=Water, 2=Earth, 3=Grass — 셰이더에서 caustics/fog 결정
    // m_nToonEnabled: 0=원본 Phong, 1=원신풍 셀 셰이딩 (F7로 토글)
    // m_fStormStrength: Earth 모래폭풍 강도 0~1, m_fGustStrength: Grass 돌풍 강도 0~1
    int m_nStageTheme; int m_nToonEnabled; float m_fStormStrength; float m_fGustStrength;
};

// Include TorchSystem after PassConstants is defined (avoid circular include)
#include "TorchSystem.h"

class Scene
{
public:
    Scene();
    ~Scene();

    // 씬 초기화 전에 호출하여 플레이어 원소(캐릭터)를 지정
    void SetSelectedElement(ElementType e) { m_eSelectedElement = e; }
    ElementType GetSelectedElement() const { return m_eSelectedElement; }

    void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList);
    void LoadSceneFromFile(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, const char* pstrFileName);
    // Warmup — 로딩 화면 중 호출. 인게임 런타임 hitch 방지용.
    //   Tier 2: vector capacity 예약 (Scene/Room/CollisionManager).
    //   Tier 1: 가능 범위에서 enemy preset animation 클립 prewarm.
    void PerformWarmup();
    void Update(float deltaTime, InputSystem* pInputSystem);
    void RenderShadowPass(ID3D12GraphicsCommandList* pCommandList);
    void Render(ID3D12GraphicsCommandList* pCommandList, D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle,
                D3D12_CPU_DESCRIPTOR_HANDLE mainRTV, D3D12_CPU_DESCRIPTOR_HANDLE mainDSV,
                ID3D12Resource* pMainRTBuffer = nullptr);
    void OnResizeSSF(UINT width, UINT height);

    CCamera* GetCamera() const { return m_pCamera.get(); } // Added getter for CCamera
    CRoom* GetCurrentRoom() const { return m_pCurrentRoom; } // Added getter for current room
    void SetCurrentRoom(CRoom* pRoom) { m_pCurrentRoom = pRoom; }
    ProjectileManager* GetProjectileManager() { return m_pProjectileManager.get(); }
    DecalManager*      GetDecalManager()      { return m_pDecalManager.get(); }
    // 통합 VFX 파사드: 신규 코드는 이 API만 사용한다.
    VFXManager* GetVFXManager() { return m_pVFXManager.get(); }

    // 방 전환 / 스테이지 전환 시 남아 있는 전투용 임시 이펙트 정리
    void ClearTransientCombatEffects();
     
    // ─── 호환용 게터: 기존 SetVFXManager(FluidSkillVFXManager*) 패턴 보존 ───
    // 신규 코드에서 사용하지 말 것 — 점진적 마이그레이션을 위해 잠시 유지.
    FluidSkillVFXManager* GetFluidVFXManager()      { return m_pVFXManager ? m_pVFXManager->GetPlayerVFX() : nullptr; }
    FluidSkillVFXManager* GetEnemyFluidVFXManager() { return m_pVFXManager ? m_pVFXManager->GetEnemyVFX()  : nullptr; }
    TorchSystem* GetTorchSystem() { return m_pTorchSystem.get(); }
    GameObject* GetPlayer() const { return m_pPlayerGameObject; }
    StageTheme  GetCurrentTheme() const { return m_eCurrentTheme; }

    // ── 전환 prerequisite 가드 ─────────────────────────────────────────────
    //   디버그 키(B, N) / 네트워크 매니저가 Scene::Init 완료 전 TransitionTo* 호출 시
    //   m_vShaders[0]->ClearRenderComponents() 등에서 nullptr 디레퍼런스 → 크래시.
    //   모든 TransitionTo* 시작에 호출되어 prerequisite 미충족 시 안전하게 리턴.
    bool IsReadyForTransition() const;

    // ── Sandstorm (Earth theme) ─────────────────────────────────────────────
    //   서버 권위화 시 패킷 수신 시 TriggerSandstorm(duration) 호출. 현재는 로컬 타이머 자동 발생.
    //   GetStormStrength() 는 매 프레임 envelope (attack-release) 결과 0~1. 셰이더 g_StormStrength 로 푸시.
    void  TriggerSandstorm(float duration);
    bool  IsSandstormActive()  const { return m_bSandstormActive; }
    float GetSandstormStrength() const { return m_fSandstormStrength; }

    // ── Wind Gust (Grass theme) ─────────────────────────────────────────────
    //   Sandstorm 과 동일 패턴 — 서버 패킷 마이그레이션 호환.
    void  TriggerWindGust(float duration);
    bool  IsWindGustActive()  const { return m_bWindGustActive; }
    float GetWindGustStrength() const { return m_fWindGustStrength; }
    std::vector<GameObject*> GetAllPlayers() const;  // 로컬 + 원격 플레이어 목록 반환
    void RegisterPlayersToEnemy(class EnemyComponent* pEnemy);  // 적에게 플레이어 등록
    Shader* GetDefaultShader() const { return m_vShaders.empty() ? nullptr : m_vShaders[0].get(); }

    // Interaction system
    bool IsNearInteractionCube() const;
    bool IsInteractionCubeActive() const { return m_bInteractionCubeActive; }
    void TriggerInteraction();
    void HideInteractionCubeByNetworkStart();

    // Portal interaction system
    bool IsNearPortalCube() const;
    void TriggerPortalInteraction();
    void TransitionToNextRoom();
    void TransitionToRoomByIndex(int index); // pool 인덱스 직접 지정 이동 (서버 동기화 / 9·0 디버그)
    void TransitionToBossRoom();        // 불 보스전 (Dragon)
    void TransitionToFireStage(int roomIndex = 0);       // 불 스테이지 (파밍 루프 회귀용)
    void TransitionToWaterStage(int roomIndex = 0);      // 물 스테이지 (N: 불→물)
    void TransitionToWaterBossRoom();   // 물 보스전 (Kraken)
    void TransitionToEarthStage(int roomIndex = 0);      // 땅 스테이지 (N: 물→땅)
    void TransitionToEarthBossRoom();   // 땅 보스전 (Golem)
    void TransitionToGrassStage(int roomIndex = 0);      // 풀 스테이지 (N: 땅→풀)
    void TransitionToGrassBossRoom();   // 풀 보스전 (Demon)
    void TransitionToDarkLordRoom();    // 최종 보스전 (DarkKnight, 오프라인)
    int  GetCycleCount() const { return m_nCycleCount; }
    bool IsGameClear() const { return m_bGameClear; }

    // 4스테이지 바람 ambient — 모든 grass 방에 공통 spawn (배경 토네이도/업드래프트/잎 드리프트)
    void SetupWindAmbient(const DirectX::BoundingBox& roomBB);
    void CleanupWindAmbient();

    // 최종 보스(DarkLord) 입장 컷씬 — 5 페이즈 state machine. Update 안에서 매 프레임 호출.
    //   설계 페이즈/타이밍은 DarkLordIntroStage / DLI_T_* 상수 참조.
    void UpdateDarkLordIntro(float dt);
    bool IsDarkLordIntroPlaying() const { return m_eDarkLordIntroStage != DarkLordIntroStage::None; }

    // 방/스테이지 전환 직전 호출 — 이전 방의 잔여 컷씬/카메라/효과 상태를 강제 정리.
    //   해결 문제: DarkLord(또는 Red Dragon) 입장 컷씬 도중 디버그 키(B/N/0/9)로 방 이동 시
    //   raw pointer 가 dangling 되어 다음 프레임 UpdateDarkLordIntro / Dragon intro 블록에서 UAF.
    //   추가로 카메라 cinematic / shake / boss grace / drop 상호작용 / Sanctum decal 도 함께 reset.
    void CancelTransientStateBeforeTransition();
    // 컷씬 Sever 페이즈의 스크린 베기 오버레이 — 카멘/메이플 히어로 타임라인을 다중 envelope 로 분해.
    //   Dx12App::RenderText 가 매 프레임 호출해 SpriteBatch 로 각 레이어를 합성.
    //   타임라인 (t = Sever 진행도 0~1):
    //     0.00 ~ 0.27 : pre-dark (화면 어둡게)
    //     0.27 ~ 0.40 : warning line (얇은 흰 선)
    //     0.40 ~ 0.50 : flash (풀스크린 화이트)
    //     0.45 ~ 0.85 : 화면 균열 (검은 갭 + 붉은 glow)
    //     0.85 ~ 1.00 : recovery (붉은 잔광만 남음)
    struct IntroSeverState {
        bool  active           = false;
        float progress         = 0.0f;   // 0~1 전체 진행률
        float predarkAlpha     = 0.0f;   // 화면 어둡게 덮는 black 알파
        float warningLineAlpha = 0.0f;   // 얇은 예고 흰 선
        float flashAlpha       = 0.0f;   // 풀스크린 화이트
        float coreAlpha        = 0.0f;   // 얇은 흰 절단선 (main)
        float voidAlpha        = 0.0f;   // 검은 갭 (검정 균열)
        float redGlowAlpha     = 0.0f;   // 가장자리 어두운 진홍 잔광
        float afterGlowAlpha   = 0.0f;   // recovery 단계의 옅은 잔광
        // ── Strike head — 슬래시 위를 지나가는 밝은 점.
        //   slashHeadT 가 0→1 로 sweep 하는 동안 한 점이 화면 좌하 → 우상 직선을 따라 이동.
        //   카멘/히어로식 "칼이 지나갔다" 의 인과를 만든다. headAlpha 는 warning~flash 동안만 visible.
        float slashHeadT       = 0.0f;
        float slashHeadAlpha   = 0.0f;
    };
    IntroSeverState GetIntroSeverState() const;

    // 후속 연출 — Sever 이후 ~ Dominion 동안의 시네마틱 보강.
    //   letterbox : 위/아래 검은 띠 (0 = 없음, 1 = 풀 두께). Sever 후반 slide in → Dominion 후반 slide out.
    //   bossName  : 중앙 보스 이름 텍스트 알파 (0~1). Devour 초반 fade in, hold, Dominion 후반 fade out.
    //   edgeVignette : 가장자리 어둡게 (0~1). Sever 직후 ramp, Dominion 후반 fade.
    float GetIntroLetterboxAmt() const;
    float GetIntroBossNameAlpha() const;
    float GetIntroEdgeVignetteAmt() const;
    // 풀스크린 암전 — Sever 끝~Devour 초반. 슬래시 이후 화면이 한 박자 검정으로 꺼졌다가
    //   보스 솟구침과 함께 천천히 fade out. Devour 페이즈의 정적인 느낌 해소.
    float GetIntroBlackoutAlpha() const;
    // Phase 1 시작 시 4 원소 sigil + 빛기둥 spawn — 보스방 BB 의 cardinal 4방에 자동 배치.
    void SetupElementalSanctum(const DirectX::BoundingBox& roomBB);
    void CleanupElementalSanctum();
    // 네트워크 맵 토네이도 이벤트 시작
    void StartNetworkMapTornadoEvent(const DirectX::XMFLOAT3& pos, float warningSec, float activeSec);
    // 스테이지 테마별 sky/clear color 적용 (Dx12App 의 m_fClearColor 갱신)
    void ApplyThemeSkyColor();

    // Drop interaction system
    DropInteractionState GetDropInteractionState() const { return m_eDropState; }
    bool IsNearDropItem() const;
    void StartDropInteraction();  // Called when F pressed near drop
    void SelectRune(int choice);  // Called when 1/2/3 pressed during selection
    void SelectRuneByClick(int runeIndex);  // Called when mouse clicks on a rune option
    void SelectSkillSlot(SkillSlot slot, int runeSlotIndex);  // Called when clicking skill's rune slot
    void CancelDropInteraction(); // Cancel selection (e.g., ESC or walk away)
    bool IsSelectingRune() const { return m_eDropState == DropInteractionState::SelectingRune; }
    bool IsSelectingSkill() const { return m_eDropState == DropInteractionState::SelectingSkill; }
    const std::string& GetSelectedRune() const { return m_sSelectedRuneId; }

    GameObject* CreateGameObject(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList);

    // GameObject deletion (deferred to end of frame)
    void MarkForDeletion(GameObject* pGameObject);
    CollisionManager* GetCollisionManager() { return m_pCollisionManager.get(); }

    // MapLoader support
    CRoom* CreateRoomFromBounds(const XMFLOAT3& center, const XMFLOAT3& extents);
    EnemySpawner* GetEnemySpawner() { return m_pEnemySpawner.get(); }
    const std::vector<std::unique_ptr<CRoom>>& GetRooms() const { return m_vRooms; }

    // 메아리 룬(ABY_ECO) 등 지연 발동 시 사용 — 모든 방에서 가장 가까운 살아있는 적 반환
    EnemyComponent* FindNearestEnemy(const DirectX::XMFLOAT3& pos) const;

    // Network support
    void AddRenderComponentsToHierarchy(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, GameObject* pGameObject, Shader* pShader, bool bCastsShadow = false);

    void AllocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* pCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* pGpuHandle)
    {
        if (m_nNextDescriptorIndex >= 16384)
        {
            OutputDebugString(L"[Scene] ERROR: Descriptor heap overflow! Increase heap size.\n");
            *pCpuHandle = m_pDescriptorHeap->GetCPUHandle(0);
            *pGpuHandle = m_pDescriptorHeap->GetGPUHandle(0);
            return;
        }
        *pCpuHandle = m_pDescriptorHeap->GetCPUHandle(m_nNextDescriptorIndex);
        *pGpuHandle = m_pDescriptorHeap->GetGPUHandle(m_nNextDescriptorIndex);
        m_nNextDescriptorIndex++;
    }

    // Update persistent descriptor watermark (call after allocating permanent descriptors like Shadow Map SRV)
    void UpdatePersistentDescriptorEnd()
    {
        m_nPersistentDescriptorEnd = m_nNextDescriptorIndex;
    }

    D3D12_GPU_VIRTUAL_ADDRESS GetPassCBVAddress() const { if(m_pd3dcbPass) return m_pd3dcbPass->GetGPUVirtualAddress(); return 0; }

    // 서버 BossEvent PhaseChange 수신 시 호출되는 네트워크 Kraken 컷신 시작 함수
    // 서버가 스폰한 Kraken GameObject와 monsterId를 기존 컷신 상태머신에 연결한다.
    void StartNetworkKrakenCutscene(GameObject* pKrakenObj, uint64 monsterId);
	// 컷신 진행 중 매 프레임마다 Update에서 호출하여 컷신 상태머신 업데이트 및 Kraken GameObject 제어
    bool IsNetworkKrakenCutsceneTarget(uint64 monsterId) const;

private:
    // 캐릭터 선택 화면에서 결정된 플레이어 원소 (Init() 전에 SetSelectedElement로 설정)
    ElementType m_eSelectedElement = ElementType::Water;

    float m_fTotalTime = 0.0f;
    float m_fLastDeltaTime = 0.016f;
    bool m_bInBossRoom = false;  // 보스 룸 여부 (클리어 시 다음 스테이지 전환)

    // 디버그용 영구 wind VFX (데몬 보스방 진입 시 활성, 튜닝 가시성 확보)
    int   m_nDebugWindVFXId    = -1;
    float m_fDebugWindVFXTimer = 0.0f;
    DirectX::XMFLOAT3 m_xmf3DebugWindPos = { 0.0f, 0.0f, 0.0f };

    // 인터랙션 큐브 포탈 VFX — 3 레이어(Ring + Suction + Beam) 무한 루프. 각 이미터 1회 spawn 후 입자가 죽으므로
    // 주기적으로 재 spawn 하여 continuous 유지. RespawnTimer 가 PORTAL_RING_RESPAWN_INTERVAL 도달 시 stop+spawn.
    int   m_nInteractionCubePortalRingVFXId    = -1; // Portal_Ring 슬롯 (보색 마젠타-시안 회전 링)
    int   m_nInteractionCubePortalSuctionVFXId = -1; // Portal_Suction 슬롯 (외곽→중심 흡입)
    int   m_nInteractionCubePortalBeamVFXId    = -1; // Portal_Beam 슬롯 (수직 광주)
    float m_fPortalRingRespawnTimer = 0.0f;          // 재 spawn 누적 타이머 (3 레이어 공통)

    // 4스테이지 바람 ambient — 절차적 풀 군집
    //   각 군집 = 1 GameObject + 1 GrassClumpMesh. mesh refcount는 GameObject SetMesh가 1로 만듦.
    //   CleanupWindAmbient에서 SetMesh(nullptr) → Release → delete (refcount 0).
    std::vector<GameObject*> m_vGrassClumpObjects;   // 군집 GameObject 포인터(MarkForDeletion 대상)

    // 4스테이지 바람 ambient — 배경 토네이도/업드래프트/드리프트 잎 + 주기 큰 토네이도(트랩)
    std::vector<int> m_vAmbientWindIds;          // 영구 ambient VFX 슬롯
    enum class TornadoEventPhase { Idle, Warning, Active, Cooldown };
    TornadoEventPhase m_eTornadoPhase    = TornadoEventPhase::Idle;
    DirectX::XMFLOAT3 m_xmf3TornadoEventPos = { 0.0f, 0.0f, 0.0f };
    int   m_nPeriodicTornadoId = -1;             // Active 페이즈 토네이도 VFX 슬롯
    int   m_nTornadoWarningVFXId = -1;           // Warning 페이즈 경고 VFX 슬롯
    float m_fPeriodicTornadoTimer = 0.0f;        // 현재 페이즈 경과 시간
    float m_fTornadoDamageTickTimer = 0.0f;      // Active 중 데미지 tick
    float m_fGustBurstTimer = 0.0f;              // gust 꽃가루 burst — 풀숲에서 주기적 emit
	float m_fNetworkTornadoWarningSec = 2.0f;    // 네트워크 맵 토네이도 이벤트 시간 (경고 지속 시간)
	float m_fNetworkTornadoActiveSec = 6.0f;     // 네트워크 맵 토네이도 이벤트 시간 (활성 지속 시간)
	bool m_bUseNetworkTornadoEvent = false;      // 네트워크 맵 토네이도 이벤트 사용 여부 
    StageTheme m_eLastAppliedTheme = StageTheme::Fire;  // sky color 변경 감지용
    StageTheme m_eCurrentTheme = StageTheme::Fire; // 현재 스테이지 테마

    // ── Sandstorm state (Earth theme) ───────────────────────────────────────
    //   서버 권위화 대비: TriggerSandstorm() 가 패킷-드리븐 진입점. 로컬 cycleTimer 가 임시 트리거.
    //   envelope: attack 1.5s → 지속 (duration-3s) → release 1.5s. 활성 끝나면 m_bSandstormActive=false.
    float m_fSandstormCycleTimer  = 0.0f;   // 다음 폭풍까지 경과 (Earth 진입 시 0 으로 reset)
    float m_fSandstormPhaseTimer  = 0.0f;   // 활성 중 경과
    float m_fSandstormDuration    = 0.0f;   // 현재 활성 폭풍 총 지속 시간
    float m_fSandstormStrength    = 0.0f;   // 셰이더에 push 할 0~1 envelope
    bool  m_bSandstormActive      = false;
    static constexpr float kSandstormCycleSec   = 35.0f;  // 다음 폭풍 간격
    static constexpr float kSandstormDefaultSec = 8.0f;   // 기본 폭풍 지속 (램프 길이 늘리면서 함께 증가)
    static constexpr float kSandstormRampSec    = 2.5f;   // attack=release ramp — 너무 빠르면 binary 함

    // ── Wind gust state (Grass theme) — Sandstorm 과 동일 패턴 ─────────────
    float m_fWindGustCycleTimer  = 0.0f;
    float m_fWindGustPhaseTimer  = 0.0f;
    float m_fWindGustDuration    = 0.0f;
    float m_fWindGustStrength    = 0.0f;
    bool  m_bWindGustActive      = false;
    static constexpr float kWindGustCycleSec   = 30.0f;
    static constexpr float kWindGustDefaultSec = 5.0f;
    static constexpr float kWindGustRampSec    = 1.5f;
    bool m_bToonEnabled = true;  // F7로 토글: 원신풍 셀 셰이딩 (기본 ON)
    GameObject* m_pLavaPlane = nullptr; // 용암 바닥 평면
    GameObject* m_pWaterPlane = nullptr; // 물 바닥 평면
    GameObject* m_pRockPlane = nullptr; // 바위/동굴 바닥 평면 (Earth)

    // Additional water textures (Water_6 + foam4)
    ComPtr<ID3D12Resource> m_pd3dWaterNormal2 = nullptr;      // Water_6_Normal.png (t7)
    ComPtr<ID3D12Resource> m_pd3dWaterHeight2 = nullptr;      // Water_6_Height.png (t8)
    ComPtr<ID3D12Resource> m_pd3dFoamOpacity = nullptr;       // foam4_Opacity.tga (t9)
    ComPtr<ID3D12Resource> m_pd3dFoamDiffuse = nullptr;       // foam4_Diffuse.tif (t10)
    ComPtr<ID3D12Resource> m_pd3dWaterNormal2Upload = nullptr;
    ComPtr<ID3D12Resource> m_pd3dWaterHeight2Upload = nullptr;
    ComPtr<ID3D12Resource> m_pd3dFoamOpacityUpload = nullptr;
    ComPtr<ID3D12Resource> m_pd3dFoamDiffuseUpload = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE m_d3dWaterNormal2GpuHandle = {};  // GPU handle for t7
    D3D12_GPU_DESCRIPTOR_HANDLE m_d3dWaterHeight2GpuHandle = {};  // GPU handle for t8
    D3D12_GPU_DESCRIPTOR_HANDLE m_d3dFoamOpacityGpuHandle = {};   // GPU handle for t9
    D3D12_GPU_DESCRIPTOR_HANDLE m_d3dFoamDiffuseGpuHandle = {};   // GPU handle for t10

    std::vector<std::unique_ptr<GameObject>> m_vGameObjects; // Global Objects (Player, etc.)
    std::vector<GameObject*> m_vPendingDeletions; // Objects marked for deletion (processed at end of frame)
    std::vector<std::unique_ptr<CRoom>> m_vRooms; // Room List
    CRoom* m_pCurrentRoom = nullptr; // Pointer to the current active room
    int m_nRoomCount = 0; // Room counter for tracking progression

    // 4스테이지(Grass) 보스 클리어 후 파밍 루프를 돌 때마다 1씩 증가 — 적 HP/데미지 스케일에 사용
    int  m_nCycleCount = 0;
    bool m_bGameClear  = false;
    // 풀 보스 클리어 후 분기 포탈 한 번만 spawn 하기 위한 가드
    bool m_bBranchPortalsSpawned = false;

    // Flight prototype (4스테이지 바람 보스 레일 슈팅) — F6 토글
    GameObject* m_pFlightBossDummy = nullptr;
    float m_fFlightBossSpeed = 22.0f;       // 보스 전진 속도 (m/s)
    float m_fFlightBossYawDeg = 0.0f;       // 보스 진행 방향 yaw (deg). +Z 기준
    float m_fFlightCurveTime  = 0.0f;       // 곡선 코스 누적 시간
    float m_fFlightBossHitFlashTimer = 0.0f; // 사격 피격 시 보스 플래시
    int   m_nFlightHitCount = 0;             // 누적 명중 (HUD/디버그)
    float m_fFlightWindAccum = 0.f;          // 비행 윈드 LightEmitter 재스폰 누적
    float m_fFlightFovOffsetCur = 0.0f;      // 현재 적용된 FOV 보정(deg)
    float m_fFlightBossSkillTimer = 0.0f;    // 보스 기본 공격 쿨다운 (s)
    static constexpr float kFlightHitFlashDuration = 0.18f;
    static constexpr float kFlightBaseFovOffset    = 6.0f;  // 비행 진입 시 항상 +6deg
    static constexpr float kFlightBoostFovOffset   = 22.0f; // 부스트 시 +22deg (기본+16)
    static constexpr float kFlightBossSkillCooldown = 2.2f; // 보스 기본 공격 발사 주기(s)

    // 보스 기본 공격 = 부채꼴 탄막 (속성별 색만 다름, 메커닉은 동일)
    struct FlightBossBullet
    {
        XMFLOAT3 pos;
        XMFLOAT3 vel;
        float    lifeRemain;
        int      fluidId;  // FluidSkillVFXManager 슬롯 (유체 트레일)
    };
    std::vector<FlightBossBullet> m_FlightBossBullets;
    ElementType m_eFlightBossElement = ElementType::Wind;
    static constexpr int   kFlightBulletsPerVolley = 5;
    static constexpr float kFlightBulletSpeed      = 38.0f;
    static constexpr float kFlightBulletLifetime   = 2.5f;
    static constexpr float kFlightBulletFanDeg     = 22.0f; // 부채꼴 반각
    static constexpr float kFlightBulletHitRadius  = 1.6f;  // 플레이어 피격 반경
    void FireFlightBossBarrage();
    void UpdateFlightBossBullets(float deltaTime);
    void GetFlightBulletColors(ElementType e, XMFLOAT4& outStart, XMFLOAT4& outEnd) const;
    void SpawnFlightBossDummy(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList);
    void ToggleFlightMode(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList);
    void UpdateFlightBoss(float deltaTime);
    void UpdateFlightFX(float deltaTime, InputSystem* pInputSystem);

public:
    // 사격: 외부(PlayerComponent)에서 호출
    void FlightShoot(const XMFLOAT3& muzzlePos, const XMFLOAT3& dirNormalized);
    int  GetFlightHitCount() const { return m_nFlightHitCount; }
    bool IsFlightHUDActive() const;

private:

    // Interaction Cube
    GameObject* m_pInteractionCube = nullptr;
    bool m_bInteractionCubeActive = true;
    bool m_bEnemiesSpawned = false;
    float m_fInteractionDistance = 5.0f;

    // Fire boss Dragon intro cutscene
    EnemyComponent* m_pDragonIntroEnemy = nullptr;
    BossIntroPhase  m_eLastDragonPhase  = BossIntroPhase::None;

    // Water boss 2-phase: Kraken pre-spawned, emerges after Blue Dragon dies
    bool m_bPendingKrakenSpawn = false;          // death callback set this
    XMFLOAT3 m_xmf3PendingKrakenPos = {};        // dragon death position
    EnemyComponent* m_pPreloadedKraken = nullptr; // pre-spawned but hidden
	
    // 온라인/네트워크 모드에서 서버가 스폰한 Kraken GameObject를 컷신 대상으로 사용
    // 기존 오프라인 컷신은 EnemyComponent* m_pPreloadedKraken 기준이지만,
    // 서버 권위 몬스터는 EnemyComponent가 없으므로 GameObject*를 별도로 보관한다.
    GameObject* m_pNetworkKrakenCutsceneObject = nullptr;

    // 온라인/네트워크 Kraken 컷신 대상 몬스터 ID
    // 컷신 종료 시 C_BOSS_CUTSCENE_END 패킷에 실어 서버로 보낸다.
    uint64 m_nNetworkKrakenCutsceneMonsterId = 0;

    // Kraken 컷씬: Rumble→Rise→Burst→Reveal→Roar→Jump→Slam→WaterRise→None
    //  - Jump: Roar 위치 → 맵 바깥 수면 위로 포물선 점프
    //  - Slam: 수면 쾅 내려치기 (카메라 쉐이크)
    //  - WaterRise: 서서히 차오르는 수면이 플레이어를 기존 맵보다 높은 곳으로 띄움
    enum class KrakenCutsceneStage {
        None,
        Rumble, Rise, Burst, Reveal,
        Roar,
        Jump,
        Slam,
        WaterRise
    };
    KrakenCutsceneStage m_eKrakenStage = KrakenCutsceneStage::None;
    float m_fKrakenEmergeTimer = 0.0f;
    bool  m_bSlamShakeTriggered = false;
    bool  m_bKrakenRoarFadedToIdle = false;   // Roar 중 Unreal Take 종료 후 Idle 루프 전환 플래그
    XMFLOAT3 m_xmf3KrakenJumpStart = {}; // Roar 종료 시 크라켄 위치 (점프 시작점)
    XMFLOAT3 m_xmf3KrakenJumpEnd   = {}; // 점프 착지점 (맵 바깥 수면 위 슬램 지점)
    static constexpr float KRAKEN_SCALE = 2.0f;
    // Stage durations (cumulative) — 연출 텀 확보를 위해 일부 단계 확장
    static constexpr float KRAKEN_T_RUMBLE     = 1.2f;   // +0.4s (진동 예고)
    static constexpr float KRAKEN_T_RISE       = 2.4f;   // Rumble + 1.2s
    static constexpr float KRAKEN_T_BURST      = 4.0f;   // Rise + 1.6s
    static constexpr float KRAKEN_T_REVEAL     = 6.0f;   // Burst + 2.0s (몸 드러내는 여운)
    static constexpr float KRAKEN_T_ROAR       = 9.5f;   // Reveal + 3.5s (포효 fully 재생)
    static constexpr float KRAKEN_T_JUMP       = 14.0f;  // Roar + 4.5s (거리 -100)
    static constexpr float KRAKEN_T_SLAM       = 16.0f;  // Jump + 2.0s
    static constexpr float KRAKEN_T_WATER_RISE = 26.0f;  // Slam + 10s

    // 슬램/점프 착지점 (맵 바깥 수면 위)
    static constexpr float KRAKEN_SLAM_OFFSET_X = 0.0f;
    static constexpr float KRAKEN_SLAM_OFFSET_Z = -125.0f;  // 맵 바깥 물 장판 위 내려찍기
    static constexpr float KRAKEN_LAND_Y        = -2.0f;   // 수면(-4) 위 몸체 노출 Y
    static constexpr float KRAKEN_JUMP_PEAK_DY  = 25.0f;   // 점프 최고점 추가 Y

    // 물 상승: 기존 맵 타일(Y=0)보다 훨씬 위로 → "더 높은 곳에서 전투" 연출
    static constexpr float KRAKEN_WATER_Y_START = -4.0f;
    static constexpr float KRAKEN_WATER_Y_END   = 28.0f;   // 15 → 28: 확실한 고지대 상승

    // ── DarkLord 입장 컷씬 ────────────────────────────────────────────────────
    // Phase 1 Sanctum(화려한 4원소 맵 + 카메라 팬인) → 2 Dread(라이팅 dim + tremor)
    //       → 3 Sever(슬래시 + 카메라 셰이크 + 즉시 테마 스왑) → 4 Devour(보스 솟구침 + 4원소 흡수)
    //       → 5 Dominion(보스 포효 + 카메라 홀드 → 입력 복구).
    // 보스 SpawnEnemy 는 TransitionToDarkLordRoom 에서 즉시 호출되지 않고 Phase 4 진입 시점에 호출.
    enum class DarkLordIntroStage {
        None,
        Sanctum, Dread, Sever, Devour, Dominion
    };
    DarkLordIntroStage m_eDarkLordIntroStage = DarkLordIntroStage::None;
    float    m_fDarkLordIntroTimer = 0.0f;
    // 0.0 = 화려한 Sanctum 톤, 1.0 = 어두운 Dark 톤. Sever 페이즈에서 빠르게 1.0 로 lerp.
    //   PassConstants 의 light/ambient 가 이 값으로 sanctum 팔레트 ↔ 기존 Dark 팔레트 사이를 보간.
    float    m_fSanctumBlend       = 1.0f;   // Dark 비활성 디폴트 (이전 동작 유지)
    bool     m_bIntroSeverShakeTriggered = false;
    bool     m_bIntroBossSpawned         = false;
    int      m_nIntroAbsorbCount         = 0;   // Phase 4 에서 0~3 으로 카운트 (4 원소 차례로 흡수)
    GameObject*           m_pDarkLordCutsceneObject = nullptr;   // Phase 4 에서 spawn 된 보스 — driver 가 scale/pos 트래킹
    GameObject*           m_pIntroSlashOverlay      = nullptr;   // legacy — 스크린 SpriteBatch 오버레이로 대체, nullptr 유지
    SwordTrailMesh*       m_pIntroSlashMesh         = nullptr;   // legacy
    // ── Sanctum 4 zone 트래킹 — 각 zone (방 사분면) 마다 다수 sigil + aura + pillar 무작위 흩뿌림.
    //   인위적 cardinal 배치 대신 organic scatter — "그 구역에 그 원소 색이 확 보임".
    struct SanctumElement {
        int              auraDecalSlot = -1;     // zone 한가운데 큰 색 wash (조명 느낌)
        std::vector<int> sigilDecalSlots;        // zone 안에 흩뿌린 작은 마법진들
        std::vector<int> pillarVFXIds;           // zone 안에 흩뿌린 파티클 컬럼들
    };
    std::vector<SanctumElement> m_vSanctumElements;

    // Cumulative timings — Kraken 패턴.
    static constexpr float DLI_T_SANCTUM  = 3.0f;     // 0   ~ 3.0s  : vivid 4원소 reveal + camera 슬로 팬인
    static constexpr float DLI_T_DREAD    = 5.0f;     // 3.0 ~ 5.0s  : 라이팅 dim + 가벼운 tremor + edge vignette
    static constexpr float DLI_T_SEVER    = 7.2f;     // 5.0 ~ 7.2s  : 슬래시 + flash + 테마 즉시 스왑 (2.2s 로 늘려 베기 연출 여유)
    static constexpr float DLI_T_DEVOUR   = 10.8f;    // 7.2 ~ 10.8s : 보스 솟구침 + 4원소 흡수
    static constexpr float DLI_T_DOMINION = 12.0f;    // 10.8 ~ 12.0s : 보스 포효 + 카메라 홀드 (1.2s) → 입력 복구
    // 컷씬 종료 후 보스 공격 grace — 입력 / AI 풀리지만 보스는 이 시간 동안 정지·무적 유지.
    static constexpr float DLI_T_BOSS_GRACE_AFTER_CUTSCENE = 3.0f;
    float m_fBossGracePeriodRemain = 0.0f;

    // Drop interaction
    DropInteractionState m_eDropState = DropInteractionState::None;
    GameObject* m_pCurrentDropItem = nullptr;  // The drop we're interacting with
    float m_fDropInteractionDistance = 5.0f;
    std::string m_sSelectedRuneId;  // Selected rune ID waiting for skill slot assignment ("" = none)
    int m_nSelectedRuneOptionIndex = -1; // 이번 보상 룬 3개 중 선택한 번호: 0~2

    // ── Map pool ──────────────────────────────────────────────────────────────
    // Add map JSON paths here. TransitionToNextRoom picks one at random.
    std::vector<std::string> m_vMapPool;
    std::string              m_strCurrentMap;    // Path of the currently loaded map
    std::string              m_strBossMap;       // Path of the boss room JSON (from rooms.json "bossRoom")
    int                      m_nCurrentPoolIndex = 0; // Index into m_vMapPool for 9/0 nav

    void ReAddRenderComponentsToShader(GameObject* pGO);  // Traverse hierarchy

    std::vector<std::unique_ptr<Shader>> m_vShaders;

    std::unique_ptr<CDescriptorHeap> m_pDescriptorHeap;
    UINT m_nNextDescriptorIndex = 0;
    UINT m_nPersistentDescriptorEnd = 0;  // Watermark after permanent objects; recyclable slots start here

    // 재활용 구간의 CB 리소스 캐시 — 맵 전환마다 CreateCommittedResource 호출 방지
    // key = 디스크립터 슬롯 번호 (SRV 슬롯이 끼어들어도 정확히 매핑됨)
    std::unordered_map<UINT, ComPtr<ID3D12Resource>> m_vCBCache;

    // Pass Constant Buffer
    ComPtr<ID3D12Resource> m_pd3dcbPass = nullptr;
    PassConstants* m_pcbMappedPass = nullptr;

    std::unique_ptr<CCamera> m_pCamera; // Added CCamera member
    GameObject* m_pPlayerGameObject = nullptr; // Added player GameObject pointer

    // Collision System
    std::unique_ptr<CollisionManager> m_pCollisionManager;

    // Enemy System
    std::unique_ptr<EnemySpawner> m_pEnemySpawner;

    // Projectile System
    std::unique_ptr<ProjectileManager> m_pProjectileManager;

    // 통합 VFX 매니저 (player SSF + enemy 빌보드 두 슬롯 풀을 내부에서 소유)
    // 환경 파티클(Ember/Dust/Sandstorm)은 마이그레이션 과정에서 제거됨.
    std::unique_ptr<VFXManager> m_pVFXManager;

    // Screen-Space Fluid Renderer
    std::unique_ptr<ScreenSpaceFluid> m_pSSF;

    // Debug Renderer (F1 to toggle)
    std::unique_ptr<DebugRenderer> m_pDebugRenderer;

    // Torch System (flickering lights and flame billboards)
    std::unique_ptr<TorchSystem> m_pTorchSystem;

    // Ground Decal Manager (회전 마법진, 스코치 마크 등)
    std::unique_ptr<DecalManager> m_pDecalManager;

    // Decorative terrain (장식용, 충돌 없음)
    std::unique_ptr<Terrain> m_pTerrain;
    void LoadTerrain(const char* configJsonPath, int subdivisionStep = 4);

    void PrintHierarchy(GameObject* pGameObject, int nDepth);
    void CollectColliders(GameObject* pGameObject, std::vector<ColliderComponent*>& outColliders);
    void ProcessPendingDeletions();
    void UpdateRenderList();  // Update RenderComponent list for current frame
};