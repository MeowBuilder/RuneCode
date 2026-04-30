#pragma once
//==============================================================================
// VFXManager — VFX/파티클 시스템 통합 파사드
//------------------------------------------------------------------------------
// 두 개의 FluidSkillVFXManager(플레이어 SSF / 적 빌보드)를 내부에서 소유하고
// EffectRegistry 기반의 스폰 API를 단일 진입점으로 노출한다.
//
// ID 체계:
//   - PLAYER_ID_BASE (0)         ~ PLAYER_ID_BASE + MAX_EFFECTS - 1: 플레이어 슬롯
//   - ENEMY_ID_BASE  (1000)      ~ ENEMY_ID_BASE  + MAX_EFFECTS - 1: 적 슬롯
//   - 외부에는 항상 전역 ID만 노출하며, 내부에서 슬롯 ID로 변환한다.
//
// 모든 호출은 ID로 어느 매니저에 위임할지 결정되므로 호출자는
// 슬롯이 어느 매니저에서 발급되었는지 신경 쓰지 않아도 된다.
//==============================================================================

#include "stdafx.h"
#include "FluidSkillVFXManager.h"
#include <memory>
#include <string>
#include <string_view>

class CDescriptorHeap;
class ScreenSpaceFluid;

class VFXManager {
public:
    static constexpr int PLAYER_ID_BASE = 0;
    static constexpr int ENEMY_ID_BASE  = 1000;

    void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
              CDescriptorHeap* pDescHeap, UINT& nNextDescIndex);

    // ─── EffectRegistry 기반 스폰 (새 방식) ────────────────────────────────
    // effectId가 EffectRegistry에 등록되어 있으면 EffectDef → SpawnEffectDef 경로로
    // 위임한다. 등록되어 있지 않으면 -1 반환.
    int Spawn(std::string_view effectId, const XMFLOAT3& pos, const XMFLOAT3& dir,
              uint32_t runeFlags = 0, bool isPlayer = true);

    // ─── EffectLayer 직접 스폰 (단일 레이어 경량 이미터 / SPH 모두 지원) ────
    // EffectRegistry에 등록 없이 즉석 EffectLayer를 스폰할 때 사용.
    // 반환된 전역 ID로 Track/Stop 가능.
    int SpawnLightLayer(const XMFLOAT3& pos, const XMFLOAT3& dir,
                        const EffectLayer& layer, bool isPlayer = true);

    // ─── 적 투사체 호환 (FluidSkillVFXDef 기반) ─────────────────────────────
    // ProjectileManager 기존 경로 유지를 위해 호환 API 그대로 노출
    int SpawnEffect(const XMFLOAT3& origin, const XMFLOAT3& direction,
                    const FluidSkillVFXDef& def);

    // ─── 생명주기 관리 ─────────────────────────────────────────────────────
    void Track(int id, const XMFLOAT3& pos, const XMFLOAT3& dir);
    void Stop(int id);
    void Impact(int id, const XMFLOAT3& impactPos);
    void Explode(int id, const XMFLOAT3& impactPos);

    // ─── 매 프레임 호출 ────────────────────────────────────────────────────
    void Update(float dt);
    void DispatchSPH(ID3D12GraphicsCommandList* pCmdList, float dt);
    void Render(ID3D12GraphicsCommandList* pCmdList, const XMFLOAT4X4& viewProj,
                const XMFLOAT3& camRight, const XMFLOAT3& camUp);

    // ─── SSF 파이프라인 지원 ──────────────────────────────────────────────
    void RenderDepth(ID3D12GraphicsCommandList* pCmdList,
                     const XMFLOAT4X4& viewProjT,
                     const XMFLOAT4X4& viewT,
                     const XMFLOAT3& camRight,
                     const XMFLOAT3& camUp,
                     float projA, float projB,
                     ScreenSpaceFluid* pSSF,
                     bool blurOnly = false);

    void RenderThicknessOnly(ID3D12GraphicsCommandList* pCmdList,
                             ScreenSpaceFluid* pSSF,
                             bool blurOnly = false);

    bool HasActiveSlots(bool blurOnly) const;

    void RenderEnemyEffects(ID3D12GraphicsCommandList* pCmdList,
                            const XMFLOAT4X4& viewProj,
                            const XMFLOAT3& camRight,
                            const XMFLOAT3& camUp);

    // ─── 색상 쿼리 (SSF composite용) ──────────────────────────────────────
    XMFLOAT4 GetDominantFluidColor() const;
    FluidElementColor GetDominantFluidColors(bool blurOnly) const;

    // ─── Wave 모드 API (FluidSkillVFXManager API 그대로 위임) ──────────────
    void     StopWave(int id);
    XMFLOAT3 GetWaveFrontPos(int id) const;
    float    GetWaveDist(int id) const;
    bool     IsWaveActive(int id) const;
    bool     IsPointInWave(int id, const XMFLOAT3& point) const;

    // ─── Spawn 변형들 (기존 API 호환) ─────────────────────────────────────
    int SpawnFireTrailEffect(const XMFLOAT3& pos, const XMFLOAT3& waveRight,
                             float halfWidth, float lifetime = 3.0f);

    // ─── 슬롯 판별 ────────────────────────────────────────────────────────
    bool IsPlayerSlot(int id) const;
    bool IsEnemySlot(int id) const;

    // ─── 내부 매니저 직접 접근 (점진적 마이그레이션 호환) ───────────────────
    // 신규 코드에서는 사용 금지 — 기존 SetVFXManager(FluidSkillVFXManager*)
    // 패턴을 위한 임시 게터.
    FluidSkillVFXManager* GetPlayerVFX() { return m_pPlayerVFX.get(); }
    FluidSkillVFXManager* GetEnemyVFX()  { return m_pEnemyVFX.get(); }

private:
    bool IsPlayerId(int id) const { return id >= PLAYER_ID_BASE && id < PLAYER_ID_BASE + FluidSkillVFXManager::MAX_EFFECTS; }
    bool IsEnemyId(int id)  const { return id >= ENEMY_ID_BASE  && id < ENEMY_ID_BASE  + FluidSkillVFXManager::MAX_EFFECTS; }

    int  ToLocalId(int id) const;            // 전역 id → 슬롯 내 local id
    int  ToGlobalId(int localId, bool isPlayer) const;

    FluidSkillVFXManager*       GetManager(int id);
    const FluidSkillVFXManager* GetManager(int id) const;

    std::unique_ptr<FluidSkillVFXManager> m_pPlayerVFX;
    std::unique_ptr<FluidSkillVFXManager> m_pEnemyVFX;
};
