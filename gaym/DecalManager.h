#pragma once
#include "stdafx.h"
#include <memory>

class QuadMesh;
class CDescriptorHeap;
class Shader;

enum class DecalTexture : int
{
    Scorch1 = 0,  // 메테오 최종 착지 (대형)
    Scorch2 = 1,  // 파이어볼 폭발 / 빔 끝점 / 소형 메테오
    Scorch3 = 2,  // WaveSlash 파도 진행로
    Magic2  = 3,  // 메테오 최종 착지 (광원 서클)
    Count   = 4
};

class DecalManager
{
public:
    static constexpr int  MAX_DECALS = 32;
    static constexpr UINT kCBStride  = 8448; // (sizeof(ObjectConstants)+255)&~255

    DecalManager()  = default;
    ~DecalManager() = default;

    void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
              CDescriptorHeap* pHeap, UINT& nNextIndex, Shader* pShader);

    void LoadTexture(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
                     CDescriptorHeap* pHeap, UINT& nNextIndex,
                     DecalTexture type, const wchar_t* pPath);

    void Spawn(DecalTexture tex, const DirectX::XMFLOAT3& pos,
               float size, float rotY, float lifetime);

    void Update(float dt);

    void Render(ID3D12GraphicsCommandList* pCmdList,
                D3D12_GPU_VIRTUAL_ADDRESS passCBGpuAddr);

private:
    struct DecalEntry
    {
        DirectX::XMFLOAT3 pos{};
        float  size       = 1.f;
        float  rotY       = 0.f;
        float  lifeMax    = 1.f;
        float  lifeRemain = 0.f;
        float  spawnTime  = 0.f; // 가장 오래된 슬롯 교체용
        DecalTexture tex  = DecalTexture::Scorch1;
        bool   active     = false;
    };

    struct TexSlot
    {
        ComPtr<ID3D12Resource>       resource;
        ComPtr<ID3D12Resource>       upload;
        D3D12_GPU_DESCRIPTOR_HANDLE  srvGpu{};
        bool loaded = false;
    };

    DecalEntry m_pool[MAX_DECALS]{};
    float      m_totalTime = 0.f;

    std::unique_ptr<QuadMesh>      m_pQuad;
    ComPtr<ID3D12Resource>         m_pCB;
    BYTE*                          m_pMappedCB    = nullptr;
    UINT                           m_nCBVStart    = 0;
    CDescriptorHeap*               m_pDescHeap    = nullptr;
    TexSlot                        m_texSlots[4]{};
    ID3D12PipelineState*           m_pPSO         = nullptr;
    ID3D12RootSignature*           m_pRootSig     = nullptr;
};
