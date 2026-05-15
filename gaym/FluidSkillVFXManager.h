#pragma once
#include "stdafx.h"
#include "FluidParticle.h"
#include "FluidParticleSystem.h"
#include "LightEmitterSystem.h"
#include "VFXTypes.h"
#include "EffectRegistry.h"   // EffectDef / EffectLayer
#include <array>
#include <memory>
#include <string>

class CDescriptorHeap;
class ScreenSpaceFluid;

// 한 슬롯 = 하나의 활성 투사체 유체 이펙트
struct FluidVFXSlot {
    std::unique_ptr<FluidParticleSystem> pSystem;
    bool             isActive    = false;
    bool             isFadingOut    = false;   // 충돌 후 수렴 중
    float            fadeTimer      = 0.0f;    // 수렴 후 소멸까지 남은 시간
    bool             isExplodeMode  = false;   // 폭발(ExplodeEffect) 모드 여부
    float            explodeTotalTime = 1.5f;  // 폭발 소멸 총 시간
    float            elapsed     = 0.0f;
    XMFLOAT3         origin      = {0, 0, 0};
    XMFLOAT3         prevOrigin  = {0, 0, 0};  // 이전 프레임 origin (파티클 공동이동용)
    XMFLOAT3         direction   = {0, 0, 1};
    FluidSkillVFXDef def;

    // 시퀀스 기반 VFX 확장 멤버 (EffectLayer.sph 기반)
    EffectLayer      sphLayer;              // 현재 재생 중인 SPH 레이어 정의 (sph/coreColor/edgeColor 포함)
    std::string      effectName;            // 부모 EffectDef::name (Dragon_MegaBreath 등 분기 비교용)
    int              currentPhaseIndex = 0; // 현재 페이즈 인덱스
    bool             useSequence = false;   // 시퀀스 모드 활성 여부
    float            masterCPFallY = 0.f;   // 메테오용 마스터 CP Y 위치 (낙하 추적)
    XMFLOAT3         masterCPPos = {};      // 메테오 마스터 CP 현재 위치
    float            masterCPFallSpeed = 15.f; // 낙하 속도 (units/s)

    // Wave 모드 상태
    bool             isWaveMode  = false;
    float            waveDist    = 0.f;    // 현재 이동 거리
    bool             waveStopped = false;  // 충돌/최대거리로 멈춘 여부

    // SSF bilateral blur 적용 여부 (EffectLayer.useSSF에서 설정)
    bool             useBlur     = false;

    // 플레이어 스킬 여부: SpawnSPHLayer → 인자, SpawnEffect(적 투사체) → false
    // false인 슬롯은 SSF 파이프라인에서 제외되고 빌보드로 별도 렌더
    bool             isPlayerEffect = false;

    // 스폰 순서 — GetDominantFluidColors에서 가장 최근 슬롯 우선 선택에 사용
    uint32_t         spawnGeneration = 0;

    // 경량 이미터 (LightEmitterSystem) 지원
    std::unique_ptr<LightEmitterSystem> pLightEmitter;
    EffectLayer      lightLayer;        // useLightEmitter=true 일 때만 유효
    bool             useLightEmitter = false;

    // 다중 레이어 연결 (SpawnEffectDef가 레이어마다 슬롯을 만들 때 연결)
    // StopEffect/TrackEffect 가 체인을 따라 모든 레이어에 적용됨
    int              nextLinkedId = -1;
};

class FluidSkillVFXManager
{
public:
    static constexpr int MAX_EFFECTS = 64;

