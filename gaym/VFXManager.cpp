#include "stdafx.h"
#include "VFXManager.h"
#include "EffectRegistry.h"
#include "DescriptorHeap.h"

//==============================================================================
// VFXManager — 두 개의 FluidSkillVFXManager(player/enemy)를 묶는 파사드
//------------------------------------------------------------------------------
// 외부에는 글로벌 ID(PLAYER_ID_BASE/ENEMY_ID_BASE 기반)만 노출하고,
// 내부에서 슬롯 매니저로 분기 위임한다.
//==============================================================================

void VFXManager::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmdList,
                      CDescriptorHeap* pDescHeap, UINT& nNextDescIndex)
{
    m_pPlayerVFX = std::make_unique<FluidSkillVFXManager>();
    m_pEnemyVFX  = std::make_unique<FluidSkillVFXManager>();

    // 플레이어 전용 매니저 (SSF 파이프라인)
    UINT nPlayerStart = nNextDescIndex;
    nNextDescIndex   += FluidSkillVFXManager::MAX_EFFECTS * 2;
    m_pPlayerVFX->Init(pDevice, pCmdList, pDescHeap, nPlayerStart);

    // 적 전용 매니저 (빌보드 렌더, SSF 분리)
    UINT nEnemyStart = nNextDescIndex;
    nNextDescIndex   += FluidSkillVFXManager::MAX_EFFECTS * 2;
    m_pEnemyVFX->Init(pDevice, pCmdList, pDescHeap, nEnemyStart);

    OutputDebugStringA("[VFXManager] Initialized (player + enemy)\n");
}

// ─── ID 변환 ────────────────────────────────────────────────────────────────

int VFXManager::ToLocalId(int id) const
{
    if (IsPlayerId(id)) return id - PLAYER_ID_BASE;
    if (IsEnemyId(id))  return id - ENEMY_ID_BASE;
    return -1;
}

int VFXManager::ToGlobalId(int localId, bool isPlayer) const
{
    if (localId < 0) return -1;
    return localId + (isPlayer ? PLAYER_ID_BASE : ENEMY_ID_BASE);
}

FluidSkillVFXManager* VFXManager::GetManager(int id)
{
    if (IsPlayerId(id)) return m_pPlayerVFX.get();
    if (IsEnemyId(id))  return m_pEnemyVFX.get();
    return nullptr;
}

const FluidSkillVFXManager* VFXManager::GetManager(int id) const
{
    if (IsPlayerId(id)) return m_pPlayerVFX.get();
    if (IsEnemyId(id))  return m_pEnemyVFX.get();
    return nullptr;
}

bool VFXManager::IsPlayerSlot(int id) const { return IsPlayerId(id); }
bool VFXManager::IsEnemySlot(int id)  const { return IsEnemyId(id);  }

// ─── 스폰 API ───────────────────────────────────────────────────────────────

int VFXManager::Spawn(std::string_view effectId, const XMFLOAT3& pos, const XMFLOAT3& dir,
                      uint32_t runeFlags, bool isPlayer)
{
    auto& reg = EffectRegistry::Get();
    std::string name(effectId);
    if (!reg.HasEffect(name)) return -1;

    EffectDef def = reg.GetEffect(name, runeFlags);

    FluidSkillVFXManager* mgr = isPlayer ? m_pPlayerVFX.get() : m_pEnemyVFX.get();
    if (!mgr) return -1;

    int local = mgr->SpawnEffectDef(pos, dir, def, isPlayer);
    return ToGlobalId(local, isPlayer);
}

int VFXManager::SpawnLightLayer(const XMFLOAT3& pos, const XMFLOAT3& dir,
                                const EffectLayer& layer, bool isPlayer)
{
    FluidSkillVFXManager* mgr = isPlayer ? m_pPlayerVFX.get() : m_pEnemyVFX.get();
    if (!mgr) return -1;
    // 즉석 EffectLayer 스폰: EffectRegistry 등록이 없으므로 effectName은 빈 문자열
    int local = mgr->SpawnEffectLayer(pos, dir, std::string(), layer, isPlayer);
    return ToGlobalId(local, isPlayer);
}

int VFXManager::SpawnEffect(const XMFLOAT3& origin, const XMFLOAT3& direction,
                            const FluidSkillVFXDef& def)
{
    // FluidSkillVFXDef는 적 투사체 전용 (SSF 미적용)
    if (!m_pEnemyVFX) return -1;
    int local = m_pEnemyVFX->SpawnEffect(origin, direction, def);
    return ToGlobalId(local, /*isPlayer*/false);
}

// ─── 생명주기 ───────────────────────────────────────────────────────────────

void VFXManager::Track(int id, const XMFLOAT3& pos, const XMFLOAT3& dir)
{
    if (auto* m = GetManager(id))
        m->TrackEffect(ToLocalId(id), pos, dir);
}

void VFXManager::Stop(int id)
{
    if (auto* m = GetManager(id))
        m->StopEffect(ToLocalId(id));
}

void VFXManager::Impact(int id, const XMFLOAT3& impactPos)
{
    if (auto* m = GetManager(id))
        m->ImpactEffect(ToLocalId(id), impactPos);
}

