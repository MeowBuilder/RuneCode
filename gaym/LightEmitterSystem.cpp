#include "stdafx.h"
#include "LightEmitterSystem.h"
#include "FluidParticleSystem.h"
#include "DescriptorHeap.h"
#include <cstring>

// ── 렌더 패스 CB (FluidParticleSystem 빌보드 셰이더와 동일 레이아웃) ──────────
struct alignas(256) LightPassCB
{
    XMFLOAT4X4 viewProj;        // 64
    XMFLOAT3   camRight; float _p0; // 16
    XMFLOAT3   camUp;    float _p1; // 16
    float      _pad[40];            // 160  → total 256
};
static_assert(sizeof(LightPassCB) == 256, "LightPassCB must be 256 bytes");

// ── 임베디드 컴퓨트 셰이더 (light_emitter.hlsl 동일 내용) ────────────────────
static const char* g_LightEmitterCS = R"HLSL(
struct LightParticle {
    float3 pos; float life;
    float3 vel; float maxLife;
    float4 startColor;
    float  size; float startSize; uint seed; float pad;
};
struct FluidParticleRenderData { float3 position; float size; float4 color; };

cbuffer LightEmitterCB : register(b0) {
    int    particleCount; int emitterType; float dt; float elapsed;
    float3 origin; float pad0; float3 direction; float pad1;
    float3 rightAxis; float pad2; float3 upAxis; float pad3;
    float3 gravity; float pad4;
    float4 coreColor; float4 edgeColor;
    float  sizeBase; float sizeScale; uint frameSeed; int spawnCount;
    float  speedMin; float speedMax; float lifetimeMin; float lifetimeMax;
    float  linearLength; float linearWidth; float linearRecycleRate; float linearSwirlSpeed;
    float  coneHalfAngleDeg; float coneGravityScale; float coneStartSizeMult; float coneEndSizeMult;
    float  sphereRadius; float sphereShellFraction; int sphereInward; float sphereRotationSpeed;
    float  ringRadius; float ringWidth; float ringExpandSpeed; float ringTiltX;
    float  ringRotateSpeed; float ringNormalSpeedMin; float ringNormalSpeedMax; float _rpad;
    float  burstBounceCoeff; float burstGroundY; float coneSpawnRadius; float _bpad1;
    // Crescent (emitterType==5)
    float  crescentRadius; float crescentThickness; float crescentArcAngle; float crescentArcOffset;
    float  crescentTiltX; float crescentNSpeedMin; float crescentNSpeedMax; float crescentRotateSpeed;
    // 카메라 벡터 (Crescent 스크린-페이싱 스폰)
    float3 camRight3; float _crpad;
    float3 camUp3;    float _cupad;
    float4 _extPad[12];
};

RWStructuredBuffer<LightParticle>           gParticles : register(u0);
RWStructuredBuffer<FluidParticleRenderData> gRenderBuf : register(u1);

uint WangHash(uint s) {
    s = (s^61u)^(s>>16u); s*=9u; s^=s>>4u; s*=0x27d4eb2du; s^=s>>15u; return s;
}
float Rand01(inout uint s) { s=WangHash(s); return float(s&0xFFFFFFu)*(1.0f/16777216.0f); }

float3 RandomInCone(float halfDeg, float3 fwd, float3 right, float3 up, inout uint seed) {
    float halfRad=halfDeg*0.01745329f, cosMax=cos(halfRad);
    float r1=Rand01(seed), r2=Rand01(seed);
    float cosA=1.0f-r1*(1.0f-cosMax), sinA=sqrt(max(0.0f,1.0f-cosA*cosA)), phi=r2*6.28318f;
    return normalize(fwd*cosA+right*sinA*cos(phi)+up*sinA*sin(phi));
}
float3 RandomOnSphere(inout uint seed) {
    float r1=Rand01(seed),r2=Rand01(seed);
    float phi=acos(clamp(1.0f-2.0f*r1,-1.0f,1.0f)),th=r2*6.28318f;
    return float3(sin(phi)*cos(th),cos(phi),sin(phi)*sin(th));
}

