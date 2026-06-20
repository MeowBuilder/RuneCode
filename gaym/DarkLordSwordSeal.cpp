#include "stdafx.h"
#include "DarkLordSwordSeal.h"
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
#include "VFXManager.h"
#include "SlashCue.h"
#include "EnemySpawner.h"
#include <functional>

using namespace DirectX;

namespace
{
    XMFLOAT3 SealElementColor(ElementType e)
    {
        switch (e)
        {
        case ElementType::Fire:  return { 1.00f, 0.30f, 0.10f };
        case ElementType::Water: return { 0.20f, 0.60f, 1.00f };
        case ElementType::Wind:  return { 0.55f, 1.00f, 0.45f };
        case ElementType::Earth: return { 0.85f, 0.65f, 0.20f };
        default:                 return { 1.00f, 0.30f, 0.10f };
        }
    }

    const char* SealHeavyName(ElementType e)
    {
        switch (e)
        {
        case ElementType::Fire:  return "Boss_CrescentSigil_Fire_Heavy";
        case ElementType::Water: return "Boss_CrescentSigil_Water_Heavy";
        case ElementType::Wind:  return "Boss_CrescentSigil_Wind_Heavy";
        case ElementType::Earth: return "Boss_CrescentSigil_Earth_Heavy";
        default:                 return "Boss_CrescentSigil_Fire_Heavy";
        }
    }
}

DarkLordSwordSeal::DarkLordSwordSeal(ElementType element,
                                     float fDamage,
                                     float fDuration,
                                     float fOrbitRadius,
                                     float fOrbitSpeedDegPerSec,
                                     float fSwordHitRadius,
                                     float fSwordVisualScale,
                                     int   nSwordCount)
    : m_eElement(element)
    , m_fDamage(fDamage)
    , m_fDuration(fDuration)
    , m_fOrbitRadius(fOrbitRadius)
    , m_fOrbitSpeedDegPerSec(fOrbitSpeedDegPerSec)
    , m_fSwordHitRadius(fSwordHitRadius)
    , m_fSwordVisualScale(fSwordVisualScale)
    , m_nSwordCount(nSwordCount < 1 ? 1 : nSwordCount)
{
}

void DarkLordSwordSeal::Execute(EnemyComponent* pEnemy)
{
    Reset();
    if (!pEnemy) return;
    m_pRoom = pEnemy->GetRoom();
    if (!m_pRoom) return;
    m_pScene = m_pRoom->GetScene();
    if (!m_pScene) return;

    // [Day 7+] 무적 제거 — 견제 패턴 톤. 보스도 같이 hit 가능.
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
}

