#include "stdafx.h"
#include "DarkLordSigilField.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "RenderComponent.h"
#include "PlayerComponent.h"
#include "Room.h"
#include "Scene.h"
#include "Camera.h"
#include "Shader.h"
#include "Mesh.h"
#include "Dx12App.h"
#include "VFXManager.h"
#include "SlashCue.h"

using namespace DirectX;

// 공유 Ring/Disc 메시 — RockFall 과는 별도로 SigilField 전용 보관 (lifetime 충돌 회피).
static RingMesh* s_pSigilRing = nullptr;
static RingMesh* s_pSigilDisc = nullptr;

static RingMesh* GetOrCreateSigilRing(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd)
{
    if (!s_pSigilRing)
        s_pSigilRing = new RingMesh(pDevice, pCmd, 1.0f, 0.92f, 48);
    return s_pSigilRing;
}
static RingMesh* GetOrCreateSigilDisc(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd)
{
    if (!s_pSigilDisc)
        s_pSigilDisc = new RingMesh(pDevice, pCmd, 1.0f, 0.0f, 48);
    return s_pSigilDisc;
}

// 원소별 색 (인장 emissive / ambient).
namespace
{
    XMFLOAT3 ElementColor(ElementType e)
    {
        switch (e)
        {
        case ElementType::Fire:  return XMFLOAT3(1.00f, 0.30f, 0.10f);
        case ElementType::Water: return XMFLOAT3(0.20f, 0.60f, 1.00f);
        case ElementType::Wind:  return XMFLOAT3(0.55f, 1.00f, 0.45f);
        case ElementType::Earth: return XMFLOAT3(0.85f, 0.65f, 0.20f);
        default:                 return XMFLOAT3(1.00f, 0.30f, 0.10f);
        }
    }
}

DarkLordSigilField::DarkLordSigilField(ElementType element,
                                       float fDamage,
                                       float fRadius,
                                       float fDelay,
                                       int   nSigilCount,
                                       float fSpreadRadius,
                                       float fRecoveryTime)
    : m_eElement(element)
    , m_fDamage(fDamage)
    , m_fRadius(fRadius)
    , m_fDelay(fDelay)
    , m_nSigilCount(nSigilCount < 1 ? 1 : nSigilCount)
    , m_fSpreadRadius(fSpreadRadius)
    , m_fRecoveryTime(fRecoveryTime)
{
}

void DarkLordSigilField::Execute(EnemyComponent* pEnemy)
{
    Reset();
    if (!pEnemy) return;

    m_pRoom = pEnemy->GetRoom();
    if (!m_pRoom) return;
    m_pScene = m_pRoom->GetScene();
    if (!m_pScene) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;

    XMFLOAT3 bossPos = pOwner->GetTransform()->GetPosition();

    m_vSigils.clear();

    // 네트워크 모드에서는 서버가 보낸 인장 위치를 그대로 사용한다.
    if (!m_vNetworkEffectPositions.empty())
    {
        m_vSigils.reserve(m_vNetworkEffectPositions.size());

        for (const XMFLOAT3& p : m_vNetworkEffectPositions)
        {
            SigilInstance s;
            s.center = { p.x, 0.0f, p.z };
            m_vSigils.push_back(s);
        }
    }
    else
    {
        // 오프라인 기존 로직 유지:
        // 첫 인장은 항상 target 발치, 추가 인장은 보스 주변 spreadRadius 반경 균등 배치.
        XMFLOAT3 targetPos = bossPos;
        if (GameObject* pTarget = pEnemy->GetTarget())
            if (auto* pTT = pTarget->GetTransform())
                targetPos = pTT->GetPosition();

        m_vSigils.reserve(m_nSigilCount);

        {
            SigilInstance s;
            s.center = { targetPos.x, 0.0f, targetPos.z };
            m_vSigils.push_back(s);
        }

        for (int i = 1; i < m_nSigilCount; ++i)
        {
            float t = static_cast<float>(i - 1) / static_cast<float>(m_nSigilCount - 1);
            float angle = t * XM_2PI;

            SigilInstance s;
            s.center = { bossPos.x + cosf(angle) * m_fSpreadRadius,
                         0.0f,
                         bossPos.z + sinf(angle) * m_fSpreadRadius };
            m_vSigils.push_back(s);
        }
    }

    SpawnSigilVisuals(pEnemy);

    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
}

