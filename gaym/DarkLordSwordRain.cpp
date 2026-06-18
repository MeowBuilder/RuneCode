#include "stdafx.h"
#include "DarkLordSwordRain.h"
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

static RingMesh* s_pRainRing = nullptr;
static RingMesh* s_pRainDisc = nullptr;

static RingMesh* GetOrCreateRainRing(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd)
{
    if (!s_pRainRing)
        s_pRainRing = new RingMesh(pDevice, pCmd, 1.0f, 0.93f, 36);
    return s_pRainRing;
}
static RingMesh* GetOrCreateRainDisc(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd)
{
    if (!s_pRainDisc)
        s_pRainDisc = new RingMesh(pDevice, pCmd, 1.0f, 0.0f, 36);
    return s_pRainDisc;
}

namespace
{
    XMFLOAT3 RainColor(ElementType e)
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

DarkLordSwordRain::DarkLordSwordRain(ElementType element,
                                     int   nSwordCount,
                                     float fDamage,
                                     float fAoeRadius,
                                     float fSpawnMinRadius,
                                     float fSpawnMaxRadius,
                                     float fWindup,
                                     float fRecovery)
    : m_eElement(element)
    , m_nSwordCount(nSwordCount < 1 ? 1 : nSwordCount)
    , m_fDamage(fDamage)
    , m_fAoeRadius(fAoeRadius)
    , m_fSpawnMinRadius(fSpawnMinRadius)
    , m_fSpawnMaxRadius(fSpawnMaxRadius)
    , m_fWindup(fWindup)
    , m_fRecovery(fRecovery)
{
}

void DarkLordSwordRain::Execute(EnemyComponent* pEnemy)
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

    XMFLOAT3 anchorPos = bossPos;
    if (GameObject* pTarget = pEnemy->GetTarget())
        if (auto* pTT = pTarget->GetTransform())
            anchorPos = pTT->GetPosition();

    m_vDrops.clear();
    m_vDrops.reserve(m_nSwordCount);

    float radiusRange = m_fSpawnMaxRadius - m_fSpawnMinRadius;
    for (int i = 0; i < m_nSwordCount; ++i)
    {
        float angle = ((float)rand() / RAND_MAX) * XM_2PI;
        float t = (float)rand() / RAND_MAX;
        float r = m_fSpawnMinRadius + sqrtf(t) * radiusRange;

        Drop d;
        // Anchor 가 플레이어이므로 비가 플레이어 주변에 더 집중됨.
        d.center.x = anchorPos.x + cosf(angle) * r;
        d.center.y = 0.0f;
        d.center.z = anchorPos.z + sinf(angle) * r;
        m_vDrops.push_back(d);
    }

    SpawnIndicators();
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
}

void DarkLordSwordRain::SpawnIndicators()
{
    if (!m_pScene || !m_pRoom) return;

    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    RingMesh* pRing = GetOrCreateRainRing(pDevice, pCmd);
    RingMesh* pDisc = GetOrCreateRainDisc(pDevice, pCmd);

    XMFLOAT3 col = RainColor(m_eElement);

    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    for (auto& d : m_vDrops)
    {
        GameObject* pRingObj = m_pScene->CreateGameObject(pDevice, pCmd);
        if (pRingObj)
        {
            auto* pT = pRingObj->GetTransform();
            pT->SetPosition(d.center.x, 0.15f, d.center.z);
            pT->SetScale(m_fAoeRadius, 1.0f, m_fAoeRadius);

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
            d.pRing = pRingObj;
        }

        GameObject* pFillObj = m_pScene->CreateGameObject(pDevice, pCmd);
        if (pFillObj)
        {
            auto* pT = pFillObj->GetTransform();
            pT->SetPosition(d.center.x, 0.10f, d.center.z);
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
            d.pFill = pFillObj;
        }
    }

    m_pScene->SetCurrentRoom(pPrevRoom);
}

void DarkLordSwordRain::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;
    m_fTimer += dt;

    switch (m_ePhase)
    {
    case Phase::Windup:
    {
        float progress = (std::min)(m_fTimer / m_fWindup, 1.0f);
        float fillR = m_fAoeRadius * progress;
        if (fillR < 0.01f) fillR = 0.01f;
        for (auto& d : m_vDrops)
        {
            if (d.pFill)
                if (auto* pT = d.pFill->GetTransform())
                    pT->SetScale(fillR, 1.0f, fillR);
        }

        if (m_fTimer >= m_fWindup)
        {
            DetonateAll();
            m_ePhase = Phase::Recovery;
            m_fTimer = 0.0f;
        }
        break;
    }
    case Phase::Recovery:
        if (m_fTimer >= m_fRecovery)
        {
            CleanupAll();
            m_bFinished = true;
        }
        break;
    }
}

void DarkLordSwordRain::DetonateAll()
{
    if (!m_pScene) return;

    // ── 임팩트 강화 (Day 6 +) ─────────────────────────────────────────
    //   각 낙하지점에 Heavy crescent (수직 솟구침) + Impact 중심 + Impact 살짝 위 의 3-layer.
    //   다중 spawn 이므로 셰이크는 강하게 잡되 시간 짧게.
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
        //   drop 수가 많아 (default 7~10) crescent 다방향 spawn 은 VFX slot 폭주 위험.
        //   drop 당 crescent 1발(수직) + Impact 3-stack 만으로 폭발감 보존.
        for (auto& d : m_vDrops)
        {
            XMFLOAT3 spawnHeavy = { d.center.x, d.center.y + 0.6f, d.center.z };
            pVFX->Spawn(sigilHeavy, spawnHeavy, dirUp, 0u, false);

            XMFLOAT3 sLow  = { d.center.x, d.center.y + 0.2f, d.center.z };
            XMFLOAT3 sMid  = { d.center.x, d.center.y + 1.4f, d.center.z };
            XMFLOAT3 sHigh = { d.center.x, d.center.y + 2.8f, d.center.z };
            pVFX->Spawn(impactName, sLow,  dirUp, 0u, false);
            pVFX->Spawn(impactName, sMid,  dirUp, 0u, false);
            pVFX->Spawn(impactName, sHigh, dirUp, 0u, false);
        }
    }

    if (CCamera* pCam = m_pScene->GetCamera())
        pCam->StartShake(3.0f, 0.55f);

    for (auto& d : m_vDrops)
    {
        if (d.pRing) { m_pScene->MarkForDeletion(d.pRing); d.pRing = nullptr; }
        if (d.pFill) { m_pScene->MarkForDeletion(d.pFill); d.pFill = nullptr; }
    }

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
        for (auto& d : m_vDrops)
        {
            float dx = pp.x - d.center.x;
            float dz = pp.z - d.center.z;
            if (sqrtf(dx*dx + dz*dz) <= m_fAoeRadius)
            { bHit = true; break; }
        }
        if (bHit)
            pPC->TakeDamage(m_fDamage);
    }
}

void DarkLordSwordRain::CleanupAll()
{
    if (!m_pScene) return;
    for (auto& d : m_vDrops)
    {
        if (d.pRing) m_pScene->MarkForDeletion(d.pRing);
        if (d.pFill) m_pScene->MarkForDeletion(d.pFill);
    }
    m_vDrops.clear();
}

void DarkLordSwordRain::Reset()
{
    m_ePhase    = Phase::Windup;
    m_fTimer    = 0.0f;
    m_bFinished = false;
    CleanupAll();
}
