#include "stdafx.h"
#include "GaleSlashAttackBehavior.h"
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
        if (!s_pBorder) s_pBorder = new RingMesh(d, c, 1.0f, 0.92f, 32);
        return s_pBorder;
    }
    RingMesh* GetFill(ID3D12Device* d, ID3D12GraphicsCommandList* c)
    {
        if (!s_pFill) s_pFill = new RingMesh(d, c, 1.0f, 0.0f, 32);
        return s_pFill;
    }
}

GaleSlashAttackBehavior::GaleSlashAttackBehavior(
    SlashShape eShape, float fDamage, float fLineLength, float fLineHalfWidth,
    float fWindupTime, float fImpactTime, float fRecoveryTime,
    float fCameraShakeIntensity, float fCameraShakeDuration)
    : m_eShape(eShape)
    , m_fDamage(fDamage)
    , m_fLineLength(fLineLength)
    , m_fLineHalfWidth(fLineHalfWidth)
    , m_fWindupTime(fWindupTime)
    , m_fImpactTime(fImpactTime)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fCameraShakeIntensity(fCameraShakeIntensity)
    , m_fCameraShakeDuration(fCameraShakeDuration)
{
}

void GaleSlashAttackBehavior::Execute(EnemyComponent* pEnemy)
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

    m_vLines.clear();
    m_vLines.reserve(4);
    if (m_eShape == SlashShape::Cross)
    {
        m_vLines.push_back({ {  1,  0,  0 } });
        m_vLines.push_back({ { -1,  0,  0 } });
        m_vLines.push_back({ {  0,  0,  1 } });
        m_vLines.push_back({ {  0,  0, -1 } });
    }
    else
    {
        float s = 0.7071f;
        m_vLines.push_back({ {  s,  0,  s } });
        m_vLines.push_back({ { -s,  0,  s } });
        m_vLines.push_back({ {  s,  0, -s } });
        m_vLines.push_back({ { -s,  0, -s } });
    }

    SpawnIndicators(pEnemy);
    m_ePhase = Phase::Windup;
}

