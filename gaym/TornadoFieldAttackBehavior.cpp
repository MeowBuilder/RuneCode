#include "stdafx.h"
#include "TornadoFieldAttackBehavior.h"
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

    static RingMesh* s_pBorderMesh = nullptr;
    static RingMesh* s_pFillMesh   = nullptr;

    RingMesh* GetBorder(ID3D12Device* d, ID3D12GraphicsCommandList* c)
    {
        if (!s_pBorderMesh) s_pBorderMesh = new RingMesh(d, c, 1.0f, 0.90f, 32);
        return s_pBorderMesh;
    }
    RingMesh* GetFill(ID3D12Device* d, ID3D12GraphicsCommandList* c)
    {
        if (!s_pFillMesh) s_pFillMesh = new RingMesh(d, c, 1.0f, 0.0f, 32);
        return s_pFillMesh;
    }
}

TornadoFieldAttackBehavior::TornadoFieldAttackBehavior(
    int nTornadoCount, float fTickDamage, float fTickInterval, float fTornadoRadius,
    float fSpawnMinRadius, float fSpawnMaxRadius,
    float fWindupTime, float fActiveDuration, float fRecoveryTime,
    float fCameraShakeIntensity, float fCameraShakeDuration)
    : m_nTornadoCount(nTornadoCount)
    , m_fTickDamage(fTickDamage)
    , m_fTickInterval(fTickInterval)
    , m_fTornadoRadius(fTornadoRadius)
    , m_fSpawnMinRadius(fSpawnMinRadius)
    , m_fSpawnMaxRadius(fSpawnMaxRadius)
    , m_fWindupTime(fWindupTime)
    , m_fActiveDuration(fActiveDuration)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fCameraShakeIntensity(fCameraShakeIntensity)
    , m_fCameraShakeDuration(fCameraShakeDuration)
{
}

void TornadoFieldAttackBehavior::Execute(EnemyComponent* pEnemy)
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

    // 토네이도 위치 결정
// 온라인 모드에서 서버 좌표가 들어온 경우 모든 클라가 같은 위치를 사용한다.
// 서버 좌표가 없으면 오프라인/테스트용으로 기존 로컬 랜덤 위치를 사용한다.
    m_vTornadoes.clear();

    if (!m_vServerPositions.empty())
    {
        m_vTornadoes.reserve(m_vServerPositions.size());

        for (const XMFLOAT3& serverPos : m_vServerPositions)
        {
            TornadoInstance t0;
            t0.pos = serverPos;
            t0.pos.y = 0.0f;
            m_vTornadoes.push_back(t0);
        }
    }
    else
    {
        m_vTornadoes.reserve(m_nTornadoCount);

        float angleOffset = ((float)rand() / RAND_MAX) * XM_2PI;
        float radiusRange = m_fSpawnMaxRadius - m_fSpawnMinRadius;

        for (int i = 0; i < m_nTornadoCount; ++i)
        {
            float a = angleOffset + (XM_2PI / m_nTornadoCount) * i
                + ((float)rand() / RAND_MAX - 0.5f) * 0.4f;  // ±0.2 rad jitter

            float t = (float)rand() / RAND_MAX;
            float r = m_fSpawnMinRadius + sqrtf(t) * radiusRange;

            TornadoInstance t0;
            t0.pos.x = bossPos.x + cosf(a) * r;
            t0.pos.y = 0.0f;
            t0.pos.z = bossPos.z + sinf(a) * r;
            m_vTornadoes.push_back(t0);
        }
    }

    SpawnIndicators(pEnemy);
    m_ePhase = Phase::Windup;
}

void TornadoFieldAttackBehavior::SpawnIndicators(EnemyComponent* pEnemy)
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

    VFXManager* pVFX = GetVFX();

    for (auto& t : m_vTornadoes)
    {
        // 테두리 링
        if (GameObject* pB = m_pScene->CreateGameObject(pDevice, pCmd))
        {
            auto* pT = pB->GetTransform();
            pT->SetPosition(t.pos.x, 0.15f, t.pos.z);
            pT->SetScale(m_fTornadoRadius, 1.0f, m_fTornadoRadius);
            pB->SetMesh(pBorder); pBorder->AddRef();
            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.5f, 0.02f, 0.02f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.2f, 0.1f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = XMFLOAT4(2.0f, 0.3f, 0.1f, 1.0f);
            pB->SetMaterial(mat);
            auto* pRC = pB->AddComponent<RenderComponent>();
            pRC->SetMesh(pBorder);
            pRC->SetOverlay(true);
            pShader->AddRenderComponent(pRC);
            t.pBorder = pB;
        }

        // 차오름 fill
        if (GameObject* pF = m_pScene->CreateGameObject(pDevice, pCmd))
        {
            auto* pT = pF->GetTransform();
            pT->SetPosition(t.pos.x, 0.10f, t.pos.z);
            pT->SetScale(0.01f, 1.0f, 0.01f);
            pF->SetMesh(pFill); pFill->AddRef();
            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.3f, 0.02f, 0.0f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.35f, 0.05f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = XMFLOAT4(1.3f, 0.5f, 0.08f, 1.0f);
            pF->SetMaterial(mat);
            auto* pRC = pF->AddComponent<RenderComponent>();
            pRC->SetMesh(pFill);
            pRC->SetOverlay(true);
            pShader->AddRenderComponent(pRC);
            t.pFill = pF;
        }

        // 경고 VFX (windup 동안 펄스 링)
        if (pVFX)
        {
            XMFLOAT3 dir(0.0f, 1.0f, 0.0f);
            t.nWarningVFX = pVFX->Spawn("Wind_TornadoWarning", t.pos, dir, 0u, false);
        }
    }

    m_pScene->SetCurrentRoom(pPrev);
}

