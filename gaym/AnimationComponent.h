#pragma once
#include "Component.h"
#include "Animation.h"
#include <map>
#include <vector>

class TransformComponent;
class SkinnedMesh;

class AnimationComponent : public Component
{
public:
    AnimationComponent(GameObject* pOwner);
    virtual ~AnimationComponent();

    virtual void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList) override;
    virtual void Update(float deltaTime) override;
    virtual void Render(ID3D12GraphicsCommandList* pCommandList) override {}

    void LoadAnimation(const char* pstrFileName);
    void Play(std::string strClipName, bool bLoop = true);
    void Stop();

    // Blending
    void CrossFade(const std::string& strClipName, float fBlendDuration, bool bLoop = true, bool bForceRestart = false);
    bool IsBlending() const { return m_bIsBlending; }
    bool IsPlaying() const { return m_bIsPlaying; }

    // Restart current animation from beginning
    void Restart();

    // Animation time offset (for desynchronizing multiple instances)
    void SetTimeOffset(float fOffset) { m_fTimeOffset = fOffset; }
    float GetTimeOffset() const { return m_fTimeOffset; }

    // Debug: F3 = static bind pose (no skinning), F4 = no texture (see raw geometry)
    static bool s_bDebugStaticPose;
    bool m_bBoneLogDone = false;  // per-instance bone match log flag

    // ── LOD / Culling 상태 ──────────────────────────────────────────────
    // Phase offset: 모든 AnimationComponent 가 매 프레임 전부 본 계산하면 부담 크니
    //   monster/npc 를 N 그룹으로 나눠 프레임마다 한 그룹씩만 돌림.
    //   skip 된 프레임의 deltaTime 은 누적 → ON 프레임에 큰 step 으로 한번에 처리 → 재생속도 동일.
    // Frustum culling: 카메라 frustum 밖 몬스터는 본 계산 skip (time 도 정지).
    //   다음에 시야 안 들어오면 그때부터 다시 진행.
    static constexpr int kPhaseGroupCount = 2;           // 2그룹 분산 (≈ 30fps 상당)
    static void TickGlobalFrame() { ++s_nGlobalFrame; }  // Scene::Update 에서 매 프레임 한 번 호출
    static int  GetGlobalFrame()  { return s_nGlobalFrame; }

    void SetCullEnabled(bool b) { m_bCullEnabled = b; }  // 플레이어 등 항상 풀 업데이트 대상은 false 로

    // Playback speed (1.0 = normal, 0.5 = half speed, 2.0 = double speed)
    void SetPlaybackSpeed(float fSpeed) { m_fPlaybackSpeed = fSpeed; }
    float GetPlaybackSpeed() const { return m_fPlaybackSpeed; }

    // 디버그용 — 현재 재생 중인 클립이 특정 클립인지 확인한다.
    bool IsCurrentClip(const std::string& strClipName) const;

    // 디버그용 — 현재 애니메이션 재생 시간을 확인한다.
    float GetCurrentTime() const { return m_fCurrentTime; }

private:
    std::shared_ptr<AnimationSet> m_pAnimationSet;
    AnimationClip* m_pCurrentClip = nullptr;

    float m_fCurrentTime = 0.0f;
    float m_fTimeOffset = 0.0f;  // Random offset to desync animations
    float m_fPlaybackSpeed = 1.0f;  // Playback speed multiplier
    bool m_bLoop = true;
    bool m_bIsPlaying = false;

    // Blending state
    AnimationClip* m_pPreviousClip = nullptr;
    float m_fPreviousTime = 0.0f;
    bool m_bPreviousLoop = false;

    bool m_bIsBlending = false;
    float m_fBlendDuration = 0.2f;
    float m_fBlendTimer = 0.0f;

    // Cache to quickly find bone TransformComponents by name
    std::map<std::string, TransformComponent*> m_mapBoneTransforms;

    void BuildBoneCache(GameObject* pGameObject);

    // 매 프레임 재귀 스캔 제거용 캐시. Init()의 BuildBoneCache 에서 한 번만 채움.
    struct CachedSkinnedMesh
    {
        SkinnedMesh* pMesh;
        GameObject*  pHolder;
    };
    std::vector<CachedSkinnedMesh>     m_vSkinnedMeshes;
    std::vector<TransformComponent*>   m_vHierarchyTransforms; // ForceUpdateTransforms 대상
    void CollectHierarchyNodes(GameObject* pGameObject);       // BuildBoneCache 시 호출

    // LOD 내부 상태
    static int s_nGlobalFrame;         // 전역 프레임 (Scene::Update 에서 tick)
    int        m_iUpdatePhase = 0;     // ctor 에서 ptr 해시 기반 지정
    float      m_fAccumDelta   = 0.f;  // phase skip 된 프레임 delta 누적
    bool       m_bCullEnabled  = true; // false 면 frustum/phase skip 무시 (플레이어 용)
};
