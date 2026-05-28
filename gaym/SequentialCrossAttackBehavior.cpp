#include "stdafx.h"
#include "SequentialCrossAttackBehavior.h"
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
#include <algorithm>

static RingMesh* s_pSeqCrossBorderMesh = nullptr;
static RingMesh* s_pSeqCrossFillMesh   = nullptr;

static RingMesh* GetSeqBorderMesh(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd)
{
    if (!s_pSeqCrossBorderMesh)
        s_pSeqCrossBorderMesh = new RingMesh(pDevice, pCmd, 1.0f, 0.92f, 32);
    return s_pSeqCrossBorderMesh;
}
static RingMesh* GetSeqFillMesh(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd)
{
    if (!s_pSeqCrossFillMesh)
        s_pSeqCrossFillMesh = new RingMesh(pDevice, pCmd, 1.0f, 0.0f, 32);
    return s_pSeqCrossFillMesh;
}

SequentialCrossAttackBehavior::SequentialCrossAttackBehavior(
    float fDamagePerCross, float fBarHalfLength, float fBarHalfWidth,
    float fWindupTime, float fExplosionInterval, float fExplosionFlash,
    float fRecoveryTime, float fCameraShakeIntensity, float fCameraShakeDuration)
    : m_fDamagePerCross(fDamagePerCross)
    , m_fBarHalfLength(fBarHalfLength)
    , m_fBarHalfWidth(fBarHalfWidth)
    , m_fWindupTime(fWindupTime)
    , m_fExplosionInterval(fExplosionInterval)
    , m_fExplosionFlash(fExplosionFlash)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fCameraShakeIntensity(fCameraShakeIntensity)
    , m_fCameraShakeDuration(fCameraShakeDuration)
{
}

void SequentialCrossAttackBehavior::SetNetworkEffectSeed(uint32 seed)
{
    // 서버가 보내준 seed를 저장한다.
    // 이 seed를 사용해 모든 클라에서 같은 돌 jitter / 회전 / 스케일을 만든다.
    m_uNetworkSeed = seed;
    m_bUseNetworkSeed = true;
}

void SequentialCrossAttackBehavior::Execute(EnemyComponent* pEnemy)
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

    // 십자 3개 — yaw 0°, 30°, 60° (비대칭 분할로 안전 웨지가 매 폭발마다 이동)
    // order 는 yaw 순서와 독립적 — 랜덤 셔플해서 "예측" 난이도를 더 할 수 있지만
    // 일단 확정적(0°→30°→60°) 로 시작, 추후 튜닝 여지.
    m_vCrosses.clear();
    m_vCrosses.reserve(3);

    const float yaws[3]  = { 0.0f, 30.0f, 60.0f };
    const int   orders[3] = { 0, 1, 2 };

    for (int i = 0; i < 3; ++i)
    {
        Cross c;
        c.yawDeg = yaws[i];
        c.order  = orders[i];
        // fill 시작/폭발 시각: order 0 은 즉시 시작해 m_fWindupTime 에 폭발,
        // 뒤 order 는 interval 만큼 늦게 시작해 같은 fill 시간 유지 (속도 동일)
        c.fillStartTime = (float)c.order * m_fExplosionInterval;
        c.explosionTime = m_fWindupTime + (float)c.order * m_fExplosionInterval;
        m_vCrosses.push_back(c);
    }

    SpawnIndicators(pEnemy);
    m_ePhase = Phase::Windup;
}

