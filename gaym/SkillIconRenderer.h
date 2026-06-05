#pragma once

#include "stdafx.h"
#include <DescriptorHeap.h>
#include <string>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// SkillIconRenderer
//   2D UI 아이콘을 그리는 저수준 렌더러. DebugRenderer 와 동일하게 자체 루트시그/PSO/
//   인라인 셰이더를 소유한다. SpriteBatch 와 별개의 패스(자체 PSO)로 동작한다.
//
//   핵심 기능:
//   - SkillIcons / RuneIcons 폴더의 PNG 를 파일명(확장자 제외) 키로 일괄 로드.
//   - 키로 아이콘을 찾으면 원본 아트를, 못 찾으면 폴백색 단색 타일을 그린다(절차적 폴백).
//   - 방사형 쿨타임: cooldownProgress(0=막 사용, 1=완료)에 따라 12시→시계방향으로
//     색이 복구되고, 아직 쿨다운인 부채꼴은 회색으로 보인다.
// ─────────────────────────────────────────────────────────────────────────────
class SkillIconRenderer
{
public:
    SkillIconRenderer() = default;
    ~SkillIconRenderer() = default;

    // PSO/루트시그/디스크립터힙 생성 + 흰 텍스처 + 아이콘 PNG 일괄 로드.
    // pCommandList 는 열린 상태로 전달되며, 호출자가 Close/Execute/Wait 한다.
    void Initialize(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList);

    // 아이콘 패스 시작 — PSO/루트시그/디스크립터힙/뷰포트 바인딩.
    void Begin(ID3D12GraphicsCommandList* pCommandList, float screenWidth, float screenHeight);

    // 아이콘 1장 그리기.
    //   key              : 아이콘 키(스킬 이름 또는 룬 ID). 없으면 폴백색 단색 타일.
    //   x,y,w,h          : 화면 픽셀 좌표(좌상단 기준).
    //   fallbackColor    : 키를 못 찾았을 때 단색 타일 색(원소색 등). 찾으면 흰색 곱(원본 그대로).
    //   cooldownProgress : 1=정상(색 100%), 0~1=시계방향 색 복구. 1 미만일 때만 회색 적용.
    //   alpha            : 전체 알파 배수.
    void DrawIcon(ID3D12GraphicsCommandList* pCommandList,
                  const std::string& key,
                  float x, float y, float w, float h,
                  const XMFLOAT4& fallbackColor = XMFLOAT4(1, 1, 1, 1),
                  float cooldownProgress = 1.0f,
                  float alpha = 1.0f);

    // 단색 사각형(툴팁 배경, 빈 룬칸 등). 흰 텍스처에 color 를 곱한다.
    void DrawSolid(ID3D12GraphicsCommandList* pCommandList,
                   float x, float y, float w, float h,
                   const XMFLOAT4& color);

    // 해당 키의 PNG 아이콘이 로드되어 있는지.
    bool HasIcon(const std::string& key) const { return m_icons.find(key) != m_icons.end(); }

private:
    struct IconTex
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        UINT width  = 0;
        UINT height = 0;
    };

    void CreatePipelineState(ID3D12Device* pDevice);
    void CreateWhiteTexture(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, UINT heapSlot);
    // path 의 PNG 를 heapSlot 에 로드해 key 로 등록. 성공 시 true.
    bool LoadIconFile(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                      const std::wstring& path, const std::string& key, UINT heapSlot);
    // folderW 의 *.png 를 전부 스캔해 파일명(stem)을 키로 로드.
    void LoadIconFolder(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                        const std::wstring& folderW);

    // 내부 공통 draw. gpu 핸들/텍스처여부/tint/쿨타임을 받아 쿼드 1장 발행.
    void DrawQuad(ID3D12GraphicsCommandList* pCommandList,
                  D3D12_GPU_DESCRIPTOR_HANDLE gpu, bool hasTexture,
                  float x, float y, float w, float h,
                  const XMFLOAT4& tint, float cooldownProgress);

private:
    ComPtr<ID3D12RootSignature> m_pRootSignature;
    ComPtr<ID3D12PipelineState> m_pPipelineState;

    std::unique_ptr<DirectX::DescriptorHeap> m_pHeap;
    UINT m_nNextSlot = 0;  // 다음 비어있는 힙 슬롯

    // 흰 1x1 텍스처(폴백/단색용)
    ComPtr<ID3D12Resource> m_pWhiteTex;
    D3D12_GPU_DESCRIPTOR_HANDLE m_hWhiteGPU{};

    // 로드된 아이콘 텍스처들(GPU 수명 동안 유지)
    std::vector<ComPtr<ID3D12Resource>> m_textures;
    std::vector<ComPtr<ID3D12Resource>> m_uploads;  // 업로드 버퍼(수명 유지)

    std::unordered_map<std::string, IconTex> m_icons;

    float m_screenW = 1920.0f;
    float m_screenH = 1080.0f;

    static constexpr UINT HEAP_CAPACITY = 192;  // 흰텍스처 + 스킬/룬 아이콘 넉넉히
};
