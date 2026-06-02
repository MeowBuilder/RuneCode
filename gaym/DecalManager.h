#pragma once
#include "stdafx.h"
#include <memory>

class QuadMesh;
class CDescriptorHeap;
class Shader;

enum class DecalTexture : int
{
    Scorch1      = 0,  // 메테오 최종 착지 (대형)
    Scorch2      = 1,  // 파이어볼 폭발 / 빔 끝점 / 소형 메테오
    Scorch3      = 2,  // WaveSlash 파도 진행로
    Magic2       = 3,  // 메테오 최종 착지 (광원 서클)
    Magic3       = 4,  // 메아리 룬(ABY_ECO) 마법진 — 회전 + 원소 색상
    Skull        = 5,  // 처형자 룬(ABY_EXC) 처형 킬 마커
    MagicCircle  = 6,  // R 스킬 발동 마법진 (MagicCircle.png)
    Star08       = 7,  // 설치 룬 함정 인디케이터 (star_08.png)
    Count        = 8
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

    // color: RGBA 틴트 (1,1,1,1 = 원본 색상 그대로)
    // rotateSpeed: rad/s (0 = 정지)
    // revealDuration: 중앙부터 바깥으로 펼쳐지는 시간 (0 = 즉시 표시)
    // 반환값: 슬롯 인덱스 (-1 실패) — SetPosition으로 추적 가능
    int Spawn(DecalTexture tex, const DirectX::XMFLOAT3& pos,
              float size, float rotY, float lifetime,
              DirectX::XMFLOAT4 color = { 1.f, 1.f, 1.f, 1.f },
              float rotateSpeed = 0.f,
              float revealDuration = 0.f);

    // 활성 데칼의 위치를 갱신 (캐릭터 추적 등에 사용)
    void SetPosition(int slotIdx, const DirectX::XMFLOAT3& pos);

    // 조기 종료
    void Stop(int slotIdx);

    void Update(float dt);

    void Render(ID3D12GraphicsCommandList* pCmdList,
                D3D12_GPU_VIRTUAL_ADDRESS passCBGpuAddr);

private:
    struct DecalEntry
    {
        DirectX::XMFLOAT3 pos{};
        float  size        = 1.f;
        float  rotY        = 0.f;
        float  rotateSpeed = 0.f;
        float  lifeMax     = 1.f;
        float  lifeRemain  = 0.f;
        float  spawnTime   = 0.f;
        DirectX::XMFLOAT4 color = { 1.f, 1.f, 1.f, 1.f };
        DecalTexture tex      = DecalTexture::Scorch1;
        float  revealDuration = 0.f;  // 0 = 즉시 표시
        float  revealProgress = 1.f;  // 0→1 (1 = 완전히 표시됨)
        bool   active         = false;
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
    TexSlot                        m_texSlots[static_cast<int>(DecalTexture::Count)]{};
    ID3D12PipelineState*           m_pPSO         = nullptr;
    ID3D12RootSignature*           m_pRootSig     = nullptr;
};
