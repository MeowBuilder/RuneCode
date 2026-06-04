#include "stdafx.h"
#include "GroundRuptureAttackBehavior.h"
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
#include "MeshLoader.h"
#include "Dx12App.h"
#include "AnimationComponent.h"
#include <functional>

// 공유 Ring/Disc mesh — 균열 인디케이터 용 (얇게 stretched → 직사각형 느낌)
static RingMesh* s_pRuptureBorderMesh = nullptr;
static RingMesh* s_pRuptureFillMesh   = nullptr;

static RingMesh* GetBorderMesh(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd)
{
    if (!s_pRuptureBorderMesh)
        s_pRuptureBorderMesh = new RingMesh(pDevice, pCmd, 1.0f, 0.92f, 32);
    return s_pRuptureBorderMesh;
}
static RingMesh* GetFillMesh(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd)
{
    if (!s_pRuptureFillMesh)
        s_pRuptureFillMesh = new RingMesh(pDevice, pCmd, 1.0f, 0.0f, 32);
    return s_pRuptureFillMesh;
}

GroundRuptureAttackBehavior::GroundRuptureAttackBehavior(
    RuptureShape eShape, float fDamage, float fLineLength, float fLineHalfWidth,
    float fWindupTime, float fImpactTime, float fRecoveryTime,
    float fCameraShakeIntensity, float fCameraShakeDuration,
    const char* pClipOverride)
    : m_eShape(eShape)
    , m_fDamage(fDamage)
    , m_fLineLength(fLineLength)
    , m_fLineHalfWidth(fLineHalfWidth)
    , m_fWindupTime(fWindupTime)
    , m_fImpactTime(fImpactTime)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fCameraShakeIntensity(fCameraShakeIntensity)
    , m_fCameraShakeDuration(fCameraShakeDuration)
    , m_strClipOverride(pClipOverride)
{
}

void GroundRuptureAttackBehavior::SetNetworkEffectSeed(uint32 seed)
{
    // 1. 서버가 보내준 seed를 저장한다.
    m_uNetworkSeed = seed;

    // 2. seed가 0이어도 기본 seed로 보정해서 네트워크 모드를 켠다.
    m_bUseNetworkSeed = true;
}

void GroundRuptureAttackBehavior::Execute(EnemyComponent* pEnemy)
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

    // 4방향 결정
    m_vLines.clear();
    m_vLines.reserve(4);

    if (m_eShape == RuptureShape::Cross)
    {
        // 십자 — 4방향 (+X, -X, +Z, -Z)
        m_vLines.push_back({ {  1,  0,  0 } });
        m_vLines.push_back({ { -1,  0,  0 } });
        m_vLines.push_back({ {  0,  0,  1 } });
        m_vLines.push_back({ {  0,  0, -1 } });
    }
    else
    {
        // X 대각선
        float s = 0.7071f;
        m_vLines.push_back({ {  s,  0,  s } });
        m_vLines.push_back({ { -s,  0,  s } });
        m_vLines.push_back({ {  s,  0, -s } });
        m_vLines.push_back({ { -s,  0, -s } });
    }

    SpawnIndicators(pEnemy);
    m_ePhase = Phase::Windup;
}

void GroundRuptureAttackBehavior::SpawnIndicators(EnemyComponent* pEnemy)
{
    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    RingMesh* pBorderMesh = GetBorderMesh(pDevice, pCmd);
    RingMesh* pFillMesh   = GetFillMesh(pDevice, pCmd);

    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    for (auto& line : m_vLines)
    {
        // 균열 중심 = 보스 + direction * (length/2)
        float cx = m_xmf3BossCenter.x + line.direction.x * (m_fLineLength * 0.5f);
        float cz = m_xmf3BossCenter.z + line.direction.z * (m_fLineLength * 0.5f);

        // direction 의 yaw 계산 (Z 방향 기준 atan2(x, z))
        float yawDeg = atan2f(line.direction.x, line.direction.z) * (180.0f / XM_PI);

        // 테두리 — 고정 크기 (전체 길이)
        GameObject* pBorder = m_pScene->CreateGameObject(pDevice, pCmd);
        if (pBorder)
        {
            auto* pT = pBorder->GetTransform();
            pT->SetPosition(cx, 0.18f, cz);
            pT->SetRotation(0.0f, yawDeg, 0.0f);
            // scale: x = width (2*halfWidth), z = length. Ring mesh 는 원인데 스케일로 stretched → 타원/긴 링 형태
            pT->SetScale(m_fLineHalfWidth * 2.0f, 1.0f, m_fLineLength);

            pBorder->SetMesh(pBorderMesh);
            pBorderMesh->AddRef();

            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.5f, 0.02f, 0.02f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.2f, 0.1f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = XMFLOAT4(2.0f, 0.3f, 0.1f, 1.0f);
            pBorder->SetMaterial(mat);

            auto* pRC = pBorder->AddComponent<RenderComponent>();
            pRC->SetMesh(pBorderMesh);
            pRC->SetOverlay(true);
            pShader->AddRenderComponent(pRC);
            line.pBorder = pBorder;
        }

        // Fill — windup 동안 보스에서 끝으로 뻗어나감
        GameObject* pFill = m_pScene->CreateGameObject(pDevice, pCmd);
        if (pFill)
        {
            auto* pT = pFill->GetTransform();
            pT->SetPosition(m_xmf3BossCenter.x, 0.15f, m_xmf3BossCenter.z);  // 보스에서 시작
            pT->SetRotation(0.0f, yawDeg, 0.0f);
            pT->SetScale(m_fLineHalfWidth * 2.0f, 1.0f, 0.01f);  // 길이 0 시작

            pFill->SetMesh(pFillMesh);
            pFillMesh->AddRef();

            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.3f, 0.02f, 0.0f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.35f, 0.05f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            mat.m_cEmissive = XMFLOAT4(1.3f, 0.5f, 0.08f, 1.0f);
            pFill->SetMaterial(mat);

            auto* pRC = pFill->AddComponent<RenderComponent>();
            pRC->SetMesh(pFillMesh);
            pRC->SetOverlay(true);
            pShader->AddRenderComponent(pRC);
            line.pFill = pFill;
        }
    }

    m_pScene->SetCurrentRoom(pPrevRoom);
}