[numthreads(64,1,1)]
void CS_LightEmit(uint3 DTid:SV_DispatchThreadID) {
    uint idx=DTid.x; if((int)idx>=particleCount)return;
    uint seed=WangHash(frameSeed^(idx*2654435761u));
    float r0=Rand01(seed),r1=Rand01(seed),r2=Rand01(seed),r3=Rand01(seed);
    float speed=lerp(speedMin,speedMax,r2), lifetime=lerp(lifetimeMin,lifetimeMax,r3);
    LightParticle p; p.seed=seed; p.pad=0;
    p.maxLife=lifetime; p.life=lifetime;
    p.startColor=lerp(edgeColor,coreColor,r0);

    if(emitterType==0){ // Linear
        float t=r0,ang=r1*6.28318f;
        // Inverted funnel — base 35% 좁음, top 100% — 전형적 토네이도 깔때기
        float taperT=0.35f+0.65f*t;
        float rad=sqrt(Rand01(seed))*linearWidth*taperT;
        p.pos=origin+direction*t*linearLength+rightAxis*rad*cos(ang)+upAxis*rad*sin(ang);
        p.vel=direction*speed; p.life=99.f; p.maxLife=99.f;
        p.size=sizeBase*sizeScale; p.startSize=p.size;
    } else if(emitterType==1){ // Cone
        float3 d=RandomInCone(coneHalfAngleDeg,direction,rightAxis,upAxis,seed);
        float3 spawnOff=float3(0,0,0);
        if(coneSpawnRadius>0.001f){
            float3 rs=RandomOnSphere(seed);
            float rf=pow(Rand01(seed),0.333f); // 구체 볼륨에 균일 분포
            spawnOff=rs*rf*coneSpawnRadius;
        }
        p.pos=origin+spawnOff; p.vel=d*speed;
        p.size=sizeBase*sizeScale*coneStartSizeMult; p.startSize=p.size;
    } else if(emitterType==2){ // Sphere
        float3 sdir=RandomOnSphere(seed);
        float rMin=sphereRadius*(1.0f-sphereShellFraction), rad=lerp(rMin,sphereRadius,sqrt(r2));
        p.pos=origin+sdir*rad;
        p.vel=((sphereInward!=0)?-sdir:sdir)*speed;
        p.size=sizeBase*sizeScale; p.startSize=p.size;
    } else if(emitterType==3){ // Ring
        float ang=r0*6.28318f, roff=(r1-0.5f)*ringWidth;
        float cosT=cos(ringTiltX),sinT=sin(ringTiltX);
        float3 radDir=rightAxis*cos(ang)+(upAxis*cosT-direction*sinT)*sin(ang);
        p.pos=origin+radDir*(ringRadius+roff);
        float3 ndir=direction*cosT+upAxis*sinT;
        p.vel=ndir*lerp(ringNormalSpeedMin,ringNormalSpeedMax,r2);
        p.size=sizeBase*sizeScale; p.startSize=p.size;
    } else if(emitterType==5){ // Crescent — 진행 방향 기준 카메라 평면 스폰
        float arcStartRad=crescentArcOffset*0.017453293f;
        float arcRangeRad=crescentArcAngle*0.017453293f;
        float ang=arcStartRad+r0*arcRangeRad;
        float roff=(r1-0.5f)*crescentThickness;
        // 진행 방향을 카메라 평면에 투영 → 아크 X축 (볼록 쪽이 항상 진행 방향을 향함)
        float3 camFwd=normalize(cross(camRight3,camUp3));
        float3 proj=direction-dot(direction,camFwd)*camFwd;
        float  plen=length(proj);
        float3 arcX=(plen>0.001f)?proj/plen:camRight3;
        float3 arcY=cross(camFwd,arcX);
        float3 radDir=arcX*cos(ang)+arcY*sin(ang);
        // 앞뒤 두께: direction 축으로 랜덤 분산 (r0와 독립된 seed)
        uint ds=WangHash(seed^0x1234ABCDu); float depthR=Rand01(ds);
        p.pos=origin+radDir*(crescentRadius+roff)+direction*(depthR-0.5f)*crescentThickness*2.5f;
        p.vel=direction*lerp(crescentNSpeedMin,crescentNSpeedMax,r2);
        p.size=sizeBase*sizeScale; p.startSize=p.size;
        // 색상: r0(아크 위치)와 독립된 seed로 균일하게 분포
        uint cs=WangHash(seed^0x9E3779B9u);
        p.startColor=lerp(edgeColor,coreColor,Rand01(cs));
    } else { // Burst
        p.pos=origin; p.vel=RandomOnSphere(seed)*speed;
        p.size=sizeBase*sizeScale; p.startSize=p.size;
    }
    gParticles[idx]=p;
}

