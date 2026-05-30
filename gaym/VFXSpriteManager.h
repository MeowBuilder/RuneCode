#pragma once
#include "stdafx.h"
#include <SpriteBatch.h>
#include <string>
#include <unordered_map>

// 화면에 2D 스프라이트로 표시되는 VFX 아이콘 매니저.
// 3D 월드 좌표를 스크린으로 투영해 SpriteBatch로 렌더링.
// 메아리 룬 마법진, 처형자 skull 등 룬/스킬 이미지 이펙트에 사용.

enum class VFXSpriteAnim : uint8_t
{
    FadeOut,   // 라이프타임 마지막 30% 구간에서 페이드
    SkullPop,  // pop-in(0~0.2s) → settle(0.2~0.5s) → fade(0.5~1.0s)
};

class VFXSpriteManager
{
public:
    static VFXSpriteManager& Get();

    struct TexInfo
    {
        D3D12_GPU_DESCRIPTOR_HANDLE srv{};
        UINT width  = 1;
        UINT height = 1;
    };

    // 초기화 시 텍스처 등록 (id는 Spawn 시 참조하는 문자열 키)
    void RegisterTex(const std::string& id, D3D12_GPU_DESCRIPTOR_HANDLE srv, UINT w, UINT h);

    // 아이콘 스폰. 반환값: 슬롯 인덱스 (SetPosition/Stop에 사용), -1이면 풀 초과
    int  Spawn(const std::string& texId,
               const DirectX::XMFLOAT3& worldPos,
               float screenSize,
               float lifetime,
               DirectX::XMFLOAT4 color      = { 1.f, 1.f, 1.f, 1.f },
               float rotateSpeed             = 0.f,
               VFXSpriteAnim anim            = VFXSpriteAnim::FadeOut);

    // 활성 아이콘 위치 갱신 (캐릭터 추적 등)
    void SetPosition(int slot, const DirectX::XMFLOAT3& pos);

    // 조기 종료
    void Stop(int slot);

    void Update(float dt);
    void Render(DirectX::SpriteBatch* pBatch,
                const DirectX::XMFLOAT4X4& viewProj,
                int screenW, int screenH);

private:
    VFXSpriteManager() = default;

    struct Entry
    {
        std::string           texId;
        DirectX::XMFLOAT3     worldPos{};
        float                 screenSize = 64.f;
        float                 lifeMax    = 1.f;
        float                 lifeRemain = 0.f;
        DirectX::XMFLOAT4     color{ 1.f, 1.f, 1.f, 1.f };
        float                 rotAngle   = 0.f;
        float                 rotSpeed   = 0.f;
        VFXSpriteAnim         anim       = VFXSpriteAnim::FadeOut;
        bool                  active     = false;
    };

    static constexpr int MAX_ENTRIES = 64;
    Entry m_pool[MAX_ENTRIES]{};

    std::unordered_map<std::string, TexInfo> m_texMap;
};