void GroundRuptureAttackBehavior::UpdateIndicatorsFill(float progress)
{
    // Fill 길이 = m_fLineLength × progress, 보스에서 전방으로 뻗어나감
    float fillLen = m_fLineLength * progress;
    if (fillLen < 0.01f) fillLen = 0.01f;

    for (auto& line : m_vLines)
    {
        if (!line.pFill) continue;
        auto* pT = line.pFill->GetTransform();
        if (!pT) continue;

        // 중심 = 보스 + direction * (fillLen / 2)
        float cx = m_xmf3BossCenter.x + line.direction.x * (fillLen * 0.5f);
        float cz = m_xmf3BossCenter.z + line.direction.z * (fillLen * 0.5f);
        pT->SetPosition(cx, 0.15f, cz);
        pT->SetScale(m_fLineHalfWidth * 2.0f, 1.0f, fillLen);
    }
}

void GroundRuptureAttackBehavior::SpawnBurstRocks(EnemyComponent* pEnemy)
{
    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    // 균열 돌 연출용 랜덤 함수
    // 온라인에서는 서버 seed 기반 결정론적 난수를 사용해서 모든 클라가 같은 돌 모양/배치를 만든다.
    // 오프라인에서는 기존 rand()를 그대로 사용한다.
    uint32 seed = (m_uNetworkSeed != 0) ? m_uNetworkSeed : 0x9E3779B9u;

    auto Rand01FromSeed = [](uint32& s) -> float
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return (s & 0x00FFFFFF) / static_cast<float>(0x01000000);
        };

    auto RandRange = [&](float a, float b) -> float
        {
            float r = 0.0f;

            if (m_bUseNetworkSeed)
                r = Rand01FromSeed(seed);
            else
                r = (float)rand() / RAND_MAX;

            return a + (b - a) * r;
        };

    for (auto& line : m_vLines)
    {
        // 각 line 에 5개 정도 바위가 선을 따라 솟음
        const int nPerLine = 5;
        for (int i = 0; i < nPerLine; ++i)
        {
            float t = (float)(i + 1) / (float)nPerLine;  // 0.2, 0.4, 0.6, 0.8, 1.0
            float dist = m_fLineLength * t;
            // 선 따라 위치 + 약간 수직 방향 jitter
            float jitter = RandRange(-m_fLineHalfWidth * 0.5f, m_fLineHalfWidth * 0.5f);
            float perpX = -line.direction.z;  // 수직 (right hand rule)
            float perpZ =  line.direction.x;

            float px = m_xmf3BossCenter.x + line.direction.x * dist + perpX * jitter;
            float pz = m_xmf3BossCenter.z + line.direction.z * dist + perpZ * jitter;

            GameObject* pRock = MeshLoader::LoadGeometryFromFile(
                m_pScene, pDevice, pCmd, nullptr,
                "Assets/Enemies/Rock&Golem/SM_Rocks_03.bin");
            if (!pRock) continue;

            auto* pT = pRock->GetTransform();
            // 시작 위치 — 지면 더 깊숙이 (솟아오르는 드라마 확대)
            pT->SetPosition(px, -5.0f, pz);

            // 랜덤 자세
            pT->SetRotation(
                RandRange(0.0f, 360.0f),
                atan2f(line.direction.x, line.direction.z) * (180.0f / XM_PI) + RandRange(-20.0f, 20.0f),
                RandRange(0.0f, 360.0f));

            // 스케일 — 위협감 크게 (2.5~4 → 5~8)
            float scale = RandRange(5.0f, 8.0f);
            pT->SetScale(scale, scale, scale);

            // 재질 (돌)
            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(0.28f, 0.23f, 0.20f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(0.55f, 0.48f, 0.42f, 1.0f);
            mat.m_cSpecular = XMFLOAT4(0.1f, 0.1f, 0.1f, 14.0f);
            mat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            pRock->SetMaterial(mat);

            std::function<void(GameObject*)> Reg = [&](GameObject* p) {
                if (!p) return;
                auto* pRC = p->GetComponent<RenderComponent>();
                if (!pRC && p->GetMesh()) {
                    pRC = p->AddComponent<RenderComponent>();
                    pRC->SetMesh(p->GetMesh());
                }
                if (pRC) pShader->AddRenderComponent(pRC);
                if (p->m_pChild) Reg(p->m_pChild);
                if (p->m_pSibling) Reg(p->m_pSibling);
            };
            Reg(pRock);

            line.vBurstRocks.push_back(pRock);
        }
    }

    m_pScene->SetCurrentRoom(pPrevRoom);
}

void GroundRuptureAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;
    m_fTimer += dt;

    // 공격 애니 1회 재생 후 idle 전환
    if (!m_bAnimReturnedToIdle && m_fTimer > 0.1f)
    {
        if (auto* pAnim = pEnemy->GetAnimationComponent())
        {
            if (!pAnim->IsPlaying())
            {
                pAnim->CrossFade("Golem_battle_stand_ge", 0.25f, true, true);
                m_bAnimReturnedToIdle = true;
            }
        }
    }

    switch (m_ePhase)
    {
    case Phase::Windup:
    {
        // Fill 뻗어남
        float progress = (std::min)(m_fTimer / m_fWindupTime, 1.0f);
        UpdateIndicatorsFill(progress);

        if (m_fTimer >= m_fWindupTime)
        {
            // Impact 진입 — 데미지 + 바위 솟음 + 쉐이크
            DealLineDamage(pEnemy);
            SpawnBurstRocks(pEnemy);

            if (m_fCameraShakeIntensity > 0.0f)
            {
                if (CCamera* pCam = m_pScene->GetCamera())
                    pCam->StartShake(m_fCameraShakeIntensity, m_fCameraShakeDuration);
            }

            m_ePhase = Phase::Impact;
            m_fTimer = 0.0f;
        }
        break;
    }

    case Phase::Impact:
    {
        // 바위 솟음 애니메이션 — -5 에서 솟아 +3 까지 올라옴 (더 높게, 위협감 증폭)
        float t = (std::min)(m_fTimer / m_fImpactTime, 1.0f);
        // easeOut — 처음 빠르게 솟고 점차 감속
        float smoothT = 1.0f - (1.0f - t) * (1.0f - t);
        float targetY = -5.0f + 8.0f * smoothT;  // -5 → 3

        for (auto& line : m_vLines)
        {
            for (GameObject* pRock : line.vBurstRocks)
            {
                if (!pRock) continue;
                auto* pT = pRock->GetTransform();
                if (!pT) continue;
                XMFLOAT3 pos = pT->GetPosition();
                pos.y = targetY;
                pT->SetPosition(pos);
            }
        }

        if (m_fTimer >= m_fImpactTime)
        {
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

void GroundRuptureAttackBehavior::DealLineDamage(EnemyComponent* pEnemy)
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

        bool bHitAny = false;
        for (auto& line : m_vLines)
        {
            // 플레이어의 보스 상대 위치 → 선 방향 투영 → 직사각형 판정
            float rx = pp.x - m_xmf3BossCenter.x;
            float rz = pp.z - m_xmf3BossCenter.z;

            // 선을 따라 얼마나 진행했는지 (알롱 축)
            float along = rx * line.direction.x + rz * line.direction.z;
            if (along < 0.0f || along > m_fLineLength) continue;

            // 선에 수직인 거리
            float perp_x = rx - line.direction.x * along;
            float perp_z = rz - line.direction.z * along;
            float perpDist = sqrtf(perp_x * perp_x + perp_z * perp_z);
            if (perpDist <= m_fLineHalfWidth)
            {
                bHitAny = true;
                break;
            }
        }

        if (bHitAny)
            pPlayer->TakeDamage(m_fDamage);
    }
}

void GroundRuptureAttackBehavior::CleanupAll()
{
    if (!m_pScene) return;
    for (auto& line : m_vLines)
    {
        if (line.pBorder) m_pScene->MarkForDeletion(line.pBorder);
        if (line.pFill)   m_pScene->MarkForDeletion(line.pFill);
        for (GameObject* pR : line.vBurstRocks)
            if (pR) m_pScene->MarkForDeletion(pR);
    }
    m_vLines.clear();
}

bool GroundRuptureAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void GroundRuptureAttackBehavior::Reset()
{
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
    m_bFinished = false;
    m_bDamageDealt = false;
    CleanupAll();
}