[numthreads(64,1,1)]
void CS_LightUpdate(uint3 DTid:SV_DispatchThreadID) {
    uint idx=DTid.x; if((int)idx>=particleCount)return;
    LightParticle p=gParticles[idx];
    if(p.life<0.f){
        // Cone: 죽은 파티클을 현재 origin에서 재스폰 → 연속 방출 꼬리 효과
        if(emitterType==1){
            uint s2=WangHash(p.seed^asuint(elapsed)^(idx*2654435761u));
            float3 d=RandomInCone(coneHalfAngleDeg,direction,rightAxis,upAxis,s2);
            float sp=lerp(speedMin,speedMax,Rand01(s2));
            float lt=lerp(lifetimeMin,lifetimeMax,Rand01(s2));
            float3 spawnOff=float3(0,0,0);
            if(coneSpawnRadius>0.001f){
                float3 rs=RandomOnSphere(s2);
                float rf=pow(Rand01(s2),0.333f);
                spawnOff=rs*rf*coneSpawnRadius;
            }
            p.pos=origin+spawnOff; p.vel=d*sp;
            p.life=lt; p.maxLife=lt;
            p.startColor=lerp(edgeColor,coreColor,Rand01(s2));
            p.size=sizeBase*sizeScale*coneStartSizeMult; p.startSize=p.size;
            p.seed=s2;
        }
        gParticles[idx]=p; return;
    }
    p.vel+=gravity*dt;
    if(emitterType==0){
        float proj=dot(p.pos-origin,direction);
        if(proj<0.f||proj>linearLength){
            uint s2=WangHash(p.seed^asuint(elapsed));
            float t=Rand01(s2),ang=Rand01(s2)*6.28318f;
            float taperT=0.35f+0.65f*t;   // base 좁음 → top 넓음
            float rad=sqrt(Rand01(s2))*linearWidth*taperT;
            p.pos=origin+direction*t*linearLength+rightAxis*rad*cos(ang)+upAxis*rad*sin(ang);
        }
        // Swirl + 입자별/높이별 속도 변동성으로 정직한 원통 느낌 깨기
        if(linearSwirlSpeed!=0.f){
            float3 rel=p.pos-origin;
            float fwdComp=dot(rel,direction);
            float3 radial=rel-direction*fwdComp;
            float curR=length(radial);
            if(curR>0.001f){
                // 입자별 ±30% (seed 기반, 결정적)
                float seedF=float(p.seed&0xFFFFu)*(1.0f/65535.0f);
                float perPart=0.7f+0.6f*seedF;                    // 0.7~1.3
                // 높이별: 위로 갈수록 더 빠르게 (twist)
                float heightT=saturate(fwdComp/max(linearLength,0.0001f));
                float heightMul=0.7f+1.0f*heightT;                // 0.7~1.7
                float swA=linearSwirlSpeed*dt*perPart*heightMul;
                float curCos=dot(radial,rightAxis)/curR;
                float curSin=dot(radial,upAxis)/curR;
                float ca=cos(swA),sa=sin(swA);
                float newCos=curCos*ca-curSin*sa;
                float newSin=curSin*ca+curCos*sa;
                p.pos=origin+direction*fwdComp+(rightAxis*newCos+upAxis*newSin)*curR;
            }
        }
        // Turbulence — 매 프레임 작은 랜덤 워크 (난기류 흉내)
        {
            uint ts=WangHash(p.seed^asuint(elapsed*61.0f)^idx);
            float jx=(Rand01(ts)-0.5f)*0.06f;
            float jy=(Rand01(ts)-0.5f)*0.04f;
            float jz=(Rand01(ts)-0.5f)*0.06f;
            p.pos+=float3(jx,jy,jz);
        }
    } else if(emitterType==3&&ringExpandSpeed>0.f){
        float3 toP=p.pos-origin;
        float3 ndir=direction*cos(ringTiltX)+upAxis*sin(ringTiltX);
        toP-=ndir*dot(toP,ndir);
        float dist=length(toP);
        if(dist>0.001f) p.pos+=normalize(toP)*ringExpandSpeed*dt;
    } else if(emitterType==4&&p.pos.y<=burstGroundY&&p.vel.y<0.f){
        p.vel.y*=-burstBounceCoeff;
    }
    p.pos+=p.vel*dt; p.life-=dt;
    gParticles[idx]=p;
}

