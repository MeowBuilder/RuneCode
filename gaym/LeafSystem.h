#pragma once
#include "stdafx.h"
#include <DirectXMath.h>
#include <vector>
#include <string>

class CDescriptorHeap;

// ─── LeafSystem ──────────────────────────────────────────────────────────────
//
// Wind 테마 맵에서 벚꽃잎/단풍/연두잎 등이 3D 월드 안에 떠다니는 시스템.
// LightEmitter 와 별개 — 자체 root sig + PSO + 텍스처 binding.
//
//   m_pLeaves->Init(device, cmd, {"sakura.png","green_leaf.png","red_leaf.png"}, heap, idx);
//   m_pLeaves->SetSpawnArea(roomCenter, halfExtents);
//   m_pLeaves->SetEnabled(true);   // Wind 테마 진입 시
//   m_pLeaves->Update(rawDt);
//   m_pLeaves->Render(cmd, viewProj, camRight, camUp, time);
//
// 입자: 각자 위치/속도/회전/lifetime/텍스처 인덱스.
// 동작: 위에서 천천히 떨어지며 + 바람 방향 drift + 좌우 sway + 자체 회전.
class LeafSystem
{
public:
    static constexpr int kMaxLeaves = 240;

    LeafSystem() = default;
    ~LeafSystem();

    // leafTexturePaths : 잎 텍스처 .png 경로 배열 (최대 4종 권장).
    // 호출자가 cmd list 를 reset 한 상태로 넘기고 Init 후 close + execute + wait.
    void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
              const std::vector<std::wstring>& leafTexturePaths);

    // Wind 테마 진입 시 호출 — 잎 spawn 영역 설정.
    void SetSpawnArea(const DirectX::XMFLOAT3& center,
                      const DirectX::XMFLOAT3& halfExtents);
    void SetWind(const DirectX::XMFLOAT3& dir, float strength);
    void SetEnabled(bool b);
    bool IsEnabled() const { return m_bEnabled; }

    void Update(float rawDt);
    void Render(ID3D12GraphicsCommandList* pCmdList,
                const DirectX::XMFLOAT4X4& viewProj,
                const DirectX::XMFLOAT3& camRight,
                const DirectX::XMFLOAT3& camUp,
                float time);

private:
    struct Leaf
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 baseVel;       // 기본 drift 속도
        float    size;
        float    lifetime;
        float    maxLifetime;
        DirectX::XMFLOAT4 color;
        int      texIdx;
        // 자연스러운 swirl/회전 — 입자별 다른 phase/freq/속도로 다양성
        float    swirlPhase;
        float    swirlFreqXZ;   // 좌우 회오리 빈도
        float    swirlFreqY;    // 위/아래 떨림 빈도
        float    swirlAmpXZ;    // 좌우 진폭
        float    yawAng;        // 누적 yaw (rad)
        float    yawVel;        // yaw angular vel (입자별 다름)
        float    pitchPhase;    // pitch 흔들림 phase
    };

    // GPU 측 인스턴스 데이터 (셰이더 LeafParticleData) — 32 byte
    struct LeafGPU
    {
        DirectX::XMFLOAT3 position;
        float             size;
        DirectX::XMFLOAT4 color;
        // 회전 — VS 가 이 값 그대로 사용 (CPU 누적)
        float             yawAng;
        float             pitchAng;
        float             pad0;
        float             pad1;
    };

    struct PassCB
    {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT3   camRight; float padR;
        DirectX::XMFLOAT3   camUp;    float padU;
        float time;
        float rotSpeed;
        float swayAmp;
        float swayFreq;
    };

    void CreateRootSignature(ID3D12Device* pDevice);
    void CreatePipelineState(ID3D12Device* pDevice);
    void CreateBuffers(ID3D12Device* pDevice);
    void LoadLeafTexture(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
                         const std::wstring& path, int slot);
    void RespawnLeaf(Leaf& leaf, bool initialSpawn);

    bool m_bEnabled = false;
    DirectX::XMFLOAT3 m_spawnCenter = { 0, 0, 0 };
    DirectX::XMFLOAT3 m_spawnHalfExt = { 60, 30, 60 };
    DirectX::XMFLOAT3 m_windDir      = { 0.3f, 0.0f, 0.5f };  // 정규화 안 됨 — drift vel 직접 사용
    float             m_windStrength = 2.0f;

    std::vector<Leaf>    m_leaves;
    std::vector<LeafGPU> m_gpuBuffer;

    // GPU
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_pRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pPSO;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_pParticleBuf;  // UPLOAD, 매 프레임 memcpy
    UINT8*                                  m_pParticleMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_pParticleSRVBuf; // shader-visible structured buffer (SRV)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_pPassCB;
    UINT8*                                  m_pCBMapped = nullptr;

    // 텍스처 (3~4종)
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_pLeafTexs;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_pLeafUploads;
    std::vector<UINT> m_nSrvIndices;

    // 자체 descriptor heap — 인스턴스 SRV(0) + 텍스처 SRV(1..N)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_pSrvHeap;
    UINT m_srvIncr = 0;
    static constexpr UINT kInstanceSrvSlot = 0;   // 슬롯 0 = instance buffer SRV
    // 텍스처는 슬롯 1, 2, 3, ... 에 등록

    int m_numTextures = 0;
};