void SequentialCrossAttackBehavior::MakeBar(
    EnemyComponent* /*pEnemy*/, Cross& /*c*/, float yawDegBar,
    bool bIsFill, const XMFLOAT4& emissive,
    GameObject** outObj)
{
    *outObj = nullptr;
    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    RingMesh* pMesh = bIsFill ? GetSeqFillMesh(pDevice, pCmd)
                              : GetSeqBorderMesh(pDevice, pCmd);

    GameObject* pObj = m_pScene->CreateGameObject(pDevice, pCmd);
    if (!pObj) return;

    auto* pT = pObj->GetTransform();
    // 보스 중심에 위치 (막대는 중심에서 양방향으로 뻗는다 — 스케일로 길이 표현)
    pT->SetPosition(m_xmf3BossCenter.x,
                    bIsFill ? 0.19f : 0.17f,   // fill 이 살짝 위
                    m_xmf3BossCenter.z);
    pT->SetRotation(0.0f, yawDegBar, 0.0f);

    // border 는 전체 크기 고정, fill 은 length 0 에서 시작
    float lenScale = bIsFill ? 0.01f : (m_fBarHalfLength * 2.0f);
    pT->SetScale(m_fBarHalfWidth * 2.0f, 1.0f, lenScale);

    pObj->SetMesh(pMesh);
    pMesh->AddRef();

    MATERIAL mat;
    if (bIsFill)
    {
        mat.m_cAmbient  = XMFLOAT4(0.25f, 0.02f, 0.0f, 1.0f);
        mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.4f, 0.1f, 1.0f);
    }
    else
    {
        mat.m_cAmbient  = XMFLOAT4(0.5f, 0.04f, 0.04f, 1.0f);
        mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.25f, 0.15f, 1.0f);
    }
    mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    mat.m_cEmissive = emissive;
    pObj->SetMaterial(mat);

    auto* pRC = pObj->AddComponent<RenderComponent>();
    pRC->SetMesh(pMesh);
    pRC->SetOverlay(true);
    pShader->AddRenderComponent(pRC);

    *outObj = pObj;
}

void SequentialCrossAttackBehavior::SpawnIndicators(EnemyComponent* pEnemy)
{
    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    // order 별 emissive: 빨강(가장 먼저) > 주황 > 노랑(가장 나중)
    //   border 는 약하게, fill 은 강하게 — fill 의 밝기가 순서를 전달
    const XMFLOAT4 borderEmiss[3] = {
        XMFLOAT4(1.4f, 0.20f, 0.10f, 1.0f),
        XMFLOAT4(1.2f, 0.45f, 0.10f, 1.0f),
        XMFLOAT4(0.9f, 0.80f, 0.10f, 1.0f),
    };
    const XMFLOAT4 fillEmiss[3] = {
        XMFLOAT4(2.4f, 0.35f, 0.10f, 1.0f),
        XMFLOAT4(2.0f, 0.80f, 0.15f, 1.0f),
        XMFLOAT4(1.5f, 1.40f, 0.20f, 1.0f),
    };

    for (auto& c : m_vCrosses)
    {
        int idx = std::clamp(c.order, 0, 2);
        // 세로 막대 (yaw = c.yawDeg)
        MakeBar(pEnemy, c, c.yawDeg,          false, borderEmiss[idx], &c.pBorderV);
        MakeBar(pEnemy, c, c.yawDeg,          true,  fillEmiss[idx],   &c.pFillV);
        // 가로 막대 (yaw = c.yawDeg + 90°)
        MakeBar(pEnemy, c, c.yawDeg + 90.0f,  false, borderEmiss[idx], &c.pBorderH);
        MakeBar(pEnemy, c, c.yawDeg + 90.0f,  true,  fillEmiss[idx],   &c.pFillH);
    }

    m_pScene->SetCurrentRoom(pPrevRoom);
}

void SequentialCrossAttackBehavior::UpdateCrossFill(Cross& c, float windupElapsed)
{
    if (c.bExploded) return;
    // fill 은 fillStartTime 부터 explosionTime 까지 선형 증가
    float fillDuration = c.explosionTime - c.fillStartTime;
    if (fillDuration < 0.001f) fillDuration = 0.001f;
    float t = (windupElapsed - c.fillStartTime) / fillDuration;
    t = std::clamp(t, 0.0f, 1.0f);

    float lenScale = m_fBarHalfLength * 2.0f * t;
    if (lenScale < 0.01f) lenScale = 0.01f;

    auto applyScale = [&](GameObject* pObj) {
        if (!pObj) return;
        auto* pT = pObj->GetTransform();
        if (!pT) return;
        pT->SetScale(m_fBarHalfWidth * 2.0f, 1.0f, lenScale);
    };
    applyScale(c.pFillV);
    applyScale(c.pFillH);
}