    void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
              CDescriptorHeap* pDescriptorHeap, UINT nStartDescIndex);

    // 새 이펙트 생성, 슬롯 ID 반환 (-1: 실패)
    int SpawnEffect(const XMFLOAT3& origin, const XMFLOAT3& direction,
                    const FluidSkillVFXDef& def);

    // SPH 레이어 직접 스폰 (EffectLayer.sph 기반, 내부 라우팅 + SpawnFireTrailEffect용)
    // isPlayerEffect=true  → SSF 파이프라인 (플레이어 스킬)
    // isPlayerEffect=false → 빌보드 렌더 (적 스킬) — SSF 색상과 완전 분리
    int SpawnSPHLayer(const XMFLOAT3& origin, const XMFLOAT3& direction,
                      const std::string& effectName,
                      const EffectLayer& layer, bool isPlayerEffect = true);

    // EffectDef/EffectLayer 기반 이펙트 생성 (새 이미터 시스템)
    // SPH 레이어 → SpawnSPHLayer, 경량 이미터 레이어 → LightEmitterSystem
    int SpawnEffectLayer(const XMFLOAT3& origin, const XMFLOAT3& direction,
                         const std::string& effectName,
                         const EffectLayer& layer, bool isPlayerEffect = true);
    int SpawnEffectDef(const XMFLOAT3& origin, const XMFLOAT3& direction,
                       const EffectDef& def, bool isPlayerEffect = true);

    // 매 프레임 투사체 위치/방향 추적
    void TrackEffect(int id, const XMFLOAT3& origin, const XMFLOAT3& direction);

    // 투사체 소멸 시 이펙트 정지
    void StopEffect(int id);

    // 피격 시 파티클을 충돌 위치로 수렴시킨 뒤 소멸
    void ImpactEffect(int id, const XMFLOAT3& impactPos);

    // 피격 시 CP 제거 + 방사형 폭발 (R/우클릭 전용)
    void ExplodeEffect(int id, const XMFLOAT3& impactPos);

    // Wave 모드 API
    void     StopWave(int id);                    // 파도를 충돌 위치에서 멈춤
    XMFLOAT3 GetWaveFrontPos(int id) const;       // 파도 선두 현재 위치 (origin + dir*dist)
    float    GetWaveDist(int id) const;           // 파도가 이동한 거리
    XMFLOAT3 GetWaveOrigin(int id) const;         // 파도 스폰 위치 (back wall)
    XMFLOAT3 GetWaveDir(int id) const;            // 파도 진행 방향
    bool     IsWaveActive(int id) const;          // 파도가 아직 이동 중이면 true
    bool     IsPointInWave(int id, const XMFLOAT3& point) const; // 점이 VFX 파도 영역 안에 있으면 true

    // Q 스킬 불꽃 자국 VFX: 지정 위치에 바닥 화염 파티클 스폰, ID 반환
    // waveRight: 파도 진행 방향의 직교(좌우) 벡터 — 파티클을 파도 폭만큼 분산
    int SpawnFireTrailEffect(const XMFLOAT3& pos, const XMFLOAT3& waveRight,
                             float halfWidth, float lifetime = 3.0f);

    void Update(float deltaTime);

    // GPU SPH dispatch (Render 전에 호출, Beam 모드는 CPU update 후 GPU 복사)
    void DispatchSPH(ID3D12GraphicsCommandList* pCmdList, float deltaTime);

    void Render(ID3D12GraphicsCommandList* pCommandList,
                const XMFLOAT4X4& viewProj, const XMFLOAT3& camRight, const XMFLOAT3& camUp);

    // Screen-Space Fluid: 구체 깊이 렌더링
    // blurOnly=false: blur 미사용 슬롯만, blurOnly=true: blur 슬롯만 렌더
    void RenderDepth(ID3D12GraphicsCommandList* pCmdList,
                     const XMFLOAT4X4& viewProjTransposed,
                     const XMFLOAT4X4& viewTransposed,
                     const XMFLOAT3& camRight,
                     const XMFLOAT3& camUp,
                     float projA, float projB,
                     ScreenSpaceFluid* pSSF,
                     bool blurOnly = false);

    // Screen-Space Fluid: 두께 렌더링 (RenderDepth 이후 호출, 깊이 테스트 없음)
    void RenderThicknessOnly(ID3D12GraphicsCommandList* pCmdList,
                              ScreenSpaceFluid* pSSF,
                              bool blurOnly = false);

    // 현재 블러/비블러 활성 슬롯 존재 여부 (플레이어 슬롯만 집계)
    bool HasActiveSlots(bool blurOnly) const;

    // 적 전용 슬롯 빌보드 렌더 (SSF 파이프라인 완료 후 호출)
    void RenderEnemyEffects(ID3D12GraphicsCommandList* pCommandList,
                            const XMFLOAT4X4& viewProj,
                            const XMFLOAT3& camRight,
                            const XMFLOAT3& camUp);

    // 플레이어 LightEmitter 슬롯만 빌보드 렌더 (SSF 브랜치에서 SPH와 분리 렌더)
    void RenderPlayerLightEmitters(ID3D12GraphicsCommandList* pCommandList,
                                   const XMFLOAT4X4& viewProj,
                                   const XMFLOAT3& camRight,
                                   const XMFLOAT3& camUp);

    // 원소별 내장 VFX 정의 반환 (룬 combo에 따라 파라미터 조정)
    static FluidSkillVFXDef GetVFXDef(ElementType element, const RuneCombo& combo = {}, float chargeRatio = 0.0f);

    // 슬롯의 현재 페이즈 인덱스 조회 (-1: 비활성/미시작/유효하지 않은 ID)
    // MeteorBehavior 등 외부 로직에서 VFX 페이즈 전환 시점에 동기화하기 위해 사용
    int GetSlotPhase(int id) const;

    // 현재 활성 슬롯의 대표 유체 색상 (composite fluidColor용)
    // blurOnly=true 시 useBlur 슬롯만 대상, false 시 비blur 슬롯 우선(전체 fallback)
    XMFLOAT4 GetDominantFluidColor() const;
    FluidElementColor GetDominantFluidColors() const;
    FluidElementColor GetDominantFluidColors(bool blurOnly) const;

private:
    void PushControlPoints(FluidVFXSlot& slot) const;

    // 시퀀스 기반 페이즈 전환 로직
    void UpdatePhase(FluidVFXSlot& slot, float dt);
    void UpdateOrbitalCPs(FluidVFXSlot& slot, float dt);

    // Player 우선 슬롯 할당 — free 가 없을 때 isPlayerEffect=true 면 가장 오래된 non-player 강제 evict.
    //   결과: 보스 VFX 가 풀 채워도 플레이어 스킬 시각이 보장됨. 비플레이어 스폰은 free 만 사용.
    int FindSlotForSpawn(bool isPlayerEffect);

    // 프레임 단위 frustum 캐시 — Update 시작 시 Camera 로부터 갱신, Render 에서 슬롯 cull 에 사용.
    //   m_frustumValid=false 면 cull 없이 모두 렌더 (안전 fallback).
    void UpdateCachedFrustum();
    bool IsSlotVisible(const FluidVFXSlot& slot) const;
    DirectX::BoundingFrustum m_cachedFrustum;
    bool                     m_frustumValid = false;

    std::array<FluidVFXSlot, MAX_EFFECTS> m_Slots;
    uint32_t         m_nextSpawnGeneration = 0;
    ID3D12Device*    m_pDevice    = nullptr; // 경량 이미터 지연 초기화용
    CDescriptorHeap* m_pDescHeap  = nullptr;
    UINT             m_nStartDesc = 0;
};