[numthreads(64,1,1)]
void CS_LightToRender(uint3 DTid:SV_DispatchThreadID) {
    uint idx=DTid.x; if((int)idx>=particleCount)return;
    LightParticle p=gParticles[idx];
    FluidParticleRenderData rd; rd.position=p.pos;
    if(p.life<0.f){rd.size=0.f;rd.color=float4(0,0,0,0);}
    else{
        float t=(p.maxLife>1.f)?0.f:saturate(1.f-p.life/max(p.maxLife,0.0001f));
        rd.color=lerp(p.startColor,edgeColor,t);
        float szMult=1.f;
        if(emitterType==1){rd.color.a*=(1.f-t);szMult=lerp(coneStartSizeMult,coneEndSizeMult,t);}
        else if(emitterType==4){rd.color.a*=(1.f-t);szMult=lerp(1.f,0.2f,t);}
        else if(emitterType==5){rd.color.a*=(1.f-t*t);szMult=lerp(1.f,0.3f,t);}
        rd.size=p.startSize*szMult;
    }
    gRenderBuf[idx]=rd;
}
)HLSL";

// ── static 멤버 ───────────────────────────────────────────────────────────────
ComPtr<ID3D12RootSignature> LightEmitterSystem::s_pComputeRootSig;
ComPtr<ID3D12PipelineState> LightEmitterSystem::s_pEmitPSO;
ComPtr<ID3D12PipelineState> LightEmitterSystem::s_pUpdatePSO;
ComPtr<ID3D12PipelineState> LightEmitterSystem::s_pToRenderPSO;
bool LightEmitterSystem::s_bPipelinesBuilt = false;

// ── 파이프라인 빌드 ───────────────────────────────────────────────────────────
void LightEmitterSystem::BuildPipelines(ID3D12Device* pDevice)
{
    if (s_bPipelinesBuilt) return;

    HRESULT hr;
    ComPtr<ID3DBlob> errBlob;

    // Compute root sig: [0] CBV b0, [1] UAV u0 (particles), [2] UAV u1 (render)
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].Descriptor.ShaderRegister = 0;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].Descriptor.ShaderRegister = 1;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters   = rp;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob;
    hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr)) { if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer()); return; }

    hr = pDevice->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                       IID_PPV_ARGS(&s_pComputeRootSig));
    if (FAILED(hr)) { OutputDebugStringA("[LightEmitter] Compute root sig 생성 실패\n"); return; }

    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto CompileCS = [&](const char* entry, ComPtr<ID3D12PipelineState>& outPSO) -> bool
    {
        ComPtr<ID3DBlob> csBlob;
        errBlob.Reset();
        hr = D3DCompile(g_LightEmitterCS, strlen(g_LightEmitterCS), "LightEmitter",
                        nullptr, nullptr, entry, "cs_5_1", compileFlags, 0, &csBlob, &errBlob);
        if (FAILED(hr))
        {
            if (errBlob) OutputDebugStringA((char*)errBlob->GetBufferPointer());
            char msg[128]; sprintf_s(msg, "[LightEmitter] CS %s 컴파일 실패\n", entry);
            OutputDebugStringA(msg); return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = s_pComputeRootSig.Get();
        pd.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
        hr = pDevice->CreateComputePipelineState(&pd, IID_PPV_ARGS(&outPSO));
        if (FAILED(hr)) { OutputDebugStringA("[LightEmitter] PSO 생성 실패\n"); return false; }
        return true;
    };

    if (!CompileCS("CS_LightEmit",     s_pEmitPSO))     return;
    if (!CompileCS("CS_LightUpdate",   s_pUpdatePSO))   return;
    if (!CompileCS("CS_LightToRender", s_pToRenderPSO)) return;

    s_bPipelinesBuilt = true;
    OutputDebugStringA("[LightEmitter] 파이프라인 빌드 완료\n");
}

