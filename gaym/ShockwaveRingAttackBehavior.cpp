#include "stdafx.h"
#include "ShockwaveRingAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "RenderComponent.h"
#include "PlayerComponent.h"
#include "AnimationComponent.h"
#include "Room.h"
#include "Scene.h"
#include "Camera.h"
#include "Shader.h"
#include "Mesh.h"
#include "Dx12App.h"
#include "VFXManager.h"

namespace {
    VFXManager* GetVFX()
    {
        if (auto* pApp = Dx12App::GetInstance())
            if (auto* pScene = pApp->GetScene())
                return pScene->GetVFXManager();
        return nullptr;
    }

    static RingMesh* s_pBorder = nullptr;
    static RingMesh* s_pFill   = nullptr;
    RingMesh* GetBorder(ID3D12Device* d, ID3D12GraphicsCommandList* c)
    {
        if (!s_pBorder) s_pBorder = new RingMesh(d, c, 1.0f, 0.92f, 64);
        return s_pBorder;
    }
    RingMesh* GetFill(ID3D12Device* d, ID3D12GraphicsCommandList* c)
    {
        if (!s_pFill) s_pFill = new RingMesh(d, c, 1.0f, 0.0f, 64);
        return s_pFill;
    }
}

ShockwaveRingAttackBehavior::ShockwaveRingAttackBehavior(
    float fDamage, float fMaxRadius, float fWaveThickness,
    float fWindupTime, float fExpandDuration, float fRecoveryTime,
    float fCameraShakeIntensity, float fCameraShakeDuration)
    : m_fDamage(fDamage)
    , m_fMaxRadius(fMaxRadius)
    , m_fWaveThickness(fWaveThickness)
    , m_fWindupTime(fWindupTime)
    , m_fExpandDuration(fExpandDuration)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fCameraShakeIntensity(fCameraShakeIntensity)
    , m_fCameraShakeDuration(fCameraShakeDuration)
{
}

void ShockwaveRingAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();
    if (!pEnemy) return;

    m_pRoom = pEnemy->GetRoom();
    if (!m_pRoom) return;
    m_pScene = m_pRoom->GetScene();
    if (!m_pScene) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;
    m_xmf3BossCenter = pOwner->GetTransform()->GetPosition();
    m_xmf3BossCenter.y = 0.0f;

    SpawnIndicators(pEnemy);
    m_ePhase = Phase::Windup;
}

void ShockwaveRingAttackBehavior::SpawnIndicators(EnemyComponent* pEnemy)
{
    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    RingMesh* pBorderMesh = GetBorder(pDevice, pCmd);
    RingMesh* pFillMesh   = GetFill(pDevice, pCmd);

    CRoom* pPrev = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    // 최종 반경 경고 링
    if (GameObject* pB = m_pScene->CreateGameObject(pDevice, pCmd))
    {
        auto* pT = pB->GetTransform();
        pT->SetPosition(m_xmf3BossCenter.x, 0.18f, m_xmf3BossCenter.z);
        pT->SetScale(m_fMaxRadius, 1.0f, m_fMaxRadius);
        pB->SetMesh(pBorderMesh); pBorderMesh->AddRef();
        MATERIAL mat;
        mat.m_cAmbient  = XMFLOAT4(0.5f, 0.02f, 0.02f, 1.0f);
        mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.2f, 0.1f, 1.0f);
        mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        mat.m_cEmissive = XMFLOAT4(2.0f, 0.3f, 0.1f, 1.0f);
        pB->SetMaterial(mat);
        auto* pRC = pB->AddComponent<RenderComponent>();
        pRC->SetMesh(pBorderMesh);
        pRC->SetOverlay(true);
        pShader->AddRenderComponent(pRC);
        m_pBorder = pB;
    }

    // 차오름 fill — 보스 중심부터 외곽까지 채워짐
    if (GameObject* pF = m_pScene->CreateGameObject(pDevice, pCmd))
    {
        auto* pT = pF->GetTransform();
        pT->SetPosition(m_xmf3BossCenter.x, 0.15f, m_xmf3BossCenter.z);
        pT->SetScale(0.01f, 1.0f, 0.01f);
        pF->SetMesh(pFillMesh); pFillMesh->AddRef();
        MATERIAL mat;
        mat.m_cAmbient  = XMFLOAT4(0.3f, 0.02f, 0.0f, 1.0f);
        mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.35f, 0.05f, 1.0f);
        mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        mat.m_cEmissive = XMFLOAT4(1.3f, 0.5f, 0.08f, 1.0f);
        pF->SetMaterial(mat);
        auto* pRC = pF->AddComponent<RenderComponent>();
        pRC->SetMesh(pFillMesh);
        pRC->SetOverlay(true);
        pShader->AddRenderComponent(pRC);
        m_pFill = pF;
    }

    m_pScene->SetCurrentRoom(pPrev);
}