void DarkLordSwordSeal::SpawnSwords(EnemyComponent* pEnemy)
{
    if (!m_pScene || !m_pRoom || !pEnemy) return;
    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;

    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
    Shader* pShader = m_pScene->GetDefaultShader();
    if (!pDevice || !pCmd || !pShader) return;

    XMFLOAT3 bossPos = pOwner->GetTransform()->GetPosition();
    XMFLOAT3 col     = SealElementColor(m_eElement);

    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(m_pRoom);

    m_vSwords.clear();
    m_vSwords.reserve(m_nSwordCount);

    for (int i = 0; i < m_nSwordCount; ++i)
    {
        float angDeg = (360.0f / m_nSwordCount) * i;
        SealSword sw;
        sw.baseAngleDeg = angDeg;

        GameObject* pSword = MeshLoader::LoadGeometryFromFile(
            m_pScene, pDevice, pCmd, nullptr,
            "Assets/Enemies/DeathKnight/SM_DarkKnight2_Sword.bin");
        if (!pSword)
        {
            m_vSwords.push_back(sw);
            continue;
        }

        // 위치 / 자세 — 보스 머리 위 (스케일 14 → Y+39 정도).
        float yawRad = XMConvertToRadians(angDeg);
        XMFLOAT3 pos = {
            bossPos.x + sinf(yawRad) * m_fOrbitRadius,
            bossPos.y + 39.0f,
            bossPos.z + cosf(yawRad) * m_fOrbitRadius
        };
        auto* pT = pSword->GetTransform();
        pT->SetPosition(pos);
        pT->SetScale(m_fSwordVisualScale, m_fSwordVisualScale, m_fSwordVisualScale);
        // 검 끝이 위를 향하도록 — 본 default 가 grip→pommel +Y 라면 그대로. 디버그 후 조정.
        pT->SetRotation(0.0f, angDeg, 0.0f);

        // RenderComponent + 텍스처 — EnemySpawner 의 정통 경로 사용.
        //   AddRenderComponentsToHierarchy: hierarchy 전체 RenderComponent 등록.
        //   LoadTextureToHierarchy: 동일 hierarchy 에 텍스처 + SRV 할당 + material 갈아끼움.
        //     ⇒ 호출 후에 emissive 덮어쓰기 필요 (그 안에서 emissive 0 으로 set 함).
        if (EnemySpawner* pSpawner = m_pScene->GetEnemySpawner())
        {
            pSpawner->AddRenderComponentsToHierarchy(pSword);
            pSpawner->LoadTextureToHierarchy(
                pSword,
                "Assets/Enemies/DeathKnight/Textures/T_Skin1_DeathKnight_Sword_Albedo.png",
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                nullptr);

            // emissive 적용 없음 — T_Skin1_DeathKnight_Sword_Albedo 텍스처 본연 톤 그대로.
            // (LoadTextureToHierarchy 안에서 emissive=0 으로 set 됨 → 그 상태 유지.)
        }

        sw.pObj = pSword;

        // 검기 본체 VFX 부착 — Track 으로 매 프레임 위치 추적.
        if (auto* pVFX = m_pScene->GetVFXManager())
        {
            XMFLOAT3 dirOut = { sinf(yawRad), 0.0f, cosf(yawRad) };
            sw.vfxSlot = pVFX->Spawn(SealHeavyName(m_eElement), pos, dirOut, 0u, false);
        }

        // ★ ownership 을 보스 (EnemyComponent) 로 넘김 — attack behavior 종료 후에도 검 유지.
        EnemyComponent::OrbitingSwordEntry entry;
        entry.pObj         = sw.pObj;
        entry.vfxSlot      = sw.vfxSlot;
        entry.baseAngleDeg = sw.baseAngleDeg;
        pEnemy->AddOrbitingSword(entry);
    }

    // 보스에 orbit 파라미터 전달 — lifetime 동안 EnemyComponent 가 회전/충돌 처리.
    float clientDamage = m_bNetworkVisualOnly ? 0.0f : m_fDamage;

    pEnemy->SetOrbitingSwordParams(
        m_fOrbitRadius, m_fOrbitSpeedDegPerSec, 39.0f,
        clientDamage, m_fSwordHitRadius, m_fDuration,
        m_eElement);

    // 우리는 검 관리 책임 없음 — 검 ownership 은 보스가 가짐. m_vSwords 클리어.
    m_vSwords.clear();

    m_pScene->SetCurrentRoom(pPrevRoom);

    // Spawn 임팩트 — 봉인 시작 모먼트 강조: 카메라 셰이크 + 임팩트 VFX 보스 발치.
    if (auto* pVFX = m_pScene->GetVFXManager())
    {
        const char* impactName = SlashCue::ImpactEffectName(m_eElement);
        XMFLOAT3 ringPos = { bossPos.x, bossPos.y + 0.2f, bossPos.z };
        XMFLOAT3 dirUp = { 0, 1, 0 };
        pVFX->Spawn(impactName, ringPos, dirUp, 0u, false);
    }
    if (CCamera* pCam = m_pScene->GetCamera())
        pCam->StartShake(2.6f, 0.45f);
}