void GaleSlashAttackBehavior::SpawnIndicators(EnemyComponent* pEnemy)
{
    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    RingMesh* pBorder = GetBorder(pDevice, pCmd);
    RingMesh* pFill   = GetFill(pDevice, pCmd);

    CRoom* pPrev = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    for (auto& line : m_vLines)
    {
        float cx = m_xmf3BossCenter.x + line.direction.x * (m_fLineLength * 0.5f);
        float cz = m_xmf3BossCenter.z + line.direction.z * (m_fLineLength * 0.5f);
        float yawDeg = atan2f(line.direction.x, line.direction.z) * (180.0f / XM_PI);

        // 테두리 — 녹색 톤 (바람)
        if (GameObject* pB = m_pScene->CreateGameObject(pDevice, pCmd))
        {
            auto* pT = pB->GetTransform();
            pT->SetPosition(cx, 0.18f, cz);
            pT->SetRotation(0.0f, yawDeg, 0.0f);
            pT->SetScale(m_fLineHalfWidth * 2.0f, 1.0f, m_fLineLength);
            pB->SetMesh(pBorder); pBorder->AddRef();
            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.1f, 0.35f, 0.15f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(0.4f, 1.0f, 0.5f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = XMFLOAT4(0.5f, 2.0f, 0.7f, 1.0f);
            pB->SetMaterial(mat);
            auto* pRC = pB->AddComponent<RenderComponent>();
            pRC->SetMesh(pBorder);
            pRC->SetOverlay(true);
            pShader->AddRenderComponent(pRC);
            pB->SetDecal(true);   // 셰이더 indicator path (shimmer + 톤다운) 활성화
            line.pBorder = pB;
        }

        // Fill
        if (GameObject* pF = m_pScene->CreateGameObject(pDevice, pCmd))
        {
            auto* pT = pF->GetTransform();
            pT->SetPosition(m_xmf3BossCenter.x, 0.15f, m_xmf3BossCenter.z);
            pT->SetRotation(0.0f, yawDeg, 0.0f);
            pT->SetScale(m_fLineHalfWidth * 2.0f, 1.0f, 0.01f);
            pF->SetMesh(pFill); pFill->AddRef();
            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.05f, 0.25f, 0.10f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(0.35f, 0.95f, 0.45f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = XMFLOAT4(0.4f, 1.5f, 0.5f, 1.0f);
            pF->SetMaterial(mat);
            auto* pRC = pF->AddComponent<RenderComponent>();
            pRC->SetMesh(pFill);
            pRC->SetOverlay(true);
            pShader->AddRenderComponent(pRC);
            pF->SetDecal(true);   // 셰이더 indicator path 활성화
            line.pFill = pF;
        }
    }

    m_pScene->SetCurrentRoom(pPrev);
}

void GaleSlashAttackBehavior::UpdateIndicatorsFill(float progress)
{
    float fillLen = m_fLineLength * progress;
    if (fillLen < 0.01f) fillLen = 0.01f;
    for (auto& line : m_vLines)
    {
        if (!line.pFill) continue;
        auto* pT = line.pFill->GetTransform();
        if (!pT) continue;
        float cx = m_xmf3BossCenter.x + line.direction.x * (fillLen * 0.5f);
        float cz = m_xmf3BossCenter.z + line.direction.z * (fillLen * 0.5f);
        pT->SetPosition(cx, 0.15f, cz);
        pT->SetScale(m_fLineHalfWidth * 2.0f, 1.0f, fillLen);
    }
}

void GaleSlashAttackBehavior::FireConeVFX(EnemyComponent* pEnemy)
{
    VFXManager* pVFX = GetVFX();
    if (!pVFX) return;

    for (auto& line : m_vLines)
    {
        // 보스 중심에서 라인 방향으로 cone 발사
        XMFLOAT3 origin = m_xmf3BossCenter;
        origin.y = 1.0f;
        line.nConeVFX = pVFX->Spawn("E_GaleRush_Cone", origin, line.direction, 0u, false);
    }

    if (m_fCameraShakeIntensity > 0.0f && m_pScene)
        if (CCamera* pCam = m_pScene->GetCamera())
            pCam->StartShake(m_fCameraShakeIntensity, m_fCameraShakeDuration);
}

void GaleSlashAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
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
        UpdateIndicatorsFill(p);
        if (m_fTimer >= m_fWindupTime)
        {
            DealLineDamage(pEnemy);
            FireConeVFX(pEnemy);
            m_ePhase = Phase::Impact;
            m_fTimer = 0.0f;
        }
        break;
    }
    case Phase::Impact:
        if (m_fTimer >= m_fImpactTime)
        {
            // 인디케이터 제거 (impact 끝나면 진공파 사라짐)
            for (auto& line : m_vLines)
            {
                if (line.pBorder) m_pScene->MarkForDeletion(line.pBorder);
                if (line.pFill)   m_pScene->MarkForDeletion(line.pFill);
                line.pBorder = nullptr;
                line.pFill = nullptr;
            }
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

void GaleSlashAttackBehavior::DealLineDamage(EnemyComponent* pEnemy)
{
    if (m_bDamageDealt || !m_pScene) return;
    m_bDamageDealt = true;

    std::vector<GameObject*> vPlayers = m_pScene->GetAllPlayers();
    for (GameObject* pPlayerObj : vPlayers)
    {
        if (!pPlayerObj) continue;
        auto* pPT = pPlayerObj->GetTransform();
        if (!pPT) continue;
        PlayerComponent* pPlayer = pPlayerObj->GetComponent<PlayerComponent>();
        if (!pPlayer) continue;

        XMFLOAT3 pp = pPT->GetPosition();
        bool bHit = false;
        for (auto& line : m_vLines)
        {
            float rx = pp.x - m_xmf3BossCenter.x;
            float rz = pp.z - m_xmf3BossCenter.z;
            float along = rx * line.direction.x + rz * line.direction.z;
            if (along < 0.0f || along > m_fLineLength) continue;
            float px = rx - line.direction.x * along;
            float pz = rz - line.direction.z * along;
            if (sqrtf(px * px + pz * pz) <= m_fLineHalfWidth)
            {
                bHit = true;
                break;
            }
        }
        if (bHit) pPlayer->TakeDamage(m_fDamage);
    }
}

void GaleSlashAttackBehavior::CleanupAll()
{
    VFXManager* pVFX = GetVFX();
    for (auto& line : m_vLines)
    {
        if (m_pScene)
        {
            if (line.pBorder) m_pScene->MarkForDeletion(line.pBorder);
            if (line.pFill)   m_pScene->MarkForDeletion(line.pFill);
        }
        if (pVFX && line.nConeVFX >= 0) pVFX->Stop(line.nConeVFX);
    }
    m_vLines.clear();
}

bool GaleSlashAttackBehavior::IsFinished() const { return m_bFinished; }

void GaleSlashAttackBehavior::Reset()
{
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
    m_bFinished = false;
    m_bDamageDealt = false;
    m_bAnimReturnedToIdle = false;
    CleanupAll();
}