void ShockwaveRingAttackBehavior::FireRingVFX(EnemyComponent* pEnemy)
{
    VFXManager* pVFX = GetVFX();
    if (!pVFX) return;

    XMFLOAT3 origin = m_xmf3BossCenter;
    origin.y = 0.5f;
    XMFLOAT3 dir(0.0f, 1.0f, 0.0f);
    m_nRingVFX = pVFX->Spawn("E_GaleRush_Ring", origin, dir, 0u, false);

    if (m_fCameraShakeIntensity > 0.0f && m_pScene)
        if (CCamera* pCam = m_pScene->GetCamera())
            pCam->StartShake(m_fCameraShakeIntensity, m_fCameraShakeDuration);

    // expand 시작 — wave front 인디케이터 생성 (옅은 녹색 링)
    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp || !m_pScene) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    RingMesh* pBorderMesh = GetBorder(pDevice, pCmd);
    CRoom* pPrev = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    if (GameObject* pW = m_pScene->CreateGameObject(pDevice, pCmd))
    {
        auto* pT = pW->GetTransform();
        pT->SetPosition(m_xmf3BossCenter.x, 0.20f, m_xmf3BossCenter.z);
        pT->SetScale(0.5f, 1.0f, 0.5f);
        pW->SetMesh(pBorderMesh); pBorderMesh->AddRef();
        MATERIAL mat;
        mat.m_cAmbient  = XMFLOAT4(0.10f, 0.35f, 0.15f, 1.0f);
        mat.m_cDiffuse  = XMFLOAT4(0.40f, 1.00f, 0.50f, 1.0f);
        mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        mat.m_cEmissive = XMFLOAT4(0.6f, 2.2f, 0.8f, 1.0f);
        pW->SetMaterial(mat);
        auto* pRC = pW->AddComponent<RenderComponent>();
        pRC->SetMesh(pBorderMesh);
        pRC->SetOverlay(true);
        pShader->AddRenderComponent(pRC);
        m_pWaveBorder = pW;
    }

    m_pScene->SetCurrentRoom(pPrev);
}

