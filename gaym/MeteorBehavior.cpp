#include "stdafx.h"
#include "MeteorBehavior.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "SkillComponent.h"
#include "Scene.h"
#include "Room.h"
#include "EnemyComponent.h"
#include <cmath>
#include <algorithm>

MeteorBehavior::MeteorBehavior()
    : m_SkillData(FireSkillPresets::Meteor())
{
}

MeteorBehavior::MeteorBehavior(const SkillData& customData)
    : m_SkillData(customData)
{
}

MeteorBehavior::~MeteorBehavior()
{
    // 큐브 코어 상수버퍼 unmap
    for (int i = 0; i < 2; ++i)
    {
        if (m_pCoreCBUpload[i] && m_pMappedCoreCB[i])
        {
            m_pCoreCBUpload[i]->Unmap(0, nullptr);
            m_pMappedCoreCB[i] = nullptr;
        }
    }

    // 공유 메시 참조 해제 (AddRef/Release 패턴)
    if (m_pCoreMesh)
    {
        m_pCoreMesh->Release();
        m_pCoreMesh = nullptr;
    }
}

// ─── 디바이스 리소스 바인딩 ──────────────────────────────────────────────────
void MeteorBehavior::SetDeviceResources(ID3D12Device* pDevice,
                                        ID3D12GraphicsCommandList* pCmdList,
                                        CDescriptorHeap* pDescHeap,
                                        UINT nDescStart)
{
    m_pDevice      = pDevice;
    m_pInitCmdList = pCmdList;
    m_pDescHeap    = pDescHeap;
    m_nDescStart   = nDescStart;
    InitCoreResources();
}

void MeteorBehavior::InitCoreResources()
{
    if (m_bCoreInited || !m_pDevice || !m_pInitCmdList || !m_pDescHeap) return;

    // 공유 큐브 메시 생성 (두 큐브 모두 동일 메시 사용 — Stella Octangula 효과)
    m_pCoreMesh = new CubeMesh(m_pDevice, m_pInitCmdList, CORE_SIZE, CORE_SIZE, CORE_SIZE);
    m_pCoreMesh->AddRef();

    // 큐브당 상수버퍼 생성 (256 byte alignment)
    m_nCoreCBSize = (sizeof(MeteorCoreCB) + 255) & ~255;

    for (int i = 0; i < 2; ++i)
    {
        ID3D12Resource* pRaw = CreateBufferResource(m_pDevice, m_pInitCmdList, nullptr,
                                                    m_nCoreCBSize,
                                                    D3D12_HEAP_TYPE_UPLOAD,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr);
        m_pCoreCBUpload[i].Attach(pRaw);

        // 영구 매핑 (range null = 전체)
        HRESULT hr = m_pCoreCBUpload[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_pMappedCoreCB[i]));
        if (FAILED(hr) || !m_pMappedCoreCB[i])
        {
            OutputDebugStringA("[Meteor] Failed to map core CB upload heap\n");
            return;
        }
        memset(m_pMappedCoreCB[i], 0, m_nCoreCBSize);

        // CBV 생성
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = m_pCoreCBUpload[i]->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes    = m_nCoreCBSize;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_pDescHeap->GetCPUHandle(m_nDescStart + i);
        m_pDevice->CreateConstantBufferView(&cbvDesc, cpuHandle);
    }

    m_bCoreInited = true;
    OutputDebugStringA("[Meteor] Core cube rendering resources initialized\n");
}

