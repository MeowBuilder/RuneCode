#pragma once

#include "stdafx.h"
#include "FluidParticle.h"
#include "VFXTypes.h"
#include <vector>
#include <array>

class CDescriptorHeap;
class ScreenSpaceFluid;

// GPU 파티클 상태 (HLSL과 동일한 레이아웃, 64 bytes)
struct GPUParticle {
    XMFLOAT3 pos;          // 12
    float    density;      // 4
    XMFLOAT3 vel;          // 12
    float    nearDensity;  // 4  (이중 밀도 완화용)
    XMFLOAT3 force;        // 12
    float    mass;         // 4
    int      active;       // 4
    int      cpGroup;      // 4  담당 CP 인덱스 (-1=전체, 0=핵, 1+=위성)
    float    _pad[2];      // 8
};  // total 64 bytes
static_assert(sizeof(GPUParticle) == 64, "GPUParticle size mismatch");

// GPU 제어점 (32 bytes)
struct GPUControlPoint {
    XMFLOAT3 position;            // 12
    float    attractionStrength;  // 4
    float    sphereRadius;        // 4
    XMFLOAT3 pad;                 // 12
};  // total 32 bytes
static_assert(sizeof(GPUControlPoint) == 32, "GPUControlPoint size mismatch");

// SPH 상수 버퍼 (256 bytes 이내)
struct SPHConstants {
    int      particleCount; float h; float h2; float restDensity;     // 16
    float    stiffness; float viscosity; float dt; float damping;     // 16
    float    maxSpeed; int motionMode; float globalGravity; int boxActive; // 16
    float    pad0;                                                     // 4
    XMFLOAT3 gravityVec; float pad1;                                   // 16
    XMFLOAT3 boxCenter; float pad2;                                    // 16 (offset 68)
    float    _pad[3];                                                  // 12 (offset 84, float4를 r6=offset 96에 정렬)
    XMFLOAT4 boxAxisXH;                                                // 16 (offset 96)
    XMFLOAT4 boxAxisYH;                                                // 16 (offset 112)
    XMFLOAT4 boxAxisZH;                                                // 16 (offset 128)
    int      cpCount; XMFLOAT3 pad3;                                   // 16 (offset 144)
    XMFLOAT4 coreColor;                                                // 16 (offset 160)
    XMFLOAT4 edgeColor;                                                // 16 (offset 176)
    float    colorRestDensity; XMFLOAT3 pad4;                          // 16 (offset 192)
    XMFLOAT3 posOffset;        float    _padPO;                        // 16 (offset 208, GPU pos 델타 - OffsetParticles용)
    XMFLOAT3 velDelta;         float    _padVD;                        // 16 (offset 224, GPU vel 델타 - ApplyDirectionalForce용)
    // one-shot 페이즈 조작 필드 (CS_Integrate에서 적용, 다음 DispatchSPH에서 자동 0 초기화)
    int      opFlags;       uint32_t opFrameSeed; float _opR[2];       // 16 (offset 240)
    XMFLOAT3 opZeroAxis;    float    _opPadZ;                          // 16 (offset 256)
    XMFLOAT3 opSidewaysAxis; float   opSidewaysImpulse;               // 16 (offset 272)
    XMFLOAT3 opSpreadAxis;  float    opSpreadImpulse;                  // 16 (offset 288)
    XMFLOAT3 opSpreadOrigin; float   _opPadSO;                        // 16 (offset 304)
    XMFLOAT3 opBurstCenter; float    opBurstMinSpeed;                  // 16 (offset 320)
    float    opBurstMaxSpeed; float  _opPadB[3];                       // 16 (offset 336)
    // foam / velocity color 파라미터
    float    foamThreshold;   float foamStrength;                      //  8 (offset 352)
    float    velocityColorBoost; float _foamPad;                       //  8 (offset 360)
    // Near-pressure + SPH 커널 상수 (Sebastian Lague 이중 밀도 완화)
    float nearPressureMult; float kSpikyPow2; float kSpikyPow3; float kSpikyPow2Grad;  // 16 (offset 368)
    float kSpikyPow3Grad;   float elapsedTime; float explodeFade; float particleSizeMult; // 16 (offset 384) — particleSizeMult: dynSize 곱셈 배율 (1.0=기본)
    // Traveling wave oscillation (Q스킬 파도 수직 요동)
    float waveOscAmplitude; float waveOscFrequency; float waveOscWaveNumber; float waveOscEnabled; // 16 (offset 400)
    XMFLOAT3 waveOscFwdDir; float _woPad0;                                                          // 16 (offset 416)
    XMFLOAT3 waveOscUpDir;  float _woPad1;                                                          // 16 (offset 432)
};  // total 448 bytes
static_assert(sizeof(SPHConstants) <= 512, "SPHConstants exceeds 512 bytes");