// ── 생성자/소멸자 ──────────────────────────────────────────────────────────────
LightEmitterSystem::LightEmitterSystem() = default;
LightEmitterSystem::~LightEmitterSystem()
{
    if (m_pMappedCB   && m_pCB)     m_pCB->Unmap(0, nullptr);
    if (m_pMappedPassCB && m_pPassCB) m_pPassCB->Unmap(0, nullptr);
}

// ── Init ──────────────────────────────────────────────────────────────────────
void LightEmitterSystem::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
                               CDescriptorHeap* pDescriptorHeap, UINT nSrvDescIndex)
{
    m_pDescriptorHeap = pDescriptorHeap;
    m_nSrvDescIndex   = nSrvDescIndex;

    BuildPipelines(pDevice);
    FluidParticleSystem::EnsureRenderPipeline(pDevice);

    HRESULT hr;
    const UINT64 stateBufSize  = 64ULL * MAX_PARTICLES; // LightParticle is 64 bytes (matches HLSL)
    const UINT64 renderBufSize = sizeof(FluidParticleRenderData) * MAX_PARTICLES;

    // ── 파티클 상태 버퍼 (DEFAULT, UAV) ──────────────────────────────────────
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = stateBufSize;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pStateBuffer));
        if (FAILED(hr)) { OutputDebugStringA("[LightEmitter] State buffer 생성 실패\n"); return; }
    }

    // ── 렌더 데이터 버퍼 (DEFAULT, UAV + SRV descriptor) ────────────────────
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = renderBufSize;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pRenderBuffer));
        if (FAILED(hr)) { OutputDebugStringA("[LightEmitter] Render buffer 생성 실패\n"); return; }
        m_eRenderBufState = D3D12_RESOURCE_STATE_COMMON;

        // SRV 등록 (렌더 파이프라인이 descriptor table로 접근)
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                     = DXGI_FORMAT_UNKNOWN;
        srvDesc.Buffer.NumElements         = MAX_PARTICLES;
        srvDesc.Buffer.StructureByteStride = sizeof(FluidParticleRenderData);
        srvDesc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

        pDevice->CreateShaderResourceView(m_pRenderBuffer.Get(), &srvDesc,
            pDescriptorHeap->GetCPUHandle(nSrvDescIndex));
    }

    // ── 상수 버퍼 (UPLOAD, persistently mapped) ───────────────────────────────
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width     = sizeof(LightEmitterConstants);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pCB));
        if (FAILED(hr)) { OutputDebugStringA("[LightEmitter] CB 생성 실패\n"); return; }
        m_pCB->Map(0, nullptr, reinterpret_cast<void**>(&m_pMappedCB));
    }

    // ── 패스 CB (렌더 viewProj/camRight/camUp, 256-byte aligned) ─────────────
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width     = sizeof(LightPassCB);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = pDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pPassCB));
        if (FAILED(hr)) { OutputDebugStringA("[LightEmitter] PassCB 생성 실패\n"); return; }
        m_pPassCB->Map(0, nullptr, reinterpret_cast<void**>(&m_pMappedPassCB));
    }

    m_bInited = true;
    OutputDebugStringA("[LightEmitter] Init 완료\n");
}