// ─── Execute: 메테오 발사 ────────────────────────────────────────────────────
void MeteorBehavior::Execute(GameObject* caster, const DirectX::XMFLOAT3& targetPosition, float damageMultiplier)
{
    m_bIsFinished = false;

    if (!m_pVFXManager)
    {
        OutputDebugString(L"[Meteor] Warning: No VFXManager set!\n");
        m_bIsFinished = true;
        return;
    }

    // 1. 타겟 위치 결정 (마우스 클릭 위치 또는 캐스터 전방 폴백)
    XMFLOAT3 targetPos = targetPosition;
    if (fabsf(targetPos.x) < 0.001f && fabsf(targetPos.z) < 0.001f)
    {
        if (caster && caster->GetTransform())
        {
            XMFLOAT3 casterPos = caster->GetTransform()->GetPosition();
            XMVECTOR look = caster->GetTransform()->GetLook();
            look = XMVectorSetY(look, 0.f);
            look = XMVector3Normalize(look);

            XMVECTOR targetV = XMVectorAdd(
                XMLoadFloat3(&casterPos),
                XMVectorScale(look, METEOR_FORWARD_DIST)
            );
            XMStoreFloat3(&targetPos, targetV);
        }
    }

    // 2. 스폰 위치 = 타겟 상공
    XMFLOAT3 spawnPos = { targetPos.x, targetPos.y + METEOR_SPAWN_HEIGHT, targetPos.z };
    XMFLOAT3 direction = { 0.f, -1.f, 0.f };

    // 3. 룬 + 원소 스탯 수집
    uint32_t runeFlags = GetRuneFlags(caster);
    SkillStats stats;
    if (caster) {
        auto* pSkillComp = caster->GetComponent<SkillComponent>();
        if (pSkillComp && m_slot != SkillSlot::Count)
            stats = pSkillComp->BuildSkillStats(m_slot, m_SkillData.activationType);
    }

    // 4. 원소 색상 오버라이드 헬퍼
    auto applyElement = [](EffectDef& def, ElementType e, bool reduceCount) {
        FluidElementColor ec = FluidElementColors::Get(e);
        def.element = e;
        for (auto& l : def.layers) {
            l.element   = e;
            l.coreColor = ec.coreColor;
            l.edgeColor = ec.edgeColor;
            if (reduceCount) {
                bool isSPH = (l.type >= EmitterType::SPH_Attract &&
                              l.type <= EmitterType::SPH_Beam);
                if (isSPH)
                    l.sph.particleCount = max(100, (int)(l.sph.particleCount * 0.6f));
                else
                    l.particleCount = max(100, (int)(l.particleCount * 0.6f));
            }
        }
    };

    // 5. 낙하 중 아우라 없음 — 큐브 메쉬 + Trail로만 구성
    m_vfxId = -1;

    // 6. 혜성 꼬리 Trail — 큐브 TOP에서 위쪽(낙하 반대)으로 좁게 분출
    //    파티클 수명(0.3-0.7s) 동안 큐브가 아래로 멀어지면서 Trail 형성
    {
        // Trail 기준점: 큐브 상단면 (낙하 방향 반대쪽 = 뒤쪽)
        XMFLOAT3 topPos = { targetPos.x, spawnPos.y + CORE_SIZE * 0.5f, targetPos.z };
        XMFLOAT3 upDir  = { 0.f, 1.f, 0.f };

        EffectDef trailDef = EffectRegistry::Get().GetEffect("R_MeteorTrail", runeFlags);
        if (!stats.elementSet.empty())
            applyElement(trailDef, stats.elementSet[0], false);
        if (!trailDef.layers.empty())
            m_trailVfxId = m_pVFXManager->SpawnEffectLayer(topPos, upDir, trailDef.name,
                                                            trailDef.layers[0], /*isPlayerEffect*/true);

        EffectDef outerDef = EffectRegistry::Get().GetEffect("R_MeteorTrailOuter");
        if (!outerDef.layers.empty())
            m_trailOuterId = m_pVFXManager->SpawnEffectLayer(topPos, upDir, outerDef.name,
                                                              outerDef.layers[0], /*isPlayerEffect*/true);
    }

    // 7. 추가 원소 VFX (서브) — 메인 색상 변형
    m_extraVFXIds.clear();
    for (size_t ei = 1; ei < stats.elementSet.size(); ++ei)
    {
        EffectDef extraDef = EffectRegistry::Get().GetEffect("R_Meteor", runeFlags);
        applyElement(extraDef, stats.elementSet[ei], /*reduceCount*/true);
        int eid = m_pVFXManager->SpawnEffectDef(spawnPos, direction, extraDef, /*isPlayerEffect*/true);
        if (eid >= 0) m_extraVFXIds.push_back(eid);
    }

    // 8. 상태 초기화 (전방 화염 스폰 전에 ID 초기화)
    m_spawnPos    = spawnPos;
    m_targetPos   = targetPos;
    m_damageMult  = damageMultiplier > 0.f ? damageMultiplier : 1.f;
    m_elapsed     = 0.f;
    m_bExploded   = false;
    m_lastPhase   = -1;
    m_rotY0       = 0.f;
    m_rotX0       = 0.f;
    m_rotY1       = 0.f;
    m_rotZ1       = 0.f;
    m_impactVfxId = -1;
    m_groundFireId= -1;
    m_frontFireId = -1;
    m_bIsFinished = false;

    wchar_t buf[256];
    swprintf_s(buf, 256,
        L"[Meteor] Execute: vfxId=%d, trailId=%d, target=(%.1f,%.1f,%.1f), spawn=(%.1f,%.1f,%.1f), runeFlags=0x%X\n",
        m_vfxId, m_trailVfxId,
        targetPos.x, targetPos.y, targetPos.z,
        spawnPos.x, spawnPos.y, spawnPos.z, runeFlags);
    OutputDebugString(buf);
}