class FluidParticleSystem
{
public:
    FluidParticleSystem();
    ~FluidParticleSystem();

    // D3D12 initialization - call once after Scene creates descriptor heap
    void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
              CDescriptorHeap* pDescriptorHeap, UINT nSrvDescriptorIndex);

    // Spawn particles around a center point (replaces current particles)
    void Spawn(const XMFLOAT3& center, const FluidParticleConfig& config);

    // Remove all particles
    void Clear();

    // Control points
    void SetControlPoints(const std::vector<FluidControlPoint>& cps);
    void AddControlPoint(const FluidControlPoint& cp);
    void ClearControlPoints();

    // Per-frame update (SPH simulation)
    void Update(float deltaTime);

    // GPU SPH dispatch (Render 전에 호출)
    void DispatchSPH(ID3D12GraphicsCommandList* pCmdList, float dt);

    // Render - call after main render pass
    void Render(ID3D12GraphicsCommandList* pCommandList,
                const XMFLOAT4X4& viewProj, const XMFLOAT3& cameraRight, const XMFLOAT3& cameraUp);

    // Screen-Space Fluid: 구체 깊이 렌더링 (SSF depth pass에서 호출)
    void RenderDepth(ID3D12GraphicsCommandList* pCmdList,
                     const XMFLOAT4X4& viewProjTransposed,
                     const XMFLOAT4X4& viewTransposed,
                     const XMFLOAT3& cameraRight,
                     const XMFLOAT3& cameraUp,
                     float projA, float projB,
                     ScreenSpaceFluid* pSSF);

    // Screen-Space Fluid: 두께 렌더링 (RenderDepth 이후 호출, 깊이 테스트 없음)
    // GPU 버퍼 상태는 RenderDepth에서 이미 설정됨
    void RenderThicknessOnly(ID3D12GraphicsCommandList* pCmdList,
                              ScreenSpaceFluid* pSSF);

    bool IsActive() const { return !m_Particles.empty(); }
    int  GetParticleCount() const { return static_cast<int>(m_Particles.size()); }

    // LightEmitterSystem이 동일한 빌보드 렌더 PSO를 재사용할 때 사용
    static void EnsureRenderPipeline(ID3D12Device* pDevice);
    static ID3D12RootSignature* SharedRootSig() { return s_pRootSignature.Get(); }
    static ID3D12PipelineState* SharedPSO()     { return s_pPSO.Get(); }

    // Shift all particle positions by delta (used to co-move with a projectile)
    void OffsetParticles(const XMFLOAT3& delta);

    // 운동 모드 설정
    void SetMotionMode(ParticleMotionMode mode);
    void SetConfinementBox(const ConfinementBoxDesc& box);
    void SetBeamDesc(const BeamDesc& beam);
    const BeamDesc& GetBeamDesc() const { return m_BeamDesc; }
    void SetGravityDesc(const GravityDesc& grav);
    void InitBeamParticles();  // Beam 모드 전용 초기화
    void ApplyRadialBurst(XMFLOAT3 center, float minSpeed, float maxSpeed);
    void ApplyDirectionalForce(const XMFLOAT3& direction, float impulse);
    void SetGlobalGravity(float strength);

    // Traveling wave 수직 진동 (Q스킬 파도용)
    // F_y(pos,t) = amplitude * sin(waveNumber * dot(pos,fwdDir) - frequency * t)
    void SetWaveOscillation(float amplitude, float frequency, float waveNumber,
                            const XMFLOAT3& fwdDir, const XMFLOAT3& upDir);
    void ClearWaveOscillation();

    // 특정 축 방향 속도 성분 제거 (예: forward 방향 속도만 0으로)
    void ZeroAxisVelocity(const XMFLOAT3& worldAxis);

    // 색상 직접 설정 (Spawn 이후 오버라이드용)
    void SetColors(const FluidElementColor& colors);

    // 폭발 페이드: 2.0=정상, 1.0..0.0=폭발 후 소멸 (크기 축소 + 선명 고정)
    void SetExplodeFade(float ratio) { m_explodeFade = ratio; }

    // 폭발 시 속도 상한 변경 (Spawn 이후 오버라이드용)
    void SetMaxParticleSpeed(float speed) { m_Config.maxParticleSpeed = speed; }

    // 양방향 분산 힘: 중심점(originPoint) 기준으로 각 파티클이 axisDir의 양쪽으로 밀림
    void ApplyAxisSpreadForce(const XMFLOAT3& axisDir, const XMFLOAT3& originPoint, float impulse);

    // 각 파티클에 worldAxis 방향 랜덤 속도 부여 (-maxImpulse ~ +maxImpulse)
    void ApplyRandomSidewaysImpulse(const XMFLOAT3& worldAxis, float maxImpulse);