// ── Spawn ─────────────────────────────────────────────────────────────────────
void LightEmitterSystem::Spawn(const XMFLOAT3& origin, const XMFLOAT3& direction,
                                const EffectLayer& layer)
{
    m_Layer         = layer;
    m_Origin        = origin;
    m_Direction     = direction;
    m_Elapsed       = 0.f;
    m_ParticleCount = min(layer.particleCount, MAX_PARTICLES);
    m_bActive       = true;
    m_bNeedsSpawn   = true;
    m_FrameSeed     = static_cast<uint32_t>(GetTickCount64());
}

void LightEmitterSystem::Clear()
{
    m_bActive     = false;
    m_bNeedsSpawn = false;
}

void LightEmitterSystem::SetOrigin(const XMFLOAT3& origin, const XMFLOAT3& direction)
{
    m_Origin    = origin;
    m_Direction = direction;
}

// ── 상수 버퍼 채우기 ──────────────────────────────────────────────────────────
void LightEmitterSystem::FillConstants(LightEmitterConstants& cb, float dt, bool isSpawn) const
{
    cb.particleCount = m_ParticleCount;
    cb.dt            = dt;
    cb.elapsed       = m_Elapsed;
    cb.frameSeed     = m_FrameSeed + (isSpawn ? 0u : 1u);

    // emitterType
    switch (m_Layer.type) {
    case EmitterType::Linear:   cb.emitterType = 0; break;
    case EmitterType::Cone:     cb.emitterType = 1; break;
    case EmitterType::Sphere:   cb.emitterType = 2; break;
    case EmitterType::Ring:     cb.emitterType = 3; break;
    case EmitterType::Crescent: cb.emitterType = 5; break;
    default:                    cb.emitterType = 4; break; // Burst
    }

    cb.origin    = m_Origin;    cb.pad0 = 0;
    cb.direction = m_Direction; cb.pad1 = 0;

    // 직교 축 계산
    XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&m_Direction));
    XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    float dotUp = XMVectorGetX(XMVector3Dot(fwd, worldUp));
    XMVECTOR right = (fabsf(dotUp) > 0.99f)
        ? XMVectorSet(1, 0, 0, 0)
        : XMVector3Normalize(XMVector3Cross(worldUp, fwd));
    XMVECTOR up = XMVector3Cross(fwd, right);
    XMStoreFloat3(&cb.rightAxis, right); cb.pad2 = 0;
    XMStoreFloat3(&cb.upAxis,    up);    cb.pad3 = 0;

    // 중력
    float gravScale = 0.f;
    if (m_Layer.type == EmitterType::Cone)  gravScale = m_Layer.cone.gravityScale;
    if (m_Layer.type == EmitterType::Burst) gravScale = 1.f;
    cb.gravity = { 0.f, -9.8f * gravScale, 0.f }; cb.pad4 = 0;

    cb.coreColor = m_Layer.coreColor;
    cb.edgeColor = m_Layer.edgeColor;
    cb.sizeBase  = 0.35f;
    cb.sizeScale = m_Layer.sizeScale;
    cb.spawnCount = 0;

    cb.speedMin    = m_Layer.speedMin;
    cb.speedMax    = m_Layer.speedMax;
    cb.lifetimeMin = m_Layer.lifetimeMin;
    cb.lifetimeMax = m_Layer.lifetimeMax;

    // Linear
    cb.linearLength      = m_Layer.linear.length;
    cb.linearWidth       = m_Layer.linear.width;
    cb.linearRecycleRate = m_Layer.linear.recycleRate;
    cb.linearSwirlSpeed  = m_Layer.linear.swirlSpeed;

    // Cone
    cb.coneHalfAngleDeg  = m_Layer.cone.halfAngle;
    cb.coneGravityScale  = m_Layer.cone.gravityScale;
    cb.coneStartSizeMult = m_Layer.cone.startSizeMult;
    cb.coneEndSizeMult   = m_Layer.cone.endSizeMult;

    // Sphere
    cb.sphereRadius        = m_Layer.sphere.radius;
    cb.sphereShellFraction = m_Layer.sphere.shellFraction;
    cb.sphereInward        = m_Layer.sphere.inward ? 1 : 0;
    cb.sphereRotationSpeed = m_Layer.sphere.rotationSpeed;

    // Ring
    cb.ringRadius        = m_Layer.ring.radius;
    cb.ringWidth         = m_Layer.ring.width;
    cb.ringExpandSpeed   = m_Layer.ring.expandSpeed;
    cb.ringTiltX         = m_Layer.ring.tiltX;
    cb.ringRotateSpeed   = m_Layer.ring.rotateSpeed;
    cb.ringNormalSpeedMin = m_Layer.ring.normalSpeedMin;
    cb.ringNormalSpeedMax = m_Layer.ring.normalSpeedMax;
    cb._rpad             = 0;

    // Burst
    cb.burstBounceCoeff  = m_Layer.burst.bounceCoeff;
    cb.burstGroundY      = m_Layer.burst.groundY;
    cb.coneSpawnRadius   = m_Layer.cone.spawnRadius;
    cb._bpad1            = 0;

    // Crescent
    cb.crescentRadius      = m_Layer.crescent.radius;
    cb.crescentThickness   = m_Layer.crescent.thickness;
    cb.crescentArcAngle    = m_Layer.crescent.arcAngle;
    cb.crescentArcOffset   = m_Layer.crescent.arcOffset;
    cb.crescentTiltX       = m_Layer.crescent.tiltX;  // 현재 미사용 (카메라 평면 모드)
    cb.crescentNSpeedMin   = m_Layer.crescent.normalSpeedMin;
    cb.crescentNSpeedMax   = m_Layer.crescent.normalSpeedMax;
    cb.crescentRotateSpeed = m_Layer.crescent.rotateSpeed;

    // 카메라 벡터 (Crescent 스크린-페이싱 스폰에 사용)
    cb.camRight3 = m_LastCamRight; cb._crpad = 0;
    cb.camUp3    = m_LastCamUp;    cb._cupad = 0;
}