void SequentialCrossAttackBehavior::ExplodeCross(EnemyComponent* pEnemy, Cross& c)
{
    if (c.bExploded) return;
    c.bExploded = true;

    DealCrossDamage(pEnemy, c);

    // 바위 솟음 — V/H 각 막대 따라 몇 개
    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp || !m_pScene) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    // 십자 폭발 돌 연출용 랜덤 함수
    // 온라인에서는 서버 seed 기반 결정론적 난수를 사용해서 모든 클라가 같은 돌 배치를 만든다.
    // 오프라인에서는 기존 rand()를 그대로 사용한다.
    uint32 seed = (m_uNetworkSeed != 0) ? m_uNetworkSeed : 0x9E3779B9u;

    // 폭발 순서별로 seed를 조금 섞어서 1번째/2번째/3번째 십자가 서로 다른 돌 배치를 갖게 한다.
    seed ^= static_cast<uint32>((c.order + 1) * 0x45D9F3Bu);

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

    // 두 개 막대 방향 (yaw, yaw+90)
    float yawRadV = c.yawDeg * (XM_PI / 180.0f);
    float yawRadH = (c.yawDeg + 90.0f) * (XM_PI / 180.0f);
    XMFLOAT2 dirs[2] = {
        XMFLOAT2(sinf(yawRadV), cosf(yawRadV)),
        XMFLOAT2(sinf(yawRadH), cosf(yawRadH)),
    };

    const int nPerBar = 3;
    for (int d = 0; d < 2; ++d)
    {
        for (int i = 0; i < nPerBar; ++i)
        {
            // 양방향 (−length ~ +length) 에 골고루
            float t = -1.0f + 2.0f * ((float)(i + 1) / (float)(nPerBar + 1));  // -0.5, 0, 0.5 (nPerBar=3 기준)
            float dist = m_fBarHalfLength * t;
            float jitter = RandRange(-m_fBarHalfWidth * 0.5f, m_fBarHalfWidth * 0.5f);
            float perpX = -dirs[d].y;
            float perpZ =  dirs[d].x;

            float px = m_xmf3BossCenter.x + dirs[d].x * dist + perpX * jitter;
            float pz = m_xmf3BossCenter.z + dirs[d].y * dist + perpZ * jitter;

            GameObject* pRock = MeshLoader::LoadGeometryFromFile(
                m_pScene, pDevice, pCmd, nullptr,
                "Assets/Enemies/Rock&Golem/SM_Rocks_03.bin");
            if (!pRock) continue;

            auto* pT = pRock->GetTransform();
            pT->SetPosition(px, -5.0f, pz);

            pT->SetRotation(
                RandRange(0.0f, 360.0f),
                atan2f(dirs[d].x, dirs[d].y) * (180.0f / XM_PI) + RandRange(-20.0f, 20.0f),
                RandRange(0.0f, 360.0f));

            float scale = RandRange(4.5f, 7.0f);
            pT->SetScale(scale, scale, scale);

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

            c.vBurstRocks.push_back(pRock);
        }
    }

    m_pScene->SetCurrentRoom(pPrevRoom);

    // 카메라 쉐이크 (폭발마다)
    if (m_fCameraShakeIntensity > 0.0f)
    {
        if (CCamera* pCam = m_pScene->GetCamera())
            pCam->StartShake(m_fCameraShakeIntensity, m_fCameraShakeDuration);
    }

    // 터진 십자의 fill/border 는 즉시 정리 (다음 폭발 가독성 확보)
    CleanupCrossMeshes(c);
}

void SequentialCrossAttackBehavior::UpdateBurstRocks(float /*dt*/)
{
    for (auto& c : m_vCrosses)
    {
        if (!c.bExploded) continue;
        float timeSinceExplosion = m_fWindupElapsed - c.explosionTime;
        if (timeSinceExplosion < 0.0f) continue;
        float t = std::clamp(timeSinceExplosion / m_fExplosionFlash, 0.0f, 1.0f);
        float smoothT = 1.0f - (1.0f - t) * (1.0f - t);
        float targetY = -5.0f + 8.0f * smoothT;
        for (GameObject* pRock : c.vBurstRocks)
        {
            if (!pRock) continue;
            auto* pT = pRock->GetTransform();
            if (!pT) continue;
            XMFLOAT3 pos = pT->GetPosition();
            pos.y = targetY;
            pT->SetPosition(pos);
        }
    }
}

