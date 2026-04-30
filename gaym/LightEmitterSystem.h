#pragma once
#include "stdafx.h"
#include "VFXTypes.h"
#include "FluidParticle.h"   // FluidParticleRenderData

class CDescriptorHeap;

// GPU 상수 버퍼 (light_emitter.hlsl cbuffer LightEmitterCB 와 동일 레이아웃, 256 bytes)
struct alignas(256) LightEmitterConstants
{
    // [0-15]
    int      particleCount;
    int      emitterType;
    float    dt;
    float    elapsed;

    // [16-95] 벡터 (5 x float4)
    XMFLOAT3 origin;    float pad0;
    XMFLOAT3 direction; float pad1;
    XMFLOAT3 rightAxis; float pad2;
    XMFLOAT3 upAxis;    float pad3;
    XMFLOAT3 gravity;   float pad4;

    // [96-127] 색상
    XMFLOAT4 coreColor;
    XMFLOAT4 edgeColor;

    // [128-143] 공통 스칼라
    float    sizeBase;
    float    sizeScale;
    uint32_t frameSeed;
    int      spawnCount;

    // [144-159] 공용 이미터 파라미터
    float    speedMin;
    float    speedMax;
    float    lifetimeMin;
    float    lifetimeMax;

    // [160-175] Linear
    float    linearLength;
    float    linearWidth;
    float    linearRecycleRate;
    float    linearSwirlSpeed;

    // [176-191] Cone
    float    coneHalfAngleDeg;
    float    coneGravityScale;
    float    coneStartSizeMult;
    float    coneEndSizeMult;

    // [192-207] Sphere
    float    sphereRadius;
    float    sphereShellFraction;
    int      sphereInward;
    float    sphereRotationSpeed;

    // [208-239] Ring (32 bytes)
    float    ringRadius;
    float    ringWidth;
    float    ringExpandSpeed;
    float    ringTiltX;
    float    ringRotateSpeed;
    float    ringNormalSpeedMin;
    float    ringNormalSpeedMax;
    float    _rpad;

    // [240-255] Burst
    float    burstBounceCoeff;
    float    burstGroundY;
    float    _bpad0;
    float    _bpad1;
};
static_assert(sizeof(LightEmitterConstants) == 256, "LightEmitterConstants must be 256 bytes");

// 경량 파티클 이미터
// - 5종 기본 이미터 (Linear/Cone/Sphere/Ring/Burst) 를 컴퓨트 셰이더로 처리
// - 렌더는 FluidParticleSystem 과 동일한 빌보드 PSO 재사용
class LightEmitterSystem
{
public:
    static constexpr int MAX_PARTICLES = 2048;

    LightEmitterSystem();
    ~LightEmitterSystem();

    void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
              CDescriptorHeap* pDescriptorHeap, UINT nSrvDescIndex);

    void Spawn(const XMFLOAT3& origin, const XMFLOAT3& direction, const EffectLayer& layer);
    void Clear();

    // 위치 업데이트 (attachToProjectile)
    void SetOrigin(const XMFLOAT3& origin, const XMFLOAT3& direction);

    // 매 프레임: Update → Dispatch → Render 순서로 호출
    void Dispatch(ID3D12GraphicsCommandList* pCmdList, float dt);
    void Render(ID3D12GraphicsCommandList* pCmdList,
                const XMFLOAT4X4& viewProj,
                const XMFLOAT3& camRight, const XMFLOAT3& camUp);

    bool IsActive() const  { return m_bActive; }
    bool IsInited() const  { return m_bInited; }
    int  GetParticleCount() const { return m_ParticleCount; }

    // 파이프라인만 사전 컴파일 (스폰 전 스터터 방지용, FluidSkillVFXManager::Init에서 호출)
    static void EnsurePipelines(ID3D12Device* pDevice) { BuildPipelines(pDevice); }

private:
    void FillConstants(LightEmitterConstants& cb, float dt, bool isSpawn) const;
    void UploadRenderData(ID3D12GraphicsCommandList* pCmdList);

    // GPU 버퍼
    ComPtr<ID3D12Resource>   m_pStateBuffer;    // DEFAULT UAV  - LightParticle[]
    ComPtr<ID3D12Resource>   m_pRenderBuffer;   // DEFAULT UAV  - FluidParticleRenderData[]
    ComPtr<ID3D12Resource>   m_pReadbackBuffer; // READBACK     - CPU 읽기
    ComPtr<ID3D12Resource>   m_pUploadSRV;      // UPLOAD SRV   - 렌더 입력
    FluidParticleRenderData* m_pMappedUpload = nullptr;

    ComPtr<ID3D12Resource>   m_pCB;             // UPLOAD CB
    LightEmitterConstants*   m_pMappedCB = nullptr;

    ComPtr<ID3D12Resource>   m_pPassCB;         // UPLOAD PassCB (viewProj/camVecs)
    void*                    m_pMappedPassCB = nullptr;

    CDescriptorHeap* m_pDescriptorHeap   = nullptr;
    UINT             m_nSrvDescIndex     = 0;

    // 공유 컴퓨트 파이프라인 (모든 인스턴스 공유)
    static ComPtr<ID3D12RootSignature> s_pComputeRootSig;
    static ComPtr<ID3D12PipelineState> s_pEmitPSO;
    static ComPtr<ID3D12PipelineState> s_pUpdatePSO;
    static ComPtr<ID3D12PipelineState> s_pToRenderPSO;
    static bool s_bPipelinesBuilt;
    static void BuildPipelines(ID3D12Device* pDevice);

    EffectLayer m_Layer;
    XMFLOAT3    m_Origin    = {};
    XMFLOAT3    m_Direction = { 0, 0, 1 };
    float       m_Elapsed   = 0.f;
    int         m_ParticleCount = 0;
    bool        m_bActive   = false;
    bool        m_bInited   = false;
    bool        m_bNeedsSpawn = false;

    uint32_t    m_FrameSeed = 0;

    D3D12_RESOURCE_STATES m_eStateBufState  = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_eRenderBufState = D3D12_RESOURCE_STATE_COMMON;
};