// ─── Update: 페이즈 전환 검출 + 큐브 회전 ────────────────────────────────────
void MeteorBehavior::Update(float deltaTime)
{
    if (m_bIsFinished) return;

    m_elapsed += deltaTime;

    // 두 큐브 독립 다축 회전 (다른 속도/방향 → 역동적 외관)
    m_rotY0 += 1.8f * deltaTime;   // Cube0: 시계방향 Y
    m_rotX0 += 0.7f * deltaTime;   // Cube0: X축 서서히 기울기
    m_rotY1 -= 1.3f * deltaTime;   // Cube1: 반시계방향 Y
    m_rotZ1 += 1.1f * deltaTime;   // Cube1: Z 롤링

    // 폭발 전 — 큐브 위치 계산 후 트레일/전방화염 마스터 CP 갱신
    if (!m_bExploded && m_pVFXManager)
    {
        float fallT    = (m_elapsed < FALL_DURATION) ? m_elapsed : FALL_DURATION;
        float cubeY    = m_spawnPos.y - MASTER_CP_FALL_SPEED * fallT;

        // Trail: 큐브 TOP 위치 추적 (낙하 반대 ↑) → 파티클이 뒤에 떠 있으면서 Trail 형성
        XMFLOAT3 upDir2 = { 0.f, 1.f, 0.f };
        XMFLOAT3 topPos = { m_targetPos.x, cubeY + CORE_SIZE * 0.5f, m_targetPos.z };
        if (m_trailVfxId >= 0)
            m_pVFXManager->TrackEffect(m_trailVfxId, topPos, upDir2);
        if (m_trailOuterId >= 0)
            m_pVFXManager->TrackEffect(m_trailOuterId, topPos, upDir2);
    }

    // 메인 VFX 슬롯의 페이즈 전환 검출 → Phase0(낙하) → Phase1(폭발) 전환 시 OnImpact()
    int curPhase = (m_pVFXManager && m_vfxId >= 0)
                 ? m_pVFXManager->GetSlotPhase(m_vfxId)
                 : -1;

    if (!m_bExploded && curPhase >= 1 && m_lastPhase < 1)
    {
        OnImpact();
    }
    m_lastPhase = curPhase;

    // 폴백: VFX 슬롯이 없거나 추적 실패 시 elapsed 기반 트리거
    if (!m_bExploded && m_elapsed >= FALL_DURATION + 0.15f)
    {
        OnImpact();
    }
}

// ─── 충돌 시점 처리 ──────────────────────────────────────────────────────────
void MeteorBehavior::OnImpact()
{
    m_bExploded   = true;
    m_bIsFinished = true; // 데미지 1회 + Behavior 종료 (VFX는 자체 페이즈 진행)

    // 폭발 데미지 (경직 허용)
    ApplyExplosionDamage(m_SkillData.damage * m_damageMult, EXPLODE_RADIUS, true);

    // 전방 화염 + Trail 정지 (큐브가 사라지면 꼬리도 같이 사라짐)
    if (m_pVFXManager)
    {
        if (m_frontFireId >= 0)  { m_pVFXManager->StopEffect(m_frontFireId);  m_frontFireId  = -1; }
        if (m_trailVfxId >= 0)   { m_pVFXManager->StopEffect(m_trailVfxId);   m_trailVfxId   = -1; }
        if (m_trailOuterId >= 0) { m_pVFXManager->StopEffect(m_trailOuterId); m_trailOuterId = -1; }
    }

    if (m_pVFXManager)
    {
        XMFLOAT3 up = { 0.f, 1.f, 0.f };

        // 충격 링 + 폭발구 스폰
        EffectDef impDef = EffectRegistry::Get().GetEffect("R_MeteorImpact");
        m_impactVfxId = m_pVFXManager->SpawnEffectDef(m_targetPos, up, impDef, /*isPlayerEffect*/true);

        // 바닥 화염 확산 스폰
        EffectDef fireDef = EffectRegistry::Get().GetEffect("R_MeteorGroundFire");
        m_groundFireId = m_pVFXManager->SpawnEffectDef(m_targetPos, up, fireDef, /*isPlayerEffect*/true);
    }

    OutputDebugStringA("[Meteor] IMPACT! Explosion + ring + ground fire spawned.\n");
}

