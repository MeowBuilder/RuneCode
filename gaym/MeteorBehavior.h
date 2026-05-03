#pragma once

#include "ISkillBehavior.h"
#include "SkillData.h"
#include "Mesh.h"
#include "DescriptorHeap.h"
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

class FluidSkillVFXManager;
class Scene;
class CDescriptorHeap;

// 큐브 코어용 상수 버퍼 (cbGameObject 레이아웃과 호환 — ProjectileConstants와 동일)
struct MeteorCoreCB
{
    XMFLOAT4X4 World;
    UINT MaterialIndex = 0;
    UINT bIsSkinned = 0;
    UINT bHasTexture = 0;
    UINT bIsLava = 0;
    UINT bHasEmissiveTexture = 0;
    UINT _pad1 = 0;
    UINT _pad2 = 0;
    UINT _pad3 = 0;
    XMFLOAT4 cAmbient;
    XMFLOAT4 cDiffuse;
    XMFLOAT4 cSpecular;
    XMFLOAT4 cEmissive;
    XMFLOAT4X4 BoneTransforms[96]; // unused, layout 호환용
};

// R 슬롯 — 메테오 (불타는 큐브 코어 낙하 → 충돌 시 폭발 + 충격 링 + 바닥 화염)
class MeteorBehavior : public ISkillBehavior
{
public:
    MeteorBehavior();
    MeteorBehavior(const SkillData& customData);
    virtual ~MeteorBehavior();

    void SetVFXManager(FluidSkillVFXManager* mgr) { m_pVFXManager = mgr; }
    void SetScene(Scene* pScene)                  { m_pScene = pScene; }

    // 큐브 코어 반투명 렌더링용 PSO 주입 (Shader::GetWaterPSO 결과)
    // null이면 기본 불투명 PSO로 렌더 (현재 바인딩된 PSO 사용)
    void SetBlendedPSO(ID3D12PipelineState* pso) { m_pBlendPSO = pso; }

    // 큐브 코어 렌더링용 GPU 리소스 바인딩 (Scene 초기화 시 1회 호출)
    void SetDeviceResources(ID3D12Device* pDevice,
                            ID3D12GraphicsCommandList* pCmdList,
                            CDescriptorHeap* pDescHeap,
                            UINT nDescStart);

    // ISkillBehavior 인터페이스
    virtual void Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier = 1.0f) override;
    virtual void Update(float deltaTime) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    virtual void Render(ID3D12GraphicsCommandList* pCmdList) override;
    virtual const SkillData& GetSkillData() const override { return m_SkillData; }

private:
    uint32_t GetRuneFlags(GameObject* caster) const;
    void     ApplyExplosionDamage(float damage, float radius, bool bTriggerStagger);
    void     OnImpact();
    void     InitCoreResources();

    SkillData m_SkillData;
    bool      m_bIsFinished = true;
    FluidSkillVFXManager* m_pVFXManager = nullptr;
    Scene*    m_pScene = nullptr;

    // ─── 큐브 코어 렌더링 리소스 ────────────────────────────────────────
    ID3D12Device*               m_pDevice      = nullptr;
    ID3D12GraphicsCommandList*  m_pInitCmdList = nullptr;
    CDescriptorHeap*            m_pDescHeap    = nullptr;
    UINT                        m_nDescStart   = 0;
    CubeMesh*                   m_pCoreMesh    = nullptr;     // 두 큐브가 공유하는 메시
    ComPtr<ID3D12Resource>      m_pCoreCBUpload[2];           // 큐브1, 큐브2 상수버퍼
    MeteorCoreCB*               m_pMappedCoreCB[2] = { nullptr, nullptr };
    UINT                        m_nCoreCBSize  = 0;
    bool                        m_bCoreInited  = false;

    // ─── VFX 슬롯 IDs ──────────────────────────────────────────────────
    int m_vfxId        = -1;  // 메인 화염구 (R_Meteor)
    int m_trailVfxId   = -1;  // 혜성 꼬리 코어 (R_MeteorTrail)
    int m_trailOuterId = -1;  // 혜성 꼬리 외곽 글로우 (R_MeteorTrailOuter)
    int m_impactVfxId  = -1;  // 충격 링 + 폭발구 (R_MeteorImpact)
    int m_groundFireId = -1;  // 바닥 화염 (R_MeteorGroundFire)
    int m_frontFireId  = -1;  // 큐브 바닥 분출 화염 (R_MeteorFrontFire) — 낙하 중 추적
    std::vector<int> m_extraVFXIds; // 다중 원소 추가 VFX 슬롯

    // 큐브 반투명 렌더용 PSO (alpha blend) — Scene 초기화 시 주입
    ID3D12PipelineState* m_pBlendPSO = nullptr;

    // ─── 상태 ──────────────────────────────────────────────────────────
    XMFLOAT3 m_spawnPos    = {};
    XMFLOAT3 m_targetPos   = {};
    float    m_damageMult  = 1.f;
    float    m_elapsed     = 0.f;
    bool     m_bExploded   = false;
    int      m_lastPhase   = -1;   // 메인 슬롯의 직전 페이즈 (전환 검출용)

    // 큐브 회전 상태 — 두 큐브가 독립 다축으로 회전 (역동적 외관)
    float    m_rotY0       = 0.f;  // Cube0 Y축 회전
    float    m_rotX0       = 0.f;  // Cube0 X축 기울기 (서서히)
    float    m_rotY1       = 0.f;  // Cube1 Y축 회전 (반대 방향)
    float    m_rotZ1       = 0.f;  // Cube1 Z축 롤링

    static constexpr float METEOR_SPAWN_HEIGHT  = 50.f;
    static constexpr float METEOR_FORWARD_DIST  = 15.f;
    static constexpr float FALL_DURATION        = 3.0f;   // EffectRegistry R_Meteor Phase0와 동기화
    static constexpr float MASTER_CP_FALL_SPEED = 22.f;   // EffectRegistry와 동기화
    static constexpr float EXPLODE_RADIUS       = 10.0f;  // 폭발 AoE 반경
    static constexpr float CORE_SIZE            = 3.5f;   // 큐브 한 변
};