void VFXManager::Explode(int id, const XMFLOAT3& impactPos)
{
    if (auto* m = GetManager(id))
        m->ExplodeEffect(ToLocalId(id), impactPos);
}

// ─── 프레임 업데이트/렌더 ────────────────────────────────────────────────────

void VFXManager::Update(float dt)
{
    if (m_pPlayerVFX) m_pPlayerVFX->Update(dt);
    if (m_pEnemyVFX)  m_pEnemyVFX->Update(dt);
}

void VFXManager::DispatchSPH(ID3D12GraphicsCommandList* pCmdList, float dt)
{
    if (m_pPlayerVFX) m_pPlayerVFX->DispatchSPH(pCmdList, dt);
    if (m_pEnemyVFX)  m_pEnemyVFX->DispatchSPH(pCmdList, dt);
}

void VFXManager::Render(ID3D12GraphicsCommandList* pCmdList, const XMFLOAT4X4& viewProj,
                        const XMFLOAT3& camRight, const XMFLOAT3& camUp)
{
    if (m_pPlayerVFX) m_pPlayerVFX->Render(pCmdList, viewProj, camRight, camUp);
    if (m_pEnemyVFX)  m_pEnemyVFX->Render(pCmdList, viewProj, camRight, camUp);
}

// ─── SSF 지원 ───────────────────────────────────────────────────────────────

void VFXManager::RenderDepth(ID3D12GraphicsCommandList* pCmdList,
                             const XMFLOAT4X4& viewProjT,
                             const XMFLOAT4X4& viewT,
                             const XMFLOAT3& camRight,
                             const XMFLOAT3& camUp,
                             float projA, float projB,
                             ScreenSpaceFluid* pSSF,
                             bool blurOnly)
{
    // SSF는 플레이어 전용 매니저에서만 처리
    if (m_pPlayerVFX)
        m_pPlayerVFX->RenderDepth(pCmdList, viewProjT, viewT, camRight, camUp, projA, projB, pSSF, blurOnly);
}

void VFXManager::RenderThicknessOnly(ID3D12GraphicsCommandList* pCmdList,
                                     ScreenSpaceFluid* pSSF,
                                     bool blurOnly)
{
    if (m_pPlayerVFX)
        m_pPlayerVFX->RenderThicknessOnly(pCmdList, pSSF, blurOnly);
}

bool VFXManager::HasActiveSlots(bool blurOnly) const
{
    return m_pPlayerVFX && m_pPlayerVFX->HasActiveSlots(blurOnly);
}

void VFXManager::RenderEnemyEffects(ID3D12GraphicsCommandList* pCmdList,
                                    const XMFLOAT4X4& viewProj,
                                    const XMFLOAT3& camRight,
                                    const XMFLOAT3& camUp)
{
    if (m_pEnemyVFX)
        m_pEnemyVFX->RenderEnemyEffects(pCmdList, viewProj, camRight, camUp);
}

// ─── 색상 쿼리 ───────────────────────────────────────────────────────────────

XMFLOAT4 VFXManager::GetDominantFluidColor() const
{
    if (m_pPlayerVFX) return m_pPlayerVFX->GetDominantFluidColor();
    return XMFLOAT4(1, 1, 1, 1);
}

FluidElementColor VFXManager::GetDominantFluidColors(bool blurOnly) const
{
    if (m_pPlayerVFX) return m_pPlayerVFX->GetDominantFluidColors(blurOnly);
    return FluidElementColor{};
}

// ─── Wave API ───────────────────────────────────────────────────────────────

void VFXManager::StopWave(int id)
{
    if (auto* m = GetManager(id))
        m->StopWave(ToLocalId(id));
}

XMFLOAT3 VFXManager::GetWaveFrontPos(int id) const
{
    if (auto* m = GetManager(id))
        return m->GetWaveFrontPos(ToLocalId(id));
    return XMFLOAT3(0, 0, 0);
}

float VFXManager::GetWaveDist(int id) const
{
    if (auto* m = GetManager(id))
        return m->GetWaveDist(ToLocalId(id));
    return 0.0f;
}

bool VFXManager::IsWaveActive(int id) const
{
    if (auto* m = GetManager(id))
        return m->IsWaveActive(ToLocalId(id));
    return false;
}

bool VFXManager::IsPointInWave(int id, const XMFLOAT3& point) const
{
    if (auto* m = GetManager(id))
        return m->IsPointInWave(ToLocalId(id), point);
    return false;
}

// ─── Spawn 변형들 ───────────────────────────────────────────────────────────

int VFXManager::SpawnFireTrailEffect(const XMFLOAT3& pos, const XMFLOAT3& waveRight,
                                     float halfWidth, float lifetime)
{
    // 화염 자국은 플레이어 스킬 한정
    if (!m_pPlayerVFX) return -1;
    int local = m_pPlayerVFX->SpawnFireTrailEffect(pos, waveRight, halfWidth, lifetime);
    return ToGlobalId(local, /*isPlayer*/true);
}