// ─── Render: 큐브 코어 두 개를 겹쳐 그려 화염구 핵 표현 ─────────────────────
void MeteorBehavior::Render(ID3D12GraphicsCommandList* pCmdList)
{
    if (!m_bCoreInited || m_bExploded || !m_pCoreMesh || !m_pDescHeap) return;
    if (m_trailVfxId < 0 && m_trailOuterId < 0) return; // 미발동 상태

    // 큐브 현재 Y 좌표: spawnPos.y에서 MASTER_CP_FALL_SPEED * elapsed만큼 하강
    // (Windows.h가 min 매크로를 정의하므로 (min)(...) 괄호 트릭 또는 직접 비교)
    float fallT = (m_elapsed < FALL_DURATION) ? m_elapsed : FALL_DURATION;
    float coreY = m_spawnPos.y - MASTER_CP_FALL_SPEED * fallT;
    float coreX = m_targetPos.x;
    float coreZ = m_targetPos.z;

    // 머티리얼: 어두운 적-갈색 + 알파 0.30 (반투명 외각 셸 느낌) + HDR 발광 코어
    XMFLOAT4 ambient  = { 0.10f, 0.05f, 0.00f, 1.0f };
    XMFLOAT4 diffuse  = { 0.20f, 0.10f, 0.05f, 0.30f }; // alpha 0.30 — 반투명
    XMFLOAT4 specular = { 1.00f, 0.80f, 0.50f, 64.0f };
    XMFLOAT4 emissive = { 4.00f, 2.00f, 0.40f, 1.0f };  // HDR 발광 (큐브 내부에서 빛이 새어나오는 느낌)

    XMMATRIX scaleM = XMMatrixScaling(CORE_SIZE, CORE_SIZE, CORE_SIZE);
    XMMATRIX trans  = XMMatrixTranslation(coreX, coreY, coreZ);

    // ── 반투명 PSO 바인딩 (alpha blend, depth write off) ─────────
    // Shader가 다음 프레임에 PSO를 다시 설정하므로 복원 불필요
    if (m_pBlendPSO)
        pCmdList->SetPipelineState(m_pBlendPSO);

    // ── Cube 0: Y축 회전 + X축 서서히 기울기 ────────────────────
    {
        XMMATRIX rot0  = XMMatrixRotationX(m_rotX0) * XMMatrixRotationY(m_rotY0);
        XMMATRIX world = scaleM * rot0 * trans;

        XMStoreFloat4x4(&m_pMappedCoreCB[0]->World, XMMatrixTranspose(world));
        m_pMappedCoreCB[0]->MaterialIndex       = 0;
        m_pMappedCoreCB[0]->bIsSkinned          = 0;
        m_pMappedCoreCB[0]->bHasTexture         = 0;
        m_pMappedCoreCB[0]->bIsLava             = 0;
        m_pMappedCoreCB[0]->bHasEmissiveTexture = 0;
        m_pMappedCoreCB[0]->cAmbient  = ambient;
        m_pMappedCoreCB[0]->cDiffuse  = diffuse;
        m_pMappedCoreCB[0]->cSpecular = specular;
        m_pMappedCoreCB[0]->cEmissive = emissive;

        D3D12_GPU_DESCRIPTOR_HANDLE hGPU = m_pDescHeap->GetGPUHandle(m_nDescStart + 0);
        pCmdList->SetGraphicsRootDescriptorTable(0, hGPU);
        m_pCoreMesh->Render(pCmdList, 0);
    }

    // ── Cube 1: 반대 방향 Y회전 + Z 롤링 + X축 35.26° 기울기 (Stella Octangula 외관 + 추가 다이내믹) ─
    {
        XMMATRIX rot1  = XMMatrixRotationZ(m_rotZ1)
                       * XMMatrixRotationY(m_rotY1)
                       * XMMatrixRotationX(XMConvertToRadians(35.26f));
        XMMATRIX world = scaleM * rot1 * trans;

        XMStoreFloat4x4(&m_pMappedCoreCB[1]->World, XMMatrixTranspose(world));
        m_pMappedCoreCB[1]->MaterialIndex       = 0;
        m_pMappedCoreCB[1]->bIsSkinned          = 0;
        m_pMappedCoreCB[1]->bHasTexture         = 0;
        m_pMappedCoreCB[1]->bIsLava             = 0;
        m_pMappedCoreCB[1]->bHasEmissiveTexture = 0;
        m_pMappedCoreCB[1]->cAmbient  = ambient;
        m_pMappedCoreCB[1]->cDiffuse  = diffuse;
        m_pMappedCoreCB[1]->cSpecular = specular;
        m_pMappedCoreCB[1]->cEmissive = emissive;

        D3D12_GPU_DESCRIPTOR_HANDLE hGPU = m_pDescHeap->GetGPUHandle(m_nDescStart + 1);
        pCmdList->SetGraphicsRootDescriptorTable(0, hGPU);
        m_pCoreMesh->Render(pCmdList, 0);
    }
}