void DarkLordSwordSeal::UpdateOrbit(float dt, EnemyComponent* pEnemy)
{
    if (!pEnemy) return;
    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;
    XMFLOAT3 bossPos = pOwner->GetTransform()->GetPosition();

    m_fAngleDeg += m_fOrbitSpeedDegPerSec * dt;
    if (m_fAngleDeg > 360.0f) m_fAngleDeg -= 360.0f;

    VFXManager* pVFX = m_pScene ? m_pScene->GetVFXManager() : nullptr;

    for (auto& sw : m_vSwords)
    {
        if (!sw.pObj) continue;
        float angDeg = sw.baseAngleDeg + m_fAngleDeg;
        float yawRad = XMConvertToRadians(angDeg);
        XMFLOAT3 pos = {
            bossPos.x + sinf(yawRad) * m_fOrbitRadius,
            bossPos.y + 39.0f,
            bossPos.z + cosf(yawRad) * m_fOrbitRadius
        };
        auto* pT = sw.pObj->GetTransform();
        pT->SetPosition(pos);
        // 검을 회전 진행 방향(접선)으로 — yaw 90도 미리.
        pT->SetRotation(0.0f, angDeg + 90.0f, 0.0f);

        // VFX 추적
        if (pVFX && sw.vfxSlot >= 0)
        {
            XMFLOAT3 dirOut = { sinf(yawRad), 0.0f, cosf(yawRad) };
            pVFX->Track(sw.vfxSlot, pos, dirOut);
        }
    }
}

void DarkLordSwordSeal::DealCollisionDamage(EnemyComponent* pEnemy)
{
    // 네트워크 모드에서는 데미지는 서버가 처리한다.
    if (m_bNetworkVisualOnly)
        return;

    if (!m_pScene) return;
    if (m_fDmgCooldown > 0.0f) return;   // 데미지 간격 (피해 누적 폭주 방지)

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
        for (auto& sw : m_vSwords)
        {
            if (!sw.pObj) continue;
            XMFLOAT3 sp = sw.pObj->GetTransform()->GetPosition();
            float dx = pp.x - sp.x;
            float dz = pp.z - sp.z;
            if (sqrtf(dx*dx + dz*dz) <= m_fSwordHitRadius)
            { bHit = true; break; }
        }
        if (bHit)
        {
            pPC->TakeDamage(m_fDamage);
            m_fDmgCooldown = kDmgInterval;
            break;   // 같은 프레임 동일 플레이어 다중 hit 방지
        }
    }
}

void DarkLordSwordSeal::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished || !pEnemy) return;
    m_fTimer += dt;
    if (m_fDmgCooldown > 0.0f) m_fDmgCooldown -= dt;

    switch (m_ePhase)
    {
    case Phase::Windup:
        if (m_fTimer >= m_fSpawnDelay)
        {
            SpawnSwords(pEnemy);
            // ★ spawn 직후 즉시 종료 — 검은 EnemyComponent 가 자체 lifetime 으로 관리.
            //   보스는 다음 frame 부터 다른 짤패턴 정상 사용 가능 (검은 계속 회전).
            m_bFinished = true;
        }
        break;

    case Phase::Active:   // 더 이상 사용 안 함 (spawn 후 즉시 finished)
    case Phase::End:
        m_bFinished = true;
        break;
    }
}

void DarkLordSwordSeal::Cleanup()
{
    // 검 ownership 은 보스(EnemyComponent::m_vOrbitingSwords) 가 가짐. 여기선 자체 vec 만 clear.
    m_vSwords.clear();
}

void DarkLordSwordSeal::Reset()
{
    Cleanup();
    m_ePhase = Phase::Windup;
    m_fTimer = 0.0f;
    m_fAngleDeg = 0.0f;
    m_fDmgCooldown = 0.0f;
    m_bFinished = false;
    // m_bInvincibleSet 해제는 owner 의 SetInvincible(false) 와 결합되어야 안전 —
    // Execute 가 다시 호출되어 이 멤버 false 일 때는 일단 그대로 둠.
}