// ── 배리어 헬퍼 ───────────────────────────────────────────────────────────────
static void TransitionBuffer(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res,
                              D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &b);
}

static void UAVBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    cmdList->ResourceBarrier(1, &b);
}

// ── Dispatch (매 프레임) ──────────────────────────────────────────────────────
void LightEmitterSystem::Dispatch(ID3D12GraphicsCommandList* pCmdList, float dt)
{
    if (!m_bActive || !m_bInited || m_ParticleCount == 0) return;
    if (!s_pComputeRootSig || !s_pEmitPSO) return;

    m_Elapsed  += dt;
    m_FrameSeed = static_cast<uint32_t>(m_Elapsed * 10000.f);

    // 렌더 버퍼 → UAV (컴퓨트 쓰기 전)
    TransitionBuffer(pCmdList, m_pRenderBuffer.Get(),
        m_eRenderBufState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_eRenderBufState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // 상태 버퍼 → UAV (첫 프레임 또는 상태가 다를 때)
    if (m_eStateBufState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        TransitionBuffer(pCmdList, m_pStateBuffer.Get(),
            m_eStateBufState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_eStateBufState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // 루트 시그니처/PSO 바인딩
    pCmdList->SetComputeRootSignature(s_pComputeRootSig.Get());
    pCmdList->SetComputeRootConstantBufferView(0, m_pCB->GetGPUVirtualAddress());
    pCmdList->SetComputeRootUnorderedAccessView(1, m_pStateBuffer->GetGPUVirtualAddress());
    pCmdList->SetComputeRootUnorderedAccessView(2, m_pRenderBuffer->GetGPUVirtualAddress());

    UINT groups = (m_ParticleCount + 63) / 64;

    // ① Emit (스폰 시 한 번)
    if (m_bNeedsSpawn)
    {
        LightEmitterConstants cb; FillConstants(cb, 0.f, true);
        memcpy(m_pMappedCB, &cb, sizeof(cb));

        pCmdList->SetPipelineState(s_pEmitPSO.Get());
        pCmdList->Dispatch(groups, 1, 1);
        UAVBarrier(pCmdList, m_pStateBuffer.Get());
        m_bNeedsSpawn = false;
    }

    // ② Update
    {
        LightEmitterConstants cb; FillConstants(cb, dt, false);
        memcpy(m_pMappedCB, &cb, sizeof(cb));

        pCmdList->SetComputeRootConstantBufferView(0, m_pCB->GetGPUVirtualAddress());
        pCmdList->SetPipelineState(s_pUpdatePSO.Get());
        pCmdList->Dispatch(groups, 1, 1);
        UAVBarrier(pCmdList, m_pStateBuffer.Get());
    }

    // ③ ToRender
    {
        pCmdList->SetPipelineState(s_pToRenderPSO.Get());
        pCmdList->Dispatch(groups, 1, 1);
        UAVBarrier(pCmdList, m_pRenderBuffer.Get());
    }

    // 렌더 버퍼 → SRV (VS에서 읽기)
    TransitionBuffer(pCmdList, m_pRenderBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_eRenderBufState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

// ── Render (매 프레임, Dispatch 이후) ─────────────────────────────────────────
void LightEmitterSystem::Render(ID3D12GraphicsCommandList* pCmdList,
                                 const XMFLOAT4X4& viewProj,
                                 const XMFLOAT3& camRight, const XMFLOAT3& camUp)
{
    static bool s_bLoggedOnce = false;
    if (!m_bActive || !m_bInited || m_ParticleCount == 0)
    {
        if (!s_bLoggedOnce) {
            char msg[128];
            sprintf_s(msg, "[LightEmitter::Render] SKIP: active=%d inited=%d count=%d\n",
                      m_bActive, m_bInited, m_ParticleCount);
            OutputDebugStringA(msg);
            s_bLoggedOnce = true;
        }
        return;
    }
    if (!FluidParticleSystem::SharedRootSig() || !FluidParticleSystem::SharedPSO())
    {
        static bool s_bLoggedPSO = false;
        if (!s_bLoggedPSO) {
            OutputDebugStringA("[LightEmitter::Render] SKIP: SharedRootSig or SharedPSO is NULL!\n");
            s_bLoggedPSO = true;
        }
        return;
    }
    if (m_eRenderBufState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        static bool s_bLoggedState = false;
        if (!s_bLoggedState) {
            char msg[128];
            sprintf_s(msg, "[LightEmitter::Render] SKIP: renderBufState=%d (expected %d)\n",
                      (int)m_eRenderBufState,
                      (int)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            OutputDebugStringA(msg);
            s_bLoggedState = true;
        }
        return;
    }

    // 다음 프레임 Dispatch(Crescent 스폰)를 위해 카메라 벡터 저장
    m_LastCamRight = camRight;
    m_LastCamUp    = camUp;

    // 패스 CB 업데이트
    LightPassCB pcb = {};
    pcb.viewProj  = viewProj;
    pcb.camRight  = camRight;
    pcb.camUp     = camUp;
    memcpy(reinterpret_cast<LightPassCB*>(m_pMappedPassCB), &pcb, sizeof(pcb));

    // 디스크립터 힙 바인딩
    ID3D12DescriptorHeap* heaps[] = { m_pDescriptorHeap->GetHeap() };
    pCmdList->SetDescriptorHeaps(1, heaps);

    pCmdList->SetGraphicsRootSignature(FluidParticleSystem::SharedRootSig());
    pCmdList->SetPipelineState(FluidParticleSystem::SharedPSO());
    pCmdList->SetGraphicsRootConstantBufferView(0, m_pPassCB->GetGPUVirtualAddress());
    pCmdList->SetGraphicsRootDescriptorTable(1, m_pDescriptorHeap->GetGPUHandle(m_nSrvDescIndex));

    pCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    pCmdList->DrawInstanced(4, static_cast<UINT>(m_ParticleCount), 0, 0);

    static bool s_bLoggedDraw = false;
    if (!s_bLoggedDraw) {
        char msg[128];
        sprintf_s(msg, "[LightEmitter::Render] OK — DrawInstanced %d particles\n", m_ParticleCount);
        OutputDebugStringA(msg);
        s_bLoggedDraw = true;
    }
}