void SequentialCrossAttackBehavior::DealCrossDamage(EnemyComponent* /*pEnemy*/, const Cross& c)
{
    if (!m_pScene) return;

    std::vector<GameObject*> vPlayers = m_pScene->GetAllPlayers();

    float yawRad = c.yawDeg * (XM_PI / 180.0f);
    float s = sinf(yawRad);
    float co = cosf(yawRad);

    for (GameObject* pPlayerObj : vPlayers)
    {
        if (!pPlayerObj) continue;
        auto* pPT = pPlayerObj->GetTransform();
        if (!pPT) continue;
        PlayerComponent* pPlayer = pPlayerObj->GetComponent<PlayerComponent>();
        if (!pPlayer) continue;

        XMFLOAT3 pp = pPT->GetPosition();
        float rx = pp.x - m_xmf3BossCenter.x;
        float rz = pp.z - m_xmf3BossCenter.z;

        // V 막대 로컬 좌표 (bar axis = (s, co))
        float along = rx * s  + rz * co;   // V 막대 축
        float perp  = rx * co - rz * s;    // V 막대 수선

        bool hitV = (fabsf(along) <= m_fBarHalfLength) && (fabsf(perp) <= m_fBarHalfWidth);
        // H 막대는 V 막대에 수직 → 축/수선이 반대
        bool hitH = (fabsf(perp)  <= m_fBarHalfLength) && (fabsf(along) <= m_fBarHalfWidth);

        if (hitV || hitH)
            pPlayer->TakeDamage(m_fDamagePerCross);
    }
}

void SequentialCrossAttackBehavior::CleanupCrossMeshes(Cross& c)
{
    if (!m_pScene) return;
    if (c.pBorderV) { m_pScene->MarkForDeletion(c.pBorderV); c.pBorderV = nullptr; }
    if (c.pBorderH) { m_pScene->MarkForDeletion(c.pBorderH); c.pBorderH = nullptr; }
    if (c.pFillV)   { m_pScene->MarkForDeletion(c.pFillV);   c.pFillV   = nullptr; }
    if (c.pFillH)   { m_pScene->MarkForDeletion(c.pFillH);   c.pFillH   = nullptr; }
}

void SequentialCrossAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;
    m_fTimer += dt;
    // Explosions/Recovery phase 에서도 windup-기준 elapsed 는 계속 증가 (폭발 타이밍 판정용)
    m_fWindupElapsed += dt;

    // 공격 애니 1회 재생 후 idle 전환
    if (!m_bAnimReturnedToIdle && m_fWindupElapsed > 0.1f)
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

    if (m_ePhase != Phase::Recovery)
    {
        // fill 갱신 (아직 안 터진 것만)
        for (auto& c : m_vCrosses)
            if (!c.bExploded) UpdateCrossFill(c, m_fWindupElapsed);

        // 폭발 타이밍 도달한 십자 폭발
        bool anyExplodedThisFrame = false;
        for (auto& c : m_vCrosses)
        {
            if (!c.bExploded && m_fWindupElapsed >= c.explosionTime)
            {
                ExplodeCross(pEnemy, c);
                anyExplodedThisFrame = true;
            }
        }
        if (anyExplodedThisFrame && m_ePhase == Phase::Windup)
        {
            m_ePhase = Phase::Explosions;
            m_fTimer = 0.0f;
        }

        UpdateBurstRocks(dt);

        // 전부 폭발했고 마지막 flash 지속 끝났으면 recovery
        bool allExploded = true;
        float lastExplosion = 0.0f;
        for (const auto& c : m_vCrosses)
        {
            if (!c.bExploded) { allExploded = false; break; }
            if (c.explosionTime > lastExplosion) lastExplosion = c.explosionTime;
        }
        if (allExploded && m_fWindupElapsed >= lastExplosion + m_fExplosionFlash)
        {
            m_ePhase = Phase::Recovery;
            m_fTimer = 0.0f;
        }
    }
    else
    {
        UpdateBurstRocks(dt);
        if (m_fTimer >= m_fRecoveryTime)
        {
            CleanupAll();
            m_bFinished = true;
        }
    }
}

void SequentialCrossAttackBehavior::CleanupAll()
{
    if (!m_pScene) return;
    for (auto& c : m_vCrosses)
    {
        CleanupCrossMeshes(c);
        for (GameObject* pR : c.vBurstRocks)
            if (pR) m_pScene->MarkForDeletion(pR);
        c.vBurstRocks.clear();
    }
    m_vCrosses.clear();
}

bool SequentialCrossAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void SequentialCrossAttackBehavior::Reset()
{
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
    m_fWindupElapsed = 0.0f;
    m_bFinished = false;
    m_bAnimReturnedToIdle = false;
    CleanupAll();
}