void DarkLordSigilField::SpawnSigilVisuals(EnemyComponent* /*pEnemy*/)
{
    if (!m_pScene || !m_pRoom) return;

    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    RingMesh* pRing = GetOrCreateSigilRing(pDevice, pCmd);
    RingMesh* pDisc = GetOrCreateSigilDisc(pDevice, pCmd);

    XMFLOAT3 col = ElementColor(m_eElement);

    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    for (auto& sig : m_vSigils)
    {
        // 테두리 링
        GameObject* pRingObj = m_pScene->CreateGameObject(pDevice, pCmd);
        if (pRingObj)
        {
            auto* pT = pRingObj->GetTransform();
            pT->SetPosition(sig.center.x, 0.15f, sig.center.z);
            pT->SetScale(m_fRadius, 1.0f, m_fRadius);

            pRingObj->SetMesh(pRing);
            pRing->AddRef();

            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(col.x * 0.4f, col.y * 0.4f, col.z * 0.4f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(col.x,        col.y,        col.z,        1.0f);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = XMFLOAT4(col.x * 2.0f, col.y * 2.0f, col.z * 2.0f, 1.0f);
            pRingObj->SetMaterial(mat);

            auto* pRC = pRingObj->AddComponent<RenderComponent>();
            pRC->SetMesh(pRing);
            pRC->SetOverlay(true);
            pShader->AddRenderComponent(pRC);
            pRingObj->SetDecal(true);
            sig.pRing = pRingObj;
        }

        // 차오름 Disc (0 → m_fRadius)
        GameObject* pFillObj = m_pScene->CreateGameObject(pDevice, pCmd);
        if (pFillObj)
        {
            auto* pT = pFillObj->GetTransform();
            pT->SetPosition(sig.center.x, 0.10f, sig.center.z);
            pT->SetScale(0.01f, 1.0f, 0.01f);

            pFillObj->SetMesh(pDisc);
            pDisc->AddRef();

            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(col.x * 0.3f, col.y * 0.3f, col.z * 0.3f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(col.x * 0.9f, col.y * 0.9f, col.z * 0.9f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = XMFLOAT4(col.x * 1.3f, col.y * 1.3f, col.z * 1.3f, 1.0f);
            pFillObj->SetMaterial(mat);

            auto* pRC = pFillObj->AddComponent<RenderComponent>();
            pRC->SetMesh(pDisc);
            pRC->SetOverlay(true);
            pShader->AddRenderComponent(pRC);
            pFillObj->SetDecal(true);
            sig.pFill = pFillObj;
        }
    }

    m_pScene->SetCurrentRoom(pPrevRoom);
}

void DarkLordSigilField::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;
    m_fTimer += dt;

    switch (m_ePhase)
    {
    case Phase::Windup:
    {
        float progress = (std::min)(m_fTimer / m_fDelay, 1.0f);
        float fillR = m_fRadius * progress;
        if (fillR < 0.01f) fillR = 0.01f;
        for (auto& sig : m_vSigils)
        {
            if (sig.pFill)
                if (auto* pT = sig.pFill->GetTransform())
                    pT->SetScale(fillR, 1.0f, fillR);
        }

        if (m_fTimer >= m_fDelay)
        {
            DetonateAll(pEnemy);
            m_ePhase = Phase::Recovery;
            m_fTimer = 0.0f;
        }
        break;
    }
    case Phase::Recovery:
        if (m_fTimer >= m_fRecoveryTime)
        {
            CleanupAll();
            m_bFinished = true;
        }
        break;
    }
}

void DarkLordSigilField::DetonateAll(EnemyComponent* /*pEnemy*/)
{
    if (!m_pScene) return;

    // ── 임팩트 강화 (Day 6 +) ─────────────────────────────────────────
    //   1) Heavy crescent 본체를 인장 위치에 1발 — 지면에서 솟구치는 검기 본체.
    //   2) 같은 위치에 ImpactEffect 를 두 번 (중심 + 살짝 옆) 겹쳐 spawn → 임팩트 폭발감 강화.
    //   3) 카메라 셰이크 1.6 → 2.5 (검역 폭발은 시그니처급으로 띄움).
    if (auto* pVFX = m_pScene->GetVFXManager())
    {
        const char* impactName = SlashCue::ImpactEffectName(m_eElement);
        const char* sigilHeavy = nullptr;
        switch (m_eElement)
        {
        case ElementType::Fire:  sigilHeavy = "Boss_CrescentSigil_Fire_Heavy";  break;
        case ElementType::Water: sigilHeavy = "Boss_CrescentSigil_Water_Heavy"; break;
        case ElementType::Wind:  sigilHeavy = "Boss_CrescentSigil_Wind_Heavy";  break;
        case ElementType::Earth: sigilHeavy = "Boss_CrescentSigil_Earth_Heavy"; break;
        default:                 sigilHeavy = "Boss_CrescentSigil_Fire_Heavy";  break;
        }
        XMFLOAT3 dirUp = { 0, 1, 0 };
        // ── 응축 폭발 톤 (Day 6 +) ──────────────────────────────────────
        //   검기 본체 1발 (그 자리, 수직 dir → 솟구치는 느낌) + Impact 3발 stack.
        //   사방 sweep spawn 은 "검기가 날아가는" 인상 + enemy VFX slot(64) 고갈로
        //   후속 보스 검기가 spawn 실패 → 둘 다 해결.
        for (auto& sig : m_vSigils)
        {
            // 검기 본체 — 인장 중심, dir=Up. outOff 0 → 응축.
            XMFLOAT3 spawnHeavy = { sig.center.x, sig.center.y + 0.6f, sig.center.z };
            pVFX->Spawn(sigilHeavy, spawnHeavy, dirUp, 0u, false);

            // Impact — 지면 → 중간 → 상단 의 3-stack 펄스. 폭발 위쪽 솟구침 톤.
            XMFLOAT3 sLow  = { sig.center.x, sig.center.y + 0.2f, sig.center.z };
            XMFLOAT3 sMid  = { sig.center.x, sig.center.y + 1.4f, sig.center.z };
            XMFLOAT3 sHigh = { sig.center.x, sig.center.y + 2.8f, sig.center.z };
            pVFX->Spawn(impactName, sLow,  dirUp, 0u, false);
            pVFX->Spawn(impactName, sMid,  dirUp, 0u, false);
            pVFX->Spawn(impactName, sHigh, dirUp, 0u, false);

            sig.bDetonated = true;
        }
    }

    if (CCamera* pCam = m_pScene->GetCamera())
        pCam->StartShake(2.5f, 0.45f);

    // 인장 인디케이터 즉시 제거 (폭발 순간).
    for (auto& sig : m_vSigils)
    {
        if (sig.pRing) { m_pScene->MarkForDeletion(sig.pRing); sig.pRing = nullptr; }
        if (sig.pFill) { m_pScene->MarkForDeletion(sig.pFill); sig.pFill = nullptr; }
    }

    // 네트워크 모드에서는 데미지는 서버가 처리한다.
    // 클라는 인장/폭발 VFX만 재생한다.
    if (m_bNetworkVisualOnly)
        return;

    // AoE 데미지 — 모든 플레이어에 대해 거리 체크 (중복 hit 방지).
    auto vPlayers = m_pScene->GetAllPlayers();
    for (GameObject* pPO : vPlayers)
    {
        if (!pPO) continue;
        auto* pPT = pPO->GetTransform();
        if (!pPT) continue;
        PlayerComponent* pPC = pPO->GetComponent<PlayerComponent>();
        if (!pPC) continue;

        XMFLOAT3 pp = pPT->GetPosition();
        bool bHit = false;
        for (auto& sig : m_vSigils)
        {
            float dx = pp.x - sig.center.x;
            float dz = pp.z - sig.center.z;
            if (sqrtf(dx*dx + dz*dz) <= m_fRadius)
            { bHit = true; break; }
        }
        if (bHit)
            pPC->TakeDamage(m_fDamage);
    }
}

void DarkLordSigilField::CleanupAll()
{
    if (!m_pScene) return;
    for (auto& sig : m_vSigils)
    {
        if (sig.pRing) m_pScene->MarkForDeletion(sig.pRing);
        if (sig.pFill) m_pScene->MarkForDeletion(sig.pFill);
    }
    m_vSigils.clear();
}

void DarkLordSigilField::Reset()
{
    m_ePhase    = Phase::Windup;
    m_fTimer    = 0.0f;
    m_bFinished = false;
    CleanupAll();
}