void ShockwaveRingAttackBehavior::UpdateWaveFront(float dt, EnemyComponent* pEnemy)
{
    if (!m_pScene) return;

    // wave front 반경 갱신
    float p = (std::min)(m_fTimer / m_fExpandDuration, 1.0f);
    m_fCurrentWaveRadius = m_fMaxRadius * p;
    if (m_pWaveBorder)
    {
        float r = (std::max)(m_fCurrentWaveRadius, 0.5f);
        m_pWaveBorder->GetTransform()->SetScale(r, 1.0f, r);
    }

    // wave 안쪽 / 바깥쪽 반경
    float inner = (std::max)(m_fCurrentWaveRadius - m_fWaveThickness, 0.0f);
    float outer = m_fCurrentWaveRadius;
    float i2 = inner * inner;
    float o2 = outer * outer;

    // 플레이어 hit check (중복 방지)
    std::vector<GameObject*> vPlayers = m_pScene->GetAllPlayers();
    for (GameObject* pPlayerObj : vPlayers)
    {
        if (!pPlayerObj) continue;
        // 이미 맞은 플레이어는 스킵
        bool already = false;
        for (GameObject* pH : m_vAlreadyHit)
            if (pH == pPlayerObj) { already = true; break; }
        if (already) continue;

        auto* pPT = pPlayerObj->GetTransform();
        if (!pPT) continue;
        PlayerComponent* pPlayer = pPlayerObj->GetComponent<PlayerComponent>();
        if (!pPlayer) continue;

        XMFLOAT3 pp = pPT->GetPosition();
        float dx = pp.x - m_xmf3BossCenter.x;
        float dz = pp.z - m_xmf3BossCenter.z;
        float d2 = dx * dx + dz * dz;
        if (d2 >= i2 && d2 <= o2)
        {
            pPlayer->TakeDamage(m_fDamage);
            m_vAlreadyHit.push_back(pPlayerObj);
        }
    }
}

void ShockwaveRingAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;
    m_fTimer += dt;

    if (!m_bAnimReturnedToIdle && m_fTimer > 0.1f)
    {
        if (auto* pAnim = pEnemy->GetAnimationComponent())
        {
            if (!pAnim->IsPlaying())
            {
                pAnim->CrossFade("Idle1", 0.25f, true, true);
                m_bAnimReturnedToIdle = true;
            }
        }
    }

    switch (m_ePhase)
    {
    case Phase::Windup:
    {
        float p = (std::min)(m_fTimer / m_fWindupTime, 1.0f);
        float r = m_fMaxRadius * p;
        if (r < 0.01f) r = 0.01f;
        if (m_pFill) m_pFill->GetTransform()->SetScale(r, 1.0f, r);

        if (m_fTimer >= m_fWindupTime)
        {
            // windup 끝 → fill / border 제거, wave expand 시작
            if (m_pFill)   { m_pScene->MarkForDeletion(m_pFill);   m_pFill   = nullptr; }
            if (m_pBorder) { m_pScene->MarkForDeletion(m_pBorder); m_pBorder = nullptr; }
            FireRingVFX(pEnemy);
            m_ePhase = Phase::Expand;
            m_fTimer = 0.0f;
        }
        break;
    }
    case Phase::Expand:
        UpdateWaveFront(dt, pEnemy);
        if (m_fTimer >= m_fExpandDuration)
        {
            if (m_pWaveBorder) { m_pScene->MarkForDeletion(m_pWaveBorder); m_pWaveBorder = nullptr; }
            m_ePhase = Phase::Recovery;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::Recovery:
        if (m_fTimer >= m_fRecoveryTime)
        {
            CleanupAll();
            m_bFinished = true;
        }
        break;
    }
}

void ShockwaveRingAttackBehavior::CleanupAll()
{
    if (m_pScene)
    {
        if (m_pBorder)     { m_pScene->MarkForDeletion(m_pBorder);     m_pBorder = nullptr; }
        if (m_pFill)       { m_pScene->MarkForDeletion(m_pFill);       m_pFill = nullptr; }
        if (m_pWaveBorder) { m_pScene->MarkForDeletion(m_pWaveBorder); m_pWaveBorder = nullptr; }
    }
    if (VFXManager* pVFX = GetVFX())
    {
        if (m_nRingVFX >= 0) { pVFX->Stop(m_nRingVFX); m_nRingVFX = -1; }
    }
    m_vAlreadyHit.clear();
}

bool ShockwaveRingAttackBehavior::IsFinished() const { return m_bFinished; }

void ShockwaveRingAttackBehavior::Reset()
{
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
    m_fCurrentWaveRadius = 0.0f;
    m_bFinished = false;
    m_bAnimReturnedToIdle = false;
    CleanupAll();
}
