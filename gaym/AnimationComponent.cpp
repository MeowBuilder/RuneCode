#include "stdafx.h"
#include "AnimationComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "Mesh.h"
#include "Dx12App.h"
#include "Scene.h"
#include "Camera.h"
#include <functional>
#include <vector>

bool AnimationComponent::s_bDebugStaticPose = false;
int  AnimationComponent::s_nGlobalFrame    = 0;

AnimationComponent::AnimationComponent(GameObject* pOwner) : Component(pOwner)
{
    m_pAnimationSet = std::make_shared<AnimationSet>();

    // Phase 는 owner 포인터 해시 기반 — 생성 순서에 따라 그룹이 섞여 몰림 방지.
    // kPhaseGroupCount 로 나눠 Update 마다 s_nGlobalFrame 과 매칭되는 그룹만 본 계산.
    m_iUpdatePhase = static_cast<int>(reinterpret_cast<uintptr_t>(pOwner) % kPhaseGroupCount);
}

AnimationComponent::~AnimationComponent()
{
}

void AnimationComponent::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList)
{
    m_mapBoneTransforms.clear();
    m_vSkinnedMeshes.clear();
    m_vHierarchyTransforms.clear();
    BuildBoneCache(m_pOwner);
    CollectHierarchyNodes(m_pOwner);
}

void AnimationComponent::BuildBoneCache(GameObject* pGameObject)
{
    if (!pGameObject) return;

    if (pGameObject->m_pstrFrameName[0] != '\0')
    {
        m_mapBoneTransforms[pGameObject->m_pstrFrameName] = pGameObject->GetTransform();
    }

    if (pGameObject->m_pChild) BuildBoneCache(pGameObject->m_pChild);
    if (pGameObject->m_pSibling) BuildBoneCache(pGameObject->m_pSibling);
}

// 매 프레임 재귀 스캔(FindAllSkinnedMeshes / ForceUpdateTransforms) 비용 제거용.
// Init 시 한 번만 순회해서 SkinnedMesh 와 모든 TransformComponent 포인터를 캐싱.
void AnimationComponent::CollectHierarchyNodes(GameObject* pGameObject)
{
    if (!pGameObject) return;

    Mesh* pMesh = pGameObject->GetMesh();
    if (pMesh && (pMesh->GetType() & 0x10))
    {
        m_vSkinnedMeshes.push_back({ static_cast<SkinnedMesh*>(pMesh), pGameObject });
    }
    if (auto* pT = pGameObject->GetTransform())
        m_vHierarchyTransforms.push_back(pT);

    if (pGameObject->m_pChild)   CollectHierarchyNodes(pGameObject->m_pChild);
    if (pGameObject->m_pSibling) CollectHierarchyNodes(pGameObject->m_pSibling);
}

void AnimationComponent::LoadAnimation(const char* pstrFileName)
{
    m_pAnimationSet->LoadAnimationFromFile(pstrFileName);
}

void AnimationComponent::Play(std::string strClipName, bool bLoop)
{
    AnimationClip* pClip = m_pAnimationSet->GetClip(strClipName);
    if (pClip)
    {
        m_pCurrentClip = pClip;
        m_bLoop = bLoop;
        // Apply time offset to desync animations between instances
        m_fCurrentTime = fmod(m_fTimeOffset, pClip->m_fDuration);
        m_bIsPlaying = true;
    }
}

void AnimationComponent::Stop()
{
    m_bIsPlaying = false;
}

void AnimationComponent::CrossFade(const std::string& strClipName, float fBlendDuration, bool bLoop, bool bForceRestart)
{
    AnimationClip* pNewClip = m_pAnimationSet->GetClip(strClipName);
    if (!pNewClip)
    {
        char buffer[256];
        sprintf_s(buffer, "CrossFade: Clip Not Found: %s\n", strClipName.c_str());
        OutputDebugStringA(buffer);
        return;
    }

    // If same clip and not forcing restart, just continue
    if (pNewClip == m_pCurrentClip && !bForceRestart) return;

    // Store previous clip state for blending
    m_pPreviousClip = m_pCurrentClip;
    m_fPreviousTime = m_fCurrentTime;
    m_bPreviousLoop = m_bLoop;

    // Set new clip
    m_pCurrentClip = pNewClip;
    // Apply time offset for looping animations to desync between instances.
    // 단, forceRestart=true 는 "반드시 처음부터" 의도(Attack state 전환 등) — desync offset 무시.
    // 과거엔 loop=true + force=true 시 offset 이 걸려 공격 애니가 중간부터 재생되는 버그.
    m_fCurrentTime = (bLoop && !bForceRestart) ? fmod(m_fTimeOffset, pNewClip->m_fDuration) : 0.0f;
    m_bLoop = bLoop;
    m_bIsPlaying = true;

    // Start blending
    m_bIsBlending = true;
    m_fBlendDuration = fBlendDuration;
    m_fBlendTimer = 0.0f;

}