void TornadoFieldAttackBehavior::ActivateTornadoes(EnemyComponent* pEnemy)
{
    VFXManager* pVFX = GetVFX();
    if (!pVFX) return;

    for (auto& t : m_vTornadoes)
    {
        XMFLOAT3 base(t.pos.x, 0.5f, t.pos.z);
        XMFLOAT3 dir(0.0f, 1.0f, 0.0f);
        t.nTornadoVFX = pVFX->Spawn("Demon_Tornado", base, dir, 0u, false);

        // 경고 VFX 는 즉시 중단 (warning 페이즈 종료)
        if (t.nWarningVFX >= 0)
        {
            pVFX->Stop(t.nWarningVFX);
            t.nWarningVFX = -1;
        }
    }

    if (m_fCameraShakeIntensity > 0.0f && m_pScene)
        if (CCamera* pCam = m_pScene->GetCamera())
            pCam->StartShake(m_fCameraShakeIntensity, m_fCameraShakeDuration);
}

void TornadoFieldAttackBehavior::TickDamage(float dt, EnemyComponent* pEnemy)
{
    if (!m_pScene) return;

    m_fTickTimer += dt;
    if (m_fTickTimer < m_fTickInterval) return;
    m_fTickTimer -= m_fTickInterval;

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
        for (auto& t : m_vTornadoes)
        {
            float dx = pp.x - t.pos.x;
            float dz = pp.z - t.pos.z;
            if (dx * dx + dz * dz <= m_fTornadoRadius * m_fTornadoRadius)
            {
                bHit = true;
                break;
            }
        }
        if (bHit) pPlayer->TakeDamage(m_fTickDamage);
    }
}

void TornadoFieldAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;
    m_fTimer += dt;

    // 애니 1회 재생 후 idle 자동 복귀
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
        float fillR = m_fTornadoRadius * p;
        if (fillR < 0.01f) fillR = 0.01f;
        for (auto& t : m_vTornadoes)
            if (t.pFill) t.pFill->GetTransform()->SetScale(fillR, 1.0f, fillR);

        if (m_fTimer >= m_fWindupTime)
        {
            ActivateTornadoes(pEnemy);
            // 경고용 인디케이터는 active 진입 시 제거 (토네이도 VFX 가 가시화 담당)
            for (auto& t : m_vTornadoes)
            {
                if (t.pBorder) m_pScene->MarkForDeletion(t.pBorder);
                if (t.pFill)   m_pScene->MarkForDeletion(t.pFill);
                t.pBorder = nullptr;
                t.pFill = nullptr;
            }
            m_ePhase = Phase::Active;
            m_fTimer = 0.0f;
            m_fTickTimer = m_fTickInterval;  // 첫 틱 즉시
        }
        break;
    }
    case Phase::Active:
    {
        TickDamage(dt, pEnemy);
        if (m_fTimer >= m_fActiveDuration)
        {
            // 토네이도 VFX 중단
            if (VFXManager* pVFX = GetVFX())
            {
                for (auto& t : m_vTornadoes)
                {
                    if (t.nTornadoVFX >= 0)
                    {
                        pVFX->Stop(t.nTornadoVFX);
                        t.nTornadoVFX = -1;
                    }
                }
            }
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

void TornadoFieldAttackBehavior::CleanupAll()
{
    VFXManager* pVFX = GetVFX();
    for (auto& t : m_vTornadoes)
    {
        if (m_pScene)
        {
            if (t.pBorder) m_pScene->MarkForDeletion(t.pBorder);
            if (t.pFill)   m_pScene->MarkForDeletion(t.pFill);
        }
        if (pVFX)
        {
            if (t.nWarningVFX >= 0) pVFX->Stop(t.nWarningVFX);
            if (t.nTornadoVFX >= 0) pVFX->Stop(t.nTornadoVFX);
        }
    }
    m_vTornadoes.clear();
}

bool TornadoFieldAttackBehavior::IsFinished() const { return m_bFinished; }

void TornadoFieldAttackBehavior::Reset()
{
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
    m_fTickTimer = 0.0f;
    m_bFinished = false;
    m_bAnimReturnedToIdle = false;
    CleanupAll();
}