private:
    // SPH phases (CPU fallback - 유지하되 GPU 모드에서는 미사용)
    void BuildSpatialHash();
    void ComputeDensityPressure();
    void ComputeForces();
    void Integrate(float dt);

    // Collect neighbors for particle at index i (writes into out, returns count)
    int  GetNeighbors(int i, int* out, int maxOut) const;

    // SPH kernel helpers (Clavet 2005 / Sebastian Lague dual-density relaxation)
    float    ViscLaplacian(float r, float h) const;

    // Hash helpers
    int  HashCell(int cx, int cy, int cz) const;
    void CellFromPos(const XMFLOAT3& p, int& cx, int& cy, int& cz) const;

    // Upload visible particles to GPU buffer and set m_nActiveCount
    void UploadRenderData();

    // Beam 모드: CPU 렌더 데이터를 GPU 렌더 버퍼로 복사
    void CopyBeamRenderDataToGPU(ID3D12GraphicsCommandList* pCmdList);

    // Simulation state
    std::vector<FluidParticle>     m_Particles;
    std::vector<FluidControlPoint> m_ControlPoints;
    FluidParticleConfig            m_Config;
    FluidElementColor              m_Colors;

    // 확장: 운동 모드 관련 멤버
    ParticleMotionMode             m_MotionMode = ParticleMotionMode::ControlPoint;
    ConfinementBoxDesc             m_ConfinementBox;
    BeamDesc                       m_BeamDesc;
    GravityDesc                    m_GravityDesc;
    float                          m_GlobalGravityStrength = 0.f;
    std::vector<FluidControlPoint> m_OrbitalCPs; // OrbitalCP 모드 위성 CP 포함

    // Spatial hash (power-of-2 table, direct-mapped cells)
    static constexpr int HASH_TABLE_SIZE      = 8192;
    static constexpr int MAX_PER_CELL         = 32;
    static constexpr int MAX_NEIGHBOR_SEARCH  = 27 * MAX_PER_CELL;
    static constexpr int MAX_PARTICLES        = 6144;

    struct SpatialHashCell
    {
        int particles[MAX_PER_CELL];
        int count = 0;
    };
    std::array<SpatialHashCell, HASH_TABLE_SIZE> m_HashTable;

    // D3D12 resources (기존 빌보드 렌더링용)
    ComPtr<ID3D12Resource>      m_pParticleBuffer;      // Upload heap, persistently mapped
    FluidParticleRenderData*    m_pMappedParticles  = nullptr;
    int                         m_nActiveCount      = 0;

    ComPtr<ID3D12Resource>      m_pPassCB;              // Internal pass constant buffer
    void*                       m_pMappedPassCB     = nullptr;

    ComPtr<ID3D12Resource>      m_pDepthPassCB;         // SSF depth pass constant buffer
    void*                       m_pMappedDepthPassCB = nullptr;

    CDescriptorHeap*            m_pDescriptorHeap       = nullptr;
    UINT                        m_nSrvDescriptorIndex   = 0;

    // D3D12 shared pipeline (static - compiled once for all instances)
    static ComPtr<ID3D12RootSignature> s_pRootSignature;
    static ComPtr<ID3D12PipelineState> s_pPSO;
    static void BuildSharedPipeline(ID3D12Device* pDevice);

    // ============================================================================
    // GPU SPH 전용 리소스 및 파이프라인 (static - 모든 인스턴스 공유)
    // ============================================================================
    static ComPtr<ID3D12RootSignature> s_pSPHRootSig;
    static ComPtr<ID3D12PipelineState> s_pDensityPSO;
    static ComPtr<ID3D12PipelineState> s_pForcesPSO;
    static ComPtr<ID3D12PipelineState> s_pIntegratePSO;
    static ComPtr<ID3D12PipelineState> s_pHashClearPSO;
    static ComPtr<ID3D12PipelineState> s_pHashBuildPSO;
    static void BuildSPHPipeline(ID3D12Device* pDevice);

    // GPU 버퍼
    ComPtr<ID3D12Resource> m_pGPUStateBuffer;    // DEFAULT, UAV - 파티클 상태 (GPUParticle)
    ComPtr<ID3D12Resource> m_pGPURenderBuffer;   // DEFAULT, UAV - 렌더 데이터 출력 (FluidParticleRenderData)
    ComPtr<ID3D12Resource> m_pInitUpload;        // UPLOAD - Spawn시 초기 상태 업로드
    ComPtr<ID3D12Resource> m_pSPHCB;             // UPLOAD - SPH 상수 버퍼
    BYTE*                  m_pMappedSPHCB = nullptr;
    ComPtr<ID3D12Resource> m_pCPBuffer;          // UPLOAD - Control Points
    GPUControlPoint*       m_pMappedCPBuffer = nullptr;
    static constexpr int   MAX_GPU_CPS = 16;

    // 공간 해싱 GPU 버퍼
    static constexpr int   GPU_HASH_TABLE_SIZE = 8192;  // power-of-2, 2^13
    static constexpr int   GPU_MAX_PER_CELL    = 32;    // 셀당 최대 파티클 수
    ComPtr<ID3D12Resource> m_pHashCountBuffer;  // DEFAULT, UAV - 해시 셀 카운트 [TABLE_SIZE]
    ComPtr<ID3D12Resource> m_pHashEntryBuffer;  // DEFAULT, UAV - 해시 셀 엔트리 [TABLE_SIZE * MAX_PER_CELL]

    bool m_bGPUInited   = false;
    bool m_bNeedsUpload = false;

    float    m_fElapsed = 0.f;              // 박동 펄스용 누적 시간
    float    m_explodeFade = 2.0f;          // 폭발 페이드: 2.0=정상, 1.0..0.0=폭발 소멸

    // Traveling wave 수직 진동
    float    m_waveOscAmplitude  = 0.f;
    float    m_waveOscFrequency  = 4.f;
    float    m_waveOscWaveNumber = 0.8f;
    bool     m_waveOscEnabled    = false;
    XMFLOAT3 m_waveOscFwdDir    = {0,0,1};
    XMFLOAT3 m_waveOscUpDir     = {0,1,0};

    // CPU → GPU 프레임 델타 (OffsetParticles / ApplyDirectionalForce 호출 누적)
    XMFLOAT3 m_vGPUPendingOffset   = {};
    XMFLOAT3 m_vGPUPendingVelDelta = {};

    // GPU one-shot 조작 누적값 (phase transition 함수들)
    uint32_t m_uOpFlags            = 0;
    uint32_t m_uOpFrameSeed        = 0;
    XMFLOAT3 m_vOpZeroAxis         = {};
    XMFLOAT3 m_vOpSidewaysAxis     = {};
    float    m_fOpSidewaysImpulse  = 0.f;
    XMFLOAT3 m_vOpSpreadAxis       = {};
    float    m_fOpSpreadImpulse    = 0.f;
    XMFLOAT3 m_vOpSpreadOrigin     = {};
    XMFLOAT3 m_vOpBurstCenter      = {};
    float    m_fOpBurstMinSpeed    = 0.f;
    float    m_fOpBurstMaxSpeed    = 0.f;

    // GPU 렌더 버퍼의 현재 상태 추적
    D3D12_RESOURCE_STATES m_eGPURenderBufferState = D3D12_RESOURCE_STATE_COMMON;

    // PRNG for spawn
    uint32_t m_Seed = 12345;
    float    Rand01();
    float    RandRange(float lo, float hi);
    XMFLOAT3 RandInSphere(float radius);
};