void AnimationComponent::Restart()
{
    if (m_pCurrentClip)
    {
        m_fCurrentTime = 0.0f;
        m_bIsPlaying = true;
    }
}

void AnimationComponent::Update(float deltaTime)
{
    if (!m_bIsPlaying || !m_pCurrentClip) return;

    // ── LOD skip 결정 (플레이어 등 m_bCullEnabled=false 는 건너뜀) ─────────
    // (B) Phase offset: 몬스터마다 frame % N 그룹 중 본인 그룹일 때만 본 계산.
    //     Skip 된 프레임은 deltaTime 을 누적해 ON 프레임에 한번에 처리 → 재생 속도 유지.
    // (A) Frustum culling: 카메라 뷰 밖이면 본 계산 완전 skip (누적도 안 함 — 다시 보일 때 그 자리부터).
    if (m_bCullEnabled)
    {
        // Frustum 체크 — 탑뷰여도 방 경계 넘는 몬스터는 카메라 밖
        bool bInFrustum = true;
        if (Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr)
        {
            if (CCamera* pCam = pScene->GetCamera())
            {
                XMMATRIX matProj = XMLoadFloat4x4(&pCam->GetProjectionMatrix());
                XMMATRIX matView = XMLoadFloat4x4(&pCam->GetViewMatrix());
                BoundingFrustum frustum;
                BoundingFrustum::CreateFromMatrix(frustum, matProj);

                // projMatrix 기반 frustum 은 view space 기준 → view 의 역변환으로 world 로 옮김
                XMVECTOR det;
                XMMATRIX matInvView = XMMatrixInverse(&det, matView);
                frustum.Transform(frustum, matInvView);

                if (m_pOwner && m_pOwner->GetTransform())
                {
                    XMFLOAT3 p = m_pOwner->GetTransform()->GetPosition();
                    // 넉넉한 반경 (몬스터 scale 5.0 기준 + 마진)
                    BoundingSphere sph({ p.x, p.y + 3.0f, p.z }, 8.0f);
                    bInFrustum = frustum.Intersects(sph);
                }
            }
        }

        if (!bInFrustum)
        {
            // 카메라 밖 — 본 계산 skip. time 은 그대로 두어 다시 보일 때 그 포즈 이어짐.
            return;
        }

        // Frustum 안이지만 phase 체크
        m_fAccumDelta += deltaTime;
        if ((s_nGlobalFrame % kPhaseGroupCount) != (m_iUpdatePhase % kPhaseGroupCount))
            return;

        deltaTime = m_fAccumDelta;   // 누적분 한 번에 처리
        m_fAccumDelta = 0.f;
    }

    // 매 프레임 캐시 재구축 — Init 시점 raw pointer 캐싱은 보스 전환 등
    //   hierarchy mutation 타이밍에 dangling 위험 (m_vSkinnedMeshes / m_mapBoneTransforms /
    //   m_vHierarchyTransforms). owner 가 유효하면 tree 가 곧 ground truth 이므로 매 프레임
    //   소규모 재스캔으로 처리. 보스급 1마리 ~수십 노드 수준이라 비용 무시 가능.
    if (m_pOwner)
    {
        m_mapBoneTransforms.clear();
        m_vSkinnedMeshes.clear();
        m_vHierarchyTransforms.clear();
        BuildBoneCache(m_pOwner);
        CollectHierarchyNodes(m_pOwner);
    }

    // Apply playback speed
    float scaledDelta = deltaTime * m_fPlaybackSpeed;

    // Update blend timer
    if (m_bIsBlending)
    {
        m_fBlendTimer += deltaTime;  // 블렌드 타이머는 실제 시간 사용
        if (m_fBlendTimer >= m_fBlendDuration)
        {
            m_bIsBlending = false;
            m_pPreviousClip = nullptr;
        }

        // Also update previous clip time
        if (m_pPreviousClip)
        {
            m_fPreviousTime += scaledDelta;
            if (m_fPreviousTime >= m_pPreviousClip->m_fDuration)
            {
                if (m_bPreviousLoop)
                    m_fPreviousTime = fmod(m_fPreviousTime, m_pPreviousClip->m_fDuration);
                else
                    m_fPreviousTime = m_pPreviousClip->m_fDuration;
            }
        }
    }

    // Update current clip time
    m_fCurrentTime += scaledDelta;
    if (m_fCurrentTime >= m_pCurrentClip->m_fDuration)
    {
        if (m_bLoop)
            m_fCurrentTime = fmod(m_fCurrentTime, m_pCurrentClip->m_fDuration);
        else
        {
            m_fCurrentTime = m_pCurrentClip->m_fDuration;
            m_bIsPlaying = false;
        }
    }

    // Calculate blend weight (0 = previous, 1 = current)
    float fBlendWeight = m_bIsBlending ? (m_fBlendTimer / m_fBlendDuration) : 1.0f;

    // Sample current clip
    float fFrame = m_fCurrentTime * m_pCurrentClip->m_fFrameRate;
    int nFrame = (int)fFrame;
    int nNextFrame = nFrame + 1;
    float fRatio = fFrame - nFrame;
    if (nFrame >= m_pCurrentClip->m_nTotalFrames - 1)
    {
        nFrame = m_pCurrentClip->m_nTotalFrames - 1;
        nNextFrame = nFrame;
        fRatio = 0.0f;
    }

    // Sample previous clip (if blending)
    int nPrevFrame = 0, nPrevNextFrame = 0;
    float fPrevRatio = 0.0f;
    if (m_bIsBlending && m_pPreviousClip)
    {
        float fPrevFrameF = m_fPreviousTime * m_pPreviousClip->m_fFrameRate;
        nPrevFrame = (int)fPrevFrameF;
        nPrevNextFrame = nPrevFrame + 1;
        fPrevRatio = fPrevFrameF - nPrevFrame;
        if (nPrevFrame >= m_pPreviousClip->m_nTotalFrames - 1)
        {
            nPrevFrame = m_pPreviousClip->m_nTotalFrames - 1;
            nPrevNextFrame = nPrevFrame;
            fPrevRatio = 0.0f;
        }
    }

    // Apply to bones
    //   캐시 dangling 방어 — m_mapBoneTransforms 도 raw pointer 캐시이므로
    //   동일하게 유저공간 범위 체크 후 사용.
    auto IsValidPtr = [](const void* p) -> bool {
        auto v = reinterpret_cast<uintptr_t>(p);
        return v >= 0x10000ULL && v <= 0x00007FFFFFFFFFFFULL;
    };

    for (const auto& track : m_pCurrentClip->m_vBoneTracks)
    {
        auto it = m_mapBoneTransforms.find(track.m_strBoneName);
        if (it == m_mapBoneTransforms.end()) continue;

        TransformComponent* pTransform = it->second;
        if (!IsValidPtr(pTransform)) continue;

        // Skip root object - its transform is managed by game logic (position, scale)
        if (pTransform == m_pOwner->GetTransform()) continue;

        if (nFrame >= (int)track.m_vKeyframes.size() || nNextFrame >= (int)track.m_vKeyframes.size())
            continue;

        // Sample current clip pose
        const Keyframe& k1 = track.m_vKeyframes[nFrame];
        const Keyframe& k2 = track.m_vKeyframes[nNextFrame];

        XMVECTOR currPos = XMVectorLerp(XMLoadFloat3(&k1.m_xmf3Position), XMLoadFloat3(&k2.m_xmf3Position), fRatio);
        XMVECTOR currRot = XMQuaternionSlerp(XMLoadFloat4(&k1.m_xmf4Rotation), XMLoadFloat4(&k2.m_xmf4Rotation), fRatio);
        XMVECTOR currScale = XMVectorLerp(XMLoadFloat3(&k1.m_xmf3Scale), XMLoadFloat3(&k2.m_xmf3Scale), fRatio);

        XMVECTOR finalPos = currPos;
        XMVECTOR finalRot = currRot;
        XMVECTOR finalScale = currScale;

        // Blend with previous clip if blending.
        // AnimationClip::m_mapBoneNameToIndex 로 O(1) lookup (이전엔 이전 clip 본 트랙 전체 선형 서치 — O(본²))
        if (m_bIsBlending && m_pPreviousClip)
        {
            auto mapIt = m_pPreviousClip->m_mapBoneNameToIndex.find(track.m_strBoneName);
            if (mapIt != m_pPreviousClip->m_mapBoneNameToIndex.end())
            {
                const BoneTrack& prevTrack = m_pPreviousClip->m_vBoneTracks[mapIt->second];
                if (nPrevFrame < (int)prevTrack.m_vKeyframes.size() && nPrevNextFrame < (int)prevTrack.m_vKeyframes.size())
                {
                    const Keyframe& pk1 = prevTrack.m_vKeyframes[nPrevFrame];
                    const Keyframe& pk2 = prevTrack.m_vKeyframes[nPrevNextFrame];

                    XMVECTOR prevPos = XMVectorLerp(XMLoadFloat3(&pk1.m_xmf3Position), XMLoadFloat3(&pk2.m_xmf3Position), fPrevRatio);
                    XMVECTOR prevRot = XMQuaternionSlerp(XMLoadFloat4(&pk1.m_xmf4Rotation), XMLoadFloat4(&pk2.m_xmf4Rotation), fPrevRatio);
                    XMVECTOR prevScale = XMVectorLerp(XMLoadFloat3(&pk1.m_xmf3Scale), XMLoadFloat3(&pk2.m_xmf3Scale), fPrevRatio);

                    finalPos = XMVectorLerp(prevPos, currPos, fBlendWeight);
                    finalRot = XMQuaternionSlerp(prevRot, currRot, fBlendWeight);
                    finalScale = XMVectorLerp(prevScale, currScale, fBlendWeight);
                }
            }
        }

        XMFLOAT3 outPos, outScale;
        XMFLOAT4 outRot;
        XMStoreFloat3(&outPos, finalPos);
        XMStoreFloat3(&outScale, finalScale);
        XMStoreFloat4(&outRot, finalRot);

        pTransform->SetPosition(outPos);
        pTransform->SetScale(outScale);
        pTransform->SetRotation(outRot);
        pTransform->SetLocalMatrix(Matrix4x4::Identity());
    }

    // --- Skinning Logic ---
    // Init 시점에 캐싱한 m_vSkinnedMeshes / m_vHierarchyTransforms 를 그대로 사용.
    // 이전엔 std::function 재귀 람다로 매 프레임 트리 전체 스캔 (트리 크기 + lambda 오버헤드).
    m_bBoneLogDone = true;

    // F3: skip skinning → explicitly clear bIsSkinned so shader uses raw bind-pose vertices
    if (!m_vSkinnedMeshes.empty() && s_bDebugStaticPose)
    {
        for (const auto& entry : m_vSkinnedMeshes)
            entry.pHolder->SetSkinned(false);
        return;
    }

    if (!m_vSkinnedMeshes.empty())
    {
        // 캐시된 raw pointer 가 보스 전환 타이밍에 간헐적으로 dangling 되는 케이스 보호.
        // x64 Windows 유저공간은 [0x10000, 0x7FFFFFFFFFFF] 범위 — 그 밖 값은 명백한 invalid.
        //   ex) 0xFFFFFFFFFFFFFFEF (스폰 직후 hierarchy 재구축 중 등) → 즉시 skip.
        auto IsValidPtr = [](const void* p) -> bool {
            auto v = reinterpret_cast<uintptr_t>(p);
            return v >= 0x10000ULL && v <= 0x00007FFFFFFFFFFFULL;
        };

        // 캐시된 TransformComponent 들에 대해 Update(0) 호출 (world matrix 갱신)
        for (TransformComponent* pT : m_vHierarchyTransforms)
        {
            if (!IsValidPtr(pT)) continue;
            pT->Update(0.0f);
        }

        for (const auto& entry : m_vSkinnedMeshes)
        {
            SkinnedMesh* pSkinnedMesh = entry.pMesh;
            GameObject* pMeshHolder = entry.pHolder;
            if (!IsValidPtr(pSkinnedMesh) || !IsValidPtr(pMeshHolder)) continue;

            TransformComponent* pHolderT = pMeshHolder->GetTransform();
            if (!IsValidPtr(pHolderT)) continue;

            XMMATRIX matRootInvWorld = XMMatrixInverse(nullptr, XMLoadFloat4x4(&pHolderT->GetWorldMatrix()));

            for (size_t i = 0; i < pSkinnedMesh->m_vBoneNames.size(); ++i)
            {
                auto it = m_mapBoneTransforms.find(pSkinnedMesh->m_vBoneNames[i]);
                if (it != m_mapBoneTransforms.end())
                {
                    TransformComponent* pBoneTransform = it->second;
                    if (!IsValidPtr(pBoneTransform)) continue;

                    XMMATRIX matBoneWorld = XMLoadFloat4x4(&pBoneTransform->GetWorldMatrix());
                    XMMATRIX matInvBindPose = XMLoadFloat4x4(&pSkinnedMesh->m_vBindPoses[i]);

                    XMMATRIX matFinal = matInvBindPose * matBoneWorld * matRootInvWorld;
                    XMFLOAT4X4 f;
                    XMStoreFloat4x4(&f, XMMatrixTranspose(matFinal));
                    pMeshHolder->SetBoneTransform((int)i, f);
                }
            }
            pMeshHolder->SetSkinned(true);
        }
    }
}