// ─── 폭발 AoE 데미지 ─────────────────────────────────────────────────────────
void MeteorBehavior::ApplyExplosionDamage(float damage, float radius, bool bTriggerStagger)
{
    if (!m_pScene) return;
    CRoom* pRoom = m_pScene->GetCurrentRoom();
    if (!pRoom) return;

    XMVECTOR centerV = XMLoadFloat3(&m_targetPos);

    const auto& gameObjects = pRoom->GetGameObjects();
    for (const auto& obj : gameObjects)
    {
        if (!obj) continue;
        EnemyComponent* pEnemy = obj->GetComponent<EnemyComponent>();
        if (!pEnemy || pEnemy->IsDead()) continue;

        TransformComponent* pTransform = obj->GetTransform();
        if (!pTransform) continue;

        XMFLOAT3 ePos = pTransform->GetPosition();
        XMFLOAT3 eScale = pTransform->GetScale();
        float eRadius = max(1.5f, max(eScale.x, max(eScale.y, eScale.z)) * 1.5f);

        float dist = XMVectorGetX(XMVector3Length(
            XMVectorSubtract(XMLoadFloat3(&ePos), centerV)));

        if (dist < radius + eRadius) {
            // 거리 기반 감쇠: 중심 100%, 가장자리 50%
            float falloff = 1.f - (dist / (radius + eRadius)) * 0.5f;
            falloff = max(0.5f, falloff);
            pEnemy->TakeDamage(damage * falloff, bTriggerStagger);
        }
    }
}

bool MeteorBehavior::IsFinished() const
{
    return m_bIsFinished;
}

void MeteorBehavior::Reset()
{
    if (m_pVFXManager)
    {
        if (m_trailVfxId   >= 0) m_pVFXManager->StopEffect(m_trailVfxId);
        if (m_trailOuterId >= 0) m_pVFXManager->StopEffect(m_trailOuterId);
        if (m_frontFireId  >= 0) m_pVFXManager->StopEffect(m_frontFireId);
        for (int eid : m_extraVFXIds)
            if (eid >= 0) m_pVFXManager->StopEffect(eid);
    }

    m_bIsFinished  = true;
    m_bExploded    = false;
    m_elapsed      = 0.f;
    m_lastPhase    = -1;
    m_rotY0        = 0.f;
    m_rotX0        = 0.f;
    m_rotY1        = 0.f;
    m_rotZ1        = 0.f;
    m_vfxId        = -1;
    m_trailVfxId   = -1;
    m_trailOuterId = -1;
    m_impactVfxId  = -1;
    m_groundFireId = -1;
    m_frontFireId  = -1;
    m_extraVFXIds.clear();
}

uint32_t MeteorBehavior::GetRuneFlags(GameObject* caster) const
{
    uint32_t flags = 0;
    if (!caster) return flags;

    auto* pSkillComp = caster->GetComponent<SkillComponent>();
    if (!pSkillComp || m_slot == SkillSlot::Count) return flags;

    RuneCombo combo = pSkillComp->GetRuneCombo(m_slot);
    if (combo.hasInstant) flags |= RUNE_INSTANT;
    if (combo.hasCharge)  flags |= RUNE_CHARGE;
    if (combo.hasChannel) flags |= RUNE_CHANNEL;
    if (combo.hasPlace)   flags |= RUNE_PLACE;
    if (combo.hasEnhance) flags |= RUNE_ENHANCE;
    if (combo.hasSplit)   flags |= RUNE_SPLIT;
    return flags;
}
