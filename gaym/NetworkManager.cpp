#include "stdafx.h"
#include "NetworkManager.h"
#include "Scene.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "RenderComponent.h"
#include "Mesh.h"
#include "Shader.h"
#include "MeshLoader.h"
#include "AnimationComponent.h"
#include "FluidSkillVFXManager.h"
#include "MegaBreathAttackBehavior.h"
#include "EffectRegistry.h"
#include "SkillTypes.h"
#include "SkillComponent.h"
#include "ISkillBehavior.h"
#include "SkillData.h"
#include "ProjectileManager.h"
#include "PlayerComponent.h"
#include "Camera.h"
#include "DamageNumberManager.h"
#include "DecalManager.h"
#include "Room.h"
#include "EnemySpawner.h"
#include "EnemyComponent.h"
#include "IAttackBehavior.h"
#include "JumpSlamAttackBehavior.h"
#include "RockBarrageAttackBehavior.h"
#include "RockFallAttackBehavior.h"
#include "GroundRuptureAttackBehavior.h"
#include "SequentialCrossAttackBehavior.h"
#include "TornadoFieldAttackBehavior.h"
#include "GaleSlashAttackBehavior.h"
#include "ShockwaveRingAttackBehavior.h"
#include "SpinDashAttackBehavior.h"
#include "RushAoEAttackBehavior.h"
#include "RushFrontAttackBehavior.h"
#include "MeleeAttackBehavior.h"
#include "RangedAttackBehavior.h"
#include "QuickJabAttackBehavior.h"
#include "ChargedShotAttackBehavior.h"
#include "GrenadeThrowAttackBehavior.h"
#include "SuicideExplodeAttackBehavior.h"
#include "FixatedChargeAttackBehavior.h"
#include "Dx12App.h"
#include "MapLoader.h"
#include "CharacterData.h"
#include "ColliderComponent.h"
#include "CollisionLayer.h"
#include "ComboAttackBehavior.h"
#include "DarkLordSigilSlash.h"
#include "DarkLordSigilField.h"
#include "DarkLordSwordRain.h"
#include "DarkLordSwordSeal.h"
#include "SlashVFXDesc.h"
#include <cmath>
#include <random>
#include <cstring>
#include <algorithm>

namespace
{
    constexpr float kNetSpawnPortalDelay = 0.7f;
    constexpr float kNetSpawnPortalY = 6.0f;
    constexpr float kNetSpawnFallTime = 0.4f;

    // 플레이어 포탈 Intro Fly 
    constexpr float kPlayerPortalIntroStartHeight = 22.0f;
    constexpr float kPlayerPortalIntroDuration = 3.0f;
    constexpr float kPlayerPortalIntroStandRadius = 5.0f;
    constexpr float kPlayerPortalIntroGravity = 50.0f;
    constexpr float kPlayerPortalIntroLandingHold = 0.50f;

    static XMFLOAT4 GetNetworkPlayerColor(ElementType element)
    {
        switch (element)
        {
        case ElementType::Fire:  return XMFLOAT4(1.00f, 0.55f, 0.30f, 1.0f);
        case ElementType::Water: return XMFLOAT4(0.35f, 0.85f, 0.95f, 1.0f);
        case ElementType::Wind:  return XMFLOAT4(0.65f, 0.95f, 0.55f, 1.0f);
        case ElementType::Earth: return XMFLOAT4(0.95f, 0.75f, 0.40f, 1.0f);
        default:                 return XMFLOAT4(0.85f, 0.90f, 1.00f, 1.0f);
        }
    }

    static void ApplyNetworkPlayerMaterial(GameObject* go, const XMFLOAT4& playerColor)
    {
        if (!go)
            return;

        MATERIAL mat;

        // 로컬 플레이어 Scene::Init()과 같은 머티리얼 수치를 원격 플레이어에도 적용한다.
        mat.m_cAmbient = XMFLOAT4(playerColor.x * 0.30f, playerColor.y * 0.30f, playerColor.z * 0.30f, 1.0f);
        mat.m_cDiffuse = playerColor;
        mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 32.0f);
        mat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);

        go->SetMaterial(mat);

        ApplyNetworkPlayerMaterial(go->m_pChild, playerColor);
        ApplyNetworkPlayerMaterial(go->m_pSibling, playerColor);
    }

    static void SetRenderTreeVisibleNet(GameObject* pGO, bool visible)
    {
        if (!pGO) return;

        if (auto* pRC = pGO->GetComponent<RenderComponent>())
            pRC->SetVisible(visible);

        SetRenderTreeVisibleNet(pGO->m_pChild, visible);
        SetRenderTreeVisibleNet(pGO->m_pSibling, visible);
    }

    static float NormalizeYaw(float yaw)
    {
        while (yaw < 0.0f) yaw += 360.0f;
        while (yaw >= 360.0f) yaw -= 360.0f;
        return yaw;
    }

    // Red Dragon 모델 forward가 코드 기준 forward와 180도 반대로 보여서
    // "보여주는 회전"만 180도 보정한다.
    // 서버 판정/브레스 방향 계산에는 이 값을 직접 쓰지 말 것.
    static float DragonVisualYaw(float logicalYaw)
    {
        return NormalizeYaw(logicalYaw + 180.0f);
    }

    static float YawToTargetXZ(const DirectX::XMFLOAT3& from, const DirectX::XMFLOAT3& to)
    {
        float dx = to.x - from.x;
        float dz = to.z - from.z;

        if (fabsf(dx) + fabsf(dz) <= 0.001f)
            return 0.0f;

        return atan2f(dx, dz) * (180.0f / 3.14159265f);
    }

    // 드래곤 시각 회전 세팅용.
    // 앞으로 Dragon 회전은 pT->SetRotation 직접 쓰지 말고 이걸 쓰는 게 안전함.
    static void SetDragonVisualYaw(TransformComponent* pT, float logicalYaw)
    {
        if (!pT) return;

        pT->SetRotation(
            0.0f,
            DragonVisualYaw(logicalYaw),
            0.0f
        );
    }

    // 드래곤이 특정 지점을 바라보게 하는 함수.
    // MoveToWall / Windup / ReturnFly에서 사용.
    static void SetDragonFaceTarget(
        TransformComponent* pT,
        const DirectX::XMFLOAT3& from,
        const DirectX::XMFLOAT3& to
    )
    {
        if (!pT) return;

        float dx = to.x - from.x;
        float dz = to.z - from.z;

        if (fabsf(dx) + fabsf(dz) <= 0.001f)
            return;

        float logicalYaw = YawToTargetXZ(from, to);
        SetDragonVisualYaw(pT, logicalYaw);
    }

    // effectOption packing (서버 EncodeDarkLordEffectOption 와 동기):
    //   ones        : element (1~4)
    //   tens/hundred: style   (0~99)
    //   thousands   : phase   (0~4)  [선택, 기본 0]
    static ElementType DecodeDarkLordElement(uint32 effectOption)
    {
        uint32 elemCode = effectOption % 10;

        switch (elemCode)
        {
        case 1: return ElementType::Fire;
        case 2: return ElementType::Water;
        case 3: return ElementType::Wind;
        case 4: return ElementType::Earth;
        default: return ElementType::Fire;
        }
    }

    static uint32 DecodeDarkLordStyle(uint32 effectOption)
    {
        // phase 비트 (천 단위) 마스킹.
        return (effectOption / 10) % 100;
    }

    static uint32 DecodeDarkLordPhase(uint32 effectOption)
    {
        return effectOption / 1000;
    }

    enum : uint32
    {
        DL_SLASH_SIDE_STANDARD = 0,
        DL_SLASH_QUICK_JAB = 1,
        DL_SLASH_LONG_PROJECTILE = 2,
        DL_SLASH_HEAVY_MASSIVE = 3,
        DL_SLASH_SPIN_MASSIVE = 4,
        DL_SLASH_WHIP_LIGHT = 5,
        DL_SLASH_BARRAGE = 6,
        DL_SLASH_TWIN_CLEAVE = 7,
        DL_SLASH_CROSS_SIGIL = 8,
        DL_SLASH_FINAL_JUDGMENT = 9,
        DL_SLASH_ULT_MASSIVE = 10
    };

    static const char* NetDarkLordSigilName(ElementType e, bool heavy)
    {
        switch (e)
        {
        case ElementType::Fire:
            return heavy ? "Boss_CrescentSigil_Fire_Heavy" : "Boss_CrescentSigil_Fire";
        case ElementType::Water:
            return heavy ? "Boss_CrescentSigil_Water_Heavy" : "Boss_CrescentSigil_Water";
        case ElementType::Wind:
            return heavy ? "Boss_CrescentSigil_Wind_Heavy" : "Boss_CrescentSigil_Wind";
        case ElementType::Earth:
            return heavy ? "Boss_CrescentSigil_Earth_Heavy" : "Boss_CrescentSigil_Earth";
        default:
            return "Boss_CrescentSigil_Fire";
        }
    }

    static ComboAttackBehavior::ComboHit MakeNetDarkLordBaseHit()
    {
        ComboAttackBehavior::ComboHit h;
        h.fDamage = 0.0f; // 실제 데미지는 서버가 처리
        h.fWindupTime = 0.55f;
        h.fHitTime = 0.22f;
        h.fRecoveryTime = 0.35f;
        h.fHitRange = 15.0f;
        h.fConeAngle = 115.0f;
        h.fVFXForwardOffset = 4.0f;
        h.fVFXYOffset = 11.0f;
        h.fVFXScale = 6.5f;
        h.strVFXImpact = "";
        // 네트워크 모드 — 보스 방향은 ProcessMonsterAttack 에서 서버 yaw 로 스냅됨.
        //   Execute/Recovery 의 FaceTarget 이 로컬 m_pTarget(=로컬 플레이어) 방향으로
        //   덮어쓰면 서버 의도와 다른 방향으로 검기/투사체가 발사된다.
        h.bTrackTarget = false;
        return h;
    }

    static ComboAttackBehavior::ComboHit MakeNetQuickJab(ElementType e)
    {
        auto h = MakeNetDarkLordBaseHit();
        h.fWindupTime = 0.40f;
        h.fHitTime = 0.18f;
        h.fRecoveryTime = 0.28f;
        h.fHitRange = 14.0f;
        h.fConeAngle = 95.0f;
        h.strAnimation = "attack1";
        h.strVFXOnHit = NetDarkLordSigilName(e, false);
        h.eShape = ComboAttackBehavior::SwordEnergyShape::Slim;
        return h;
    }

    static ComboAttackBehavior::ComboHit MakeNetSideCleave(ElementType e)
    {
        auto h = MakeNetDarkLordBaseHit();
        h.fWindupTime = 0.50f;
        h.fHitTime = 0.20f;
        h.fHitRange = 15.0f;
        h.fConeAngle = 125.0f;
        h.strAnimation = "attack2";
        h.strVFXOnHit = NetDarkLordSigilName(e, false);
        h.fVFXScale = 7.5f;
        h.eShape = ComboAttackBehavior::SwordEnergyShape::Wide;
        return h;
    }

    static ComboAttackBehavior::ComboHit MakeNetLongReach(ElementType e)
    {
        auto h = MakeNetDarkLordBaseHit();
        h.fWindupTime = 0.45f;
        h.fHitTime = 0.20f;
        h.fHitRange = 17.0f;
        h.fConeAngle = 85.0f;
        h.strAnimation = "attack4";
        h.strVFXOnHit = NetDarkLordSigilName(e, false);
        h.fVFXForwardOffset = 4.5f;
        h.eShape = ComboAttackBehavior::SwordEnergyShape::Long;
        return h;
    }

    static ComboAttackBehavior::ComboHit MakeNetHeavySlam(ElementType e)
    {
        auto h = MakeNetDarkLordBaseHit();
        h.fWindupTime = 0.75f;
        h.fHitTime = 0.30f;
        h.fRecoveryTime = 0.50f;
        h.fHitRange = 17.0f;
        h.fConeAngle = 130.0f;
        h.strAnimation = "Attack6";
        h.strVFXOnHit = NetDarkLordSigilName(e, true);
        h.fVFXScale = 10.0f;
        h.eShape = ComboAttackBehavior::SwordEnergyShape::Wide;
        return h;
    }

    static ComboAttackBehavior::ComboHit MakeNetSpinCleave(ElementType e)
    {
        auto h = MakeNetDarkLordBaseHit();
        h.fWindupTime = 0.55f;
        h.fHitTime = 0.28f;
        h.fHitRange = 16.5f;
        h.fConeAngle = 250.0f;
        h.strAnimation = "attack9";
        h.strVFXOnHit = NetDarkLordSigilName(e, true);
        h.fVFXScale = 9.0f;
        h.eShape = ComboAttackBehavior::SwordEnergyShape::Double;
        return h;
    }

    static ComboAttackBehavior::ComboHit MakeNetWhipTrail(ElementType e)
    {
        auto h = MakeNetDarkLordBaseHit();
        h.fWindupTime = 0.40f;
        h.fHitTime = 0.30f;
        h.fRecoveryTime = 0.55f;
        h.fHitRange = 17.0f;
        h.fConeAngle = 100.0f;
        h.strAnimation = "attack2";
        h.strVFXOnHit = NetDarkLordSigilName(e, false);
        h.fVFXScale = 11.0f;
        h.eShape = ComboAttackBehavior::SwordEnergyShape::Long;
        return h;
    }

    static std::unique_ptr<IAttackBehavior> MakeNetworkDarkLordSigilSlash(uint32 effectOption)
    {
        ElementType elem = DecodeDarkLordElement(effectOption);
        uint32 style = DecodeDarkLordStyle(effectOption);

        ComboAttackBehavior::ComboHit hit;
        SlashPresentation presentation = SlashPresentation::Standard;
        SlashPowerLevel power = SlashPowerLevel::Signature;

        switch (style)
        {
        case DL_SLASH_QUICK_JAB:
            hit = MakeNetQuickJab(elem);
            presentation = SlashPresentation::Standard;
            power = SlashPowerLevel::Medium;
            break;

        case DL_SLASH_LONG_PROJECTILE:
            hit = MakeNetLongReach(elem);
            presentation = SlashPresentation::Projectile;
            power = SlashPowerLevel::Medium;
            break;

        case DL_SLASH_HEAVY_MASSIVE:
            hit = MakeNetHeavySlam(elem);
            presentation = SlashPresentation::Massive;
            power = SlashPowerLevel::Signature;
            break;

        case DL_SLASH_SPIN_MASSIVE:
            hit = MakeNetSpinCleave(elem);
            presentation = SlashPresentation::Massive;
            power = SlashPowerLevel::Signature;
            break;

        case DL_SLASH_WHIP_LIGHT:
            hit = MakeNetWhipTrail(elem);
            presentation = SlashPresentation::Light;
            power = SlashPowerLevel::Small;
            break;

        case DL_SLASH_BARRAGE:
            hit = MakeNetLongReach(elem);
            presentation = SlashPresentation::Projectile;
            power = SlashPowerLevel::Signature;
            break;

        case DL_SLASH_TWIN_CLEAVE:
            hit = MakeNetSideCleave(elem);
            presentation = SlashPresentation::TwinCleave;
            power = SlashPowerLevel::Signature;
            break;

        case DL_SLASH_CROSS_SIGIL:
            hit = MakeNetHeavySlam(elem);
            presentation = SlashPresentation::CrossSigil;
            power = SlashPowerLevel::Ultimate;
            break;

        case DL_SLASH_FINAL_JUDGMENT:
            hit = MakeNetHeavySlam(elem);
            presentation = SlashPresentation::FinalJudgment;
            power = SlashPowerLevel::Ultimate;
            break;

        case DL_SLASH_ULT_MASSIVE:
            hit = MakeNetHeavySlam(elem);
            presentation = SlashPresentation::Massive;
            power = SlashPowerLevel::Ultimate;
            break;

        case DL_SLASH_SIDE_STANDARD:
        default:
            hit = MakeNetSideCleave(elem);
            presentation = SlashPresentation::Standard;
            power = SlashPowerLevel::Medium;
            break;
        }

        SlashVFXDesc desc = SlashVFXDesc::Preset(elem, power);
        desc.ApplyPresentation(presentation);

        if (style == DL_SLASH_BARRAGE)
        {
            desc.projectileBurstCount = 6;
            desc.projectileBurstInterval = 0.13f;
            desc.projectileBurstSpreadDeg = 14.0f;
        }

        if (style == DL_SLASH_TWIN_CLEAVE)
        {
            // 오프라인: P2 Wind = 35°, P4 Final = 30°. 서버가 phase 안 보내면 30° fallback.
            uint32 phase = DecodeDarkLordPhase(effectOption);
            desc.twinSeparationDeg = (phase == 2) ? 35.0f : 30.0f;
        }

        auto pSlash = std::make_unique<DarkLordSigilSlash>(
            desc,
            std::vector<ComboAttackBehavior::ComboHit>{ hit }
        );
        // 데미지 권위는 서버. 클라 측 cone/projectile/TwinCleave 데미지 모두 0.
        pSlash->SetNetworkVisualOnly(true);
        return pSlash;
    }

    static std::unique_ptr<IAttackBehavior> MakeNetworkDarkLordSigilField(
        uint32 effectOption,
        const std::vector<DirectX::XMFLOAT3>& effectPositions)
    {
        ElementType elem = DecodeDarkLordElement(effectOption);

        const bool isFinalStyle = (effectPositions.size() >= 4);

        float radius = isFinalStyle ? 9.5f : 9.0f;
        float delay = isFinalStyle ? 1.20f : 1.30f;
        int count = effectPositions.empty()
            ? (isFinalStyle ? 4 : 3)
            : static_cast<int>(effectPositions.size());
        float spread = isFinalStyle ? 23.0f : 18.0f;

        auto pField = std::make_unique<DarkLordSigilField>(
            elem,
            0.0f,      // 실제 데미지는 서버가 처리
            radius,
            delay,
            count,
            spread,
            0.8f
        );

        pField->SetNetworkVisualOnly(true);
        pField->SetNetworkEffectPositions(effectPositions);

        return pField;
    }

    static std::unique_ptr<IAttackBehavior> MakeNetworkDarkLordSwordRain(
        uint32 effectOption,
        const std::vector<DirectX::XMFLOAT3>& effectPositions)
    {
        ElementType elem = DecodeDarkLordElement(effectOption);

        const bool isFinalStyle = (effectPositions.size() >= 10);

        int swordCount = effectPositions.empty()
            ? (isFinalStyle ? 10 : 7)
            : static_cast<int>(effectPositions.size());

        float damage = 0.0f; // 실제 데미지는 서버가 처리
        float radius = isFinalStyle ? 9.0f : 8.5f;
        float minRadius = isFinalStyle ? 8.0f : 9.0f;
        float maxRadius = isFinalStyle ? 48.0f : 42.0f;
        float windup = isFinalStyle ? 1.5f : 1.7f;
        float recovery = isFinalStyle ? 1.4f : 1.5f;

        auto pRain = std::make_unique<DarkLordSwordRain>(
            elem,
            swordCount,
            damage,
            radius,
            minRadius,
            maxRadius,
            windup,
            recovery
        );

        pRain->SetNetworkVisualOnly(true);
        pRain->SetNetworkEffectPositions(effectPositions);

        return pRain;
    }

    static std::unique_ptr<IAttackBehavior> MakeNetworkDarkLordSwordSeal(uint32 effectOption)
    {
        ElementType elem = DecodeDarkLordElement(effectOption);
        uint32 style = DecodeDarkLordStyle(effectOption);

        const bool isFinalStyle = (style >= 1);

        // 클라 로컬 DarkLordSwordSeal 수치 그대로.
        // P3:
        //   Fire, damage=45, duration=7, orbitR=26, orbitSpeed=65, hitR=4, scale=21, count=4
        // Final:
        //   random element, damage=50, duration=6, orbitR=26, orbitSpeed=80, hitR=4, scale=21, count=4
        float damage = isFinalStyle ? 50.0f : 45.0f;
        float duration = isFinalStyle ? 6.0f : 7.0f;
        float orbitRadius = 26.0f;
        float orbitSpeed = isFinalStyle ? 80.0f : 65.0f;
        float hitRadius = 4.0f;
        float swordVisualScale = 21.0f;
        int swordCount = 4;

        auto pSeal = std::make_unique<DarkLordSwordSeal>(
            elem,
            damage,
            duration,
            orbitRadius,
            orbitSpeed,
            hitRadius,
            swordVisualScale,
            swordCount
        );

        pSeal->SetNetworkVisualOnly(true);
        return pSeal;
    }
}

// ServerPacketHandler.cpp에 정의된 파일 로그 함수 — network_log.txt에 append
extern void WriteNetworkLog(const std::string& msg);

// 네트워크 전용 RushFront 연출 Behavior
// 오프라인 RushFrontAttackBehavior의 인디케이터 타입을 그대로 따른다.
// 서버 권위 일반 몬스터는 클라에서 직접 이동/데미지 처리하지 않고,
// 원본 AttackBehavior의 telegraph / animation 연출만 재생한다.
class NetworkRushFrontVisualBehavior : public RushFrontAttackBehavior
{
public:
    using RushFrontAttackBehavior::RushFrontAttackBehavior;

    virtual int GetIndicatorTypeOverride() const override
    {
        return RushFrontAttackBehavior::GetIndicatorTypeOverride();
    }
};

// 싱글톤 인스턴스
NetworkManager* NetworkManager::s_pInstance = nullptr;

// =============================================================================
// GameSession 구현
// =============================================================================

GameSession::~GameSession()
{
    OutputDebugString(L"[Network] GameSession destroyed\n");
}

void GameSession::OnConnected()
{
    OutputDebugString(L"[Network] Connected to server!\n");

    // NetworkManager에 세션 등록
    NetworkManager::GetInstance()->SetSession(
        std::static_pointer_cast<GameSession>(shared_from_this()));

    // C_LOGIN 패킷 전송
    Protocol::C_LOGIN loginPkt;
    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(loginPkt);
    Send(sendBuffer);

    OutputDebugString(L"[Network] C_LOGIN sent\n");
}

void GameSession::OnRecvPacket(BYTE* buffer, int32 len)
{
    // ServerPacketHandler를 통해 패킷 처리
    PacketSessionRef session = GetPacketSessionRef();
    ServerPacketHandler::HandlePacket(session, buffer, len);
}

void GameSession::OnDisconnected()
{
    OutputDebugString(L"[Network] Disconnected from server\n");
}

// =============================================================================
// NetworkManager 구현
// =============================================================================

NetworkManager* NetworkManager::GetInstance()
{
    if (s_pInstance == nullptr)
    {
        s_pInstance = new NetworkManager();
    }
    return s_pInstance;
}

NetworkManager::NetworkManager()
{
    OutputDebugString(L"[Network] NetworkManager created\n");
}

NetworkManager::~NetworkManager()
{
    Shutdown();
    OutputDebugString(L"[Network] NetworkManager destroyed\n");
}

bool NetworkManager::Initialize()
{
    // ServerPacketHandler 초기화
    ServerPacketHandler::Init();

    OutputDebugString(L"[Network] NetworkManager initialized\n");
    return true;
}

void NetworkManager::Shutdown()
{
    // 방 전환 전에 네트워크 일반 몬스터 연출 정리
    // 이제 entry는 EnemyComponent*를 직접 들고 있지 않고 monsterId만 저장한다.
    // 매번 m_mapServerMonsters에서 다시 찾아 dangling pointer 접근을 막는다.
    for (auto& entry : m_vNetworkNormalMonsterBehaviors)
    {
        auto monIt = m_mapServerMonsters.find(entry.monsterId);
        if (monIt == m_mapServerMonsters.end() || !monIt->second)
            continue;

        GameObject* pMonster = monIt->second;
        EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>();
        if (!pEnemy)
            continue;

        if (IAttackBehavior* pBehavior = pEnemy->GetAttackBehavior())
            pBehavior->Reset();

        pEnemy->HideNetworkAttackIndicator();
    }

    m_vNetworkNormalMonsterBehaviors.clear();

    for (auto& entry : m_vNetworkGolemBehaviors)
    {
        if (entry.behavior)
            entry.behavior->Reset();
    }
    m_vNetworkGolemBehaviors.clear();

    for (auto& entry : m_vNetworkDemonBehaviors)
    {
        if (entry.behavior)
            entry.behavior->Reset();
    }
    m_vNetworkDemonBehaviors.clear();

    m_mapServerMonsterSpawnEffects.clear(); // 몬스터 스폰 연출 상태 초기화
    m_mapServerMonsterCurrentAnimClip.clear(); // 몬스터 애니메이션 캐시 초기화

    if (!m_bConnected && !m_pService)
        return;

    m_bShutdownRequested = true;
    m_bConnected = false;

    // 워커 스레드가 종료될 시간을 줌
    // (Dispatch 타임아웃이 10ms이므로 충분히 대기)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 모든 스레드 조인
    GThreadManager->Join();

    if (m_pService)
    {
        m_pService->CloseService();
        m_pService = nullptr;
    }

    m_pSession = nullptr;

    // 원격 플레이어 맵 클리어 (GameObject는 Scene이 관리하므로 여기서 delete 하지 않음)
    m_mapRemotePlayers.clear();

    // 큐 정리
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_vCommandQueue.clear();
    }
    m_vPendingSpawns.clear();
    m_nLocalPlayerId.store(0);

    OutputDebugString(L"[Network] NetworkManager shutdown complete\n");
}

bool NetworkManager::Connect(const std::wstring& ip, uint16 port)
{
    if (m_bConnected)
    {
        OutputDebugString(L"[Network] Already connected!\n");
        return true;
    }

    try
    {
        // ClientService 생성
        m_pService = MakeShared<ClientService>(
            NetAddress(ip, port),
            MakeShared<IocpCore>(),
            MakeShared<GameSession>,  // 세션 팩토리
            1  // 최대 세션 수
        );

        if (!m_pService->Start())
        {
            OutputDebugString(L"[Network] Failed to start ClientService\n");
            return false;
        }

        // 워커 스레드 시작 (IOCP 이벤트 처리)
        GThreadManager->Launch([this]()
            {
                while (!m_bShutdownRequested)
                {
                    // 10ms 타임아웃으로 Dispatch
                    m_pService->GetIocpCore()->Dispatch(10);
                }
            });

        m_bConnected = true;
        OutputDebugString(L"[Network] Connection started to server\n");
        return true;
    }
    catch (const std::exception& e)
    {
        OutputDebugStringA("[Network] Exception during Connect: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        return false;
    }
}

void NetworkManager::Disconnect()
{
    if (!m_bConnected)
        return;

    if (m_pSession)
    {
        m_pSession->Disconnect(L"User requested disconnect");
    }

    m_bConnected = false;
    OutputDebugString(L"[Network] Disconnected\n");
}

void NetworkManager::SyncLocalPlayerPositionToServer(Scene* pScene)
{
    if (!pScene)
        return;

    if (!m_bConnected || !m_pSession)
        return;

    if (m_nLocalPlayerId.load() == 0)
        return;

    GameObject* pLocal = pScene->GetPlayer();
    if (!pLocal)
        return;

    TransformComponent* pLocalT = pLocal->GetTransform();
    if (!pLocalT)
        return;

    XMFLOAT3 pos = pLocalT->GetPosition();

    // 인트로 낙하 중이면 현재 y는 공중 높이일 수 있다.
    // 서버에는 시각용 공중 y가 아니라 착지 기준 y를 저장한다.
    float syncY = pos.y;
    if (syncY > 10.0f)
        syncY -= kPlayerPortalIntroStartHeight;

    // 서버의 player->x/y/z를 실제 시작 위치로 보정한다.
    SendMove(
        pos.x, syncY, pos.z,
        0.0f, 0.0f, 1.0f);

    WriteNetworkLog("[Network] Initial local position sync sent");
}

void NetworkManager::Update(Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, float deltaTime)
{
    if (!pScene || !pDevice || !pCommandList)
        return;

    // 네트워크 시간 누적 — 채널 throttle 등 비교용
    m_fNetworkAccumulatedTime += deltaTime;

    if (m_fMegaBreathInputLockTimer > 0.0f)
    {
        m_fMegaBreathInputLockTimer -= deltaTime;
        if (m_fMegaBreathInputLockTimer < 0.0f)
            m_fMegaBreathInputLockTimer = 0.0f;
    }

    if (m_fLocalMoveCorrectionBlockTimer > 0.0f)
    {
        m_fLocalMoveCorrectionBlockTimer -= deltaTime;
        if (m_fLocalMoveCorrectionBlockTimer < 0.0f)
            m_fLocalMoveCorrectionBlockTimer = 0.0f;
    }

    for (auto it = m_mapServerMonsterHitAnimCooldown.begin();
        it != m_mapServerMonsterHitAnimCooldown.end(); )
    {
        it->second -= deltaTime;

        if (it->second <= 0.0f)
            it = m_mapServerMonsterHitAnimCooldown.erase(it);
        else
            ++it;
    }

    // 이번 Update에서 방 전환이 처리됐는지 확인
    // 같은 프레임에 TorchInteract까지 보내지 않기 위한 방어
    bool roomTransitionProcessedThisFrame = false;

    // 큐에 쌓인 명령들을 메인 스레드에서 처리
    std::vector<NetworkCommandData> commands;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        commands.swap(m_vCommandQueue);
    }

    // 1차: SetLocalPlayerId 명령을 먼저 처리 (Spawn보다 먼저 ID가 설정되어야 함)
    bool localIdWasSet = false;
    for (const auto& cmd : commands)
    {
        if (cmd.type == NetworkCommand::SetLocalPlayerId)
        {
            m_nLocalPlayerId.store(cmd.playerId);
            localIdWasSet = true;

            // LocalPlayerId를 받은 직후부터 몇 프레임 동안 초기 위치를 서버에 반복 전송한다.
            m_nInitialLocalPositionSyncFrames = 60;

            wchar_t buf[128];
            swprintf_s(buf, L"[Network] Local player ID set to: %llu\n", cmd.playerId);
            OutputDebugString(buf);
        }
    }

    // LocalPlayerId가 방금 설정되었으면 pending spawn 처리
    if (localIdWasSet && !m_vPendingSpawns.empty())
    {
        OutputDebugString(L"[Network] Processing pending spawns after LocalPlayerId set\n");
        for (const auto& pending : m_vPendingSpawns)
        {
            ProcessSpawnPlayer(pScene, pDevice, pCommandList,
                pending.playerId, pending.name, pending.playerType,
                pending.x, pending.y, pending.z);
        }
        m_vPendingSpawns.clear();
    }

    // LocalPlayerId가 설정된 직후, 내 실제 시작 위치를 서버에 한 번 알려준다.
    if (localIdWasSet)
    {
        if (GameObject* pLocal = pScene->GetPlayer())
        {
            if (auto* pLocalT = pLocal->GetTransform())
            {
                XMFLOAT3 groundPos = pLocalT->GetPosition();

                // 현재 위치를 착지 기준점으로 사용한다.
                float groundY = groundPos.y;
                if (groundY > 10.0f)
                    groundY -= kPlayerPortalIntroStartHeight;

                groundPos.y = groundY;

                // 서버에는 착지 기준 좌표를 먼저 알려준다.
                SendMove(
                    groundPos.x,
                    groundPos.y,
                    groundPos.z,
                    0.0f,
                    0.0f,
                    1.0f
                );

                // 로컬 플레이어는 위에서 떨어지는 연출로 시작한다.
                pLocalT->SetPosition(
                    groundPos.x,
                    groundPos.y + kPlayerPortalIntroStartHeight,
                    groundPos.z
                );

                if (auto* pPC = pLocal->GetComponent<PlayerComponent>())
                {
                    pPC->StartIntroFly(
                        kPlayerPortalIntroDuration,
                        groundPos.y,
                        XMFLOAT3(groundPos.x, groundPos.y, groundPos.z),
                        kPlayerPortalIntroStandRadius
                    );
                }

                // 낙하 인트로 중에는 서버 S_MOVE 보정이 Transform을 덮어쓰지 않게 막는다.
                m_fLocalMoveCorrectionBlockTimer = kPlayerPortalIntroDuration + 0.35f;

                // 서버가 초기 위치를 안정적으로 잡도록 몇 프레임 반복 동기화
                m_nInitialLocalPositionSyncFrames = 60;

                WriteNetworkLog("[Network] Initial local portal intro started");
            }
        }
    }

    // 2차: 나머지 명령 처리
    for (const auto& cmd : commands)
    {
        switch (cmd.type)
        {
        case NetworkCommand::Spawn:
            // LocalPlayerId가 아직 설정되지 않았으면 pending 큐에 보관
            if (m_nLocalPlayerId.load() == 0)
            {
                wchar_t buf[128];
                swprintf_s(buf, L"[Network] Spawn deferred (LocalPlayerId not set): PlayerId=%llu\n", cmd.playerId);
                OutputDebugString(buf);
                m_vPendingSpawns.push_back(cmd);
            }
            else
            {
                ProcessSpawnPlayer(pScene, pDevice, pCommandList,
                    cmd.playerId, cmd.name, cmd.playerType, cmd.x, cmd.y, cmd.z);
            }
            break;

        case NetworkCommand::Despawn:
            ProcessDespawnPlayer(pScene, cmd.playerId);
            break;

        case NetworkCommand::Move:
            ProcessMovePlayer(pScene, cmd.playerId, cmd.x, cmd.y, cmd.z, cmd.dirX, cmd.dirY, cmd.dirZ);
            break;

        case NetworkCommand::Skill:
            ProcessSkill(pScene, cmd.playerId, cmd.skillType, cmd.x, cmd.y, cmd.z, cmd.dirX, cmd.dirY, cmd.dirZ,
                cmd.skillSlot, cmd.skillRadiusMult, cmd.skillDamageMult);
            break;

        case NetworkCommand::PlayerAction:
            ProcessPlayerAction(pScene, cmd.playerId, cmd.playerActionType, cmd.x, cmd.y, cmd.z, cmd.dirX, cmd.dirY, cmd.dirZ);
            break;

        case NetworkCommand::SetLocalPlayerId:
            // 이미 1차에서 처리됨
            break;

        case NetworkCommand::RoomTransition:
            ProcessRoomTransition(pScene, cmd.stageIndex, cmd.roomIndex, cmd.isBossRoom, cmd.mapId);
            roomTransitionProcessedThisFrame = true;
            break;

        case NetworkCommand::RoomStart:
            ProcessRoomStart(pScene, cmd.playerId);
            break;

        case NetworkCommand::MonsterSpawn:
            ProcessMonsterSpawn(pScene, pDevice, pCommandList, cmd.monsterId, cmd.monsterType, cmd.monsterAttackType, cmd.monsterVisualType, cmd.x, cmd.y, cmd.z, cmd.monsterYaw, cmd.monsterHp, cmd.monsterIsBoss);
            break;

        case NetworkCommand::MonsterMove:
            ProcessMonsterMove(cmd.monsterId, cmd.x, cmd.y, cmd.z, cmd.monsterYaw);
            break;

        case NetworkCommand::MonsterDespawn:
            ProcessMonsterDespawn(pScene, cmd.monsterId);
            break;

        case NetworkCommand::MonsterAttack:
            ProcessMonsterAttack(pScene, cmd.monsterId, cmd.attackType, cmd.windupSec, cmd.targetPlayerId, cmd.x, cmd.y, cmd.z, cmd.monsterYaw, cmd.effectPositions, cmd.effectOption);
            break;

        case NetworkCommand::PlayerDamage:
            ProcessPlayerDamage(pScene, cmd.playerId, cmd.damage, cmd.currentHp, cmd.isDead, cmd.attackerMonsterId);
            break;

        case NetworkCommand::MonsterDamage:
            ProcessMonsterDamage(pScene, cmd.monsterId, cmd.damage, cmd.currentHp, cmd.isDead, cmd.attackerPlayerId, cmd.skillType);
            break;

        case NetworkCommand::MonsterStagger:
            ProcessMonsterStagger(cmd.monsterId, cmd.duration);
            break;

        case NetworkCommand::MapTornadoEvent:
        {
            if (pScene)
            {
                // Grass Boss Room 맵 토네이도 이벤트
                // 서버가 정한 좌표를 Scene에 전달해서 모든 클라가 같은 위치에 경고 링 / 토네이도를 생성한다.
                pScene->StartNetworkMapTornadoEvent(
                    DirectX::XMFLOAT3(
                        cmd.mapTornadoX,
                        cmd.mapTornadoY,
                        cmd.mapTornadoZ
                    ),
                    cmd.mapTornadoWarningSec,
                    cmd.mapTornadoActiveSec
                );
            }
            break;
        }

        case NetworkCommand::RoomCleared:
            ProcessRoomCleared(pScene, cmd.stageIndex, cmd.roomIndex);
            break;

        case NetworkCommand::RoomRewardSpawn:
            ProcessRoomRewardSpawn(pScene, cmd.stageIndex, cmd.roomIndex, DirectX::XMFLOAT3(cmd.rewardPortalX, cmd.rewardPortalY, cmd.rewardPortalZ), cmd.rewardHasSecondPortal, DirectX::XMFLOAT3(cmd.rewardSecondPortalX, cmd.rewardSecondPortalY, cmd.rewardSecondPortalZ), cmd.rewardRuneObjects);
            break;

        case NetworkCommand::RuneRewardPicked:
            ProcessRuneRewardPicked(pScene, cmd.playerId);
            break;

        case NetworkCommand::RuneEquip:
            ProcessRuneEquip(pScene, cmd.playerId, cmd.runeSkillSlot, cmd.runeSlotIndex, cmd.runeId, cmd.runeStackCount);
            break;

        case NetworkCommand::RuneHomingTarget:
            ProcessRuneHomingTarget(
                pScene, cmd.playerId, cmd.runeHomingSkillSlot, cmd.runeHomingSkillType, cmd.runeHomingTargetMonsterId,
                DirectX::XMFLOAT3(cmd.runeHomingTargetX, cmd.runeHomingTargetY, cmd.runeHomingTargetZ),
                DirectX::XMFLOAT3(cmd.runeHomingOriginX, cmd.runeHomingOriginY, cmd.runeHomingOriginZ)
            );
            break;

        case NetworkCommand::RuneTrigger:
            ProcessRuneTrigger(
                pScene,
                cmd.playerId,
                cmd.runeTriggerSkillSlot,
                cmd.runeTriggerSkillType,
                cmd.runeId,
                cmd.runeTriggerType,
                cmd.runeTriggerTargetMonsterId,
                cmd.runeTriggerTargetPlayerId,
                cmd.runeTriggerObjectId,
                DirectX::XMFLOAT3(cmd.x, cmd.y, cmd.z),
                cmd.runeTriggerValue1,
                cmd.runeTriggerValue2
            );
            break;

        case NetworkCommand::BossEvent:
            ProcessBossEvent(pScene, cmd.monsterId, cmd.bossEventType, cmd.phaseIndex);
            break;
        }
    }

    // 서버 몬스터 위치 보간
    InterpolateServerMonsters(deltaTime);
    // 서버 몬스터 스폰 포탈 / 낙하 연출
    UpdateServerMonsterSpawnEffects(deltaTime);
    // 서버 몬스터 idle / attack timer 처리
    CheckServerMonsterIdle(deltaTime);

    // 원격 플레이어 연출 액션 타이머 처리
    UpdateRemotePlayerActionLocks(deltaTime);

    // 원격 플레이어 포탈 Intro Fly 연출 처리
    UpdateRemotePlayerPortalIntroFlyEffects(deltaTime);

    // 원격 플레이어 이동 보간
    UpdateRemotePlayerInterpolation(deltaTime);

    // Kraken 페이즈 수면 Y 보정
    // 크라켄 때는 바다가 올라가면서 로컬 플레이어 Y가 수면 기준으로 올라간다.
    // 원격 플레이어는 움직일 때만 S_MOVE로 Y가 갱신되므로,
    // 가만히 있으면 클라 로컬 지면/idle 보정 때문에 다시 ground로 떨어져 보일 수 있다.
    // 따라서 Kraken이 살아있는 동안에는 원격 플레이어의 표시 Y를 로컬 플레이어 Y 이상으로 유지한다.
    {
        bool krakenActive = false;

        for (const auto& kv : m_mapServerMonsterClips)
        {
            if (kv.second.monsterType == 7) // Kraken
            {
                krakenActive = true;
                break;
            }
        }

        if (krakenActive && pScene && pScene->GetPlayer())
        {
            TransformComponent* pLocalT = pScene->GetPlayer()->GetTransform();

            if (pLocalT)
            {
                float waterPlayerY = pLocalT->GetPosition().y;

                for (auto& kv : m_mapRemotePlayers)
                {
                    uint64 remotePlayerId = kv.first;
                    GameObject* pRemote = kv.second;

                    if (!pRemote)
                        continue;

                    // 죽은 원격 플레이어는 죽음 연출 유지
                    if (m_setDeadRemotePlayers.find(remotePlayerId) != m_setDeadRemotePlayers.end())
                        continue;

                    // 포탈 인트로 중이면 IntroFly가 위치를 직접 제어하므로 건드리지 않음
                    if (m_mapRemotePlayerPortalIntroFlyEffects.find(remotePlayerId) != m_mapRemotePlayerPortalIntroFlyEffects.end())
                        continue;

                    TransformComponent* pRemoteT = pRemote->GetTransform();
                    if (!pRemoteT)
                        continue;

                    DirectX::XMFLOAT3 pos = pRemoteT->GetPosition();

                    // 아래로 떨어져 보일 때만 끌어올린다.
                    // 위에 있는 점프/연출까지 강제로 내리지는 않음.
                    if (pos.y < waterPlayerY - 0.05f)
                    {
                        pos.y = waterPlayerY;
                        pRemoteT->SetPosition(pos);
                    }

                    // 보간 target도 같이 보정해둬야 다음 프레임에 다시 아래로 끌려가지 않는다.
                    auto targetIt = m_mapRemotePlayerMoveTargets.find(remotePlayerId);
                    if (targetIt != m_mapRemotePlayerMoveTargets.end() &&
                        targetIt->second.hasTarget &&
                        targetIt->second.targetPos.y < waterPlayerY - 0.05f)
                    {
                        targetIt->second.targetPos.y = waterPlayerY;
                    }
                }
            }
        }
    }

    // 원격 플레이어 룬 오라 갱신.
    // 원격 플레이어는 Scene::Update의 PlayerUpdate를 타지 않으므로,
    // 보호막/보복 같은 지속 VFX는 여기서 별도로 갱신해야 한다.
    for (auto& kv : m_mapRemotePlayers)
    {
        GameObject* pRemote = kv.second;
        if (!pRemote)
            continue;

        if (PlayerComponent* pPC = pRemote->GetComponent<PlayerComponent>())
        {
            pPC->UpdateNetworkRuneVFX(deltaTime);
        }
    }

    // 입장 직후 서버가 내 위치를 0,0,0으로 들고 있을 수 있으므로,
    // 몇 프레임 동안 실제 시작 위치를 반복 전송한다.
    if (m_nInitialLocalPositionSyncFrames > 0)
    {
        // 매 프레임 보내면 너무 많으니 5프레임마다 한 번만 보낸다.
        if ((m_nInitialLocalPositionSyncFrames % 5) == 0)
            SyncLocalPlayerPositionToServer(pScene);

        --m_nInitialLocalPositionSyncFrames;
    }

    // 방 전환이 처리된 바로 그 프레임에는 TorchInteract 타이머를 줄이지 않음
    if (roomTransitionProcessedThisFrame)
        return;

    // 방 전환 후 지연 TorchInteract 처리
    if (m_bPendingTorchInteract)
    {
        m_nPendingTorchInteractFrame--;

        if (m_nPendingTorchInteractFrame <= 0)
        {
            SendTorchInteract();
            m_bPendingTorchInteract = false;
            m_nPendingTorchInteractFrame = 0;

            WriteNetworkLog("[Network] Delayed C_TORCH_INTERACT sent");
        }
    }
}

void NetworkManager::SendEnterGame(int playerIndex)
{
    // m_bConnected 만 체크. (IsConnected() 는 LocalPlayerId 도 보지만 아직 발급 전이라 안 됨)
    if (!m_bConnected || !m_pSession)
        return;

    Protocol::C_ENTER_GAME pkt;
    pkt.set_playerindex(static_cast<uint64>(playerIndex));
    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    char buf[128];
    sprintf_s(buf, "[Network] C_ENTER_GAME sent (playerIndex=%d)", playerIndex);
    OutputDebugStringA(buf);
}

void NetworkManager::SendMove(float x, float y, float z, float dirX, float dirY, float dirZ)
{
    if (!m_bConnected || !m_pSession)
        return;

    // 컷신 중이면 이동 패킷 전송 차단 
    if (m_bCutscenePlaying)
    {
        WriteNetworkLog("[Network] C_SKILL blocked: cutscene playing");
        return;
    }

    Protocol::C_MOVE movePkt;
    movePkt.set_x(x);
    movePkt.set_y(y);
    movePkt.set_z(z);
    movePkt.set_dirx(dirX);
    movePkt.set_diry(dirY);
    movePkt.set_dirz(dirZ);

    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(movePkt);
    m_pSession->Send(sendBuffer);
}

void NetworkManager::SendSkill(int skillType, float x, float y, float z, float dirX, float dirY, float dirZ)
{
    if (!m_bConnected || !m_pSession)
        return;

    // 컷신 중이면 스킬 패킷 전송 차단
    if (m_bCutscenePlaying)
    {
        WriteNetworkLog("[Network] C_SKILL blocked: cutscene playing");
        return;
    }

    Protocol::C_SKILL skillPkt;
    skillPkt.set_skilltype(static_cast<Protocol::SkillType>(skillType));
    skillPkt.set_x(x);
    skillPkt.set_y(y);
    skillPkt.set_z(z);
    skillPkt.set_dirx(dirX);
    skillPkt.set_diry(dirY);
    skillPkt.set_dirz(dirZ);

    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(skillPkt);
    m_pSession->Send(sendBuffer);

    // 결산: 로컬 플레이어 스킬 사용 카운트
    StatOnSkillUse(m_nLocalPlayerId.load(), skillType);
}

void NetworkManager::SendPlayerAction(uint32 actionType,
    float x, float y, float z,
    float dirX, float dirY, float dirZ)
{
    if (!m_bConnected || !m_pSession)
        return;

    Protocol::C_PLAYER_ACTION pkt;
    pkt.set_actiontype(actionType);

    pkt.set_x(x);
    pkt.set_y(y);
    pkt.set_z(z);

    pkt.set_dirx(dirX);
    pkt.set_diry(dirY);
    pkt.set_dirz(dirZ);

    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    char buf[256];
    sprintf_s(buf,
        "[Network] C_PLAYER_ACTION sent: actionType=%u pos=(%.2f, %.2f, %.2f)",
        actionType, x, y, z);
    WriteNetworkLog(buf);
}

void NetworkManager::SendPortalInteract(uint32 portalType)
{
    if (!m_bConnected || !m_pSession)
    {
        WriteNetworkLog("[Network] SendPortalInteract BLOCKED (not connected or no session)");
        return;
    }

    Protocol::C_PORTAL_INTERACT pkt;
    pkt.set_portaltype(portalType);

    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    char buf[128];
    sprintf_s(buf,
        "[Network] C_PORTAL_INTERACT sent: portalType=%u",
        portalType);
    WriteNetworkLog(buf);
}

void NetworkManager::SendTorchInteract()
{
    if (!m_bConnected || !m_pSession)
    {
        WriteNetworkLog("[Network] SendTorchInteract BLOCKED (not connected or no session)");
        OutputDebugString(L"[CLIENT][SendTorchInteract] blocked - not connected\n");
        return;
    }

    Protocol::C_TORCH_INTERACT pkt;
    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    OutputDebugString(L"[CLIENT][SendTorchInteract] sent\n");
    WriteNetworkLog("[Network] C_TORCH_INTERACT sent");
}

void NetworkManager::SendRuneRewardPick()
{
    if (!m_bConnected || !m_pSession)
        return;

    // 룬 선택 완료 알림
    // 실제 ownerPlayerId는 서버가 현재 세션의 playerId로 판단한다.
    Protocol::C_RUNE_REWARD_PICK pkt;

    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    WriteNetworkLog("[Network] C_RUNE_REWARD_PICK sent");
}

// 룬 장착 요청
void NetworkManager::SendRuneEquip(uint32 rewardOptionIndex, uint32 skillSlot, uint32 runeSlotIndex)
{
    if (!m_bConnected || !m_pSession)
        return;

    Protocol::C_RUNE_EQUIP pkt;
    pkt.set_rewardoptionindex(rewardOptionIndex);
    pkt.set_skillslot(skillSlot);
    pkt.set_runeslotindex(runeSlotIndex);

    // 서버에 룬 장착 요청 패킷 전송
    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    char buf[160];
    sprintf_s(buf,
        "[Network] C_RUNE_EQUIP sent: rewardOptionIndex=%u skillSlot=%u runeSlotIndex=%u",
        rewardOptionIndex,
        skillSlot,
        runeSlotIndex);
    WriteNetworkLog(buf);
}

void NetworkManager::SendDebugRuneEquip(uint32 skillSlot, uint32 runeSlotIndex, const std::string& runeId)
{
    if (!m_bConnected || !m_pSession)
        return;

    if (skillSlot >= 4)
    {
        WriteNetworkLog("[Network] C_DEBUG_RUNE_EQUIP blocked: invalid skillSlot");
        return;
    }

    if (runeSlotIndex >= 3)
    {
        WriteNetworkLog("[Network] C_DEBUG_RUNE_EQUIP blocked: invalid runeSlotIndex");
        return;
    }

    if (runeId.empty())
    {
        WriteNetworkLog("[Network] C_DEBUG_RUNE_EQUIP blocked: empty runeId");
        return;
    }

    Protocol::C_DEBUG_RUNE_EQUIP pkt;
    pkt.set_skillslot(skillSlot);
    pkt.set_runeslotindex(runeSlotIndex);
    pkt.set_runeid(runeId);

    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    char buf[256];
    sprintf_s(
        buf,
        "[Network] C_DEBUG_RUNE_EQUIP sent: skillSlot=%u runeSlotIndex=%u runeId=%s",
        skillSlot,
        runeSlotIndex,
        runeId.c_str()
    );
    WriteNetworkLog(buf);
}

void NetworkManager::SendDebugRoomAction(uint32 actionType)
{
    if (!m_bConnected || !m_pSession)
        return;

    Protocol::C_DEBUG_ROOM_ACTION pkt;
    pkt.set_actiontype(static_cast<Protocol::DebugRoomActionType>(actionType));

    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    char buf[160];
    sprintf_s(
        buf,
        "[Network] C_DEBUG_ROOM_ACTION sent: actionType=%u",
        actionType
    );
    WriteNetworkLog(buf);
}

void NetworkManager::SendDebugKillAll()
{
    if (!m_bConnected || !m_pSession)
    {
        WriteNetworkLog("[Network] SendDebugKillAll BLOCKED (not connected or no session)");
        OutputDebugString(L"[CLIENT][SendDebugKillAll] blocked - not connected\n");
        return;
    }

    auto sendBuffer = ServerPacketHandler::MakeDebugKillAllSendBuffer();
    m_pSession->Send(sendBuffer);

    OutputDebugString(L"[CLIENT][SendDebugKillAll] sent (F11 debug)\n");
    WriteNetworkLog("[Network] C_DEBUG_KILL_ALL sent");
}

void NetworkManager::SendPlayerAttack(int skillType,
    float x, float y, float z,
    float dirX, float dirY, float dirZ,
    float targetX, float targetY, float targetZ,
    float chargeRatio)
{
    if (!m_bConnected || !m_pSession)
        return;

    // 컷신 중이면 공격 패킷 전송 차단
    if (m_bCutscenePlaying)
    {
        WriteNetworkLog("[Network] C_PLAYER_ATTACK blocked: cutscene playing");
        return;
    }

    Protocol::C_PLAYER_ATTACK pkt;
    pkt.set_skilltype(static_cast<Protocol::SkillType>(skillType));
    pkt.set_x(x);
    pkt.set_y(y);
    pkt.set_z(z);
    pkt.set_dirx(dirX);
    pkt.set_diry(dirY);
    pkt.set_dirz(dirZ);
    pkt.set_targetx(targetX);
    pkt.set_targety(targetY);
    pkt.set_targetz(targetZ);
    pkt.set_chargeratio(chargeRatio);

    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    char buf[256];
    sprintf_s(buf,
        "[Network] C_PLAYER_ATTACK sent: skillType=%d chargeRatio=%.2f pos=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f)",
        skillType, chargeRatio, x, y, z, targetX, targetY, targetZ);
    WriteNetworkLog(buf);
}

// 보스 컷신 종료 알림 전송
// Kraken 등장 컷신이 끝났을 때 서버에 알려서 서버 AI 잠금을 해제한다.
void NetworkManager::SendBossCutsceneEnd(uint64 monsterId, uint32 eventType, uint32 phaseIndex)
{
    if (!m_bConnected || !m_pSession)
        return;

    Protocol::C_BOSS_CUTSCENE_END pkt;
    pkt.set_monsterid(monsterId);
    pkt.set_eventtype(static_cast<Protocol::BossEventType>(eventType));
    pkt.set_phaseindex(phaseIndex);

    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    char buf[160];
    sprintf_s(buf, "[Network] C_BOSS_CUTSCENE_END sent: monsterId=%llu eventType=%u phase=%u",
        monsterId, eventType, phaseIndex);
    WriteNetworkLog(buf);
}

void NetworkManager::QueueRoomTransition(uint32 stageIndex, uint32 roomIndex, bool isBossRoom, const std::string& mapId)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::RoomTransition;
    cmd.stageIndex = stageIndex;
    cmd.roomIndex = roomIndex;
    cmd.isBossRoom = isBossRoom;
    cmd.mapId = mapId;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueMonsterSpawn(uint64 monsterId, uint32 monsterType,
    uint32 attackType, uint32 visualType,
    float x, float y, float z, float yaw,
    float hp, bool isBoss)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::MonsterSpawn;

    cmd.monsterId = monsterId;
    cmd.monsterType = monsterType;
    cmd.monsterAttackType = attackType;
    cmd.monsterVisualType = visualType;

    cmd.x = x;
    cmd.y = y;
    cmd.z = z;
    cmd.monsterYaw = yaw;
    cmd.monsterHp = hp;
    cmd.monsterIsBoss = isBoss;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueMonsterMove(uint64 monsterId, float x, float y, float z, float yaw)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::MonsterMove;
    cmd.monsterId = monsterId;
    cmd.x = x; cmd.y = y; cmd.z = z;
    cmd.monsterYaw = yaw;
    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueMonsterDespawn(uint64 monsterId)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::MonsterDespawn;
    cmd.monsterId = monsterId;
    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueMonsterAttack(uint64 monsterId, uint64 targetPlayerId, uint32 attackType, float x, float y, float z, float yaw, float windupSec, const std::vector<DirectX::XMFLOAT3>& effectPositions, uint32 effectOption)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::MonsterAttack;
    cmd.monsterId = monsterId;
    cmd.targetPlayerId = targetPlayerId;
    cmd.attackType = attackType;
    cmd.x = x; cmd.y = y; cmd.z = z;
    cmd.monsterYaw = yaw;
    cmd.windupSec = windupSec;
    cmd.effectPositions = effectPositions;
    cmd.effectOption = effectOption;
    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueuePlayerDamage(uint64 playerId, float damage, float currentHp,
    bool isDead, uint64 attackerMonsterId)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::PlayerDamage;
    cmd.playerId = playerId;
    cmd.damage = damage;
    cmd.currentHp = currentHp;
    cmd.isDead = isDead;
    cmd.attackerMonsterId = attackerMonsterId;
    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueRoomStart(uint64 starterPlayerId)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::RoomStart;
    cmd.playerId = starterPlayerId;

    m_vCommandQueue.push_back(cmd);
}

// 몬스터 기절/그로기 큐 등록
void NetworkManager::QueueMonsterStagger(uint64 monsterId, float duration)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::MonsterStagger;
    cmd.monsterId = monsterId;
    cmd.duration = duration;

    m_vCommandQueue.push_back(cmd);
}

// 맵 토네이도 이벤트 큐 등록
void NetworkManager::QueueMapTornadoEvent(float x, float y, float z, float warningSec, float activeSec)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::MapTornadoEvent;
    cmd.mapTornadoX = x;
    cmd.mapTornadoY = y;
    cmd.mapTornadoZ = z;
    cmd.mapTornadoWarningSec = warningSec;
    cmd.mapTornadoActiveSec = activeSec;

    m_vCommandQueue.push_back(cmd);
}

GameObject* NetworkManager::GetServerMonster(uint64 monsterId)
{
    auto it = m_mapServerMonsters.find(monsterId);
    return (it != m_mapServerMonsters.end()) ? it->second : nullptr;
}

void NetworkManager::ProcessRoomTransition(Scene* pScene, uint32 stageIndex, uint32 roomIndex, bool isBossRoom, const std::string& mapId)
{
    if (!pScene)
        return;

    bool shouldAutoStartRoom = true;

    // 중복 전환 방어
    if (m_bInRoomTransition)
    {
        WriteNetworkLog("[Network] ProcessRoomTransition skipped: already transitioning");
        return;
    }
    m_bInRoomTransition = true;

    // 방 전환 전에 네트워크 일반 몬스터 연출 정리
    for (auto& entry : m_vNetworkNormalMonsterBehaviors)
    {
        auto monIt = m_mapServerMonsters.find(entry.monsterId);
        if (monIt == m_mapServerMonsters.end() || !monIt->second)
            continue;

        GameObject* pMonster = monIt->second;
        EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>();
        if (!pEnemy)
            continue;

        pEnemy->HideNetworkAttackIndicator();

        if (IAttackBehavior* pBehavior = pEnemy->GetAttackBehavior())
            pBehavior->Reset();
    }
    m_vNetworkNormalMonsterBehaviors.clear();

    // Golem / Demon 전용 entry 는 raw EnemyComponent* owner 를 들고 있어,
    // 다음 m_mapServerMonsters 정리 + 보스 GameObject 파기 후 UpdateNetworkGolemBehaviors/
    // UpdateNetworkDemonBehaviors 가 dangling owner 의 GetAnimationComponent()->CrossFade 를
    // 호출하면 std::map<string,...> UAF (예: "Golem_battle_stand_ge" lookup) 가 난다.
    // ProcessRoomTransition 시점에 한 번에 Reset + clear 해 dangling 접근을 차단한다.
    for (auto& entry : m_vNetworkGolemBehaviors)
    {
        if (entry.behavior)
            entry.behavior->Reset();
    }
    m_vNetworkGolemBehaviors.clear();

    for (auto& entry : m_vNetworkDemonBehaviors)
    {
        if (entry.behavior)
            entry.behavior->Reset();
    }
    m_vNetworkDemonBehaviors.clear();

    wchar_t buf[512];
    swprintf_s(buf, L"[Network] ProcessRoomTransition stage=%u room=%u boss=%d mapId=%S\n",
        stageIndex, roomIndex, isBossRoom ? 1 : 0, mapId.c_str());
    OutputDebugString(buf);

    // 이전 방 서버 몬스터 전부 정리 — GameObject 는 Scene 에 MarkForDeletion 으로 삭제 예약,
    // 보조 맵들은 즉시 clear. 새 방에서 같은 monsterId 가 재전송돼도 깨끗한 상태에서 재스폰됨.
    // 일반 몬스터 공격 인디케이터는 몬스터 본체와 별도 GameObject로 생성되므로,
    // 몬스터 삭제 전에 반드시 EnemyComponent를 통해 같이 삭제해야 한다.
    for (auto& kv : m_mapServerMonsters)
    {
        GameObject* pMonster = kv.second;
        if (!pMonster)
            continue;

        if (EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>())
        {
            pEnemy->DestroyIndicators(pScene);

            if (IAttackBehavior* pBehavior = pEnemy->GetAttackBehavior())
                pBehavior->Reset();
        }

        pScene->MarkForDeletion(pMonster);
    }

    m_mapServerMonsters.clear();
    m_mapServerMonsterClips.clear();
    m_mapServerMonsterTarget.clear();
    m_mapServerMonsterSpawnEffects.clear();
    m_mapServerMonsterCurrentAnimClip.clear();
    m_mapServerMonsterMoveTime.clear();
    m_mapServerMonsterAttackTimer.clear();
    m_mapServerMonsterHitFlashTimer.clear();
    m_setDeadServerMonsters.clear();
    m_mapRemotePlayerMoveTargets.clear();
    m_mapServerMonsterHitAnimCooldown.clear();

    // 인디케이터도 같이 정리 (보스 방 → 일반 방 또는 방 전환 시 잔존 막기)
    for (auto& kv : m_mapServerMonsterIndicators)
    {
        if (kv.second.circleBorder) pScene->MarkForDeletion(kv.second.circleBorder);
        if (kv.second.circleFill)   pScene->MarkForDeletion(kv.second.circleFill);
        if (kv.second.boxBorder)    pScene->MarkForDeletion(kv.second.boxBorder);
        if (kv.second.boxFill)      pScene->MarkForDeletion(kv.second.boxFill);
    }
    m_mapServerMonsterIndicators.clear();

    // 지연 VFX 큐도 비워야 이전 방의 미발사 투사체가 새 방에서 튀지 않음
    m_vPendingMonsterVFX.clear();

    // 원격/네트워크 스킬 예약 VFX 정리
    // MeteorShower 같은 지연 스폰형 VFX가 방 전환 후 다음 스테이지에서 계속 생성되는 것 방지
    m_vPendingMeteorShowers.clear();

    // 실제로 이미 생성된 투사체 / 유체 VFX / 보스 탄막 VFX 정리
    // m_vPendingMonsterVFX.clear()는 예약만 지우므로, 이미 Spawn된 VFX는 Scene 쪽에서 직접 정리해야 한다.
    pScene->ClearTransientCombatEffects();

    // ─────────────────────────────────────────────
// 방 전환 시 네트워크 룬 VFX 상태 정리
// 설치/궤도/증강/차지처럼 "상태를 유지하는 룬"은
// 새 방으로 넘어갈 때 반드시 map과 실제 VFX를 같이 비워야 한다.
// ─────────────────────────────────────────────
    {
        DecalManager* pDecals = pScene ? pScene->GetDecalManager() : nullptr;

        if (pDecals)
        {
            // 서버 trapId 기준 설치 룬 데칼 제거
            for (auto& kv : m_mapNetworkTrapVFXByObjectId)
            {
                if (kv.second.decalId >= 0)
                    pDecals->Stop(kv.second.decalId);
            }

            // 구형 playerId/skillSlot 기준 설치 룬 데칼도 같이 제거
            for (auto& kv : m_mapRemotePlaceDecalIds)
            {
                for (int decalId : kv.second)
                {
                    if (decalId >= 0)
                        pDecals->Stop(decalId);
                }
            }
        }

        FluidSkillVFXManager* pVFX = pScene ? pScene->GetFluidVFXManager() : nullptr;

        if (pVFX)
        {
            // 원격 차지 오라 제거
            for (auto& kv : m_mapRemoteChargeVFXId)
            {
                if (kv.second >= 0)
                    pVFX->StopEffect(kv.second);
            }

            // 원격 증강 오라 제거
            for (auto& kv : m_mapRemoteEnhanceVFXId)
            {
                if (kv.second >= 0)
                    pVFX->StopEffect(kv.second);
            }

            // 서버 orbitalId 기준 궤도 룬 대기 VFX 제거
            for (auto& kv : m_mapNetworkOrbitalVFXByObjectId)
            {
                if (kv.second.vfxId >= 0)
                    pVFX->StopEffect(kv.second.vfxId);
            }

            // 원격 플레이어 지속형 스킬 VFX 제거
            for (auto& kv : m_mapRemotePlayerVFX)
            {
                if (kv.second.vfxId >= 0)
                    pVFX->StopEffect(kv.second.vfxId);
            }

            // 고정 lifetime VFX 큐에 남은 것도 즉시 제거
            for (auto& kill : m_vTimedVFXKills)
            {
                if (kill.vfxId >= 0)
                    pVFX->StopEffect(kill.vfxId);
            }
        }

        m_mapNetworkTrapVFXByObjectId.clear();
        m_mapNetworkOrbitalVFXByObjectId.clear();
        m_mapRemotePlaceDecalIds.clear();

        m_mapRemoteChargeVFXId.clear();
        m_mapRemoteEnhanceVFXId.clear();

        m_mapRemotePlayerVFX.clear();
        m_vTimedVFXKills.clear();

        m_setRemoteChannelingPlayers.clear();
        m_mapRemoteChannelLastSpawnTime.clear();

        // 클라 측 궤도 지연 투사체 큐도 새 방으로 넘어가면 무조건 제거
        m_vPendingOrbitals.clear();

        WriteNetworkLog("[Network] Cleared network rune VFX states on room transition");
    }

    // 서버가 내려준 mapId 우선 분기. mapId 가 비었거나 미매칭이면 stageIndex/roomIndex 기반 fallback.
    bool bHandled = false;

    if (!mapId.empty())
    {
        if (mapId == "fire_boss") { pScene->TransitionToBossRoom();        bHandled = true; }
        else if (mapId == "water_boss") { pScene->TransitionToWaterBossRoom();   bHandled = true; }
        else if (mapId == "earth_boss") { pScene->TransitionToEarthBossRoom();   bHandled = true; }
        else if (mapId == "grass_boss") { pScene->TransitionToGrassBossRoom();   bHandled = true; }
        else if (mapId == "dark_lord")
        {
            // 네트워크 모드에서는 맵만 로드한다.
            // 컷신은 서버 S_BOSS_EVENT_INTRO 수신 후 시작한다.
            pScene->TransitionToDarkLordRoom(false);
            bHandled = true;

            // DarkLord도 포탈 진입 방이므로 자동 C_TORCH_INTERACT를 보내 서버 스폰을 요청한다.
            shouldAutoStartRoom = true;
        }
        else if (mapId.rfind("fire_room_", 0) == 0)
        {
            if (stageIndex == 1 && roomIndex == 0)
            {
                pScene->TransitionToFireStage(0);
            }
            else
            {
                pScene->TransitionToRoomByIndex(static_cast<int>(roomIndex));
            }

            bHandled = true;
        }
        else if (mapId.rfind("water_room_", 0) == 0)
        {
            pScene->TransitionToWaterStage(static_cast<int>(roomIndex));
            bHandled = true;
        }
        else if (mapId.rfind("earth_room_", 0) == 0)
        {
            pScene->TransitionToEarthStage(static_cast<int>(roomIndex));
            bHandled = true;
        }
        else if (mapId.rfind("grass_room_", 0) == 0)
        {
            pScene->TransitionToGrassStage(static_cast<int>(roomIndex));
            bHandled = true;
        }
    }

    if (!bHandled)
    {
        // legacy / unknown mapId fallback
        if (isBossRoom)
        {
            switch (stageIndex)
            {
            case 1: pScene->TransitionToBossRoom();        break;
            case 2: pScene->TransitionToWaterBossRoom();   break;
            case 3: pScene->TransitionToEarthBossRoom();   break;
            case 4: pScene->TransitionToGrassBossRoom();   break;
            case 5:
                pScene->TransitionToDarkLordRoom(false);
                shouldAutoStartRoom = true;
                break;
            default: pScene->TransitionToBossRoom();       break;
            }
        }
        else
        {
            pScene->TransitionToRoomByIndex(static_cast<int>(roomIndex));
        }
    }

    // 방 전환 후 플레이어 포탈 Intro Fly 시작
    // 서버 HandlePortalInteract 는 플레이어 좌표를 직접 바꾸지 않으므로,
    // 새 맵 로드 직후 로컬 플레이어의 현재 위치를 착지 기준점으로 사용한다.
    if (GameObject* pLocal = pScene->GetPlayer())
    {
        if (auto* pLocalT = pLocal->GetTransform())
        {
            XMFLOAT3 groundPos = pLocalT->GetPosition();

            // 보스방은 일반방처럼 착지 발판이 없으므로,
            // 포탈 인트로의 착지 기준 Y를 항상 0으로 고정한다.
            // 낙하 연출 높이를 없애는 게 아니라, 최종 착지 기준점만 평지로 맞춘다.
            if (isBossRoom)
            {
                groundPos.y = 0.0f;
            }

            // 원격 플레이어 좌표 리셋
            // 이전 방 좌표가 그대로 남으면 새 맵에서 맵 밖/이상한 위치로 보일 수 있으므로
            // 우선 착지 기준점으로 모아둔다.
            // 단, 로컬 플레이어를 y + 22.0f 로 올리기 전에 groundPos 를 먼저 저장해야 한다.
            for (auto& kv : m_mapRemotePlayers)
            {
                if (GameObject* pRemote = kv.second)
                {
                    if (auto* pT = pRemote->GetTransform())
                    {
                        pT->SetPosition(groundPos.x, groundPos.y, groundPos.z);
                    }
                }
            }

            // 로컬 플레이어 포탈 Intro Fly 시작
            pLocalT->SetPosition(
                groundPos.x,
                groundPos.y + kPlayerPortalIntroStartHeight,
                groundPos.z);

            if (auto* pPC = pLocal->GetComponent<PlayerComponent>())
            {
                pPC->StartIntroFly(
                    kPlayerPortalIntroDuration,
                    groundPos.y,
                    XMFLOAT3(groundPos.x, groundPos.y, groundPos.z),
                    kPlayerPortalIntroStandRadius);

                // 포탈 낙하 연출 중에는 서버 S_MOVE 보정이 로컬 Transform을 덮어쓰지 않게 막는다.
                m_fLocalMoveCorrectionBlockTimer = kPlayerPortalIntroDuration + 0.35f;

                // 새 방 시작 위치를 서버에도 바로 알려서 이전 방 좌표가 다시 내려오지 않게 한다.
                SyncLocalPlayerPositionToServer(pScene);
            }

            // 네트워크 연출 액션 — 다른 클라에도 이 플레이어의 포탈 Intro Fly를 재생시킨다.
            // 위치 판정이 아니라 연출 시작 알림만 보낸다.
            SendPlayerAction(
                PLAYER_ACTION_PORTAL_INTRO_FLY,
                groundPos.x, groundPos.y, groundPos.z,
                0.0f, 0.0f, 1.0f);

            WriteNetworkLog("[Network] Local PortalIntroFly started after room transition");

            // 방 전환 직후에도 서버가 이전 위치를 들고 있을 수 있으므로,
            // 새 방의 실제 시작 위치를 몇 프레임 동안 반복 전송한다.
            m_nInitialLocalPositionSyncFrames = 60;
        }
    }

    // 방 전환이 끝나면 서버 몬스터 스폰을 트리거해야 함.
    // 서버 Room 은 HandleTorchInteract 를 받아야만 몬스터를 스폰하도록 돼 있음 (최초 방 진입과 동일 플로우).
    // 오프라인에서는 방 Active 시 자동 스폰이지만, 네트워크 모드에서는 C_TORCH_INTERACT 가 스폰 트리거.
    // 첫 방에선 맵에 배치된 횃불/큐브를 F 로 눌러 시작했지만, 다음 방 이후에는 자동으로 요청해준다.
    if (shouldAutoStartRoom)
    {
        m_bPendingTorchInteract = true;
        m_nPendingTorchInteractFrame = 30;
        WriteNetworkLog("[Network] Pending C_TORCH_INTERACT scheduled");
    }
    else
    {
        m_bPendingTorchInteract = false;
        m_nPendingTorchInteractFrame = 0;
        WriteNetworkLog("[Network] Pending C_TORCH_INTERACT skipped");
    }

    m_bInRoomTransition = false;
}

bool NetworkManager::IsBlockingServerBossIntroActive() const
{
    for (const auto& pair : m_mapServerBossIntros)
    {
        uint64 monsterId = pair.first;
        const ServerBossIntroState& st = pair.second;

        if (!st.active)
            continue;

        auto clipIt = m_mapServerMonsterClips.find(monsterId);
        uint32 mt = (clipIt != m_mapServerMonsterClips.end())
            ? clipIt->second.monsterType
            : 0;

        // Red Dragon만 입력 차단.
        // BlueDragon(mt==10)은 현재 가벼운 등장 연출이라 제외.
        if (mt == 6)
            return true;
    }

    return false;
}

void NetworkManager::QueueSpawnPlayer(uint64 playerId, const std::string& name, int playerType, float x, float y, float z)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd;
    cmd.type = NetworkCommand::Spawn;
    cmd.playerId = playerId;
    cmd.name = name;
    cmd.playerType = playerType;
    cmd.x = x;
    cmd.y = y;
    cmd.z = z;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueDespawnPlayer(uint64 playerId)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd;
    cmd.type = NetworkCommand::Despawn;
    cmd.playerId = playerId;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueMovePlayer(uint64 playerId, float x, float y, float z, float dirX, float dirY, float dirZ)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd;
    cmd.type = NetworkCommand::Move;
    cmd.playerId = playerId;
    cmd.x = x;
    cmd.y = y;
    cmd.z = z;
    cmd.dirX = dirX;
    cmd.dirY = dirY;
    cmd.dirZ = dirZ;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueSkill(uint64 playerId, int skillType, float x, float y, float z, float dirX, float dirY, float dirZ,
    int32 skillSlot, float radiusMult, float damageMult)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd;
    cmd.type = NetworkCommand::Skill;
    cmd.playerId = playerId;
    cmd.skillType = skillType;
    cmd.x = x;
    cmd.y = y;
    cmd.z = z;
    cmd.dirX = dirX;
    cmd.dirY = dirY;
    cmd.dirZ = dirZ;
    cmd.skillSlot = skillSlot;
    cmd.skillRadiusMult = (radiusMult > 0.0f) ? radiusMult : 1.0f;
    cmd.skillDamageMult = (damageMult > 0.0f) ? damageMult : 1.0f;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueuePlayerAction(uint64 playerId, uint32 actionType, float x, float y, float z, float dirX, float dirY, float dirZ)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::PlayerAction;
    cmd.playerId = playerId;
    cmd.playerActionType = actionType;

    cmd.x = x;
    cmd.y = y;
    cmd.z = z;

    cmd.dirX = dirX;
    cmd.dirY = dirY;
    cmd.dirZ = dirZ;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueSetLocalPlayerId(uint64 playerId)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd;
    cmd.type = NetworkCommand::SetLocalPlayerId;
    cmd.playerId = playerId;

    m_vCommandQueue.push_back(cmd);
}

GameObject* NetworkManager::GetRemotePlayer(uint64 playerId)
{
    auto it = m_mapRemotePlayers.find(playerId);
    if (it != m_mapRemotePlayers.end())
    {
        return it->second;
    }
    return nullptr;
}

void NetworkManager::UpdateRemotePlayerActionLocks(float deltaTime)
{
    for (auto it = m_mapRemotePlayerActionLockTimer.begin();
        it != m_mapRemotePlayerActionLockTimer.end(); )
    {
        it->second -= deltaTime;

        if (it->second <= 0.0f)
            it = m_mapRemotePlayerActionLockTimer.erase(it);
        else
            ++it;
    }
}

void NetworkManager::UpdateRemotePlayerPortalIntroFlyEffects(float deltaTime)
{
    for (auto it = m_mapRemotePlayerPortalIntroFlyEffects.begin();
        it != m_mapRemotePlayerPortalIntroFlyEffects.end(); )
    {
        uint64 playerId = it->first;
        RemotePlayerPortalIntroFlyEffect& fx = it->second;

        auto playerIt = m_mapRemotePlayers.find(playerId);
        if (playerIt == m_mapRemotePlayers.end() || !playerIt->second)
        {
            it = m_mapRemotePlayerPortalIntroFlyEffects.erase(it);
            continue;
        }

        GameObject* pRemotePlayer = playerIt->second;
        TransformComponent* pTransform = pRemotePlayer->GetTransform();
        AnimationComponent* pAnim = pRemotePlayer->GetComponent<AnimationComponent>();

        if (!pTransform)
        {
            it = m_mapRemotePlayerPortalIntroFlyEffects.erase(it);
            continue;
        }

        if (!fx.onGround)
        {
            // 원격 플레이어가 실제로 Levitating을 유지하고 있는지 확인한다.
            if (pAnim)
            {
                bool bLevitating = pAnim->IsCurrentClip("Levitating");

                char buf[256];
                sprintf_s(buf,
                    "[Network] Remote IntroFly AnimCheck: playerId=%llu isLevitating=%d animTime=%.3f y=%.2f",
                    playerId,
                    bLevitating ? 1 : 0,
                    pAnim->GetCurrentTime(),
                    pTransform->GetPosition().y);
                WriteNetworkLog(buf);
            }

            // 포탈 Intro Fly 중에는 원격 플레이어도 Levitating 상태를 유지한다.
            if (pAnim)
                pAnim->CrossFade("Levitating", 0.10f, true, false);

            // 로컬 PlayerComponent::StartIntroFly()와 같은 자유낙하 방식
            // velocityY = 0 에서 시작하고 GRAVITY=50.0f 로 내려온다.
            XMFLOAT3 pos = pTransform->GetPosition();

            fx.velocityY -= kPlayerPortalIntroGravity * deltaTime;
            pos.y += fx.velocityY * deltaTime;

            if (pos.y <= fx.groundY)
            {
                pos.y = fx.groundY;
                fx.velocityY = 0.0f;
                fx.onGround = true;
                fx.landingHoldTimer = kPlayerPortalIntroLandingHold;
                fx.introTimer = 0.0f;

                if (pAnim)
                    pAnim->CrossFade("Landing", 0.05f, false, true);
            }

            pTransform->SetPosition(fx.groundX, pos.y, fx.groundZ);

            if (fx.introTimer > 0.0f)
                fx.introTimer -= deltaTime;

            ++it;
            continue;
        }

        if (fx.landingHoldTimer > 0.0f)
        {
            fx.landingHoldTimer = fmaxf(0.0f, fx.landingHoldTimer - deltaTime);

            if (fx.landingHoldTimer <= 0.0f)
            {
                if (pAnim)
                    pAnim->CrossFade("Idle", 0.15f, true);

                m_mapRemotePlayerActionLockTimer.erase(playerId);
                it = m_mapRemotePlayerPortalIntroFlyEffects.erase(it);
                continue;
            }
        }

        ++it;
    }
}

void NetworkManager::ProcessSpawnPlayer(Scene* pScene, ID3D12Device* pDevice,
    ID3D12GraphicsCommandList* pCommandList,
    uint64 playerId, const std::string& name,
    int playerType, float x, float y, float z)
{
    wchar_t idLog[256]; // 로그용 버퍼 선언

    // 로컬 플레이어 ID 확인
    uint64 myId = GetLocalPlayerId();

    swprintf_s(idLog, 256, L"[Network] Handle Spawn: PktId=%llu, MyLocalId=%llu\n", playerId, myId);
    OutputDebugString(idLog);

    // 로컬 플레이어라면 무시
    if (playerId == myId)
    {
        OutputDebugString(L"[Network] Skipping spawn for local player (Self)\n");
        return;
    }

    // 이미 존재하는 플레이어라면 무시
    if (m_mapRemotePlayers.find(playerId) != m_mapRemotePlayers.end())
    {
        swprintf_s(idLog, 256, L"[Network] Remote player %llu already exists. Updating position.\n", playerId);
        OutputDebugString(idLog);
        // Spawn에는 방향 정보가 없으므로 기본 방향 (0, 0, 1) 사용
        ProcessMovePlayer(pScene, playerId, x, y, z, 0.0f, 0.0f, 1.0f);
        return;
    }

    // 원격 플레이어를 전역 오브젝트로 생성하기 위해 CurrentRoom을 임시 해제
    // (Room에 등록되면 Room 전환 시 삭제되거나 업데이트가 안 될 수 있음)
    CRoom* pTempRoom = pScene->GetCurrentRoom();
    pScene->SetCurrentRoom(nullptr);

    // 서버가 전달한 playerType(wire 1~4) → ElementType(Fire=1..Earth=4) 매핑.
    // 범위 밖이면 Water 로 fallback.
    ElementType remoteElement = ElementType::Water;
    if (playerType >= 1 && playerType <= 4)
        remoteElement = static_cast<ElementType>(playerType);
    const CharacterData& cdata = GetCharacterData(remoteElement);

    // 원격 플레이어가 실제로 어떤 캐릭터/애니메이션 파일을 쓰는지 확인한다.
    {
        char buf[512];
        sprintf_s(buf,
            "[Network] RemotePlayer SpawnData: playerId=%llu playerType=%d remoteElement=%d mesh=%s anim=%s",
            playerId,
            playerType,
            static_cast<int>(remoteElement),
            cdata.meshPath,
            cdata.animPath);
        WriteNetworkLog(buf);
    }

    // 새 원격 플레이어 모델 로드 — 선택한 원소의 mesh 파일.
    GameObject* pRemotePlayer = MeshLoader::LoadGeometryFromFile(pScene, pDevice, pCommandList, NULL, cdata.meshPath);
    if (!pRemotePlayer)
    {
        OutputDebugString(L"[Network] Failed to load remote player model, falling back to cube\n");
        pRemotePlayer = pScene->CreateGameObject(pDevice, pCommandList);

        CubeMesh* pCubeMesh = new CubeMesh(pDevice, pCommandList, 1.0f, 2.0f, 1.0f);
        pCubeMesh->AddRef();
        pRemotePlayer->SetMesh(pCubeMesh);
        pRemotePlayer->AddComponent<RenderComponent>()->SetMesh(pCubeMesh);
    }

    // CurrentRoom 복원
    pScene->SetCurrentRoom(pTempRoom);

    // 위치 및 스케일 설정
    TransformComponent* pTransform = pRemotePlayer->GetTransform();
    if (pTransform)
    {
        float spawnX = x;
        float spawnY = y;
        float spawnZ = z;

        // 서버가 아직 기존 플레이어의 실제 위치를 모르면 0,0,0으로 스폰될 수 있다.
        // 이 경우 첫 화면에서 벽쪽에 보이므로, 임시로 내 현재 시작 위치 근처에 배치한다.
        bool bInvalidSpawnPos =
            (fabsf(spawnX) < 0.001f && fabsf(spawnY) < 0.001f && fabsf(spawnZ) < 0.001f);

        if (bInvalidSpawnPos)
        {
            if (GameObject* pLocal = pScene->GetPlayer())
            {
                if (auto* pLocalT = pLocal->GetTransform())
                {
                    XMFLOAT3 localPos = pLocalT->GetPosition();

                    spawnX = localPos.x;
                    spawnY = localPos.y;
                    spawnZ = localPos.z;

                    // 내 캐릭터가 인트로 공중에 있으면 원격도 착지 기준 위치로 보정한다.
                    if (spawnY > 10.0f)
                        spawnY -= kPlayerPortalIntroStartHeight;

                    WriteNetworkLog("[Network] Remote spawn position fallback applied");
                }
            }
        }

        pTransform->SetPosition(spawnX, spawnY, spawnZ);
        pTransform->SetScale(5.0f, 5.0f, 5.0f);
    }

    // 애니메이션 추가 — 원소별 anim 파일.
    auto* pAnim = pRemotePlayer->AddComponent<AnimationComponent>();
    if (pAnim)
    {
        pAnim->LoadAnimation(cdata.animPath);
        pAnim->Play("Idle", true);
        pAnim->SetCullEnabled(false);
    }

    // 셰이더 등록
    Shader* pDefaultShader = pScene->GetDefaultShader();
    if (pDefaultShader)
    {
        pScene->AddRenderComponentsToHierarchy(pDevice, pCommandList, pRemotePlayer, pDefaultShader, true);
        // 원격 플레이어도 로컬 플레이어와 같은 영웅 톤 머티리얼을 적용한다.
        ApplyNetworkPlayerMaterial(pRemotePlayer, GetNetworkPlayerColor(remoteElement));
    }

    // 원격 플레이어도 PlayerComponent가 있어야 보호막/보복/흡혈 추적 VFX가 보인다.
    // 로컬처럼 입력 처리는 하지 않고, NetworkManager::Update에서 UpdateNetworkRuneVFX만 호출한다.
    PlayerComponent* pRemotePC = pRemotePlayer->GetComponent<PlayerComponent>();
    if (!pRemotePC)
        pRemotePC = pRemotePlayer->AddComponent<PlayerComponent>();

    if (pRemotePC)
        pRemotePC->SetElementType(remoteElement);

    // 원격 플레이어용 SkillComponent — m_Skills 는 비워두고 m_SkillRunes 만 사용.
    //   ProcessRuneEquip 가 SetRuneSlot 으로 룬을 저장하고, ProcessSkill 이 BuildSkillStats 로
    //   원소 색상/runeFlags 를 가져온다. 실제 스킬 실행은 서버 권위라 m_Skills 없어도 됨.
    bool bHadSkill = (pRemotePlayer->GetComponent<SkillComponent>() != nullptr);
    if (!bHadSkill)
        pRemotePlayer->AddComponent<SkillComponent>();
    bool bAfter = (pRemotePlayer->GetComponent<SkillComponent>() != nullptr);
    {
        char skbuf[160];
        sprintf_s(skbuf, "[RuneDiag] Spawn playerId=%llu hadSkill=%d after=%d", playerId, bHadSkill ? 1 : 0, bAfter ? 1 : 0);
        WriteNetworkLog(skbuf);
    }

    // 컴포넌트 초기화 (AnimationComponent::BuildBoneCache 포함)
    pRemotePlayer->Init(pDevice, pCommandList);

    // 맵에 등록
    m_mapRemotePlayers[playerId] = pRemotePlayer;
    m_mapRemotePlayerElement[playerId] = remoteElement;

    // DarkLord 컷신 중 늦게 들어온 원격 플레이어도 즉시 숨긴다.
    if (pScene && pScene->IsDarkLordIntroPlaying())
    {
        SetRenderTreeVisibleNet(pRemotePlayer, false);
    }

    // 원격 플레이어가 방금 점유한 descriptor slot 들을 "영구" 범위로 편입.
    // 이 후 방 전환 시 m_nNextDescriptorIndex 가 m_nPersistentDescriptorEnd 로 리셋되지만,
    // 그 값이 원격 플레이어 slot 뒤로 이동했으므로 새 방 오브젝트가 원격 플레이어의
    // CB/descriptor slot 을 재사용하며 덮어쓰는 충돌이 사라짐.
    // (원격 플레이어가 방 전환 후 안 보이던 증상의 근본 원인)
    pScene->UpdatePersistentDescriptorEnd();

    swprintf_s(idLog, 256, L"[Network] SUCCESS: Spawned RemotePlayer_%llu (%hs). Total RemoteCount: %zu\n",
        playerId, name.c_str(), m_mapRemotePlayers.size());
    OutputDebugString(idLog);
}

void NetworkManager::ProcessDespawnPlayer(Scene* pScene, uint64 playerId)
{
    // 로컬 플레이어라면 무시
    if (playerId == m_nLocalPlayerId.load())
        return;

    auto it = m_mapRemotePlayers.find(playerId);
    if (it == m_mapRemotePlayers.end())
    {
        wchar_t buf[128];
        swprintf_s(buf, L"[Network] Despawn failed: player %llu not found\n", playerId);
        OutputDebugString(buf);
        return;
    }

    // Scene에 삭제 요청
    GameObject* pRemotePlayer = it->second;
    pScene->MarkForDeletion(pRemotePlayer);

    // pending orbital 발사 큐 정리 — owner 가 곧 파기될 원격 플레이어이면
    // 지연 발사 시 dangling owner 로 SpawnProjectile 호출 → UAF.
    if (!m_vPendingOrbitals.empty())
    {
        FluidSkillVFXManager* pVFX = pScene ? pScene->GetFluidVFXManager() : nullptr;
        m_vPendingOrbitals.erase(
            std::remove_if(
                m_vPendingOrbitals.begin(),
                m_vPendingOrbitals.end(),
                [pRemotePlayer, pVFX](const PendingOrbitalProjectile& p)
                {
                    if (p.owner == pRemotePlayer)
                    {
                        if (pVFX && p.orbVfxId >= 0)
                            pVFX->StopEffect(p.orbVfxId);
                        return true;
                    }
                    return false;
                }),
            m_vPendingOrbitals.end()
        );
    }

    // 원격 플레이어 지속형 스킬 VFX 정리 — UpdateRemoteActivationRuneVFX 가 매 프레임
    // m_mapRemotePlayers find 로 자체 청소하지만, 그 사이 한 프레임은 stale entry 로
    // playerIt->second->GetTransform() 호출 가능. despawn 시점에 즉시 끊는다.
    if (pScene)
    {
        FluidSkillVFXManager* pVFX = pScene->GetFluidVFXManager();
        if (pVFX)
        {
            auto chargeIt = m_mapRemoteChargeVFXId.find(playerId);
            if (chargeIt != m_mapRemoteChargeVFXId.end())
            {
                if (chargeIt->second >= 0) pVFX->StopEffect(chargeIt->second);
                m_mapRemoteChargeVFXId.erase(chargeIt);
            }

            auto enhanceIt = m_mapRemoteEnhanceVFXId.find(playerId);
            if (enhanceIt != m_mapRemoteEnhanceVFXId.end())
            {
                if (enhanceIt->second >= 0) pVFX->StopEffect(enhanceIt->second);
                m_mapRemoteEnhanceVFXId.erase(enhanceIt);
            }

            auto vfxIt = m_mapRemotePlayerVFX.find(playerId);
            if (vfxIt != m_mapRemotePlayerVFX.end())
            {
                if (vfxIt->second.vfxId >= 0) pVFX->StopEffect(vfxIt->second.vfxId);
                m_mapRemotePlayerVFX.erase(vfxIt);
            }
        }

        DecalManager* pDecals = pScene->GetDecalManager();
        if (pDecals)
        {
            auto decalIt = m_mapRemotePlaceDecalIds.find(playerId);
            if (decalIt != m_mapRemotePlaceDecalIds.end())
            {
                for (int decalId : decalIt->second)
                {
                    if (decalId >= 0) pDecals->Stop(decalId);
                }
                m_mapRemotePlaceDecalIds.erase(decalIt);
            }
        }
    }

    m_setRemoteChannelingPlayers.erase(playerId);
    m_mapRemoteChannelLastSpawnTime.erase(playerId);

    // 맵에서 제거
    m_mapRemotePlayers.erase(it);
    m_mapRemotePlayerElement.erase(playerId);
    m_mapRemotePlayerMoveTime.erase(playerId);
    m_setDeadRemotePlayers.erase(playerId);
    m_mapRemotePlayerHitFlashTimer.erase(playerId);
    m_mapRemotePlayerActionLockTimer.erase(playerId);
    m_mapRemotePlayerPortalIntroFlyEffects.erase(playerId);
    m_mapRemotePlayerMoveTargets.erase(playerId);

    wchar_t buf[128];
    swprintf_s(buf, L"[Network] Despawned remote player %llu\n", playerId);
    OutputDebugString(buf);
}

void NetworkManager::ProcessMovePlayer(Scene* pScene, uint64 playerId, float x, float y, float z, float dirX, float dirY, float dirZ)
{
    // 서버 보정 좌표가 내 플레이어에게 온 경우에도 실제 Transform에 반영한다.
    if (playerId == m_nLocalPlayerId.load())
    {
        // 포탈 낙하 인트로 중에는 서버 위치 보정이 연출을 덮어쓰지 않게 무시한다.
        if (m_fLocalMoveCorrectionBlockTimer > 0.0f)
            return;

        if (!pScene)
            return;

        GameObject* pLocalPlayer = pScene->GetPlayer();
        if (!pLocalPlayer)
            return;

        TransformComponent* pTransform = pLocalPlayer->GetTransform();
        if (!pTransform)
            return;

        // 서버가 확정한 위치로 로컬 플레이어를 보정한다.
        pTransform->SetPosition(x, y, z);
        return;
    }

    auto it = m_mapRemotePlayers.find(playerId);
    if (it == m_mapRemotePlayers.end())
        return;

    GameObject* pRemotePlayer = it->second;
    TransformComponent* pTransform = pRemotePlayer->GetTransform();

    bool bPortalIntroPlaying = (m_mapRemotePlayerPortalIntroFlyEffects.find(playerId) != m_mapRemotePlayerPortalIntroFlyEffects.end());

    AnimationComponent* pAnim = pRemotePlayer->GetComponent<AnimationComponent>();

    // Kraken 페이즈 여부 확인.
    // Kraken 수면 상승 중에는 S_MOVE의 y가 높아질 수 있으므로,
    // y > 10 이라고 무조건 포탈 IntroFly로 오판하면 안 된다.
    bool krakenActiveForRemoteY = false;

    for (const auto& monKv : m_mapServerMonsterClips)
    {
        if (monKv.second.monsterType == 7) // Kraken
        {
            krakenActiveForRemoteY = true;
            break;
        }
    }

    // 초기 접속 또는 패킷 순서 차이로 PLAYER_ACTION_PORTAL_INTRO_FLY보다 S_MOVE가 먼저 올 수 있다.
    // 이때 y가 높으면 일반 이동이 아니라 포탈 Intro Fly 중인 공중 위치로 보고 Levitating을 먼저 잡아준다.
    if (!krakenActiveForRemoteY && !bPortalIntroPlaying && pTransform && y > 10.0f)
    {
        RemotePlayerPortalIntroFlyEffect fx{};
        fx.introTimer = kPlayerPortalIntroDuration;
        fx.landingHoldTimer = 0.0f;
        fx.velocityY = 0.0f;

        // S_MOVE의 y는 현재 공중 높이이므로, 기존 클라 수치 22.0f를 빼서 착지 높이를 추정한다.
        fx.groundX = x;
        fx.groundY = y - kPlayerPortalIntroStartHeight;
        fx.groundZ = z;
        fx.onGround = false;

        m_mapRemotePlayerPortalIntroFlyEffects[playerId] = fx;

        // 인트로 중에는 Walk/Idle이 끼어들지 않도록 잠깐 잠근다.
        m_mapRemotePlayerActionLockTimer[playerId] =
            kPlayerPortalIntroDuration + kPlayerPortalIntroLandingHold;

        pTransform->SetPosition(x, y, z);

        if (pAnim)
            pAnim->CrossFade("Levitating", 0.10f, true, true);

        // 인트로 중에는 idle 타이머가 끼어들면 안 된다.
        m_mapRemotePlayerMoveTime.erase(playerId);

        bPortalIntroPlaying = true;

        WriteNetworkLog("[Network] Remote PortalIntroFly fallback started from high S_MOVE");
    }

    // 이번 S_MOVE가 실제 이동인지, 마우스 회전만인지 구분한다.
    bool bPositionMoved = false;

    if (pTransform)
    {
        XMFLOAT3 oldPos = pTransform->GetPosition();

        auto introIt = m_mapRemotePlayerPortalIntroFlyEffects.find(playerId);
        if (introIt != m_mapRemotePlayerPortalIntroFlyEffects.end())
        {
            // 포탈 Intro Fly 중이면 S_MOVE 위치를 바로 Transform에 덮어쓰지 않는다.
            // 대신 착지 기준 좌표만 최신 서버 좌표로 갱신한다.
            introIt->second.groundX = x;
            introIt->second.groundZ = z;
        }
        else
        {
            // 실제 위치 변화가 있을 때만 이동으로 판단한다.
            // 마우스로 방향만 돌린 C_MOVE는 Walk 애니로 바꾸면 안 된다.
            float dx = x - oldPos.x;
            float dz = z - oldPos.z;
            bPositionMoved = (dx * dx + dz * dz) > 0.000001f;

            // 일반 상태에서는 바로 위치를 박지 않고 목표 위치만 갱신한다.
// 실제 Transform 이동은 UpdateRemotePlayerInterpolation에서 부드럽게 처리한다.
            RemotePlayerMoveTarget& moveTarget = m_mapRemotePlayerMoveTargets[playerId];

            // Kraken 페이즈에서는 서버에서 온 y가 ground 기준으로 들어올 수 있다.
            // 이 값을 그대로 targetPos.y에 넣으면 움직일 때 원격 플레이어가 물 아래로 보간된다.
            // 그래서 Kraken이 살아있는 동안에는 원격 플레이어 표시 y를 로컬 플레이어의 현재 수면 y 이상으로 고정한다.
            float visualY = y;

            if (krakenActiveForRemoteY && pScene && pScene->GetPlayer())
            {
                TransformComponent* pLocalT = pScene->GetPlayer()->GetTransform();

                if (pLocalT)
                {
                    float surfaceY = pLocalT->GetPosition().y;

                    if (visualY < surfaceY - 0.05f)
                        visualY = surfaceY;
                }
            }

            moveTarget.targetPos = XMFLOAT3(x, visualY, z);
            moveTarget.hasTarget = true;
        }

        // 방향은 이동/회전 구분과 상관없이 항상 갱신한다.
        float length = sqrtf(dirX * dirX + dirZ * dirZ);
        if (length > 0.001f)
        {
            float yaw = atan2f(dirX, dirZ);
            float yawDegrees = XMConvertToDegrees(yaw);
            m_mapRemotePlayerMoveTargets[playerId].targetYaw = yawDegrees;

            XMFLOAT3 currentRot = pTransform->GetRotation();
            pTransform->SetRotation(currentRot.x, yawDegrees, currentRot.z);
        }
    }

    // 죽은 원격 플레이어는 데스 애니 유지한다.
    bool bDead = (m_setDeadRemotePlayers.find(playerId) != m_setDeadRemotePlayers.end());

    // 대쉬/인트로 같은 연출 중에는 Walk/Idle이 끼어들면 안 된다.
    bool bActionLocked = (m_mapRemotePlayerActionLockTimer.find(playerId) != m_mapRemotePlayerActionLockTimer.end());

    if (pAnim && !bDead && !bActionLocked && !bPortalIntroPlaying && bPositionMoved)
    {
        // 실제 위치가 바뀐 경우에만 Walk로 전환한다.
        pAnim->CrossFade("Walk", 0.1f, true);
    }

    if (!bDead && !bPortalIntroPlaying && bPositionMoved)
    {
        // 실제 이동했을 때만 idle 전환 타이머를 갱신한다.
        m_mapRemotePlayerMoveTime[playerId] = 0.0f;
    }
}

void NetworkManager::CheckRemotePlayerIdle(float deltaTime)
{
    // (0) 원격 플레이어 hit flash 페이드 — 피격 직후 glow 가 남지 않도록 0 까지 감소
    for (auto it = m_mapRemotePlayerHitFlashTimer.begin(); it != m_mapRemotePlayerHitFlashTimer.end(); )
    {
        it->second -= deltaTime;
        auto playerIt = m_mapRemotePlayers.find(it->first);
        if (playerIt != m_mapRemotePlayers.end())
        {
            if (it->second > 0.0f)
            {
                float f = it->second / REMOTE_HIT_FLASH_DURATION;
                playerIt->second->SetHitFlashAll(f);
            }
            else
            {
                playerIt->second->SetHitFlashAll(0.0f);
            }
        }
        if (it->second <= 0.0f) it = m_mapRemotePlayerHitFlashTimer.erase(it);
        else ++it;
    }

    // 원격 플레이어들의 마지막 이동 시간 업데이트
    for (auto it = m_mapRemotePlayerMoveTime.begin(); it != m_mapRemotePlayerMoveTime.end(); )
    {
        uint64 playerId = it->first;
        float& timeSinceMove = it->second;
        timeSinceMove += deltaTime;

        // 일정 시간 동안 이동 패킷이 없으면 idle로 전환 (단, 죽은 플레이어는 skip)
        if (timeSinceMove >= IDLE_TRANSITION_TIME)
        {
            bool bDead = (m_setDeadRemotePlayers.find(playerId) != m_setDeadRemotePlayers.end());
            auto playerIt = m_mapRemotePlayers.find(playerId);
            bool bActionLocked = (m_mapRemotePlayerActionLockTimer.find(playerId) != m_mapRemotePlayerActionLockTimer.end());
            bool bPortalIntroPlaying = (m_mapRemotePlayerPortalIntroFlyEffects.find(playerId) != m_mapRemotePlayerPortalIntroFlyEffects.end());

            if (!bDead && !bActionLocked && !bPortalIntroPlaying && playerIt != m_mapRemotePlayers.end())
            {
                AnimationComponent* pAnim = playerIt->second->GetComponent<AnimationComponent>();
                if (pAnim)
                {
                    pAnim->CrossFade("Idle", 0.2f, true);
                }
            }

            // 처리 완료 후 맵에서 제거
            it = m_mapRemotePlayerMoveTime.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void NetworkManager::UpdateRemotePlayerInterpolation(float deltaTime)
{
    constexpr float kRemotePlayerSmoothRate = 18.0f;
    constexpr float kRemotePlayerSnapDistSq = 100.0f;   // 10m 이상 벌어지면 스냅
    constexpr float kRemotePlayerTinyDistSq = 0.0025f;  // 거의 도착하면 스냅

    float alpha = 1.0f - expf(-kRemotePlayerSmoothRate * deltaTime);

    for (auto& kv : m_mapRemotePlayerMoveTargets)
    {
        uint64 playerId = kv.first;
        RemotePlayerMoveTarget& target = kv.second;

        if (!target.hasTarget)
            continue;

        auto playerIt = m_mapRemotePlayers.find(playerId);
        if (playerIt == m_mapRemotePlayers.end() || !playerIt->second)
            continue;

        GameObject* pRemotePlayer = playerIt->second;
        TransformComponent* pTransform = pRemotePlayer->GetTransform();

        if (!pTransform)
            continue;

        // 포탈 낙하 연출 중에는 기존 IntroFly 로직이 위치를 직접 제어한다.
        if (m_mapRemotePlayerPortalIntroFlyEffects.find(playerId) != m_mapRemotePlayerPortalIntroFlyEffects.end())
            continue;

        XMFLOAT3 cur = pTransform->GetPosition();

        float dx = target.targetPos.x - cur.x;
        float dy = target.targetPos.y - cur.y;
        float dz = target.targetPos.z - cur.z;

        float distSq = dx * dx + dy * dy + dz * dz;

        if (distSq >= kRemotePlayerSnapDistSq || distSq <= kRemotePlayerTinyDistSq)
        {
            pTransform->SetPosition(target.targetPos);
        }
        else
        {
            XMFLOAT3 next;
            next.x = cur.x + dx * alpha;
            next.y = cur.y + dy * alpha;
            next.z = cur.z + dz * alpha;

            pTransform->SetPosition(next);
        }

        XMFLOAT3 rot = pTransform->GetRotation();
        pTransform->SetRotation(rot.x, target.targetYaw, rot.z);
    }
}

void NetworkManager::TickPendingMeteorShowers(FluidSkillVFXManager* pVFXManager, float deltaTime)
{
    if (!pVFXManager) return;

    // MeteorBehavior 상수와 동일 (시각 일치).
    constexpr int   SHOWER_COUNT = 6;
    constexpr float SHOWER_INTERVAL = 0.5f;
    constexpr float SMALL_SPAWN_HEIGHT = 40.0f;
    constexpr float SMALL_FALL_SPEED = 28.0f;
    constexpr float FINAL_SPAWN_HEIGHT = 60.0f;
    constexpr float FINAL_FALL_SPEED = 22.0f;
    constexpr float FINAL_DELAY = 0.5f;
    const XMFLOAT3 upDir = XMFLOAT3(0.0f, 1.0f, 0.0f);

    for (auto it = m_vPendingMeteorShowers.begin(); it != m_vPendingMeteorShowers.end(); )
    {
        PendingMeteorShower& sh = *it;
        sh.elapsed += deltaTime;

        // 소형 메테오 처리
        for (auto& sm : sh.smallMeteors)
        {
            if (sm.impacted) continue;

            // spawn 시점 도달 시 trail spawn
            if (!sm.spawned && sh.elapsed >= sm.delayUntilSpawn)
            {
                EffectDef trailDef = EffectRegistry::Get().GetEffect("R_MeteorSmallTrail", RUNE_NONE);
                if (!trailDef.layers.empty())
                {
                    sm.trailVfxId = pVFXManager->SpawnEffectLayer(
                        sm.spawnPos, upDir, trailDef.name, trailDef.layers[0], true);
                }
                sm.spawned = true;
                sm.fallElapsed = 0.0f;
                sm.fallDuration = SMALL_SPAWN_HEIGHT / SMALL_FALL_SPEED;
            }

            // trail 낙하 추적
            if (sm.spawned)
            {
                sm.fallElapsed += deltaTime;
                if (sm.trailVfxId >= 0)
                {
                    float curY = sm.spawnPos.y - SMALL_FALL_SPEED * sm.fallElapsed;
                    XMFLOAT3 curPos = XMFLOAT3(sm.scatterPos.x, curY, sm.scatterPos.z);
                    pVFXManager->TrackEffect(sm.trailVfxId, curPos, upDir);
                }
                if (sm.fallElapsed >= sm.fallDuration)
                {
                    if (sm.trailVfxId >= 0)
                    {
                        pVFXManager->StopEffect(sm.trailVfxId);
                        sm.trailVfxId = -1;
                    }
                    EffectDef impDef = EffectRegistry::Get().GetEffect("R_MeteorSmallImpact", RUNE_NONE);
                    pVFXManager->SpawnEffectDef(sm.scatterPos, upDir, impDef, true);
                    sm.impacted = true;
                }
            }
        }

        // 최종 대형 메테오 — 마지막 소형 스폰 시각(=(N-1)*0.5) + FINAL_DELAY 경과 시
        if (!sh.finalSpawned)
        {
            float lastSmallTime = (SHOWER_COUNT - 1) * SHOWER_INTERVAL;
            if (sh.elapsed >= lastSmallTime + FINAL_DELAY)
            {
                sh.finalSpawnPos = XMFLOAT3(sh.targetPos.x, sh.targetPos.y + FINAL_SPAWN_HEIGHT, sh.targetPos.z);
                EffectDef trailDef = EffectRegistry::Get().GetEffect("R_MeteorTrail", RUNE_NONE);
                if (!trailDef.layers.empty())
                    sh.finalTrailId = pVFXManager->SpawnEffectLayer(
                        sh.finalSpawnPos, upDir, trailDef.name, trailDef.layers[0], true);
                EffectDef outerDef = EffectRegistry::Get().GetEffect("R_MeteorTrailOuter", RUNE_NONE);
                if (!outerDef.layers.empty())
                    sh.finalOuterId = pVFXManager->SpawnEffectLayer(
                        sh.finalSpawnPos, upDir, outerDef.name, outerDef.layers[0], true);
                sh.finalFallElapsed = 0.0f;
                sh.finalFallDuration = FINAL_SPAWN_HEIGHT / FINAL_FALL_SPEED;
                sh.finalSpawned = true;
            }
        }

        // 최종 메테오 낙하 추적 + 착지
        if (sh.finalSpawned && !sh.finalImpacted)
        {
            sh.finalFallElapsed += deltaTime;
            float curY = sh.finalSpawnPos.y - FINAL_FALL_SPEED * sh.finalFallElapsed;
            XMFLOAT3 curPos = XMFLOAT3(sh.targetPos.x, curY, sh.targetPos.z);
            if (sh.finalTrailId >= 0) pVFXManager->TrackEffect(sh.finalTrailId, curPos, upDir);
            if (sh.finalOuterId >= 0) pVFXManager->TrackEffect(sh.finalOuterId, curPos, upDir);

            if (sh.finalFallElapsed >= sh.finalFallDuration)
            {
                if (sh.finalTrailId >= 0) { pVFXManager->StopEffect(sh.finalTrailId); sh.finalTrailId = -1; }
                if (sh.finalOuterId >= 0) { pVFXManager->StopEffect(sh.finalOuterId); sh.finalOuterId = -1; }
                EffectDef impDef = EffectRegistry::Get().GetEffect("R_MeteorImpact", RUNE_NONE);
                EffectDef fireDef = EffectRegistry::Get().GetEffect("R_MeteorGroundFire", RUNE_NONE);
                pVFXManager->SpawnEffectDef(sh.targetPos, upDir, impDef, true);
                pVFXManager->SpawnEffectDef(sh.targetPos, upDir, fireDef, true);
                sh.finalImpacted = true;
                sh.postImpactKeepalive = 1.0f;  // impact VFX 자체 lifetime 잠시 더 보호
            }
        }

        // shower 종료 판정
        bool allSmallDone = true;
        for (auto& sm : sh.smallMeteors)
            if (!sm.impacted) { allSmallDone = false; break; }

        if (allSmallDone && sh.finalImpacted)
        {
            sh.postImpactKeepalive -= deltaTime;
            if (sh.postImpactKeepalive <= 0.0f)
            {
                it = m_vPendingMeteorShowers.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void NetworkManager::CheckRemotePlayerVFXTimeout(Scene* pScene, float deltaTime)
{
    FluidSkillVFXManager* pVFXManager = pScene ? pScene->GetFluidVFXManager() : nullptr;
    if (!pVFXManager)
        return;

    // 고정 lifetime 큐 처리 (Water Q 등) — 채널 추적과 무관, remaining 카운트다운만.
    for (auto it = m_vTimedVFXKills.begin(); it != m_vTimedVFXKills.end(); )
    {
        it->remaining -= deltaTime;
        if (it->remaining <= 0.0f)
        {
            if (it->vfxId >= 0) pVFXManager->StopEffect(it->vfxId);
            it = m_vTimedVFXKills.erase(it);
        }
        else ++it;
    }

    // Meteor 샤워 시뮬레이션 (Fire R)
    TickPendingMeteorShowers(pVFXManager, deltaTime);

    for (auto it = m_mapRemotePlayerVFX.begin(); it != m_mapRemotePlayerVFX.end(); )
    {
        RemoteVFXState& state = it->second;
        state.lastUpdateTime += deltaTime;
        state.totalElapsed += deltaTime;

        // 종료 조건 1: maxLifetime > 0 이면 누적 시간이 그 값을 넘으면 종료 (Water Vortex 4s 등)
        // 종료 조건 2: maxLifetime == 0 인 채널 VFX 는 패킷이 maxIdleTime 동안 안 오면 종료 (FireBeam 등)
        bool hardExpire = (state.maxLifetime > 0.0f && state.totalElapsed >= state.maxLifetime);
        bool idleExpire = (state.maxLifetime <= 0.0f && state.lastUpdateTime >= state.maxIdleTime);

        if (hardExpire || idleExpire)
        {
            if (state.vfxId >= 0)
            {
                pVFXManager->StopEffect(state.vfxId);
            }
            it = m_mapRemotePlayerVFX.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void NetworkManager::ProcessSkill(Scene* pScene, uint64 playerId, int skillType, float x, float y, float z, float dirX, float dirY, float dirZ,
    int32 serverSkillSlot, float serverRadiusMult, float serverDamageMult)
{
    // 결산: 원격 플레이어 스킬 사용 카운트 (로컬은 SendSkill 측에서 처리)
    if (playerId != m_nLocalPlayerId.load())
        StatOnSkillUse(playerId, skillType);

    // 로컬 플레이어라면 무시 (로컬은 자체 처리)
    if (playerId == m_nLocalPlayerId.load())
        return;

    auto it = m_mapRemotePlayers.find(playerId);
    if (it == m_mapRemotePlayers.end())
        return;

    // 채널 중인 원격 플레이어 — 5Hz tick 전부 VFX spawn 하면 부하 누적 → 0.33초(3Hz) 로 throttle.
    // 단 E 스킬(skillType=2)은 본래 채널 빔(FireBeam/WaterVortex)이라 TrackEffect 로만 갱신되므로
    //   throttle 하면 state.lastUpdateTime reset 이 안 돼 maxIdleTime(0.2초) 초과로 빔이 끊긴다. 제외.
    if (skillType != 2 && m_setRemoteChannelingPlayers.find(playerId) != m_setRemoteChannelingPlayers.end())
    {
        auto lastIt = m_mapRemoteChannelLastSpawnTime.find(playerId);
        if (lastIt != m_mapRemoteChannelLastSpawnTime.end())
        {
            if (m_fNetworkAccumulatedTime - lastIt->second < 0.33f)
                return; // 같은 플레이어 채널 spawn 너무 잦음 — skip
        }
        m_mapRemoteChannelLastSpawnTime[playerId] = m_fNetworkAccumulatedTime;
    }

    GameObject* pRemotePlayer = it->second;
    TransformComponent* pTransform = pRemotePlayer->GetTransform();
    if (pTransform)
    {
        // 위치 설정
        pTransform->SetPosition(x, y, z);

        // 방향 벡터로 Y축 회전 계산 (XZ 평면 기준)
        float length = sqrtf(dirX * dirX + dirZ * dirZ);
        if (length > 0.001f)
        {
            float yaw = atan2f(dirX, dirZ);
            float yawDegrees = XMConvertToDegrees(yaw);
            XMFLOAT3 currentRot = pTransform->GetRotation();
            pTransform->SetRotation(currentRot.x, yawDegrees, currentRot.z);
        }
    }

    // 스킬 애니메이션 재생 (현재 플레이어 모델은 Attack1만 지원)
    AnimationComponent* pAnim = pRemotePlayer->GetComponent<AnimationComponent>();
    // 채널 룬 활성 중에는 매 tick 마다 Attack1 을 재시작하면 0.2초마다 끊겨 보이고 렉처럼 느껴진다.
    // CHANNEL_BEGIN/END 가 별도 채널 anim 을 깔아두므로 ProcessSkill 단계에서는 anim 갱신 skip.
    bool bChanneling = (m_setRemoteChannelingPlayers.find(playerId) != m_setRemoteChannelingPlayers.end());
    if (pAnim && !bChanneling)
    {
        // 스킬 애니메이션은 한 번만 재생 (루프 X), forceRestart=true로 연속 공격 시에도 재시작
        pAnim->CrossFade("Attack1", 0.1f, false, true);
    }

    // caster 위치(서버가 전달) — y 는 발 위치. 오프라인의 "캐릭터 머리 높이" 는 +5.0f.
    const XMFLOAT3 casterPos = XMFLOAT3(x, y, z);
    const XMFLOAT3 casterHead = XMFLOAT3(x, y + 5.0f, z);

    // wire dir 슬롯: Q/E/RC 는 lookDir(정규화), R 은 절대 target 좌표 (SkillComponent.cpp).
    // 일단 lookDir 해석부터 (R 처리에서는 별도로 target 사용).
    XMFLOAT3 lookDir = XMFLOAT3(dirX, dirY, dirZ);
    {
        float L = sqrtf(lookDir.x * lookDir.x + lookDir.y * lookDir.y + lookDir.z * lookDir.z);
        if (L > 0.001f) { lookDir.x /= L; lookDir.y /= L; lookDir.z /= L; }
        else { lookDir = XMFLOAT3(0.0f, 0.0f, 1.0f); }
    }
    XMFLOAT3 horizontalDir = XMFLOAT3(lookDir.x, 0.0f, lookDir.z);
    {
        float L = sqrtf(horizontalDir.x * horizontalDir.x + horizontalDir.z * horizontalDir.z);
        if (L > 0.001f) { horizontalDir.x /= L; horizontalDir.z /= L; }
        else { horizontalDir = XMFLOAT3(0.0f, 0.0f, 1.0f); }
    }

    FluidSkillVFXManager* pVFXManager = pScene ? pScene->GetFluidVFXManager() : nullptr;
    ProjectileManager* pProjManager = pScene ? pScene->GetProjectileManager() : nullptr;

    // 원격 플레이어 element
    ElementType remoteElement = ElementType::Water;
    {
        auto eIt = m_mapRemotePlayerElement.find(playerId);
        if (eIt != m_mapRemotePlayerElement.end())
            remoteElement = eIt->second;
    }

    // ── 원격 플레이어가 장착한 룬으로부터 시각 modifier 산출 ──────────────────
    //   ProcessRuneEquip 가 S_RUNE_EQUIP 수신 시 원격 SkillComponent 에 룬을 등록해두므로,
    //   그 컴포넌트의 RuneCombo 를 그대로 EffectRegistry::GetEffect 에 넘기면
    //   Charge/Channel/Enhance/Split/Place 활성화 룬의 RegisterRuneMod 가 자동 적용된다.
    //   (예: WaveSlash + Enhance → 파티클 강화 mod, FireBeam + Channel → 채널 mod 등)
    uint32_t remoteRuneFlags = RUNE_NONE;
    // 속성 변환 룬 (FIR_1/WAT_1/WND_1/ERT_1 등) — elementSet 첫 원소가 캐릭터 기본 원소와 다르면
    // 그 색상으로 effect 를 재칠한다. (오프라인의 Behavior 별 ApplyElementToEffectDef 와 동일)
    ElementType visualElementOverride = ElementType::None;
    {
        SkillSlot remoteSlot = SkillSlot::Count;
        switch (skillType)
        {
        case 1: remoteSlot = SkillSlot::Q;          break;
        case 2: remoteSlot = SkillSlot::E;          break;
        case 3: remoteSlot = SkillSlot::R;          break;
        case 4: remoteSlot = SkillSlot::RightClick; break;
        default: break;
        }
        if (remoteSlot != SkillSlot::Count)
        {
            if (SkillComponent* pRemoteSkill = pRemotePlayer->GetComponent<SkillComponent>())
            {
                RuneCombo combo = pRemoteSkill->GetRuneCombo(remoteSlot);
                remoteRuneFlags = ToRuneFlags(combo);

                ActivationType defType = ActivationType::Instant;
                if (auto* sk = pRemoteSkill->GetSkill(remoteSlot))
                    defType = sk->GetSkillData().activationType;
                SkillStats stats = pRemoteSkill->BuildSkillStats(remoteSlot, defType);
                // L04 randomElementOnCast 는 매 호출 RNG → 서버/클라 desync 위험이라 elementSet/elementOverride 만 사용.
                // elementSet 은 장착된 원소 룬으로 결정적이라 안전.
                // 비교 가드 제거: cached remoteElement 가 잘못된 값(맵 미등록 등) 일 때도 override 가 무효화되는 문제 방지.
                //   같은 원소면 ApplyElementToEffectDef 가 효과 동일 → 안전.
                if (!stats.elementSet.empty())
                    visualElementOverride = stats.elementSet[0];
                else if (stats.elementOverride.has_value() && !stats.randomElementOnCast)
                    visualElementOverride = *stats.elementOverride;

                // 어떤 룬이 실제 등록돼 있는지 직접 dump
                {
                    char rbuf[400];
                    sprintf_s(rbuf,
                        "[RuneDiag] ProcessSkill playerId=%llu skillType=%d cachedElem=%d setSize=%zu set0=%d override=%d randEl=%d r0=%s r1=%s r2=%s",
                        playerId, skillType, (int)remoteElement,
                        stats.elementSet.size(),
                        stats.elementSet.empty() ? -1 : (int)stats.elementSet[0],
                        (int)visualElementOverride,
                        stats.randomElementOnCast ? 1 : 0,
                        pRemoteSkill->GetRuneSlot(remoteSlot, 0).runeId.c_str(),
                        pRemoteSkill->GetRuneSlot(remoteSlot, 1).runeId.c_str(),
                        pRemoteSkill->GetRuneSlot(remoteSlot, 2).runeId.c_str());
                    WriteNetworkLog(rbuf);
                }
            }
        }
    }

    // 서버 권위 radiusMult — AMP_RAD3 같은 범위 룬은 서버가 계산해 보내준 값을 그대로 적용.
    // 1.0 (=무지정/기본) 이면 추가 스케일 없음.
    const float vfxRadiusScale = (serverRadiusMult > 0.0f) ? serverRadiusMult : 1.0f;
    auto spawnOneShot = [&](const char* effectName, const XMFLOAT3& origin, const XMFLOAT3& dir) -> int
        {
            if (!pVFXManager) return -1;
            EffectDef def = EffectRegistry::Get().GetEffect(effectName, remoteRuneFlags);
            if (visualElementOverride != ElementType::None)
                ApplyElementToEffectDef(def, visualElementOverride);
            if (vfxRadiusScale != 1.0f)
            {
                for (auto& l : def.layers)
                    l.sizeScale *= vfxRadiusScale;
            }
            return pVFXManager->SpawnEffectDef(origin, dir, def, true);
        };

    // Q/E 의 target 기반 스킬 (StoneSpike Q 등) 은 wire 에 lookDir 만 와서
    // lookDir 방향 ~8m 앞을 proxy target 으로 사용 (Earth Q 만 해당).
    // Water Q/E (WaterPuddle/Vortex) 는 송신 측이 target 을 dir 슬롯에 실어 보냄.
    auto proxyTargetAhead = [&](float dist) -> XMFLOAT3
        {
            return XMFLOAT3(casterPos.x + horizontalDir.x * dist, casterPos.y, casterPos.z + horizontalDir.z * dist);
        };

    // Water Q/E 와 R 류는 dir 슬롯이 정규화 방향이 아니라 absolute target 좌표.
    bool dirSlotIsTarget =
        (skillType == 3) ||
        (remoteElement == ElementType::Water && (skillType == 1 || skillType == 2));
    XMFLOAT3 wireTarget = XMFLOAT3(dirX, dirY, dirZ);  // dirSlotIsTarget == true 일 때만 유효

    switch (skillType)
    {
    case 1:  // Q
    {
        switch (remoteElement)
        {
        case ElementType::Fire:  // WaveSlash: caster+5, target-caster dir (≈ horizontalDir)
            spawnOneShot("Q_WaveSlash", casterHead, horizontalDir);
            break;
        case ElementType::Water: // WaterPuddle: target 기준 낙하 + 웅덩이 (송신 측이 dir 슬롯에 target 실어 보냄)
        {
            XMFLOAT3 tgt = wireTarget;
            XMFLOAT3 fallPos = XMFLOAT3(tgt.x, tgt.y + 5.5f, tgt.z);
            XMFLOAT3 puddlePos = XMFLOAT3(tgt.x, tgt.y + 2.5f, tgt.z);
            // 두 effect 모두 EffectDef.duration = -1 (offline 에선 behavior 가 수동 종료). 원격은 동일 시각에 자동 종료 필요.
            // FALL_DURATION=1.5s, DURATION=6.0s (WaterPuddleBehavior.h).
            int fallId = spawnOneShot("Q_WaterFall", fallPos, XMFLOAT3(0.0f, -1.0f, 0.0f));
            int puddId = spawnOneShot("Q_WaterPuddle", puddlePos, XMFLOAT3(0.0f, -1.0f, 0.0f));
            if (fallId >= 0) m_vTimedVFXKills.push_back({ fallId, 1.5f });
            if (puddId >= 0) m_vTimedVFXKills.push_back({ puddId, 6.0f });
            break;
        }
        case ElementType::Wind:  // WindCutter: caster+5, horizontalDir
            spawnOneShot("Q_WindCutter", casterHead, horizontalDir);
            break;
        case ElementType::Earth: // StoneSpike: caster→앞쪽으로 SPIKE_COUNT 개 일정 간격 (오프라인은 시간차)
        {
            const int   SPIKE_COUNT = 4;
            const float SPIKE_SPACING = 2.5f;
            for (int i = 0; i < SPIKE_COUNT; ++i)
            {
                float dist = SPIKE_SPACING * (i + 1);
                XMFLOAT3 spikePos = XMFLOAT3(
                    casterPos.x + horizontalDir.x * dist, casterPos.y,
                    casterPos.z + horizontalDir.z * dist);
                spawnOneShot("Q_StoneSpike", spikePos, XMFLOAT3(0.0f, 1.0f, 0.0f));
            }
            break;
        }
        default: break;
        }
        break;
    }

    case 2:  // E
    {
        // 채널링형 (Fire/Water) — TrackEffect 로 매 패킷마다 위치/방향 갱신
        bool isChannel = (remoteElement == ElementType::Fire || remoteElement == ElementType::Water);

        if (isChannel)
        {
            // Fire FireBeam: caster head + forward * 1.3
            // Water Vortex: target + y3.0 (proxy target)
            XMFLOAT3 chanOrigin;
            XMFLOAT3 chanDir;
            const char* effectName;
            if (remoteElement == ElementType::Fire)
            {
                chanOrigin = XMFLOAT3(casterHead.x + lookDir.x * 1.3f,
                    casterHead.y + lookDir.y * 1.3f,
                    casterHead.z + lookDir.z * 1.3f);
                chanDir = lookDir;
                effectName = "E_FireBeam_Core";
            }
            else // Water
            {
                // 송신 측이 dir 슬롯에 target 실어 보냄.
                XMFLOAT3 tgt = wireTarget;
                chanOrigin = XMFLOAT3(tgt.x, tgt.y + 3.0f, tgt.z);
                chanDir = XMFLOAT3(0.0f, 1.0f, 0.0f);
                effectName = "E_WaterVortex";
            }

            auto vfxIt = m_mapRemotePlayerVFX.find(playerId);
            bool hasExistingVFX = (vfxIt != m_mapRemotePlayerVFX.end() && vfxIt->second.vfxId >= 0);

            if (hasExistingVFX && vfxIt->second.skillType == skillType)
            {
                pVFXManager->TrackEffect(vfxIt->second.vfxId, chanOrigin, chanDir);
                vfxIt->second.lastUpdateTime = 0.0f;
            }
            else
            {
                if (hasExistingVFX)
                    pVFXManager->StopEffect(vfxIt->second.vfxId);

                int vfxId = spawnOneShot(effectName, chanOrigin, chanDir);
                // Fire 는 추가 layer Swirl/Burst 도 같이 spawn
                if (remoteElement == ElementType::Fire)
                {
                    spawnOneShot("E_FireBeam_Swirl", chanOrigin, chanDir);
                    spawnOneShot("E_FireBeam_Burst", chanOrigin, chanDir);
                }

                RemoteVFXState state;
                state.vfxId = vfxId;
                state.skillType = skillType;
                state.lastUpdateTime = 0.0f;
                if (remoteElement == ElementType::Water)
                {
                    // WaterVortex 는 일반적으로 Instant 활성화 — 패킷이 한 번만 옴. 로컬 DURATION 4s 와 맞춤.
                    state.maxLifetime = 4.0f;
                    state.maxIdleTime = 10.0f;  // 패킷 끊겨도 4초까지는 살아있도록 idle 한계를 길게.
                }
                m_mapRemotePlayerVFX[playerId] = state;
            }
        }
        else if (remoteElement == ElementType::Wind)
        {
            // GaleRush: 출발 burst(caster + y2.0), ring(caster 지면, y=0), trail(caster + y2.0, -forward)
            XMFLOAT3 burstPos = XMFLOAT3(casterPos.x, casterPos.y + 2.0f, casterPos.z);
            XMFLOAT3 ringPos = XMFLOAT3(casterPos.x, casterPos.y, casterPos.z);
            XMFLOAT3 backDir = XMFLOAT3(-horizontalDir.x, 0.0f, -horizontalDir.z);
            spawnOneShot("E_GaleRush_Burst", burstPos, horizontalDir);
            spawnOneShot("E_GaleRush_Ring", ringPos, horizontalDir);
            spawnOneShot("E_GaleRush_Trail", burstPos, backDir);
        }
        else // Earth EarthArmor
        {
            // caster 위치(지면), up
            spawnOneShot("E_EarthArmor_Burst", casterPos, XMFLOAT3(0.0f, 1.0f, 0.0f));
            spawnOneShot("E_EarthArmor_Aura", casterPos, XMFLOAT3(0.0f, 1.0f, 0.0f));
        }
        break;
    }

    case 3:  // R — wire dir 슬롯 = 절대 target 좌표 (정규화 아님)
    {
        XMFLOAT3 targetPos = XMFLOAT3(dirX, dirY, dirZ);

        // caster → target 평면 방향
        XMFLOAT3 toTarget = XMFLOAT3(targetPos.x - casterPos.x, 0.0f, targetPos.z - casterPos.z);
        {
            float L = sqrtf(toTarget.x * toTarget.x + toTarget.z * toTarget.z);
            if (L > 0.001f) { toTarget.x /= L; toTarget.z /= L; }
            else { toTarget = XMFLOAT3(0.0f, 0.0f, 1.0f); }
        }

        switch (remoteElement)
        {
        case ElementType::Fire:
        {
            // Meteor 샤워 — 즉시 spawn 이 아니라 시간차 시뮬레이션 큐에 등록.
            // (MeteorBehavior::Update 와 동일 흐름: 6개 소형 0.5s 간격 → 마지막 후 0.5s → 최종 대형)
            constexpr int   SHOWER_COUNT = 6;
            constexpr float SHOWER_INTERVAL = 0.5f;
            constexpr float SMALL_SPAWN_HEIGHT = 40.0f;
            constexpr float SMALL_SCATTER_RADIUS = 12.0f;

            PendingMeteorShower sh;
            sh.targetPos = targetPos;
            sh.smallMeteors.reserve(SHOWER_COUNT);

            // 결정적 난수 — playerId 와 packet 의 target 좌표를 시드로 (같은 packet 에 대해 동일 패턴, 캐릭터 별 다름).
            uint32_t tx, tz;
            std::memcpy(&tx, &targetPos.x, sizeof(uint32_t));
            std::memcpy(&tz, &targetPos.z, sizeof(uint32_t));
            uint32_t seed = static_cast<uint32_t>(playerId) ^ tx ^ tz;
            std::mt19937 rng(seed);
            std::uniform_real_distribution<float> angleDist(0.0f, 6.2831853f);
            std::uniform_real_distribution<float> radiusDist(0.0f, SMALL_SCATTER_RADIUS);

            for (int i = 0; i < SHOWER_COUNT; ++i)
            {
                PendingSmallMeteor sm;
                float angle = angleDist(rng);
                float radius = radiusDist(rng);
                sm.scatterPos = XMFLOAT3(
                    targetPos.x + radius * cosf(angle),
                    targetPos.y,
                    targetPos.z + radius * sinf(angle));
                sm.spawnPos = XMFLOAT3(sm.scatterPos.x, sm.scatterPos.y + SMALL_SPAWN_HEIGHT, sm.scatterPos.z);
                sm.delayUntilSpawn = i * SHOWER_INTERVAL;
                sh.smallMeteors.push_back(sm);
            }
            m_vPendingMeteorShowers.push_back(std::move(sh));
            break;
        }
        case ElementType::Water:
        {
            // TidalWave: caster head 에서 target 방향으로 파동 진행
            spawnOneShot("R_TidalWave", casterHead, toTarget);
            spawnOneShot("R_TidalWave_Foam", casterHead, toTarget);
            break;
        }
        case ElementType::Wind:
        {
            // Tornado 는 Instant 활성화 (TornadoBehavior::DURATION=6.0). 송신 측이 후속 패킷 안 보냄.
            // 첫 패킷에 spawn 하고 6초 lifetime 부여. 이후 같은 player 가 R 보내면 TrackEffect 로 위치 갱신.
            XMFLOAT3 tornadoPos = XMFLOAT3(targetPos.x, 0.0f, targetPos.z);
            XMFLOAT3 upDir = XMFLOAT3(0.0f, 1.0f, 0.0f);

            auto vfxIt = m_mapRemotePlayerVFX.find(playerId);
            bool hasExistingVFX = (vfxIt != m_mapRemotePlayerVFX.end() && vfxIt->second.vfxId >= 0);

            if (hasExistingVFX && vfxIt->second.skillType == skillType)
            {
                pVFXManager->TrackEffect(vfxIt->second.vfxId, tornadoPos, upDir);
                vfxIt->second.lastUpdateTime = 0.0f;
            }
            else
            {
                if (hasExistingVFX)
                    pVFXManager->StopEffect(vfxIt->second.vfxId);

                int vfxId = spawnOneShot("R_TornadoPlayer", tornadoPos, upDir);
                RemoteVFXState state;
                state.vfxId = vfxId;
                state.skillType = skillType;
                state.lastUpdateTime = 0.0f;
                state.maxLifetime = 6.0f;      // TornadoBehavior::DURATION
                state.maxIdleTime = 10.0f;     // 패킷 끊겨도 lifetime 까지는 살아있게.
                m_mapRemotePlayerVFX[playerId] = state;
            }
            break;
        }
        case ElementType::Earth:
        {
            // Earthquake: **caster 위치(epicenter)** — target 좌표 사용하지 않음.
            spawnOneShot("R_Earthquake_Burst", casterPos, XMFLOAT3(0.0f, 1.0f, 0.0f));
            spawnOneShot("R_Earthquake_Ring", casterPos, XMFLOAT3(0.0f, 1.0f, 0.0f));
            break;
        }
        default: break;
        }
        break;
    }

    case 4:  // RC — 투사체, origin = caster head + lookDir 방향 50m
    {
        if (!pProjManager) break;

        float speed = 30.0f, radius = 0.5f, explosionRadius = 3.0f, scale = 1.0f;

        switch (remoteElement)
        {
        case ElementType::Fire:  speed = 30.0f; radius = 0.50f; explosionRadius = 3.0f; scale = 1.00f; break;
        case ElementType::Water: speed = 25.0f; radius = 0.60f; explosionRadius = 3.0f; scale = 0.80f; break;
        case ElementType::Wind:  speed = 28.0f; radius = 0.35f; explosionRadius = 3.0f; scale = 0.70f; break;
        case ElementType::Earth: speed = 32.0f; radius = 0.70f; explosionRadius = 3.0f; scale = 0.90f; break;
        default: break;
        }

        XMFLOAT3 projOrigin = casterHead;
        XMFLOAT3 projTarget = XMFLOAT3(
            projOrigin.x + horizontalDir.x * 50.0f,
            projOrigin.y,
            projOrigin.z + horizontalDir.z * 50.0f);

        // 원소 변경 룬(FIR_1/WAT_1/...) 시: visualElementOverride 가 cached remoteElement 를 대체.
        //   SpawnProjectile 은 element 인자로 색을 결정하므로, 여기서 변환된 element 를 사용해야
        //   다른 클라 화면에 색이 정상 반영된다.
        ElementType projElement = (visualElementOverride != ElementType::None)
            ? visualElementOverride : remoteElement;

        // Wind RC (WindShot) 만 관통 — WindShotBehavior.cpp:53 과 일치.
        // 나머지 (Fire/Water/Earth RC) 는 첫 충돌에 폭발 (오프라인과 동일).
        // 관통 판정도 변환된 element 기준으로 (원소 변환 시 isPiercing 도 따라감)
        bool isPiercing = (projElement == ElementType::Wind);

        // 룬 분기 (HasRuneEquipped 로 직접 체크 — RuneCombo 에 해당 필드가 없음)
        SkillComponent* pRemoteSkill = pRemotePlayer->GetComponent<SkillComponent>();
        bool bOrb = pRemoteSkill && pRemoteSkill->HasRuneEquipped(SkillSlot::RightClick, "TRF_ORB");
        bool bMlt = pRemoteSkill && pRemoteSkill->HasRuneEquipped(SkillSlot::RightClick, "TRF_MLT");
        const bool bSplit = (remoteRuneFlags & RUNE_SPLIT) != 0;

        // 부채꼴 N개 타겟 생성 (가운데 + 좌/우 ±15도)
        auto buildFanTargets = [&](int count) -> std::vector<XMFLOAT3>
            {
                std::vector<XMFLOAT3> out;
                if (count <= 1) { out.push_back(projTarget); return out; }
                constexpr float DEG = 15.0f;
                for (int i = 0; i < count; ++i)
                {
                    float t = (count == 1) ? 0.f : (float(i) - float(count - 1) * 0.5f); // -..0..+
                    float angleDeg = t * DEG / std::max(1.f, float(count - 1) * 0.5f);
                    float rad = DirectX::XMConvertToRadians(angleDeg);
                    float cs = cosf(rad), sn = sinf(rad);
                    XMFLOAT3 d(horizontalDir.x * cs - horizontalDir.z * sn, 0.f,
                        horizontalDir.x * sn + horizontalDir.z * cs);
                    out.push_back(XMFLOAT3(projOrigin.x + d.x * 50.0f, projOrigin.y, projOrigin.z + d.z * 50.0f));
                }
                return out;
            };

        // 궤도(TRF_ORB): 즉시 spawn 대신 0.5초 공전 visual 후 deferred. ORB 가 가장 강력하므로 우선 적용.
        if (bOrb)
        {
            int orbVfxId = -1;
            if (pVFXManager && EffectRegistry::Get().HasEffect("sub_orbital_halo"))
            {
                EffectDef def = EffectRegistry::Get().GetEffect("sub_orbital_halo");
                ApplyElementToEffectDef(def, projElement);
                orbVfxId = pVFXManager->SpawnEffectDef(projOrigin, XMFLOAT3(0, 1, 0), def, false);
            }

            int count = bMlt ? 3 : 1;
            auto tgts = buildFanTargets(count);
            for (auto& t : tgts)
            {
                PendingOrbitalProjectile p;
                p.origin = projOrigin;
                p.target = t;
                p.speed = speed; p.radius = radius; p.explosionRadius = explosionRadius;
                p.scale = scale; p.element = projElement; p.owner = pRemotePlayer;
                p.isPiercing = isPiercing;
                p.orbVfxId = (&t == &tgts.front()) ? orbVfxId : -1;
                p.delay = 0.5f;
                m_vPendingOrbitals.push_back(p);
            }
            break;
        }

        if (bMlt)
        {
            auto tgts = buildFanTargets(3);
            for (auto& t : tgts)
            {
                pProjManager->SpawnProjectile(
                    projOrigin, t, 0.0f, speed, radius, explosionRadius,
                    projElement, pRemotePlayer, true, scale,
                    RuneCombo{}, 0.0f, 100.0f, isPiercing);
            }
            break;
        }

        if (bSplit)
        {
            XMFLOAT3 right = XMFLOAT3(-horizontalDir.z, 0.0f, horizontalDir.x);
            constexpr float SPREAD = 1.5f;
            XMFLOAT3 t1(projTarget.x + right.x * SPREAD, projTarget.y, projTarget.z + right.z * SPREAD);
            XMFLOAT3 t2(projTarget.x - right.x * SPREAD, projTarget.y, projTarget.z - right.z * SPREAD);
            pProjManager->SpawnProjectile(
                projOrigin, t1, 0.0f, speed, radius, explosionRadius,
                projElement, pRemotePlayer, true, scale,
                RuneCombo{}, 0.0f, 100.0f, isPiercing);
            pProjManager->SpawnProjectile(
                projOrigin, t2, 0.0f, speed, radius, explosionRadius,
                projElement, pRemotePlayer, true, scale,
                RuneCombo{}, 0.0f, 100.0f, isPiercing);
            break;
        }

        // 일반 — 1발
        pProjManager->SpawnProjectile(
            projOrigin, projTarget,
            0.0f,
            speed, radius, explosionRadius,
            projElement, pRemotePlayer,
            true, scale,
            RuneCombo{}, 0.0f,
            100.0f,
            isPiercing
        );
        break;
    }

    default:
        OutputDebugString(L"[Network] Unknown skill type\n");
        break;
    }

    wchar_t buf[128];
    swprintf_s(buf, L"[Network] ProcessSkill: PlayerId=%llu SkillType=%d\n", playerId, skillType);
    OutputDebugString(buf);
}

void NetworkManager::ProcessPlayerAction(Scene* pScene, uint64 playerId, uint32 actionType, float x, float y, float z, float dirX, float dirY, float dirZ)
{
    // 로컬 플레이어는 이미 자기 입력/Scene 전환 흐름에서 연출을 실행했으므로 중복 재생하지 않는다.
    if (playerId == m_nLocalPlayerId.load())
        return;

    auto it = m_mapRemotePlayers.find(playerId);
    if (it == m_mapRemotePlayers.end())
        return;

    GameObject* pRemotePlayer = it->second;
    if (!pRemotePlayer)
        return;

    AnimationComponent* pAnim = pRemotePlayer->GetComponent<AnimationComponent>();
    TransformComponent* pTransform = pRemotePlayer->GetTransform();

    switch (actionType)
    {
    case PLAYER_ACTION_DASH_CAPE_FLUTTER:
    {
        // 대쉬 망토/몸 플래시 연출
        // 위치는 건드리지 않고, 원격 클라에서 모션과 흰색 플래시만 재생한다.
        if (pAnim)
            pAnim->CrossFade("Run", 0.05f, true, true);

        // 대쉬 연출 중 S_MOVE 가 Walk 로 바로 덮지 않도록 짧게 막는다.
        m_mapRemotePlayerActionLockTimer[playerId] = 0.35f;

        // 로컬 대쉬처럼 몸이 하얗게 보이도록 원격 플레이어에도 HitFlash를 건다.
        pRemotePlayer->SetHitFlashAll(1.0f);

        // 기존 원격 hit flash 페이드 구조를 그대로 재사용한다. 수치는 클라 기존값 0.15f.
        m_mapRemotePlayerHitFlashTimer[playerId] = REMOTE_HIT_FLASH_DURATION;

        WriteNetworkLog("[Network] Remote DashCapeFlutter flash started");
        break;
    }

    case PLAYER_ACTION_PORTAL_INTRO_FLY:
    {
        if (!pTransform)
            break;

        // 원격 플레이어 포탈 Intro Fly 연출
        RemotePlayerPortalIntroFlyEffect fx{};
        fx.introTimer = kPlayerPortalIntroDuration;
        fx.landingHoldTimer = 0.0f;
        fx.velocityY = 0.0f;
        fx.groundY = y;
        fx.groundX = x;
        fx.groundZ = z;
        fx.onGround = false;

        m_mapRemotePlayerPortalIntroFlyEffects[playerId] = fx;

        // Intro Fly 중에는 S_MOVE/idle 이 Levitating/Landing 애니를 덮지 않도록 락을 길게 잡는다.
        m_mapRemotePlayerActionLockTimer[playerId] =
            kPlayerPortalIntroDuration + kPlayerPortalIntroLandingHold;

        pTransform->SetPosition(x, y + kPlayerPortalIntroStartHeight, z);

        if (pAnim)
            pAnim->CrossFade("Levitating", 0.10f, true, true);

        // 등장 연출 중에는 idle 타이머가 끼어들지 않도록 초기화한다.
        m_mapRemotePlayerMoveTime.erase(playerId);

        char buf[224];
        sprintf_s(buf,
            "[Network] Remote PortalIntroFly START: playerId=%llu ground=(%.2f, %.2f, %.2f)",
            playerId, x, y, z);
        WriteNetworkLog(buf);
        break;
    }

    case PLAYER_ACTION_CHARGE_BEGIN:
    {
        // 진단용 — 패킷이 도달했음을 NetworkLog 에서 확인 가능하게 한다.
        {
            char dbg[128];
            sprintf_s(dbg, "[Network] CHARGE_BEGIN remote playerId=%llu pos=(%.1f, %.1f, %.1f)", playerId, x, y, z);
            WriteNetworkLog(dbg);
        }
        FluidSkillVFXManager* pVFX = pScene ? pScene->GetFluidVFXManager() : nullptr;
        if (!pVFX) break;
        auto itPrev = m_mapRemoteChargeVFXId.find(playerId);
        if (itPrev != m_mapRemoteChargeVFXId.end() && itPrev->second >= 0)
            pVFX->StopEffect(itPrev->second);
        if (!EffectRegistry::Get().HasEffect("charge_gather"))
            break;

        ElementType elem = GetPlayerElement(playerId);

        EffectDef def = EffectRegistry::Get().GetEffect("charge_gather");
        // 잘 보이도록 2x 스케일/파티클 (단계 추적 어려워 큰 단계 고정).
        for (auto& l : def.layers)
        {
            l.sizeScale *= 2.0f;
            l.particleCount = static_cast<int>(l.particleCount * 1.5f);
        }
        if (elem != ElementType::None)
            ApplyElementToEffectDef(def, elem);

        // 발 위치 그대로가 아니라 살짝 띄워 가슴 높이로 — 카메라 각도에서 가려지지 않게.
        DirectX::XMFLOAT3 pos(x, y + 1.5f, z);
        DirectX::XMFLOAT3 up(0.f, 1.f, 0.f);
        int vfxId = pVFX->SpawnEffectDef(pos, up, def, false);
        m_mapRemoteChargeVFXId[playerId] = vfxId;

        if (EffectRegistry::Get().HasEffect("charge_pulse"))
        {
            EffectDef pulseDef = EffectRegistry::Get().GetEffect("charge_pulse");
            if (elem != ElementType::None)
                ApplyElementToEffectDef(pulseDef, elem);
            pVFX->SpawnEffectDef(pos, up, pulseDef, true);
        }
        break;
    }

    case PLAYER_ACTION_CHARGE_END:
    {
        FluidSkillVFXManager* pVFX = pScene ? pScene->GetFluidVFXManager() : nullptr;
        if (!pVFX) break;
        auto it = m_mapRemoteChargeVFXId.find(playerId);
        if (it != m_mapRemoteChargeVFXId.end())
        {
            if (it->second >= 0) pVFX->StopEffect(it->second);
            m_mapRemoteChargeVFXId.erase(it);
        }
        break;
    }

    case PLAYER_ACTION_ENHANCE_BEGIN:
    {
        FluidSkillVFXManager* pVFX = pScene ? pScene->GetFluidVFXManager() : nullptr;
        if (!pVFX) break;
        auto it = m_mapRemoteEnhanceVFXId.find(playerId);
        if (it != m_mapRemoteEnhanceVFXId.end() && it->second >= 0)
            pVFX->StopEffect(it->second);
        if (!EffectRegistry::Get().HasEffect("charge_gather"))
            break;
        EffectDef def = EffectRegistry::Get().GetEffect("charge_gather");
        for (auto& l : def.layers)
        {
            l.overrideColors = true;
            l.coreColor = { 1.0f, 0.9f, 0.3f, 1.0f };
            l.edgeColor = { 1.0f, 0.55f, 0.1f, 0.9f };
        }
        DirectX::XMFLOAT3 pos(x, y, z);
        DirectX::XMFLOAT3 up(0.f, 1.f, 0.f);
        int vfxId = pVFX->SpawnEffectDef(pos, up, def, false);
        m_mapRemoteEnhanceVFXId[playerId] = vfxId;
        float duration = dirY > 0.f ? dirY : 5.0f;
        if (vfxId >= 0)
            m_vTimedVFXKills.push_back({ vfxId, duration });
        break;
    }

    case PLAYER_ACTION_ENHANCE_END:
    {
        FluidSkillVFXManager* pVFX = pScene ? pScene->GetFluidVFXManager() : nullptr;
        if (!pVFX) break;
        auto it = m_mapRemoteEnhanceVFXId.find(playerId);
        if (it != m_mapRemoteEnhanceVFXId.end())
        {
            if (it->second >= 0) pVFX->StopEffect(it->second);
            m_mapRemoteEnhanceVFXId.erase(it);
        }
        break;
    }

    case PLAYER_ACTION_PLACE_SPAWN:
    {
        DecalManager* pDecals = pScene ? pScene->GetDecalManager() : nullptr;
        if (!pDecals) break;

        int slotIdx = static_cast<int>(dirX);
        if (slotIdx < 0 || slotIdx >= 4) slotIdx = 0;

        // 같은 슬롯에 이미 설치 데칼이 있으면 stop 후 새로 spawn (로컬 SpawnPlaceTrap 과 동일 패턴)
        auto [itDecal, inserted] = m_mapRemotePlaceDecalIds.try_emplace(playerId);
        if (inserted) itDecal->second.fill(-1);
        auto& slotArr = itDecal->second;
        if (slotArr[slotIdx] >= 0)
        {
            pDecals->Stop(slotArr[slotIdx]);
            slotArr[slotIdx] = -1;
        }

        ElementType elem = ElementType::Fire;
        auto eIt = m_mapRemotePlayerElement.find(playerId);
        if (eIt != m_mapRemotePlayerElement.end()) elem = eIt->second;
        DirectX::XMFLOAT4 color(1.f, 0.7f, 0.3f, 1.f);
        switch (elem)
        {
        case ElementType::Fire:  color = { 1.0f, 0.5f, 0.2f, 1.0f }; break;
        case ElementType::Water: color = { 0.4f, 0.7f, 1.0f, 1.0f }; break;
        case ElementType::Wind:  color = { 0.7f, 1.0f, 0.8f, 1.0f }; break;
        case ElementType::Earth: color = { 0.7f, 0.5f, 0.3f, 1.0f }; break;
        default: break;
        }
        int decalId = pDecals->Spawn(DecalTexture::Star08, DirectX::XMFLOAT3(x, y, z),
            8.0f, 0.f, 30.0f, color, 1.2f);
        slotArr[slotIdx] = decalId;
        break;
    }

    case PLAYER_ACTION_R_MAGIC_CIRCLE:
    {
        DecalManager* pDecals = pScene ? pScene->GetDecalManager() : nullptr;
        if (!pDecals)
            break;

        // dirY에는 revealDuration을 실어 보낸다.
        float revealDuration = (dirY > 0.0f) ? dirY : 0.6f;

        // 현재 로컬 R 마법진과 동일한 값
        constexpr float kMagicCircleSize = 12.0f;
        constexpr float kMagicCircleLifetime = 3.5f;
        constexpr float kMagicCircleRotateSpeed = 2.0f;

        // 원격 플레이어 원소 색상 적용
        ElementType elem = ElementType::Fire;
        auto eIt = m_mapRemotePlayerElement.find(playerId);
        if (eIt != m_mapRemotePlayerElement.end())
            elem = eIt->second;

        DirectX::XMFLOAT4 color(1.0f, 0.55f, 0.30f, 1.0f);
        switch (elem)
        {
        case ElementType::Fire:
            color = DirectX::XMFLOAT4(1.0f, 0.55f, 0.30f, 1.0f);
            break;
        case ElementType::Water:
            color = DirectX::XMFLOAT4(0.35f, 0.85f, 0.95f, 1.0f);
            break;
        case ElementType::Wind:
            color = DirectX::XMFLOAT4(0.65f, 0.95f, 0.55f, 1.0f);
            break;
        case ElementType::Earth:
            color = DirectX::XMFLOAT4(0.95f, 0.75f, 0.40f, 1.0f);
            break;
        default:
            break;
        }

        pDecals->Spawn(
            DecalTexture::MagicCircle,
            DirectX::XMFLOAT3(x, y, z),
            kMagicCircleSize,
            0.0f,
            kMagicCircleLifetime,
            color,
            kMagicCircleRotateSpeed,
            revealDuration);

        char buf[224];
        sprintf_s(buf,
            "[Network] Remote R MagicCircle spawned: playerId=%llu pos=(%.2f, %.2f, %.2f) reveal=%.2f",
            playerId, x, y, z, revealDuration);
        WriteNetworkLog(buf);

        break;
    }

    case PLAYER_ACTION_PLACE_FIRE:
    {
        int slot = static_cast<int>(dirX);
        if (slot < 0 || slot >= 4) slot = 0;
        int skillType = slot + 1; // SkillSlot Q=0/E=1/R=2/RC=3 → skillType 1/2/3/4

        // 해당 슬롯 데칼 정리 (트랩 발동 후 잔류 방지)
        if (DecalManager* pDecals = pScene ? pScene->GetDecalManager() : nullptr)
        {
            auto it = m_mapRemotePlaceDecalIds.find(playerId);
            if (it != m_mapRemotePlaceDecalIds.end() && it->second[slot] >= 0)
            {
                pDecals->Stop(it->second[slot]);
                it->second[slot] = -1;
            }
        }

        ElementType elem = ElementType::Fire;
        auto eIt = m_mapRemotePlayerElement.find(playerId);
        if (eIt != m_mapRemotePlayerElement.end()) elem = eIt->second;
        SpawnEchoSkillVFX(pScene, skillType, elem, DirectX::XMFLOAT3(x, y, z));
        break;
    }

    case PLAYER_ACTION_CHANNEL_BEGIN:
    {
        // 채널 활성 set 에 추가 — ProcessSkill 가 매 tick 마다 Attack1 CrossFade 재호출 안 하도록
        m_setRemoteChannelingPlayers.insert(playerId);
        // 시작 시점에 Attack1 한 번 깔아두기 — 채널 끝날 때까지 유지
        if (auto* pAnim = pRemotePlayer->GetComponent<AnimationComponent>())
            pAnim->CrossFade("Attack1", 0.1f, true /*loop*/, true /*restart*/);
        break;
    }

    case PLAYER_ACTION_CHANNEL_END:
    {
        m_setRemoteChannelingPlayers.erase(playerId);
        // 종료 시 idle 로 자연 전환되도록 CheckRemotePlayerIdle 이 처리한다.
        // 여기서 명시적으로 Idle 로 페이드해주면 깔끔하다.
        if (auto* pAnim = pRemotePlayer->GetComponent<AnimationComponent>())
            pAnim->CrossFade("Idle", 0.15f, true /*loop*/, false);
        break;
    }

    default:
        break;
    }
}

void NetworkManager::UpdatePendingOrbitals(Scene* pScene, float deltaTime)
{
    if (!pScene || m_vPendingOrbitals.empty()) return;
    FluidSkillVFXManager* pVFX = pScene->GetFluidVFXManager();
    ProjectileManager* pPM = pScene->GetProjectileManager();
    if (!pPM) return;

    for (auto it = m_vPendingOrbitals.begin(); it != m_vPendingOrbitals.end(); )
    {
        it->delay -= deltaTime;
        if (it->delay <= 0.f)
        {
            // 공전 visual 정리 (첫 발에만 orbVfxId 가 붙어있음)
            if (pVFX && it->orbVfxId >= 0)
                pVFX->StopEffect(it->orbVfxId);

            // owner 가 곧 파기될 원격 플레이어이면 ProcessDespawnPlayer 가 entry 를 erase 하지만,
            // 같은 프레임 내 명령 순서에 따라 여기까지 살아남을 수 있다. 방어적으로 null 체크.
            if (it->owner != nullptr)
            {
                pPM->SpawnProjectile(
                    it->origin, it->target, 0.0f,
                    it->speed, it->radius, it->explosionRadius,
                    it->element, it->owner,
                    /*isPlayerProjectile*/true, it->scale,
                    RuneCombo{}, 0.0f,
                    /*maxDistance*/100.0f,
                    /*isPiercing*/it->isPiercing);
            }
            it = m_vPendingOrbitals.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void NetworkManager::UpdateRemoteActivationRuneVFX(Scene* pScene)
{
    if (!pScene) return;
    FluidSkillVFXManager* pVFX = pScene->GetFluidVFXManager();
    if (!pVFX) return;

    DirectX::XMFLOAT3 up(0.f, 1.f, 0.f);

    auto trackOne = [&](std::unordered_map<uint64, int>& vfxMap)
        {
            for (auto it = vfxMap.begin(); it != vfxMap.end(); )
            {
                uint64 pid = it->first;
                int vfxId = it->second;

                auto playerIt = m_mapRemotePlayers.find(pid);
                if (playerIt == m_mapRemotePlayers.end() || !playerIt->second)
                {
                    if (vfxId >= 0) pVFX->StopEffect(vfxId);
                    it = vfxMap.erase(it);
                    continue;
                }
                if (auto* t = playerIt->second->GetTransform())
                {
                    DirectX::XMFLOAT3 pos = t->GetPosition();
                    pos.y += 1.5f; // PLAYER_ACTION_CHARGE_BEGIN/ENHANCE_BEGIN 의 spawn pos 와 동일하게
                    pVFX->TrackEffect(vfxId, pos, up);
                }
                ++it;
            }
        };

    trackOne(m_mapRemoteChargeVFXId);
    trackOne(m_mapRemoteEnhanceVFXId);
}

// =============================================================================
// 몬스터 처리 (서버 권위)
// =============================================================================

// monsterType (서버 MonsterType enum) → 클라 프리셋 메쉬/애니메이션/스케일 매핑
struct MonsterPreset
{
    const char* meshPath;
    const char* animPath;
    float scale;
    const char* idleClip;
    const char* walkClip;
    const char* texturePath;  // 명시적 텍스처 경로 — .bin에 <AlbedoMap> 없을 때 필수
    const char* attackClip;   // 공격 애니메이션 (S_MONSTER_ATTACK 수신 시 재생)
    const char* deathClip;    // 사망 애니메이션
    // 카테고리 tint (EnemySpawner.cpp 프리셋과 동일 색). diffuse 에 곱해짐.
    float colorR, colorG, colorB;
};

static MonsterPreset GetMonsterPresetByType(uint32 monsterType)
{
    // 서버 MonsterType enum 순서와 일치해야 함:
    // 0 None, 1 TestEnemy, 2 AirElemental, 3 RangedEnemy, 4 RushAoEEnemy, 5 RushFrontEnemy,
    // 6 Dragon, 7 Kraken, 8 Golem, 9 Demon, 10 BlueDragon
    // 클립 이름은 EnemySpawner.cpp의 elementalAnim / 보스별 config와 반드시 일치
    //   elementals: idle="idle", chase="Run_Forward"
    //   Dragon/BlueDragon: idle="Idle01"/"Idle", chase="Walk"
    //   Kraken: idle="Idle", chase="Walk"
    //   Golem: idle/chase="Golem_battle_stand_ge"/"Golem_battle_walk_ge"
    //   Demon: idle="Idle1", chase="Run"
    switch (monsterType)
    {
        // attackClip/deathClip 는 EnemySpawner.cpp 의 m_AnimConfig.m_strAttackClip / m_strDeathClip 과 일치해야 함
        // 네트워크 monsterType → 오프라인 JSON Fire stage preset 과 동일한 Rd (red) 메쉬로 통일
    case 2: // Melee → FireGolem_Rd  | 카테고리: 근접 (주황)
        return { "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd.bin",
                 "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/FireGolem_Rd/Textures/T_FireGolem_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.55f, 0.20f };
    case 3: // Ranged → MagmaElemental_Rd  | 카테고리: 원거리 (청록)
        return { "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd.bin",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/Textures/T_MagmaElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 0.30f, 0.95f, 0.85f };
    case 4: // RushAoE → MoltenElemental_Rd  | 카테고리: 돌진 (빨강)
        return { "Assets/Enemies/Elementals/MoltenElemental_Rd/MoltenElemental_Rd.bin",
                 "Assets/Enemies/Elementals/MoltenElemental_Rd/MoltenElemental_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/MoltenElemental_Rd/Textures/T_MoltenElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.35f, 0.30f };
    case 5: // RushFront → ChaosElemental_Rd  | 카테고리: 돌진 (빨강)
        return { "Assets/Enemies/Elementals/ChaosElemental_Rd/ChaosElemental_Rd.bin",
                 "Assets/Enemies/Elementals/ChaosElemental_Rd/ChaosElemental_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/ChaosElemental_Rd/Textures/T_ChaosElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.35f, 0.30f };
    case 6: // Dragon (Red)  | 카테고리: 보스 — 화염 컨셉
        return { "Assets/Enemies/Dragon/Red.bin",
                 "Assets/Enemies/Dragon/Red_Anim.bin",
                 3.0f, "Idle01", "Walk", "",
                 "Flame Attack", "Die",
                 1.0f, 0.50f, 0.30f };
    case 7: // Kraken  | 카테고리: 보스 — 심해 (보라)
        return { "Assets/Enemies/Kraken/KRAKEN.bin",
                 "Assets/Enemies/Kraken/KRAKEN_Anim.bin",
                 3.0f, "Idle", "Walk", "",
                 "Attack_Forward_RM", "Death",
                 0.55f, 0.35f, 1.00f };
    case 8: // Golem  | 카테고리: 보스 — 대지 (골드)
        return { "Assets/Enemies/Golem/Golem01_Generic_prefab.bin",
                 "Assets/Enemies/Golem/Golem01_Generic_prefab_Anim.bin",
                 14.0f, "Golem_stand_ge", "Golem_battle_stand_ge",
                 "Assets/Enemies/Golem/Textures/chr_04_Golem_alb.png",
                 "Golem_battle_attack01_ge", "Golem_battle_die_ge",
                 1.00f, 0.75f, 0.30f };
    case 9: // Demon  | 카테고리: 보스 — 짙은 빨강
        return { "Assets/Enemies/demon/Demon.bin",
                 "Assets/Enemies/demon/Demon_Anim.bin",
                 8.0f, "Idle1", "Run", "",
                 "attack1", "Death1",
                 1.00f, 0.30f, 0.25f };
    case 10: // BlueDragon  | 카테고리: 보스(중간) — 청색
        return { "Assets/Enemies/Dragon_blue/Blue.bin",
                 "Assets/Enemies/Dragon_blue/Blue_Anim.bin",
                 3.0f, "Idle", "Walk", "",
                 "Fireball Shoot", "Die",
                 0.30f, 0.55f, 1.00f };
    case 1: // TestEnemy — FireGolem_Rd 로 fallback (Melee 타입과 동일)
    default:
        return { "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd.bin",
                 "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/FireGolem_Rd/Textures/T_FireGolem_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.55f, 0.20f };
    }
}

// 서버 MonsterVisualType enum 값과 일치해야 한다.
// monsterType은 서버 AI / 판정용이고, visualType은 클라 외형 프리셋 선택용이다.
enum class NetworkMonsterVisualType : uint32
{
    None = 0,

    // 기본 일반 몬스터 외형
    FireGolem_Rd = 1,
    ChaosElemental_Rd = 2,
    LavaMan_Rd = 3,
    MoltenElemental_Rd = 4,
    MagmaElemental_Rd = 5,

    // 일반 몬스터 변종 외형
    FireGolem_Rd_Bomber = 6,
    FireGolem_Rd_Jabber = 7,
    MagmaElemental_Rd_Grenadier = 8,
    MagmaElemental_Rd_Sniper = 9,

    // 일반방 마지막 웨이브용 중간보스 외형
    MiniBoss_FireGolem_Rd = 10,
    MiniBoss_ChaosElemental_Rd = 11,
    MiniBoss_LavaMan_Rd = 12,
    MiniBoss_MoltenElemental_Rd = 13,
    MiniBoss_MagmaElemental_Rd = 14
};

static bool IsNetworkMiniBossVisualType(uint32 visualType)
{
    return visualType >= static_cast<uint32>(NetworkMonsterVisualType::MiniBoss_FireGolem_Rd) &&
        visualType <= static_cast<uint32>(NetworkMonsterVisualType::MiniBoss_MagmaElemental_Rd);
}

// 중간보스 공격 스케일
// 중간보스는 일반 몬스터 AttackBehavior를 그대로 사용하되,
// 몸집이 커진 비율만큼 인디케이터 / 공격 범위를 함께 키운다.
static float GetNetworkMiniBossAttackScaleRatio(uint32 visualType)
{
    return IsNetworkMiniBossVisualType(visualType) ? 2.0f : 1.0f;
}

// 클라 MapLoader.cpp의 공격 카테고리별 색상과 동일하게 맞춘다.
// JSON visual.color가 아니라 attackType 기준 색상을 사용한다.
static XMFLOAT3 GetNetworkMonsterTintByAttackType(uint32 attackType)
{
    switch (attackType)
    {
    case 1:  // Melee
    case 36: // QuickJab
        // 근접: 주황
        return XMFLOAT3(1.00f, 0.55f, 0.20f);

    case 3:  // RushAoE
    case 4:  // RushFront
        // 돌진: 빨강
        return XMFLOAT3(1.00f, 0.35f, 0.30f);

    case 2:  // Ranged
    case 37: // ChargedShot
        // 원거리: 청록
        return XMFLOAT3(0.30f, 0.95f, 0.85f);

    case 38: // GrenadeThrow
    case 39: // SuicideExplode
        // 공중 / 탄막: 보라
        return XMFLOAT3(0.75f, 0.40f, 1.00f);

    default:
        return XMFLOAT3(1.0f, 1.0f, 1.0f);
    }
}

// visualType → 클라 프리셋 메쉬 / 애니메이션 / 스케일 매핑
// visualType이 없으면 기존 monsterType 기준 매핑으로 fallback한다.
// visualType → 클라 프리셋 메쉬 / 애니메이션 / 스케일 매핑
// visualType이 없으면 기존 monsterType 기준 매핑으로 fallback한다.
static MonsterPreset GetMonsterPresetByVisualType(uint32 visualType, uint32 monsterType)
{
    switch (static_cast<NetworkMonsterVisualType>(visualType))
    {
    case NetworkMonsterVisualType::FireGolem_Rd:
        return { "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd.bin",
                 "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/FireGolem_Rd/Textures/T_FireGolem_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 1.00f, 1.00f };

    case NetworkMonsterVisualType::FireGolem_Rd_Bomber:
        return { "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd.bin",
                 "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd_Anim.bin",
                 4.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/FireGolem_Rd/Textures/T_FireGolem_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.16f, 0.12f };

    case NetworkMonsterVisualType::FireGolem_Rd_Jabber:
        return { "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd.bin",
                 "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd_Anim.bin",
                 4.0f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/FireGolem_Rd/Textures/T_FireGolem_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 0.31f, 0.90f, 0.90f };

    case NetworkMonsterVisualType::ChaosElemental_Rd:
        return { "Assets/Enemies/Elementals/ChaosElemental_Rd/ChaosElemental_Rd.bin",
                 "Assets/Enemies/Elementals/ChaosElemental_Rd/ChaosElemental_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/ChaosElemental_Rd/Textures/T_ChaosElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.35f, 0.30f };

    case NetworkMonsterVisualType::LavaMan_Rd:
        return { "Assets/Enemies/Elementals/LavaMan_Rd/LavaMan_Rd.bin",
                 "Assets/Enemies/Elementals/LavaMan_Rd/LavaMan_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/LavaMan_Rd/Textures/T_LavaMan_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.55f, 0.20f };

    case NetworkMonsterVisualType::MoltenElemental_Rd:
        return { "Assets/Enemies/Elementals/MoltenElemental_Rd/MoltenElemental_Rd.bin",
                 "Assets/Enemies/Elementals/MoltenElemental_Rd/MoltenElemental_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/MoltenElemental_Rd/Textures/T_MoltenElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.35f, 0.30f };

    case NetworkMonsterVisualType::MagmaElemental_Rd:
        return { "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd.bin",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/Textures/T_MagmaElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 1.00f, 1.00f };

    case NetworkMonsterVisualType::MagmaElemental_Rd_Grenadier:
        return { "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd.bin",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd_Anim.bin",
                 7.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/Textures/T_MagmaElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 0.96f, 0.55f, 0.16f };

    case NetworkMonsterVisualType::MagmaElemental_Rd_Sniper:
        return { "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd.bin",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd_Anim.bin",
                 6.8f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/Textures/T_MagmaElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 0.82f, 0.31f, 0.92f };

    case NetworkMonsterVisualType::MiniBoss_FireGolem_Rd:
        return { "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd.bin",
                 "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd_Anim.bin",
                 11.0f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/FireGolem_Rd/Textures/T_FireGolem_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.55f, 0.20f };

    case NetworkMonsterVisualType::MiniBoss_ChaosElemental_Rd:
        return { "Assets/Enemies/Elementals/ChaosElemental_Rd/ChaosElemental_Rd.bin",
                 "Assets/Enemies/Elementals/ChaosElemental_Rd/ChaosElemental_Rd_Anim.bin",
                 11.0f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/ChaosElemental_Rd/Textures/T_ChaosElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.35f, 0.30f };

    case NetworkMonsterVisualType::MiniBoss_LavaMan_Rd:
        return { "Assets/Enemies/Elementals/LavaMan_Rd/LavaMan_Rd.bin",
                 "Assets/Enemies/Elementals/LavaMan_Rd/LavaMan_Rd_Anim.bin",
                 11.0f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/LavaMan_Rd/Textures/T_LavaMan_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.55f, 0.20f };

    case NetworkMonsterVisualType::MiniBoss_MoltenElemental_Rd:
        return { "Assets/Enemies/Elementals/MoltenElemental_Rd/MoltenElemental_Rd.bin",
                 "Assets/Enemies/Elementals/MoltenElemental_Rd/MoltenElemental_Rd_Anim.bin",
                 11.0f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/MoltenElemental_Rd/Textures/T_MoltenElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 1.00f, 0.35f, 0.30f };

    case NetworkMonsterVisualType::MiniBoss_MagmaElemental_Rd:
        return { "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd.bin",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd_Anim.bin",
                 11.0f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/Textures/T_MagmaElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death",
                 0.30f, 0.95f, 0.85f };

    case NetworkMonsterVisualType::None:
    default:
        return GetMonsterPresetByType(monsterType);
    }
}

// EnemySpawner::LoadTextureToHierarchy 미러 — 하이러키 순회하며 텍스처+카테고리 색 머티리얼 적용.
// 목적: MATERIAL이 garbage로 초기화되어 diffuse=0 → 메쉬가 까맣게 렌더되어 보이지 않는 문제 해결.
// 추가: tint != (1,1,1) 시 카테고리 색이 텍스처 위에 입혀짐 (호스트/게스트 시각 동기화).
static void ApplyWhiteMaterialAndTextureToHierarchy(
    Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
    GameObject* pGO, const char* texturePath,
    float tintR = 1.0f, float tintG = 1.0f, float tintB = 1.0f)
{
    if (!pGO || !pScene) return;

    if (pGO->GetMesh())
    {
        MATERIAL mat;
        mat.m_cAmbient = XMFLOAT4(tintR * 0.3f, tintG * 0.3f, tintB * 0.3f, 1.0f);
        mat.m_cDiffuse = XMFLOAT4(tintR, tintG, tintB, 1.0f);
        mat.m_cSpecular = XMFLOAT4(0.3f, 0.3f, 0.3f, 32.0f);
        mat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        pGO->SetMaterial(mat);

        if (texturePath && texturePath[0] != '\0' && !pGO->HasTexture())
        {
            pGO->SetTextureName(texturePath);
            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
            pScene->AllocateDescriptor(&cpuHandle, &gpuHandle);
            pGO->LoadTexture(pDevice, pCommandList, cpuHandle);
            pGO->SetSrvGpuDescriptorHandle(gpuHandle);

            wchar_t wbuf[256];
            swprintf_s(wbuf, L"[Network] Applied white material + texture to [%hs] tex=%hs\n",
                pGO->m_pstrFrameName, texturePath);
            OutputDebugString(wbuf);

            char abuf[256];
            sprintf_s(abuf, "[Network] Applied white material + texture to [%s] tex=%s",
                pGO->m_pstrFrameName, texturePath);
            WriteNetworkLog(abuf);
        }
        else
        {
            wchar_t wbuf[256];
            swprintf_s(wbuf, L"[Network] Applied white material to [%hs] (no new texture, hasTexture=%d)\n",
                pGO->m_pstrFrameName, pGO->HasTexture() ? 1 : 0);
            OutputDebugString(wbuf);

            char abuf[256];
            sprintf_s(abuf, "[Network] Applied white material to [%s] (no new texture, hasTexture=%d)",
                pGO->m_pstrFrameName, pGO->HasTexture() ? 1 : 0);
            WriteNetworkLog(abuf);
        }
    }

    if (pGO->m_pChild)   ApplyWhiteMaterialAndTextureToHierarchy(pScene, pDevice, pCommandList, pGO->m_pChild, texturePath, tintR, tintG, tintB);
    if (pGO->m_pSibling) ApplyWhiteMaterialAndTextureToHierarchy(pScene, pDevice, pCommandList, pGO->m_pSibling, texturePath, tintR, tintG, tintB);
}

void NetworkManager::ProcessMonsterSpawn(Scene* pScene, ID3D12Device* pDevice,
    ID3D12GraphicsCommandList* pCommandList,
    uint64 monsterId, uint32 monsterType,
    uint32 attackType, uint32 visualType,
    float x, float y, float z, float yaw,
    float hp, bool isBoss)
{
    if (!pScene)
        return;

    // 서버 몬스터 스폰을 받았다는 것은 누군가 F를 눌러 방이 시작됐다는 뜻이다.
    // F를 누르지 않은 다른 클라에서도 시작 포탈이 남아있지 않도록 같이 숨긴다.
    pScene->HideInteractionCubeByNetworkStart();

    // 중복 방지: 같은 monsterId 가 이미 있으면 새 GameObject 만들지 않고 skip.
    // (기존은 위치만 갱신했으나 방 전환 후 서버가 같은 id 를 재전송할 때 이전 방 몬스터가
    //  새 방에 재등장하는 혼란 발생 — ProcessRoomTransition 이 맵을 clear 하므로 여기선 단순 skip.)
    if (m_mapServerMonsters.find(monsterId) != m_mapServerMonsters.end())
    {
        char dupBuf[128];
        sprintf_s(dupBuf, "[Network] MonsterSpawn skipped: duplicate monsterId=%llu", monsterId);
        WriteNetworkLog(dupBuf);
        return;
    }

    // attackType은 현재 스폰 외형 선택에는 직접 사용하지 않지만,
    MonsterPreset preset = GetMonsterPresetByVisualType(visualType, monsterType);

    // 서버 몬스터는 로컬 Room에 속하지 않는 전역 오브젝트로 생성
    CRoom* pPrevRoom = pScene->GetCurrentRoom();

    // DarkLord는 단순 MeshLoader로 만들면 안 된다.
    // EnemySpawner("DarkLord") 프리셋 안에 전용 텍스처/애니/페이즈/패턴 구성이 들어있다.
    if (monsterType == 11 && isBoss)
    {
        if (pPrevRoom && pPrevRoom->GetState() == RoomState::Inactive)
        {
            pPrevRoom->SetState(RoomState::Active);

            for (const auto& pGO : pPrevRoom->GetGameObjects())
            {
                if (pGO)
                    pGO->Update(0.0f);
            }

            WriteNetworkLog("[Network] Current room activated by DarkLord spawn");
        }

        GameObject* pMonster = nullptr;

        if (EnemySpawner* pSpawner = pScene->GetEnemySpawner())
        {
            XMFLOAT3 spawnPos(x, y, z);
            pMonster = pSpawner->SpawnEnemy(
                pPrevRoom,
                "DarkLord",
                spawnPos,
                pScene->GetPlayer()
            );
        }

        if (!pMonster)
        {
            WriteNetworkLog("[Network] DarkLord SpawnEnemy failed");
            return;
        }

        sprintf_s(pMonster->m_pstrFrameName, "NetMonster_%llu", monsterId);

        if (auto* pT = pMonster->GetTransform())
        {
            pT->SetPosition(x, y, z);
            pT->SetRotation(0.0f, yaw, 0.0f);
            pT->SetScale(1.0f, 1.0f, 1.0f);
        }

        if (auto* pAnim = pMonster->GetComponent<AnimationComponent>())
        {
            pAnim->SetCullEnabled(false);
            pAnim->CrossFade("fightidle", 0.15f, true);
            m_mapServerMonsterCurrentAnimClip[monsterId] = "fightidle";
        }

        // 서버 권위 보스이므로 클라 로컬 AI는 정지
        if (auto* pEC = pMonster->GetComponent<EnemyComponent>())
        {
            pEC->SetAIPaused(true);
            pEC->SetInvincible(true);
        }

        m_mapServerMonsters[monsterId] = pMonster;

        ServerMonsterClips clips;
        clips.idle = "fightidle";
        clips.walk = "run";
        clips.attack = "attack3";
        clips.death = "death1";
        clips.monsterType = monsterType;
        clips.attackType = attackType;
        clips.visualType = visualType;
        clips.isBoss = isBoss;
        clips.isMiniBoss = false;
        m_mapServerMonsterClips[monsterId] = clips;

        ServerMonsterTarget initTgt;
        initTgt.px = x;
        initTgt.py = y;
        initTgt.pz = z;
        initTgt.yaw = yaw;
        initTgt.hasTarget = true;
        m_mapServerMonsterTarget[monsterId] = initTgt;

        ServerBossSpawnPos sp{ x, y, z };
        m_mapServerBossSpawnPos[monsterId] = sp;

        if (EnemySpawner* pSpawner = pScene->GetEnemySpawner())
        {
            auto set = pSpawner->CreateNetBossIndicators();

            ServerMonsterIndicators ind;
            ind.circleBorder = set.circleBorder;
            ind.circleFill = set.circleFill;
            ind.boxBorder = set.boxBorder;
            ind.boxFill = set.boxFill;

            HideMonsterIndicators(ind);
            m_mapServerMonsterIndicators[monsterId] = ind;
        }

        char buf[256];
        sprintf_s(buf,
            "[Network] Spawned Network DarkLord monsterId=%llu hp=%.1f pos=(%.2f, %.2f, %.2f)",
            monsterId, hp, x, y, z);
        WriteNetworkLog(buf);

        return;
    }

    pScene->SetCurrentRoom(nullptr);

    // 첫 몬스터 스폰 시 현재 방을 강제로 Active로 전환한다.
    if (pPrevRoom && pPrevRoom->GetState() == RoomState::Inactive)
    {
        pPrevRoom->SetState(RoomState::Active);

        // Inactive 상태에서 멈춰 있던 방 소속 오브젝트들의 CB를 한 번 갱신한다.
        for (const auto& pGO : pPrevRoom->GetGameObjects())
        {
            if (pGO)
                pGO->Update(0.0f);
        }

        WriteNetworkLog("[Network] Current room activated by first monster spawn");
    }

    GameObject* pMonster = MeshLoader::LoadGeometryFromFile(
        pScene, pDevice, pCommandList, nullptr, preset.meshPath);

    pScene->SetCurrentRoom(pPrevRoom);

    if (!pMonster)
    {
        wchar_t buf[256];
        swprintf_s(buf, L"[Network] ProcessMonsterSpawn: mesh load FAILED type=%u visual=%u path=%hs\n",
            monsterType, visualType, preset.meshPath);
        OutputDebugString(buf);
        return;
    }

    sprintf_s(pMonster->m_pstrFrameName, "NetMonster_%llu", monsterId);

    // 위치/회전/스케일
    TransformComponent* pT = pMonster->GetTransform();
    if (pT)
    {
        float spawnY = y;

        // 일반 몬스터는 처음에 공중 포탈 위치에서 시작한다.
        // 보스는 기존 보스 컷신 / BossEvent 흐름을 유지한다.
        if (isBoss == false)
        {
            spawnY = y + kNetSpawnPortalY;
        }

        pT->SetPosition(x, spawnY, z);
        pT->SetScale(preset.scale, preset.scale, preset.scale);

        // 서버가 yaw를 도(degree)로 보냄 → 그대로 사용
        pT->SetRotation(0.0f, yaw, 0.0f);
    }

    // 일반 몬스터 스폰 포탈 VFX
    if (isBoss == false && pScene)
    {
        if (VFXManager* pVFX = pScene->GetVFXManager())
        {
            XMFLOAT3 up(0.0f, 1.0f, 0.0f);

            XMFLOAT3 portalPos(x, y + kNetSpawnPortalY, z);
            pVFX->Spawn("Spawn_Portal", portalPos, up, 0u, false);

            XMFLOAT3 groundPos(x, y + 0.15f, z);
            pVFX->Spawn("Spawn_PortalGround", groundPos, up, 0u, false);
        }
    }

    // 애니메이션
    auto* pAnim = pMonster->AddComponent<AnimationComponent>();
    if (pAnim)
    {
        pAnim->LoadAnimation(preset.animPath);
        pAnim->SetCullEnabled(false);
        pAnim->Play(preset.idleClip, true);

        // 스폰 직후 현재 애니메이션은 idle로 기록한다.
        // 이후 정지 MOVE 패킷이 와도 walk로 잘못 전환하지 않기 위한 기준값이다.
        m_mapServerMonsterCurrentAnimClip[monsterId] = preset.idleClip;
    }

    // 일반 몬스터 / 중간보스 색상은 클라 MapLoader 기준과 동일하게 attackType으로 결정한다.
 // 보스는 기존 preset 색상을 유지한다.
    XMFLOAT3 monsterTint = XMFLOAT3(preset.colorR, preset.colorG, preset.colorB);

    if (!isBoss &&
        (monsterType == 2 || monsterType == 3 || monsterType == 4 || monsterType == 5 ||
            IsNetworkMiniBossVisualType(visualType)))
    {
        monsterTint = GetNetworkMonsterTintByAttackType(attackType);
    }

    ApplyWhiteMaterialAndTextureToHierarchy(
        pScene,
        pDevice,
        pCommandList,
        pMonster,
        preset.texturePath,
        monsterTint.x,
        monsterTint.y,
        monsterTint.z);

    // 쉐이더 등록 (렌더링)
    Shader* pDefaultShader = pScene->GetDefaultShader();
    if (pDefaultShader)
    {
        pScene->AddRenderComponentsToHierarchy(pDevice, pCommandList, pMonster, pDefaultShader, true);
    }

    // AnimationComponent::BuildBoneCache 호출 포함
    pMonster->Init(pDevice, pCommandList);

    m_mapServerMonsters[monsterId] = pMonster;
    {
        ServerMonsterClips clips;
        clips.idle = preset.idleClip;
        clips.walk = preset.walkClip;
        clips.attack = preset.attackClip;
        clips.death = preset.deathClip;
        clips.monsterType = monsterType;
        clips.attackType = attackType;
        clips.visualType = visualType;
        clips.isBoss = isBoss;
        clips.isMiniBoss = IsNetworkMiniBossVisualType(visualType);
        m_mapServerMonsterClips[monsterId] = clips;

        // 일반 Rush 몬스터는 클라 오프라인 EnemyComponent 인디케이터 시스템을 그대로 사용한다.
        if (monsterType == 2 || monsterType == 3 || monsterType == 4 || monsterType == 5 || IsNetworkMiniBossVisualType(visualType))
        {
            EnemyComponent* pEnemyComp = pMonster->GetComponent<EnemyComponent>();
            if (pEnemyComp == nullptr)
                pEnemyComp = pMonster->AddComponent<EnemyComponent>();

            pEnemyComp->SetAIPaused(true);
            pEnemyComp->SetBoss(false);

            if (auto* pAnimComp = pMonster->GetComponent<AnimationComponent>())
                pEnemyComp->SetAnimationComponent(pAnimComp);

            pEnemyComp->SetRoom(pPrevRoom);

            AttackIndicatorConfig config;

            // 네트워크 일반 몬스터 인디케이터는 monsterType이 아니라 attackType 기준으로 만든다.
            // 클라 단독 MapLoader는 room_Room*.json의 attackType / indicator 값을 기준으로
            // AttackBehavior와 인디케이터를 구성하므로, 서버 네트워크도 같은 기준을 따른다.
            switch (attackType)
            {
            case 1: // Melee
            {
                // 기본 근접 원형 공격
                config.m_eType = IndicatorType::Circle;
                config.m_fHitRadius = 4.0f;
                config.m_fRushDistance = 0.0f;
                config.m_fConeAngle = 0.0f;
                config.m_fHitLength = 0.0f;
                break;
            }

            case 36: // QuickJab
            {
                // FireGolem_Rd_Jabber — 빠른 3연타
                // 클라 QuickJabAttackBehavior 기준 hitRange=3.2
                config.m_eType = IndicatorType::Circle;
                config.m_fHitRadius = 3.2f;
                config.m_fRushDistance = 0.0f;
                config.m_fConeAngle = 0.0f;
                config.m_fHitLength = 0.0f;
                break;
            }

            case 37: // ChargedShot
            {
                // MagmaElemental_Rd_Sniper — 노란 직선 조준선
                // 클라 ChargedShotAttackBehavior 기준: ForwardBox length=34, halfWidth=1.2
                config.m_eType = IndicatorType::ForwardBox;
                config.m_fHitRadius = 1.2f;
                config.m_fRushDistance = 0.0f;
                config.m_fConeAngle = 0.0f;
                config.m_fHitLength = 34.0f;
                break;
            }

            case 38: // GrenadeThrow
            {
                // MagmaElemental_Rd_Grenadier — 착탄 위치 원형 예고
                // 클라 GrenadeThrowAttackBehavior 기준: Circle radius=4.5
                config.m_eType = IndicatorType::Circle;
                config.m_fHitRadius = 4.5f;
                config.m_fRushDistance = 0.0f;
                config.m_fConeAngle = 0.0f;
                config.m_fHitLength = 0.0f;
                break;
            }

            case 39: // SuicideExplode
            {
                // FireGolem_Rd_Bomber — 자폭 원형 예고
                config.m_eType = IndicatorType::Circle;
                config.m_fHitRadius = 4.5f;
                config.m_fRushDistance = 0.0f;
                config.m_fConeAngle = 0.0f;
                config.m_fHitLength = 0.0f;
                break;
            }

            case 3: // RushAoE
            {
                // MoltenElemental_Rd — 돌진 AoE
                // 클라 MapLoader 경로는 RushAoEAttackBehavior 기본값을 사용한다.
                // 기본값: rushSpeed=15, rushDuration=0.5, aoeRadius=5 → length=12.5
                config.m_eType = IndicatorType::ForwardBox;
                config.m_fHitRadius = 5.0f;
                config.m_fRushDistance = 0.0f;
                config.m_fConeAngle = 0.0f;
                config.m_fHitLength = 12.5f;
                break;
            }

            case 4: // RushFront
            {
                // ChaosElemental_Rd — 전방 돌진
                // 클라 RushFrontAttackBehavior 기본값 기준:
                // rushSpeed=18, rushDuration=0.4, hitRange=4 → length=11.2
                config.m_eType = IndicatorType::ForwardBox;
                config.m_fHitRadius = 4.5f;
                config.m_fRushDistance = 0.0f;
                config.m_fConeAngle = 0.0f;
                config.m_fHitLength = 11.2f;
                break;
            }

            case 2: // Ranged
            default:
            {
                // 기본 원거리 투사체는 지면 telegraph 없음
                config.m_eType = IndicatorType::None;
                config.m_fHitRadius = 0.0f;
                config.m_fRushDistance = 0.0f;
                config.m_fConeAngle = 0.0f;
                config.m_fHitLength = 0.0f;
                break;
            }
            }

            // 중간보스는 일반 몬스터 패턴을 그대로 쓰되,
            // 몸집이 커진 비율만큼 지면 인디케이터 크기를 키운다.
            const float attackScale = GetNetworkMiniBossAttackScaleRatio(visualType);

            config.m_fHitRadius *= attackScale;
            config.m_fRushDistance *= attackScale;
            config.m_fHitLength *= attackScale;

            if (EnemySpawner* pSpawner = pScene ? pScene->GetEnemySpawner() : nullptr)
            {
                pSpawner->SetupNetMonsterAttackIndicators(
                    pMonster,
                    pEnemyComp,
                    config,
                    pPrevRoom
                );
            }

            pEnemyComp->ResetNetworkAttackIndicator();
        }
    }

    // 네트워크 보스 몬스터 영역 표시
    if (isBoss)
    {
        if (EnemySpawner* pSpawner = pScene ? pScene->GetEnemySpawner() : nullptr)
        {
            auto set = pSpawner->CreateNetBossIndicators();

            ServerMonsterIndicators ind;
            ind.circleBorder = set.circleBorder;
            ind.circleFill = set.circleFill;
            ind.boxBorder = set.boxBorder;
            ind.boxFill = set.boxFill;

            HideMonsterIndicators(ind);
            m_mapServerMonsterIndicators[monsterId] = ind;
        }
    }

    // 보간 타겟 초기값은 실제 바닥 위치로 저장한다.
// 화면상 Transform은 공중에서 시작하지만, 서버 기준 target은 바닥 좌표다.
    ServerMonsterTarget initTgt;
    initTgt.px = x;
    initTgt.py = y;
    initTgt.pz = z;
    initTgt.yaw = yaw;
    initTgt.hasTarget = true;
    m_mapServerMonsterTarget[monsterId] = initTgt;

    // 일반 몬스터 스폰 낙하 연출 등록
    if (isBoss == false)
    {
        ServerMonsterSpawnEffect fx;
        fx.elapsed = 0.0f;
        fx.portalDelay = kNetSpawnPortalDelay;
        fx.fallTime = kNetSpawnFallTime;
        fx.portalHeight = kNetSpawnPortalY;

        fx.groundX = x;
        fx.groundY = y;
        fx.groundZ = z;
        fx.yaw = yaw;

        m_mapServerMonsterSpawnEffects[monsterId] = fx;

        char buf[180];
        sprintf_s(
            buf,
            sizeof(buf),
            "[Network] Monster spawn portal scheduled: id=%llu pos=(%.2f,%.2f,%.2f)",
            monsterId,
            x,
            y,
            z
        );
        WriteNetworkLog(buf);
    }

    // 보스면 spawn 위치 영구 보존 — MegaBreath cover 좌표 기준점
    if (isBoss)
    {
        ServerBossSpawnPos sp{ x, y, z };
        m_mapServerBossSpawnPos[monsterId] = sp;
    }

    // 디버그: 실제 배치된 transform과 preset 클립 확인 (VS Output + file 둘 다)
    XMFLOAT3 finalPos = pT ? pT->GetPosition() : XMFLOAT3{ 0,0,0 };
    XMFLOAT3 finalRot = pT ? pT->GetRotation() : XMFLOAT3{ 0,0,0 };
    XMFLOAT3 finalSca = pT ? pT->GetScale() : XMFLOAT3{ 1,1,1 };
    char abuf[1024];
    sprintf_s(abuf, sizeof(abuf),
        "[Network] Spawned NetMonster_%llu type=%u attack=%u visual=%u boss=%d hp=%.1f | pos=(%.2f,%.2f,%.2f) rot=(0.0,%.1f,0.0) scale=(%.2f,%.2f,%.2f) | idleClip=%s walkClip=%s mesh=%s",
        monsterId,
        monsterType,
        attackType,
        visualType,
        isBoss ? 1 : 0,
        hp,
        x, y, z,
        yaw,
        preset.scale, preset.scale, preset.scale,
        preset.idleClip,
        preset.walkClip,
        preset.meshPath);
    WriteNetworkLog(abuf);
}

void NetworkManager::ProcessMonsterMove(uint64 monsterId, float x, float y, float z, float yaw)
{
    // 비정상 monsterId / 좌표 방어
    if (monsterId == 0 || monsterId > 1000000)
    {
        char buf[128];
        sprintf_s(buf, "[Network] MonsterMove skipped: invalid monsterId=%llu", monsterId);
        WriteNetworkLog(buf);
        return;
    }

    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(yaw))
    {
        char buf[256];
        sprintf_s(buf, "[Network] MonsterMove skipped: invalid pos id=%llu pos=(%.2f,%.2f,%.2f) yaw=%.2f",
            monsterId, x, y, z, yaw);
        WriteNetworkLog(buf);
        return;
    }

    // 죽은 몬스터 move 무시
    if (m_setDeadServerMonsters.find(monsterId) != m_setDeadServerMonsters.end())
    {
        return;
    }

    Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
    if (pScene &&
        (pScene->IsNetworkKrakenCutsceneTarget(monsterId) ||
            pScene->IsNetworkDarkLordCutsceneTarget(monsterId)))
    {
        // 컷신 중에는 Scene 컷신 상태머신이 위치를 직접 제어한다.
        // 서버 MOVE가 컷신 위치/스케일을 덮어쓰면 연출이 깨지므로 무시한다.
        return;
    }

    // DarkLord 사망 연출 진행 중에는 잔여 MOVE 패킷이 walk CrossFade 를 발동시켜
    //   Death 클립을 덮어쓴다 → "사망 후 다시 일어나 움직이는" 현상.
    //   packet reorder 로 MOVE 가 isDead 보다 먼저 도착해도 이 가드로 차단.
    if (pScene && pScene->IsNetworkDarkLordDeathTarget(monsterId))
    {
        return;
    }

    auto it = m_mapServerMonsters.find(monsterId);
    if (it == m_mapServerMonsters.end())
        return;

    GameObject* pMonster = it->second;

    // 직접 SetPosition하지 않고 타겟만 갱신한다.
    // 단, 서버는 실제로 움직이지 않는 몬스터에게도 매 프레임 S_MONSTER_MOVE를 보낸다.
    // 따라서 "MOVE 패킷 수신"이 아니라 "좌표 변화량"으로 walk / idle을 판단해야 한다.
    auto prevTargetIt = m_mapServerMonsterTarget.find(monsterId);

    bool hadPrevTarget =
        prevTargetIt != m_mapServerMonsterTarget.end() &&
        prevTargetIt->second.hasTarget;

    float prevX = hadPrevTarget ? prevTargetIt->second.px : x;
    float prevZ = hadPrevTarget ? prevTargetIt->second.pz : z;

    float moveDx = x - prevX;
    float moveDz = z - prevZ;
    float moveDistSq = moveDx * moveDx + moveDz * moveDz;

    // 서버 좌표가 실제로 변했는지 판단. 너무 작으면 정지 상태로 본다.
    //   0.0004 (0.02m) 는 jitter 와 walk-per-tick 거리(보통 0.13m) 사이에 있어
    //   서버 시뮬 미세 진동에도 walk↔idle 가 ping-pong → 워크 사이클이 재시작되어
    //   끊겨 보임. 0.005 (0.07m) 로 올려 jitter 는 거르고 실제 walk 는 유지.
    bool bActuallyMoved = (!hadPrevTarget || moveDistSq > 0.005f);

    // 타겟 좌표 갱신
    ServerMonsterTarget& tgt = m_mapServerMonsterTarget[monsterId];
    tgt.px = x;
    tgt.py = y;
    tgt.pz = z;
    tgt.yaw = yaw;

    if (!tgt.hasTarget)
    {
        TransformComponent* pT = pMonster->GetTransform();
        if (pT)
        {
            pT->SetPosition(x, y, z);

            XMFLOAT3 rot = pT->GetRotation();
            pT->SetRotation(rot.x, yaw, rot.z);
        }

        tgt.hasTarget = true;
    }

    // 스폰 포탈 / 낙하 연출 중이면 좌표만 저장하고 애니메이션은 절대 건드리지 않는다.
    auto spawnFxIt = m_mapServerMonsterSpawnEffects.find(monsterId);
    if (spawnFxIt != m_mapServerMonsterSpawnEffects.end())
    {
        spawnFxIt->second.groundX = x;
        spawnFxIt->second.groundY = y;
        spawnFxIt->second.groundZ = z;
        spawnFxIt->second.yaw = yaw;

        // 스폰 연출 중에는 idle 전환 타이머가 쌓이지 않게 한다.
        m_mapServerMonsterMoveTime[monsterId] = 0.0f;

        return;
    }

    // 공격 애니 재생 중이면 walk / idle 전환 금지
    bool bAttackLocked = false;
    {
        auto atkIt = m_mapServerMonsterAttackTimer.find(monsterId);
        if (atkIt != m_mapServerMonsterAttackTimer.end() && atkIt->second > 0.0f)
            bAttackLocked = true;
    }

    // 죽은 몬스터는 death 애니 유지
    bool bDead = (m_setDeadServerMonsters.find(monsterId) != m_setDeadServerMonsters.end());

    auto clipIt = m_mapServerMonsterClips.find(monsterId);
    std::string walkClip = (clipIt != m_mapServerMonsterClips.end())
        ? clipIt->second.walk
        : "Walk";
    uint32 mtMove = (clipIt != m_mapServerMonsterClips.end()) ? clipIt->second.monsterType : 0;

    // 실제로 좌표가 변했을 때만 walk로 전환한다.
    // 정지 MOVE 패킷은 walk로 보지 않는다.
    if (bActuallyMoved)
    {
        auto* pAnim = pMonster->GetComponent<AnimationComponent>();
        if (pAnim && !bAttackLocked && !bDead)
        {
            auto curIt = m_mapServerMonsterCurrentAnimClip.find(monsterId);
            bool needChange =
                curIt == m_mapServerMonsterCurrentAnimClip.end() ||
                curIt->second != walkClip;

            if (needChange)
            {
                // 보스 (Demon/DarkLord) 는 0.1 → 0.2 로 부드러운 전환
                float walkBlend = ((mtMove == 9) || (mtMove == 11)) ? 0.2f : 0.1f;
                pAnim->CrossFade(walkClip, walkBlend, true);
                m_mapServerMonsterCurrentAnimClip[monsterId] = walkClip;
            }

            // 발 끌림 픽스: 실제 이동 속도(서버 위치 변화) 기반으로 walk 클립 playback 속도 조정.
            //   패킷 간 실측 dt 로 instantaneous speed 계산 → EMA 스무딩 → refSpeed 로 정규화.
            //   서버 boss 가 SetPlaybackSpeed=1.0 고정이라 다리 사이클이 위치 이동과 어긋나 발 끌림 발생.
            if (mtMove == 9 || mtMove == 11)
            {
                double nowSec = static_cast<double>(GetTickCount64()) / 1000.0;
                double dt = (tgt.lastPacketTime > 0.0) ? (nowSec - tgt.lastPacketTime) : 0.033;
                tgt.lastPacketTime = nowSec;
                if (dt < 0.010) dt = 0.010;   // 너무 짧으면 분모 폭발 방지
                if (dt > 0.300) dt = 0.300;   // 너무 길어도 평균으로 클램프

                float instSpeed = sqrtf(moveDistSq) / static_cast<float>(dt);

                // EMA — 패킷 jitter 흡수해 playback 진동 방지.
                if (tgt.smoothedSpeed <= 0.001f)
                    tgt.smoothedSpeed = instSpeed;
                else
                    tgt.smoothedSpeed = tgt.smoothedSpeed * 0.6f + instSpeed * 0.4f;

                // 기준 walk 속도 = "이 anim 클립이 자연스러워 보이는 이동 속도".
                //   Demon: 빠른 이동(20~31 u/s) + 짧은 stride 클립 → refSpeed 작게(9) 해서
                //          playSpeed 올림 → 발 끌림 방지.
                //   DarkLord: 느린 이동(8 u/s) + 빠른 run 클립 → refSpeed 크게(16) +
                //             clamp min 0.5 까지 허용 → playSpeed 내림 → 발동동(treadmill) 방지.
                float refSpeed = (mtMove == 9) ? 9.0f : 16.0f;
                float minClamp = (mtMove == 9) ? 0.9f : 0.5f;
                float playSpeed = tgt.smoothedSpeed / refSpeed;
                if (playSpeed < minClamp) playSpeed = minClamp;
                if (playSpeed > 3.0f) playSpeed = 3.0f;
                pAnim->SetPlaybackSpeed(playSpeed);
            }
        }

        // 실제로 움직였을 때만 idle 전환 타이머를 리셋한다.
        m_mapServerMonsterMoveTime[monsterId] = 0.0f;
    }
    else
    {
        // 서버가 정지 좌표를 계속 보내는 경우에는 moveTime을 리셋하지 않는다.
        // 그래야 CheckServerMonsterIdle()이 일정 시간 후 idle로 돌릴 수 있다.
        auto curIt = m_mapServerMonsterCurrentAnimClip.find(monsterId);

        if (curIt != m_mapServerMonsterCurrentAnimClip.end() &&
            curIt->second == walkClip &&
            m_mapServerMonsterMoveTime.find(monsterId) == m_mapServerMonsterMoveTime.end())
        {
            m_mapServerMonsterMoveTime[monsterId] = 0.0f;
        }
    }
}

bool NetworkManager::IsServerMonsterSpawnEffectActive(uint64 monsterId) const
{
    return m_mapServerMonsterSpawnEffects.find(monsterId) != m_mapServerMonsterSpawnEffects.end();
}

void NetworkManager::UpdateServerMonsterSpawnEffects(float deltaTime)
{
    if (m_mapServerMonsterSpawnEffects.empty())
        return;

    for (auto it = m_mapServerMonsterSpawnEffects.begin(); it != m_mapServerMonsterSpawnEffects.end(); )
    {
        uint64 monsterId = it->first;
        ServerMonsterSpawnEffect& fx = it->second;

        auto monIt = m_mapServerMonsters.find(monsterId);
        if (monIt == m_mapServerMonsters.end() || monIt->second == nullptr)
        {
            it = m_mapServerMonsterSpawnEffects.erase(it);
            continue;
        }

        GameObject* pMonster = monIt->second;
        TransformComponent* pT = pMonster->GetTransform();
        if (pT == nullptr)
        {
            it = m_mapServerMonsterSpawnEffects.erase(it);
            continue;
        }

        fx.elapsed += deltaTime;

        float skyY = fx.groundY + fx.portalHeight;

        // 1. 포탈 대기 구간
        if (fx.elapsed < fx.portalDelay)
        {
            pT->SetPosition(fx.groundX, skyY, fx.groundZ);

            XMFLOAT3 rot = pT->GetRotation();
            pT->SetRotation(rot.x, fx.yaw, rot.z);

            ++it;
            continue;
        }

        // 2. 낙하 구간
        float fallElapsed = fx.elapsed - fx.portalDelay;
        float t = fallElapsed / fx.fallTime;

        if (t > 1.0f)
            t = 1.0f;

        float eased = t * t * (3.0f - 2.0f * t);
        float y = skyY + (fx.groundY - skyY) * eased;

        pT->SetPosition(fx.groundX, y, fx.groundZ);

        XMFLOAT3 rot = pT->GetRotation();
        pT->SetRotation(rot.x, fx.yaw, rot.z);

        // 3. 낙하 완료
        if (t >= 1.0f)
        {
            pT->SetPosition(fx.groundX, fx.groundY, fx.groundZ);

            char buf[180];
            sprintf_s(
                buf,
                sizeof(buf),
                "[Network] Monster spawn fall finished: id=%llu pos=(%.2f,%.2f,%.2f)",
                monsterId,
                fx.groundX,
                fx.groundY,
                fx.groundZ
            );
            WriteNetworkLog(buf);

            it = m_mapServerMonsterSpawnEffects.erase(it);
            continue;
        }

        ++it;
    }
}

void NetworkManager::CheckServerMonsterIdle(float deltaTime)
{
    // 서버 몬스터는 S_MONSTER_MOVE 패킷 간격이 렌더 프레임보다 느릴 수 있다.
    // 기존 IDLE_TRANSITION_TIME이 너무 짧으면 이동 중에도 walk -> idle -> walk가 반복된다.
    constexpr float SERVER_MONSTER_IDLE_TRANSITION_TIME = 0.45f;

    // 현재 클라 Transform이 서버 target 위치에 아직 도착하지 않았으면
    // MOVE 패킷이 잠깐 안 와도 이동 중으로 본다.
    constexpr float TARGET_REMAIN_DIST_SQ = 0.04f; // 0.2m

    auto IsStillMovingToTarget = [&](uint64 monsterId, GameObject* pMonster) -> bool
        {
            if (!pMonster)
                return false;

            auto tgtIt = m_mapServerMonsterTarget.find(monsterId);
            if (tgtIt == m_mapServerMonsterTarget.end() || !tgtIt->second.hasTarget)
                return false;

            TransformComponent* pT = pMonster->GetTransform();
            if (!pT)
                return false;

            XMFLOAT3 cur = pT->GetPosition();

            float dx = tgtIt->second.px - cur.x;
            float dz = tgtIt->second.pz - cur.z;
            float distSq = dx * dx + dz * dz;

            return distSq > TARGET_REMAIN_DIST_SQ;
        };

    auto PlayIdleIfNeeded = [&](uint64 monsterId, GameObject* pMonster, float fadeTime)
        {
            if (!pMonster)
                return;

            AnimationComponent* pAnim = pMonster->GetComponent<AnimationComponent>();
            if (!pAnim)
                return;

            auto clipIt = m_mapServerMonsterClips.find(monsterId);
            std::string idleClip = (clipIt != m_mapServerMonsterClips.end())
                ? clipIt->second.idle
                : "Idle";

            auto curIt = m_mapServerMonsterCurrentAnimClip.find(monsterId);
            bool needChange =
                curIt == m_mapServerMonsterCurrentAnimClip.end() ||
                curIt->second != idleClip;

            if (needChange)
            {
                pAnim->CrossFade(idleClip, fadeTime, true);
                m_mapServerMonsterCurrentAnimClip[monsterId] = idleClip;
            }
        };

    // 0. Hit flash 페이드아웃
    for (auto it = m_mapServerMonsterHitFlashTimer.begin(); it != m_mapServerMonsterHitFlashTimer.end(); )
    {
        it->second -= deltaTime;

        auto mIt = m_mapServerMonsters.find(it->first);
        if (mIt != m_mapServerMonsters.end() && mIt->second)
        {
            if (it->second > 0.0f)
            {
                float f = it->second / SERVER_MONSTER_HIT_FLASH_DURATION;
                mIt->second->SetHitFlashAll(f);
            }
            else
            {
                mIt->second->SetHitFlashAll(0.0f);
            }
        }

        if (it->second <= 0.0f)
            it = m_mapServerMonsterHitFlashTimer.erase(it);
        else
            ++it;
    }

    // 1. 공격 애니 타이머 감소
    // 공격이 끝났더라도 아직 target까지 이동 보간 중이면 idle로 돌리지 않는다.
    for (auto it = m_mapServerMonsterAttackTimer.begin(); it != m_mapServerMonsterAttackTimer.end(); )
    {
        it->second -= deltaTime;

        if (it->second <= 0.0f)
        {
            uint64 monsterId = it->first;

            auto mIt = m_mapServerMonsters.find(monsterId);
            bool bDead = (m_setDeadServerMonsters.find(monsterId) != m_setDeadServerMonsters.end());
            bool bDLDeath = false;
            if (Scene* pScn = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr)
                bDLDeath = pScn->IsNetworkDarkLordDeathTarget(monsterId);

            if (mIt != m_mapServerMonsters.end() && mIt->second && !bDead && !bDLDeath)
            {
                GameObject* pMonster = mIt->second;

                if (!IsStillMovingToTarget(monsterId, pMonster))
                {
                    PlayIdleIfNeeded(monsterId, pMonster, 0.15f);
                }
            }

            it = m_mapServerMonsterAttackTimer.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // 2. Move 이후 idle 전환 처리
    for (auto it = m_mapServerMonsterMoveTime.begin(); it != m_mapServerMonsterMoveTime.end(); )
    {
        uint64 monsterId = it->first;

        it->second += deltaTime;

        if (it->second < SERVER_MONSTER_IDLE_TRANSITION_TIME)
        {
            ++it;
            continue;
        }

        auto mIt = m_mapServerMonsters.find(monsterId);
        bool bDead = (m_setDeadServerMonsters.find(monsterId) != m_setDeadServerMonsters.end());
        bool bDLDeath = false;
        if (Scene* pScn = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr)
            bDLDeath = pScn->IsNetworkDarkLordDeathTarget(monsterId);

        if (mIt == m_mapServerMonsters.end() || !mIt->second || bDead || bDLDeath)
        {
            it = m_mapServerMonsterMoveTime.erase(it);
            continue;
        }

        GameObject* pMonster = mIt->second;

        // 공격 애니 재생 중이면 idle 전환 금지
        auto atkIt = m_mapServerMonsterAttackTimer.find(monsterId);
        bool bAttackLocked = (atkIt != m_mapServerMonsterAttackTimer.end() && atkIt->second > 0.0f);

        if (bAttackLocked)
        {
            it->second = 0.0f;
            ++it;
            continue;
        }

        // 핵심:
        // MOVE 패킷이 잠깐 안 와도, 현재 Transform이 아직 서버 target까지 보간 중이면
        // idle로 돌리면 안 된다. 이 경우 walk 유지.
        if (IsStillMovingToTarget(monsterId, pMonster))
        {
            it->second = 0.0f;
            ++it;
            continue;
        }

        // target에 거의 도착했고, 일정 시간 MOVE도 없었으면 idle 전환
        PlayIdleIfNeeded(monsterId, pMonster, 0.2f);

        it = m_mapServerMonsterMoveTime.erase(it);
    }
}

// (monsterType, attackType) → VFX 가 실제 스폰돼야 할 delay (애니 release/peak frame 기준).
//   서버 windupSec 는 데미지 타이밍을 정함. 클라 애니메이션의 "뿜기" 모션은 그것보다 빠를 수 있어,
//   windupSec 그대로 쓰면 애니가 다 끝난 뒤 VFX 가 튀는 케이스 발생.
//   여기 값은 오프라인 EnemySpawner 의 IAttackBehavior windup 값 + 애니 클립 peak frame 추정값.
static float GetVfxStartDelay(uint32 monsterType, uint32 attackType, float serverWindupSec)
{
    switch (attackType)
    {
    case 5: // Breath — 보스 입 벌리고 분사 시작 frame
        if (monsterType == 6)  return 0.4f;   // Dragon "Flame Attack"
        if (monsterType == 7)  return 0.4f;   // Kraken "Attack_Forward_RM"
        if (monsterType == 10) return 0.5f;   // BlueDragon "Fireball Shoot"
        return 0.4f;
    case 6: // MegaBreath — 충전 길게 → release. 서버 windup 2.0s 이지만 애니상 1.5s 정도가 자연스러움
        return 1.5f;
    case 7: // JumpSlam — 점프 후 착지 임팩트. 서버 windup 1.5s
        // Golem Primary: 클라 m_fWindupTime 3.35 + m_fJumpDuration 0.25 = 3.6s 임팩트
        if (monsterType == 8) return 3.6f;
        // BlueDragon은 서버가 effectPositions 착지 좌표 + delayed world circle hit로 맞춘다.
        if (monsterType == 10) return fmaxf(serverWindupSec, 0.1f);
        return 1.0f;
    case 21: // GolemJumpShock — 클라 windup 1.3 + jumpDur 1.8 = 3.1s
        if (monsterType == 8) return 3.1f;
        return fmaxf(serverWindupSec, 0.1f);
    case 22: // GolemWideSlam — 클라 windup 3.3 + jumpDur 0.3 = 3.6s
        if (monsterType == 8) return 3.6f;
        return fmaxf(serverWindupSec, 0.1f);
    case 8: // TailSweep
        if (monsterType == 7 || monsterType == 10)
            return fmaxf(serverWindupSec, 0.1f); // Kraken / BlueDragon = 0.8초
        return 0.3f;
    case 9: // GroundRupture — 콤보 첫 hit
        return 0.5f;
    case 4: // RushFront — 즉시 (이동기)
        return 0.0f;
    case 10: // FlyingBarrage — 약간 지연
        return 0.5f;
    case 2: // Ranged
        return 0.3f;

        // Kraken 전용 패턴 — 서버 delayed hit와 동일한 release timing
    case 11: // KrakenCombo 첫 타
        return (monsterType == 7) ? 0.7f : fmaxf(serverWindupSec, 0.1f);
    case 12: // SideSmash
        return (monsterType == 7) ? 1.2f : fmaxf(serverWindupSec, 0.1f);
    case 13: // WaterBurst
        return (monsterType == 7) ? 0.9f : fmaxf(serverWindupSec, 0.1f);

        // Red Dragon 추가 패턴 (서버 enum 14~20)
    case 14: // LightCombo — 첫 hit 텔레그래프
        return 0.25f;
    case 15: // HeavyCombo — 긴 텔레그래프
        return 0.4f;
    case 16: // FuryCombo — 즉발
        return 0.08f;
    case 17: // FlyingStrafe — takeOff(0.6) → 발사
        return 0.6f;
    case 18: // FlyingCircle — takeOff(0.7) → 선회 후 발사
        return 0.7f;
    case 19: // FlyingSweep — takeOff(0.5) → 직진 발사
        return 0.5f;
    case 20: // DiveBomb — takeOff(0.8) + hover(0.5)
        return 1.3f;

    default:
        // 알 수 없으면 서버 windupSec 의 절반 (안전한 절충값)
        return fmaxf(serverWindupSec * 0.5f, 0.1f);
    }
}

// (monsterType, attackType) → 인디케이터 타입/크기. 오프라인 EnemySpawner 의 m_IndicatorConfig +
//   IAttackBehavior::GetIndicatorRadius/Length override 를 시각만 미러.
//   ShouldShowHitZone() == false 인 케이스(360° spread, RushFront 등)는 type=None 으로 억제.
struct NetIndicatorParams
{
    NetworkManager::NetIndicatorType type = NetworkManager::NetIndicatorType::None;
    float radius = 0.0f;
    float length = 0.0f;   // ForwardBox 전방 길이
    XMFLOAT3 tint = XMFLOAT3(1.0f, 0.1f, 0.1f); // 기본 빨강
};

static NetIndicatorParams GetIndicatorParamsForAttack(uint32 monsterType, uint32 attackType)
{
    NetIndicatorParams p;
    switch (monsterType)
    {
    case 6: // Dragon — preset Circle r=15, JumpSlam r 7~9
        p.type = NetworkManager::NetIndicatorType::Circle;
        switch (attackType)
        {
        case 5:  p.radius = 15.0f; break;       // Breath
        case 6:  p.radius = 30.0f; break;       // MegaBreath (광역)
        case 7:  p.radius = 9.0f;  break;       // JumpSlam
        case 8:  p.radius = 12.0f; break;       // TailSweep (Circle 폴백)
        case 10: p.type = NetworkManager::NetIndicatorType::None; break;  // FlyingBarrage 억제

            // Red Dragon 추가 패턴
        case 14: // LightCombo — 90° 콘 5~6m
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 5.5f; p.length = 6.0f; break;
        case 15: // HeavyCombo — 120~150° 콘 6~7m
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 7.0f; p.length = 7.5f; break;
        case 16: // FuryCombo — 70° 좁은 콘 4.5m
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 4.5f; p.length = 5.0f; break;
        case 17: // FlyingStrafe — 비행 사격, 인디케이터 억제
        case 18: // FlyingCircle
        case 19: // FlyingSweep
        case 20: // DiveBomb
            p.type = NetworkManager::NetIndicatorType::None; break;
        default: p.radius = 12.0f; break;
        }
        break;

    case 7: // Kraken — 서버 attackType 5/8/11/12/13과 동기화
        switch (attackType)
        {
        case 5:  // Breath
        case 8:  // TailSweep
        case 11: // KrakenCombo
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 14.0f; p.length = 30.0f; break;
        case 12: // SideSmash
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 12.0f; p.length = 28.0f; break;
        case 13: // WaterBurst — 서버 원형 판정 radius 35
            p.type = NetworkManager::NetIndicatorType::Circle;
            p.radius = 35.0f; break;
        default: p.type = NetworkManager::NetIndicatorType::None; break;
        }
        break;

    case 10: // BlueDragon — Water Boss Phase1
        switch (attackType)
        {
        case 5: // Breath
            // 서버 Breath 판정은 60도 / range 80.
            // 화면상 너무 길어 보이지 않게 기존 70 유지.
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 18.0f;
            p.length = 70.0f;
            break;

        case 7: // JumpSlam
            // BlueDragon phase1 기준 서버 jumpRadius = 7.0f
            p.type = NetworkManager::NetIndicatorType::Circle;
            p.radius = 7.0f;
            break;

        case 8: // TailSweep
            // 기존 Circle은 꼬리 휘두르기와 안 맞아서 전방 박스로 변경
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 12.0f;
            p.length = 24.0f;
            break;

        case 15: // HeavyCombo
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 7.0f;
            p.length = 7.5f;
            break;

        default:
            p.type = NetworkManager::NetIndicatorType::None;
            break;
        }
        break;

    case 8: // Golem
        p.type = NetworkManager::NetIndicatorType::Circle;

        switch (attackType)
        {
        case 7:
            // Primary Slam
            p.radius = 70.0f;
            break;

        case 21:
            // GolemJumpShock — 클라 m_fSlamRadius 85
            p.type = NetworkManager::NetIndicatorType::Circle;
            p.radius = 85.0f;
            break;

        case 22:
            // GolemWideSlam — 클라 m_fSlamRadius 120
            p.type = NetworkManager::NetIndicatorType::Circle;
            p.radius = 120.0f;
            break;
        case 23:
        case 24:
        case 25:
        case 26:
            // Golem 전용 패턴은 클라 AttackBehavior 자체 인디케이터를 사용
            // 공용 네트워크 Circle/ForwardBox 인디케이터를 켜면
            // 큰 노란 원이 중복으로 남거나 맵 밖까지 표시됨
            p.type = NetworkManager::NetIndicatorType::None;
            break;

        default:
            p.radius = 42.0f;
            break;
        }
        break;

    case 9: // Demon
        switch (attackType)
        {
        case 1:  // Melee
            p.type = NetworkManager::NetIndicatorType::Circle;
            p.radius = 10.0f;
            break;

        case 4:  // 기존 RushFront
            p.type = NetworkManager::NetIndicatorType::None;
            break;

        case 27: // DemonSpinDash
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 7.0f;
            p.length = 20.0f;
            p.tint = XMFLOAT3(1.0f, 0.55f, 0.10f); // 노랑/주황
            break;

        case 28: // DemonShortRush
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 5.0f;
            p.length = 34.0f;
            p.tint = XMFLOAT3(1.0f, 0.1f, 0.1f); // 빨강
            break;

        case 29: // DemonLongRush
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 6.0f;
            p.length = 55.0f;
            p.tint = XMFLOAT3(1.0f, 0.1f, 0.1f); // 빨강
            break;

        case 30: // DemonFixatedCharge
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 6.0f;
            p.length = 120.0f;
            p.tint = XMFLOAT3(0.7f, 0.10f, 1.2f); // 보라
            break;

        case 31: // DemonTornadoField
        case 32: // DemonGaleSlash
        case 33: // DemonShockwaveRing
            p.type = NetworkManager::NetIndicatorType::None;
            break;

        case 34: // DemonJumpSlam
            p.type = NetworkManager::NetIndicatorType::Circle;
            p.radius = 16.0f;
            break;

        case 35: // DemonRageTransition
            p.type = NetworkManager::NetIndicatorType::None;
            break;

        default:
            p.type = NetworkManager::NetIndicatorType::Circle;
            p.radius = 10.0f;
            break;
        }
        break;


    default:
        p.type = NetworkManager::NetIndicatorType::None;
        break;
    }
    return p;
}

void NetworkManager::HideMonsterIndicators(ServerMonsterIndicators& ind)
{
    auto hide = [](GameObject* go) {
        if (!go) return;
        if (auto* pT = go->GetTransform())
        {
            pT->SetPosition(0.0f, -1000.0f, 0.0f);
            pT->SetScale(0.0f, 0.0f, 0.0f);
        }
        };
    hide(ind.circleBorder);
    hide(ind.circleFill);
    hide(ind.boxBorder);
    hide(ind.boxFill);
    ind.activeType = NetIndicatorType::None;
    ind.windupTimer = 0.0f;
}

void NetworkManager::UpdatePendingMonsterVFX(Scene* pScene, float deltaTime)
{
    if (m_vPendingMonsterVFX.empty()) return;

    ProjectileManager* pProj = pScene ? pScene->GetProjectileManager() : nullptr;

    for (auto it = m_vPendingMonsterVFX.begin(); it != m_vPendingMonsterVFX.end();)
    {
        it->delay -= deltaTime;
        if (it->delay <= 0.0f)
        {
            if (pProj)
            {
                if (it->kind == PendingVFXKind::Projectile)
                {
                    // 보스 실시간 위치 재조회 — windup/breath 동안 보스가 움직였어도 입에서 발사
                    DirectX::XMFLOAT3 spawnPos = it->startPos;
                    DirectX::XMFLOAT3 finalTarget = it->targetPos;
                    if (it->monsterId != 0)
                    {
                        auto mIt = m_mapServerMonsters.find(it->monsterId);
                        if (mIt != m_mapServerMonsters.end() && mIt->second)
                        {
                            if (auto* pT = mIt->second->GetTransform())
                            {
                                spawnPos = pT->GetPosition();
                                spawnPos.y += it->yOffset;

                                // 현재 위치에서 캐시된 타겟까지 forward 재계산 + fanAngle 적용
                                float dx = it->targetPos.x - spawnPos.x;
                                float dz = it->targetPos.z - spawnPos.z;
                                float len = sqrtf(dx * dx + dz * dz);
                                if (len < 0.001f) { dx = 0.f; dz = 1.f; }
                                else { dx /= len; dz /= len; }
                                float ang = it->fanAngleDeg * (3.14159265f / 180.0f);
                                float c = cosf(ang), s = sinf(ang);
                                float rdx = dx * c - dz * s;
                                float rdz = dx * s + dz * c;
                                finalTarget.x = spawnPos.x + rdx * it->fireRange;
                                finalTarget.z = spawnPos.z + rdz * it->fireRange;
                                finalTarget.y = it->targetPos.y;
                            }
                        }
                    }

                    pProj->SpawnProjectile(
                        spawnPos, finalTarget,
                        0.0f,                                  // 데미지 0 (서버 권위)
                        it->speed, it->radius, 0.0f,
                        it->element, nullptr, false,
                        it->scale, RuneCombo{}, 0.0f,
                        it->maxDist,
                        false, false, 0.f, 0.f, 0.f,
                        SkillSlot::Count);
                }
                else if (it->kind == PendingVFXKind::Explosion)
                {
                    pProj->SpawnExplosionParticles(it->startPos, it->element);
                }
            }
            // CameraShake 는 ProjectileManager 와 무관 — Scene 카메라 직접 호출
            if (it->kind == PendingVFXKind::CameraShake)
            {
                if (CCamera* pCam = pScene ? pScene->GetCamera() : nullptr)
                {
                    // FixatedCharge Dash 시작 시점에는 카메라 줌아웃 해제 후 shake
                    pCam->SetExtraOrbitDistanceTarget(0.0f);
                    pCam->StartShake(it->shakeIntensity, it->shakeDuration);
                }
            }

            // SPHBeam — 오프라인 SpawnFireWave 와 동일 EffectDef + SPH_Beam emitter
            else if (it->kind == PendingVFXKind::SPHBeam)
            {
                auto* pFluidVFX = pScene ? pScene->GetFluidVFXManager() : nullptr;
                if (pFluidVFX)
                {
                    // 보스 입 위치 — 보스가 살아있으면 실시간, 아니면 캐시된 startPos
                    DirectX::XMFLOAT3 origin = it->startPos;
                    auto mIt = m_mapServerMonsters.find(it->monsterId);
                    if (mIt != m_mapServerMonsters.end() && mIt->second && mIt->second->GetTransform())
                    {
                        DirectX::XMFLOAT3 bp = mIt->second->GetTransform()->GetPosition();
                        origin = { bp.x, bp.y + it->yOffset, bp.z };
                    }
                    // 빔 방향: origin → targetPos에 fanAngle 적용
                    float dx = it->targetPos.x - origin.x;
                    float dz = it->targetPos.z - origin.z;
                    float len = sqrtf(dx * dx + dz * dz);
                    if (len < 0.001f) { dx = 0.f; dz = 1.f; }
                    else { dx /= len; dz /= len; }
                    float ang = it->fanAngleDeg * (3.14159265f / 180.0f);
                    float c = cosf(ang), s = sinf(ang);
                    DirectX::XMFLOAT3 dir{ dx * c - dz * s, 0.f, dx * s + dz * c };

                    // EffectDef — 오프라인 MegaBreathAttackBehavior::SpawnFireWave 와 동일
                    EffectDef def;
                    def.name = "Net_MegaBreath";
                    def.element = ElementType::Fire;

                    EffectLayer layer;
                    layer.type = EmitterType::SPH_Beam;
                    layer.element = ElementType::Fire;
                    layer.coreColor = { 1.0f, 0.45f, 0.10f, 1.0f };
                    layer.edgeColor = { 0.95f, 0.35f, 0.08f, 0.95f };
                    layer.useSSF = true;

                    SPHEmitterParams& sph = layer.sph;
                    sph.particleCount = it->beamParticleCount;
                    sph.spawnRadius = (it->fanAngleDeg == 0.0f) ? 3.0f : 2.5f;
                    sph.particleSize = 1.8f;

                    VFXPhase phase;
                    phase.startTime = 0.f;
                    phase.duration = it->beamDuration + 0.5f;
                    phase.motionMode = ParticleMotionMode::Beam;
                    phase.beamDesc.beamLength = it->beamLength;
                    phase.beamDesc.spreadRadius = 6.0f * it->beamSpreadMult;
                    phase.beamDesc.speedMin = it->beamLength / (it->beamDuration * 0.7f);
                    phase.beamDesc.speedMax = phase.beamDesc.speedMin * 1.4f;
                    phase.beamDesc.swirlExpand = true;
                    phase.beamDesc.swirlSpeed = 0.6f;
                    phase.beamDesc.swirlFadeEnd = 0.f;
                    phase.beamDesc.enableFlow = true;
                    phase.beamDesc.verticalScale = 0.18f;
                    sph.phases.push_back(phase);
                    sph.maxParticleSpeed = phase.beamDesc.speedMax * 1.2f;

                    def.layers.push_back(std::move(layer));

                    pFluidVFX->SpawnEffectDef(origin, dir, def, false);
                }
            }
            it = m_vPendingMonsterVFX.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// 네트워크 일반 몬스터 공격 연출 Behavior 업데이트
void NetworkManager::UpdateNetworkNormalMonsterBehaviors(float deltaTime)
{
    for (auto it = m_vNetworkNormalMonsterBehaviors.begin();
        it != m_vNetworkNormalMonsterBehaviors.end(); )
    {
        uint64 monsterId = it->monsterId;

        // 서버 몬스터 맵에서 매 프레임 다시 조회한다.
        // raw EnemyComponent 포인터를 오래 들고 있으면 despawn / death 이후 dangling pointer가 될 수 있다.
        auto monIt = m_mapServerMonsters.find(monsterId);
        if (monIt == m_mapServerMonsters.end() || !monIt->second)
        {
            it = m_vNetworkNormalMonsterBehaviors.erase(it);
            continue;
        }

        GameObject* pMonster = monIt->second;

        // 이미 사망 처리된 몬스터는 일반 공격 연출 업데이트 중단
        if (m_setDeadServerMonsters.find(monsterId) != m_setDeadServerMonsters.end())
        {
            if (EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>())
            {
                pEnemy->HideNetworkAttackIndicator();

                if (IAttackBehavior* pBehavior = pEnemy->GetAttackBehavior())
                    pBehavior->Reset();
            }

            it = m_vNetworkNormalMonsterBehaviors.erase(it);
            continue;
        }

        EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>();
        if (!pEnemy)
        {
            it = m_vNetworkNormalMonsterBehaviors.erase(it);
            continue;
        }

        IAttackBehavior* pBehavior = pEnemy->GetAttackBehavior();
        if (!pBehavior)
        {
            pEnemy->HideNetworkAttackIndicator();
            it = m_vNetworkNormalMonsterBehaviors.erase(it);
            continue;
        }

        pBehavior->Update(deltaTime, pEnemy);

        if (pBehavior->ShouldShowHitZone())
            pEnemy->UpdateNetworkAttackIndicator(deltaTime);
        else
            pEnemy->HideNetworkAttackIndicator();

        if (pBehavior->IsFinished())
        {
            pBehavior->Reset();
            pEnemy->HideNetworkAttackIndicator();
            it = m_vNetworkNormalMonsterBehaviors.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void NetworkManager::UpdateNetworkGolemBehaviors(float deltaTime)
{
    for (auto it = m_vNetworkGolemBehaviors.begin(); it != m_vNetworkGolemBehaviors.end(); )
    {
        if (it->behavior == nullptr || it->owner == nullptr)
        {
            it = m_vNetworkGolemBehaviors.erase(it);
            continue;
        }

        it->timer += deltaTime;

        it->behavior->Update(deltaTime, it->owner);

        if (it->behavior->IsFinished() || it->timer > 8.0f)
        {
            it->behavior->Reset();
            it = m_vNetworkGolemBehaviors.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void NetworkManager::UpdateServerMonsterIndicators(float deltaTime)
{
    for (auto it = m_mapServerMonsterIndicators.begin(); it != m_mapServerMonsterIndicators.end(); ++it)
    {
        ServerMonsterIndicators& ind = it->second;
        if (ind.activeType == NetIndicatorType::None) continue;

        uint32 indicatorMonsterType = 0;

        auto clipIt = m_mapServerMonsterClips.find(it->first);
        if (clipIt != m_mapServerMonsterClips.end())
        {
            indicatorMonsterType = clipIt->second.monsterType;
        }

        const bool isGolemIndicator = (indicatorMonsterType == 8);

        // 사망/Despawn 후엔 hide 만 해주고 패스
        if (m_setDeadServerMonsters.find(it->first) != m_setDeadServerMonsters.end())
        {
            HideMonsterIndicators(ind);
            continue;
        }

        // 보스 현재 transform 추적해서 인디케이터를 보스 위치에 부착 (보스가 움직이는 동안 따라옴)
        auto mIt = m_mapServerMonsters.find(it->first);
        if (mIt != m_mapServerMonsters.end() && mIt->second)
        {
            if (auto* pT = mIt->second->GetTransform())
            {
                XMFLOAT3 pos = pT->GetPosition();
                ind.anchorX = pos.x; ind.anchorZ = pos.z;
                ind.anchorY = pos.y;
                if (ind.activeType == NetIndicatorType::ForwardBox && !ind.yawLocked)
                    ind.yawDeg = pT->GetRotation().y;
            }
        }

        ind.windupTimer += deltaTime;
        float fillProgress = (ind.windupTotal > 0.0f) ? (ind.windupTimer / ind.windupTotal) : 1.0f;
        if (fillProgress > 1.0f) fillProgress = 1.0f;

        const float indY = ind.anchorY + 1.2f;

        if (ind.activeType == NetIndicatorType::Circle)
        {
            // 작은 반경도 잘 보이게 최소값 보장 (전투 중 묻히지 않도록)
            float fullR = (ind.hitRadius < 7.0f) ? 7.0f : ind.hitRadius;
            // border (테두리) — 고정 외곽, 두께 키움 (1.03 → 1.12)
            if (ind.circleBorder)
            {
                if (auto* pT = ind.circleBorder->GetTransform())
                {
                    pT->SetPosition(ind.anchorX, indY + 0.05f, ind.anchorZ);
                    float borderR = fullR;
                    pT->SetScale(borderR, 1.0f, borderR);

                    MATERIAL mat;

                    if (isGolemIndicator)
                    {
                        // Golem은 오프라인 EnemyComponent 기본 Circle indicator 색감에 맞춤.
                        // 기존 네트워크 공용 색은 emissive/alpha가 너무 강해서 원본보다 과하게 밝아 보였음.
                        mat.m_cAmbient = XMFLOAT4(0.20f, 0.04f, 0.02f, 1.0f);
                        mat.m_cDiffuse = XMFLOAT4(1.00f, 0.20f, 0.10f, 0.60f);
                        mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                        mat.m_cEmissive = XMFLOAT4(0.55f, 0.11f, 0.05f, 1.0f);
                    }
                    else
                    {
                        mat.m_cAmbient = XMFLOAT4(0.6f, 0.02f, 0.02f, 1.0f);
                        mat.m_cDiffuse = XMFLOAT4(1.0f, 0.15f, 0.1f, 1.0f);
                        mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                        mat.m_cEmissive = XMFLOAT4(3.5f, 0.4f, 0.15f, 1.0f);
                    }

                    ind.circleBorder->SetMaterial(mat);
                }
            }
            // fill — 항상 full 반경으로 표시되되 emissive 가 진행도에 따라 밝아짐.
            //   기존엔 progress=0 일 때 사실상 안 보였음 → 처음부터 위험 영역이 보이도록 변경.
            if (ind.circleFill)
            {
                if (auto* pT = ind.circleFill->GetTransform())
                {
                    pT->SetPosition(ind.anchorX, indY, ind.anchorZ);

                    float fillR = fullR * fillProgress;
                    if (fillR < 0.01f) fillR = 0.01f;

                    pT->SetScale(fillR, 1.0f, fillR);

                    MATERIAL mat;

                    if (isGolemIndicator)
                    {
                        // Golem fill은 오프라인 기본 fill처럼 주황 계열로 차오르되,
                        // 과한 노랑/발광을 줄인다.
                        float alpha = 0.40f + 0.40f * fillProgress;
                        float emit = 0.45f + 0.80f * fillProgress;

                        mat.m_cAmbient = XMFLOAT4(0.20f, 0.06f, 0.02f, 1.0f);
                        mat.m_cDiffuse = XMFLOAT4(1.00f, 0.30f, 0.08f, alpha);
                        mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                        mat.m_cEmissive = XMFLOAT4(emit, emit * 0.30f, emit * 0.08f, 1.0f);
                    }
                    else
                    {
                        mat.m_cAmbient = XMFLOAT4(0.3f, 0.02f, 0.0f, 1.0f);
                        mat.m_cDiffuse = XMFLOAT4(1.0f, 0.2f + 0.6f * fillProgress, 0.05f, 1.0f);
                        mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                        mat.m_cEmissive = XMFLOAT4(
                            0.5f + 2.0f * fillProgress,
                            0.1f + 1.4f * fillProgress,
                            0.05f,
                            1.0f
                        );
                    }

                    ind.circleFill->SetMaterial(mat);
                }
            }
        }
        else if (ind.activeType == NetIndicatorType::ForwardBox)
        {
            // 작은 박스도 잘 보이게 최소 반폭/길이 보장
            float fHalfW = (ind.hitRadius < 5.0f) ? 5.0f : ind.hitRadius;
            float fLen = (ind.hitLength < 10.0f) ? 10.0f : ind.hitLength;
            float yawRad = ind.yawDeg * (3.14159265f / 180.0f);
            float fwdX = sinf(yawRad);
            float fwdZ = cosf(yawRad);
            float centerX = ind.anchorX + fwdX * (fLen * 0.5f);
            float centerZ = ind.anchorZ + fwdZ * (fLen * 0.5f);

            if (ind.boxBorder)
            {
                if (auto* pT = ind.boxBorder->GetTransform())
                {
                    pT->SetPosition(centerX, indY + 0.02f, centerZ);
                    pT->SetRotation(0.0f, ind.yawDeg, 0.0f);

                    // 외곽은 항상 full 길이/너비로 표시한다.
                    pT->SetScale(fHalfW * 2.0f * 1.12f, 1.0f, fLen * 1.12f);

                    MATERIAL mat;

                    // boxBorder는 항상 공격 타입 색상 사용
                    // 28/29는 GetIndicatorParamsForAttack()에서 tint가 빨강이므로 빨간 외곽이 된다.
                    mat.m_cAmbient = XMFLOAT4(0.4f * ind.tint.x, 0.4f * ind.tint.y, 0.4f * ind.tint.z, 1.0f);
                    mat.m_cDiffuse = XMFLOAT4(ind.tint.x, ind.tint.y, ind.tint.z, 1.0f);
                    mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                    mat.m_cEmissive = XMFLOAT4(3.0f * ind.tint.x, 3.0f * ind.tint.y, 3.0f * ind.tint.z, 1.0f);

                    ind.boxBorder->SetMaterial(mat);
                }
            }

            // fill — 28/29는 빨간 사각형 안에 노란색이 전방으로 차오르게 표시
            if (ind.boxFill)
            {
                if (auto* pT = ind.boxFill->GetTransform())
                {
                    float fillLen = fLen;

                    if (ind.attackType == 28 || ind.attackType == 29)
                    {
                        fillLen = fLen * fillProgress;
                        if (fillLen < 0.01f)
                            fillLen = 0.01f;
                    }

                    float fillCenterX = ind.anchorX + sinf(yawRad) * (fillLen * 0.5f);
                    float fillCenterZ = ind.anchorZ + cosf(yawRad) * (fillLen * 0.5f);

                    // fill은 border보다 살짝 낮게 해서 빨간 외곽이 덮이지 않게 한다.
                    pT->SetPosition(fillCenterX, indY - 0.02f, fillCenterZ);
                    pT->SetRotation(0.0f, ind.yawDeg, 0.0f);
                    pT->SetScale(fHalfW * 1.75f, 1.0f, fillLen);

                    MATERIAL mat;
                    float emitMul = 0.8f + 1.8f * fillProgress;

                    if (ind.attackType == 28 || ind.attackType == 29)
                    {
                        // Demon ShortRush / LongRush: 노란색 fill
                        mat.m_cAmbient = XMFLOAT4(0.35f, 0.28f, 0.03f, 1.0f);
                        mat.m_cDiffuse = XMFLOAT4(1.0f, 0.75f, 0.05f, 1.0f);
                        mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                        mat.m_cEmissive = XMFLOAT4(emitMul * 1.0f, emitMul * 0.75f, emitMul * 0.05f, 1.0f);
                    }
                    else
                    {
                        // 나머지는 기존처럼 공격 타입 색상 fill
                        mat.m_cAmbient = XMFLOAT4(0.25f * ind.tint.x, 0.25f * ind.tint.y, 0.25f * ind.tint.z, 1.0f);
                        mat.m_cDiffuse = XMFLOAT4(ind.tint.x, ind.tint.y, ind.tint.z, 1.0f);
                        mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                        mat.m_cEmissive = XMFLOAT4(emitMul * ind.tint.x, emitMul * ind.tint.y, emitMul * ind.tint.z, 1.0f);
                    }

                    ind.boxFill->SetMaterial(mat);
                }
            }
        }

        // windup 종료 → hide
        if (ind.windupTimer >= ind.windupTotal)
            HideMonsterIndicators(ind);
    }
}

// (monsterType, attackType) → 재생할 애니 클립 매핑.
//   서버는 attackType (1=Melee,2=Ranged,4=RushFront,5=Breath,6=MegaBreath,7=JumpSlam,
//   8=TailSweep,9=GroundRupture,10=FlyingBarrage) 만 보내므로 보스별 실제 클립 이름은 클라가 결정.
//   오프라인 EnemySpawner.cpp 의 IAttackBehavior 인스턴스가 사용하는 클립과 동일하게 맞춤.
//   매핑 누락 시 nullptr → preset 기본 attack 클립으로 폴백.
static const char* GetMonsterAttackClipForType(uint32 monsterType, uint32 attackType)
{
    // monsterType: 6 Dragon, 7 Kraken, 8 Golem, 9 Demon, 10 BlueDragon, 11 DarkLord
    switch (monsterType)
    {
    case 6:  // Dragon (Red) — 보유 클립 한정 (Flame Attack / Tail Attack / Walk / Idle01 / Get Hit / Die / Scream)
        switch (attackType)
        {
        case 5:  return "Flame Attack";       // Breath
        case 6:  return "Flame Attack";       // MegaBreath
        case 7:  return "Flame Attack";       // JumpSlam (점프 클립 부재)
        case 8:  return "Tail Attack";        // TailSweep
        case 10: return "Flame Attack";       // FlyingBarrage

            // Red Dragon 추가 패턴 — Tail Attack 으로 시각 차별화 (휘두름 모션이 콤보/돌진과 어울림)
        case 14: return "Tail Attack";        // LightCombo (3-hit 휘두름)
        case 15: return "Tail Attack";        // HeavyCombo (2-hit 강한 휘두름)
        case 16: return "Tail Attack";        // FuryCombo (5-hit 폭주 휘두름)
        case 17: return "Flame Attack";       // FlyingStrafe (사격이라 화염)
        case 18: return "Flame Attack";       // FlyingCircle (선회 사격)
        case 19: return "Flame Attack";       // FlyingSweep (스윕 사격)
        case 20: return "Flame Attack";       // DiveBomb (다이브 + 화염)
        default: return "Flame Attack";
        }
    case 7:  // Kraken
        switch (attackType)
        {
        case 5:  return "Attack_Forward_RM";              // Breath
        case 8:  return "Sweep_Attack";                   // TailSweep
        case 11:  return "Sweep_Smash_Attack_3_HIt_Combo"; // KrakenCombo라면
        case 12: return "Attack_Forward_RM";              // SideSmash
        case 13: return "Unreal Take";                    // WaterBurst
        default: return "Attack_Forward_RM";
        }
    case 8:  // Golem — Anim: Golem_battle_attack01_ge / attack02 / jump_ge
        switch (attackType)
        {
        case 7:  return "Golem_battle_attack01_ge";   // JumpSlam (내려찍기)
        case 9:  return "Golem_battle_attack02_ge";   // GroundRupture / RockBarrage / RockFall (팔 휘두르기)
        case 8:  return "Golem_battle_attack02_ge";   // TailSweep 도 attack02 로 처리
        case 21: return "Golem_jump_ge";              // GolemJumpShock
        case 22: return "Golem_battle_attack01_ge";   // GolemWideSlam
        case 23: return "Golem_battle_attack02_ge";   // GolemRockBarrage
        case 24: return "Golem_battle_attack02_ge";   // GolemRockFall
        case 25: return "Golem_battle_attack02_ge";   // GolemGroundRupture
        case 26: return "Golem_battle_attack02_ge";   // GolemSequentialCross
        default: return "Golem_battle_attack01_ge";
        }
    case 9:  // Demon
        switch (attackType)
        {
        case 1:  return "attack1";
        case 4:  return "Run";
        case 27: return "attack3"; // SpinDash
        case 28: return "Run";     // ShortRush
        case 29: return "Run";     // LongRush
        case 30: return "Run";     // FixatedCharge
        case 31: return "attack2"; // TornadoField
        case 32: return "attack4"; // GaleSlash
        case 33: return "attack1"; // ShockwaveRing
        case 34: return "attack4"; // JumpSlam
        case 35: return "Rage";    // RageTransition
        default: return "attack1";
        }
    case 10: // BlueDragon — Anim: Fireball Shoot / Tail Attack / Run
        switch (attackType)
        {
        case 5:  return "Fireball Shoot"; // Breath
        case 4:  return "Run";            // RushFront
        case 8:  return "Tail Attack";    // TailSweep
        case 7:  return "Tail Attack";    // JumpSlam 대체
        case 15: return "Tail Attack";    // HeavyCombo
        default: return "Fireball Shoot";
        }
    case 11: // DarkLord
        switch (attackType)
        {
        case 40:
            return "attack2"; // DarkLordSigilSlash
        case 41:
            return "attack9"; // DarkLordSigilField
        case 42:
            return "attack9"; // DarkLordSwordRain
        case 43:
            return "attack9"; // DarkLordSwordSeal
        default:
            return "attack2";
        }
    default:
        return nullptr;  // 일반 몹 → preset 기본
    }
}

static const char* GetMonsterHitReactClip(uint32 monsterType)
{
    switch (monsterType)
    {
    case 2: // AirElemental
    case 3: // RangedEnemy
    case 4: // RushAoEEnemy
    case 5: // RushFrontEnemy
        return "Combat_Stun";

    case 6:  return "Get Hit"; // Dragon
    case 7:  return "Hit";     // Kraken
    case 8:  return "Golem_battle_harddamage_ge";
    case 9:  return "Hit";     // Demon, 실제 클립 다르면 나중에 교체
    case 10: return "Hit";     // BlueDragon, 실제 클립 다르면 나중에 교체

    default:
        return "Combat_Stun";
    }
}

void NetworkManager::ProcessMonsterAttack(Scene* pScene, uint64 monsterId, uint32 attackType, float windupSec, uint64 targetPlayerId, float atkX, float atkY, float atkZ, float monsterYaw, const std::vector<DirectX::XMFLOAT3>& effectPositions, uint32 effectOption)
{
    auto it = m_mapServerMonsters.find(monsterId);
    if (it == m_mapServerMonsters.end())
    {
        char buf[128];
        sprintf_s(buf, "[Network] ProcessMonsterAttack: unknown monsterId=%llu", monsterId);
        WriteNetworkLog(buf);
        return;
    }

    // 이미 사망한 몬스터는 공격 애니 재생 skip (death 유지)
    if (m_setDeadServerMonsters.find(monsterId) != m_setDeadServerMonsters.end())
        return;

    // DarkLord 사망 연출 중에는 잔여 attack 패킷이 들어와도 무시 (공격 클립이
    //   Death 클립을 덮어쓰면 보스가 다시 일어나서 공격하는 것처럼 보임).
    {
        Scene* pScn = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
        if (pScn && pScn->IsNetworkDarkLordDeathTarget(monsterId))
            return;
    }

    GameObject* pMonster = it->second;
    auto* pAnim = pMonster->GetComponent<AnimationComponent>();
    if (!pAnim) return;

    // DarkLord 검기/투사체/cone 방향은 보스 transform yaw 기반으로 spawn 된다.
    //   S_MONSTER_MOVE 의 보간 yaw 가 attack 시점 server yaw 와 어긋나면
    //   TwinCleave/CrossSigil/Projectile 방향이 잘못 튄다. 서버가 보낸
    //   monsterYaw 로 attack 직전 스냅해서 동기화. 아울러 보간 타겟 yaw 도
    //   같이 갱신해서 InterpolateServerMonsters 가 스냅된 yaw 를 되돌리지 않게 한다.
    {
        auto clipIt2 = m_mapServerMonsterClips.find(monsterId);
        uint32 mtSnap = (clipIt2 != m_mapServerMonsterClips.end()) ? clipIt2->second.monsterType : 0;
        if (mtSnap == 7 || mtSnap == 10 || mtSnap == 11)
        {
            if (auto* pT = pMonster->GetTransform())
            {
                DirectX::XMFLOAT3 r = pT->GetRotation();
                pT->SetRotation(r.x, monsterYaw, r.z);
            }
            auto tgtIt = m_mapServerMonsterTarget.find(monsterId);
            if (tgtIt != m_mapServerMonsterTarget.end())
                tgtIt->second.yaw = monsterYaw;
        }
    }

    // (monsterType, attackType) 별 클립 우선 사용. 매핑 없으면 preset 의 기본 attack 클립으로 폴백.
    auto clipIt = m_mapServerMonsterClips.find(monsterId);
    uint32 mt = (clipIt != m_mapServerMonsterClips.end()) ? clipIt->second.monsterType : 0;
    const char* perTypeClip = GetMonsterAttackClipForType(mt, attackType);
    const char* attackClip =
        (perTypeClip && perTypeClip[0] != '\0') ? perTypeClip
        : ((clipIt != m_mapServerMonsterClips.end() && !clipIt->second.attack.empty())
            ? clipIt->second.attack.c_str() : "Attack");

    // Demon 돌진 패턴은 dash 지속시간(0.7~1.9s)이 단일 클립 길이보다 길어서
    //   loop=false 로 재생하면 1사이클 후 마지막 프레임에 멈춰 보임 → 슬라이딩.
    //   Run 계열(28/29/30) + SpinDash(27 attack3) 는 dash 동안 루프해야 한다.
    bool bLoopAttack = false;
    if (mt == 9 && (attackType == 27 || attackType == 28 || attackType == 29 || attackType == 30))
    {
        bLoopAttack = true;
    }
    // 보스 (Demon=9, DarkLord=11) 는 블렌드 시간 0.08 → 0.15 로 늘려 모션 끊김 완화.
    //   너무 짧은 블렌드는 직전 포즈에서 신규 공격 포즈로 "딱" 전환되어 급해 보임.
    float blendDur = ((mt == 9) || (mt == 11)) ? 0.15f : 0.08f;
    pAnim->CrossFade(attackClip, blendDur, bLoopAttack, true);
    m_mapServerMonsterCurrentAnimClip[monsterId] = attackClip;

    // 오프라인 EnemySpawner 의 JumpSlamAttackBehavior 가 SetPlaybackSpeed 로
    //   Golem 슬램 클립을 느리게 재생함. 네트워크 경로도 동일하게 맞춰야
    //   임팩트 프레임이 인디케이터 100% 와 일치함.
    if (mt == 8)
    {
        float playbackSpeed = 1.0f;
        switch (attackType)
        {
        case 7:  playbackSpeed = 0.7f;  break;   // Primary Slam (attack01)
        case 21: playbackSpeed = 0.7f;  break;   // JumpShock (jump)
        case 22: playbackSpeed = 0.65f; break;   // WideSlam (attack01)
        default: playbackSpeed = 1.0f;  break;
        }
        pAnim->SetPlaybackSpeed(playbackSpeed);
    }
    else
    {
        // 다른 몬스터는 기본 속도. 이전 골렘 공격에서 변경된 값이 남지 않도록 명시 복원
        pAnim->SetPlaybackSpeed(1.0f);
    }

    // 공격 애니 지속 시간 등록 — 이 기간 Move 왔을 때 walk 로 덮지 않음
    //  서버 windupSec(예고) + 추정 재생시간. 짧은 windup 공격도 최소 ATTACK_ANIM_LOCK 은 유지
    float lockDur = fmaxf(windupSec + 0.4f, ATTACK_ANIM_LOCK);

    // Golem은 공격 모션이 길어서 서버 windupSec 기준으로 idle 복귀하면 모션이 중간에 끊김
    //   오프라인 JumpSlamAttackBehavior 의 windup + jumpDur + recovery 합과 동기화 (재생속도 보정 후)
    if (mt == 8)
    {
        switch (attackType)
        {
        case 7: // Primary Slam (windup 3.35 + jumpDur 0.25 + recovery 1.3 / 0.7배속 ≈ 약 5.0s)
            lockDur = 5.0f;
            break;

        case 21: // GolemJumpShock (windup 1.3 + jumpDur 1.8 + recovery 0.7 = 3.8s)
            lockDur = 3.8f;
            break;

        case 22: // GolemWideSlam (windup 3.3 + jumpDur 0.3 + recovery 1.8 / 0.65배속 ≈ 약 5.4s)
            lockDur = 5.4f;
            break;

        case 23: // GolemRockBarrage
        case 24: // GolemRockFall
        case 25: // GolemGroundRupture
        case 26: // GolemSequentialCross
            lockDur = 4.2f;
            break;
        }
    }

    // Kraken도 일부 공격 모션이 길어서 idle 복귀가 빠르면 중간에 끊김
    if (mt == 7)
    {
        switch (attackType)
        {
        case 5:  // Breath / 잉크 발사
            lockDur = 2.2f;
            break;

        case 8:  // TailSweep
            lockDur = 2.0f;
            break;

        case 11: // KrakenCombo
            lockDur = 3.5f;
            break;

        case 12: // SideSmash
            lockDur = 2.5f;
            break;

        case 13: // WaterBurst
            lockDur = 2.8f;
            break;
        }
    }

    // Demon도 공격 모션별로 idle 복귀 타이밍 보정
    if (mt == 9)
    {
        switch (attackType)
        {
        case 27: lockDur = 1.8f; break; // SpinDash
        case 28: lockDur = 1.5f; break; // ShortRush
        case 29: lockDur = 1.8f; break; // LongRush
        case 30: lockDur = 3.8f; break; // FixatedCharge
        case 31: lockDur = 2.8f; break; // TornadoField
        case 32: lockDur = 2.3f; break; // GaleSlash
        case 33: lockDur = 2.4f; break; // ShockwaveRing
        case 34: lockDur = 2.5f; break; // JumpSlam
        case 35: lockDur = 2.6f; break; // RageTransition
        }
    }

    if (mt == 9 && attackType == 30)
    {
        if (pScene && pScene->GetCamera())
        {
            pScene->GetCamera()->SetExtraOrbitDistanceTarget(28.0f);
        }
    }

    // Demon FixatedCharge Dash 시작 시점 — 카메라 pull-back 해제 + 출발 shake 예약
    if (mt == 9 && attackType == 30)
    {
        PendingMonsterVFX p;
        p.kind = PendingVFXKind::CameraShake;
        p.delay = windupSec + 0.2f;
        p.shakeIntensity = 0.6f;
        p.shakeDuration = 0.25f;

        m_vPendingMonsterVFX.push_back(p);
    }

    m_mapServerMonsterAttackTimer[monsterId] = lockDur;
    // 공격 중엔 idle 전환 억제
    m_mapServerMonsterMoveTime.erase(monsterId);

    // 인디케이터 활성화 (windupSec > 0 이고 보스로 등록된 경우만)
    auto indIt = m_mapServerMonsterIndicators.find(monsterId);
    if (indIt != m_mapServerMonsterIndicators.end() && windupSec > 0.05f)
    {
        ServerMonsterIndicators& ind = indIt->second;
        NetIndicatorParams params = GetIndicatorParamsForAttack(mt, attackType);
        if (params.type != NetIndicatorType::None)
        {
            HideMonsterIndicators(ind);   // 이전 인디케이터 정리
            ind.activeType = params.type;
            ind.windupTotal = windupSec;
            ind.windupTimer = 0.0f;
            ind.hitRadius = params.radius;
            ind.hitLength = params.length;
            ind.tint = params.tint;
            ind.attackType = attackType;
            ind.anchorX = atkX; ind.anchorY = atkY; ind.anchorZ = atkZ;
            ind.yawLocked = false;
            // ForwardBox: 보스의 현재 yaw 사용 (transform 에서 직접 읽기).
            //   windup 동안 보스가 타겟 추적하며 회전하면 인디케이터도 같이 돌아야 한다.
            //   UpdateServerMonsterIndicators 가 매 프레임 보스 yaw 추적.
            if (ind.activeType == NetIndicatorType::ForwardBox)
            {
                if (auto* pT = pMonster->GetTransform())
                    ind.yawDeg = pT->GetRotation().y;
            }
        }
        else
        {
            HideMonsterIndicators(ind);
        }
    }

    // ── attackType 별 비쥬얼 (VFX/투사체) — 데미지 0, 서버 권위 ──
    //   오프라인 IAttackBehavior 처럼 windupSec 동안 인디케이터/wind-up 애니만 보이고,
    //   실제 발사는 windupSec 후부터 breathDuration 동안 staggered. delay 큐로 재현.
    {
        // 몬스터 입/포구 시작 위치 (몸통 위쪽)
        XMFLOAT3 startPos{ atkX, atkY + 2.0f, atkZ };

        // 타겟 플레이어 위치 (로컬 or 원격) — 없으면 yaw 방향으로 fallback
        XMFLOAT3 targetPos = startPos;
        bool bHasTarget = false;
        uint64 localId = GetLocalPlayerId();
        if (targetPlayerId == localId)
        {
            if (GameObject* pLocal = pScene ? pScene->GetPlayer() : nullptr)
                if (auto* pT = pLocal->GetTransform())
                {
                    targetPos = pT->GetPosition(); targetPos.y += 1.5f; bHasTarget = true;
                }
        }
        else
        {
            auto rIt = m_mapRemotePlayers.find(targetPlayerId);
            if (rIt != m_mapRemotePlayers.end() && rIt->second)
                if (auto* pT = rIt->second->GetTransform())
                {
                    targetPos = pT->GetPosition(); targetPos.y += 1.5f; bHasTarget = true;
                }
        }
        if (!bHasTarget)
        {
            // 타겟 못 잡으면 monster 정면(+Z) 으로 fallback — 적어도 화염은 나오게
            targetPos = { atkX, atkY + 1.5f, atkZ + 10.0f };
        }

        // monsterType → 원소
        ElementType elem = ElementType::Fire;
        if (mt == 7 || mt == 10) elem = ElementType::Water;
        else if (mt == 8)        elem = ElementType::Earth;

        // 부채꼴 staggered 큐잉 — 첫 발사 = startDelay, 이후 fireInterval 간격으로 N 발.
        //   각 발사 항목에 monsterId + fanAngleDeg 저장 → 실제 스폰 시점에 보스 현재 위치/yaw 로 재계산.
        //   결과: 보스가 windup/breath 동안 움직여도 VFX 가 보스 입에서 정확히 발사됨.
        auto QueueFan = [&](int count, float spreadDeg, float speed,
            float radius, float scale, float maxDist,
            float startDelay, float dur)
            {
                float halfSpread = (count > 1) ? (spreadDeg * 0.5f) : 0.0f;
                float fireInterval = (count > 0) ? (dur / (float)count) : 0.0f;

                for (int i = 0; i < count; ++i)
                {
                    float t = (count > 1) ? ((float)i / (count - 1)) : 0.5f;
                    float angDeg = -halfSpread + spreadDeg * t;

                    PendingMonsterVFX p;
                    p.kind = PendingVFXKind::Projectile;
                    p.delay = startDelay + i * fireInterval;
                    p.monsterId = monsterId;          // 실시간 위치 추적
                    p.startPos = startPos;           // fallback (보스 사라지면 사용)
                    p.targetPos = targetPos;          // 패킷 시점 타겟 위치
                    p.yOffset = 2.0f;
                    p.fanAngleDeg = angDeg;
                    p.fireRange = 60.0f;
                    p.speed = speed;
                    p.radius = radius;
                    p.scale = scale;
                    p.maxDist = maxDist;
                    p.element = elem;
                    m_vPendingMonsterVFX.push_back(p);
                }
            };

        auto QueueExplosion = [&](const XMFLOAT3& pos, float delaySec)
            {
                PendingMonsterVFX p;
                p.kind = PendingVFXKind::Explosion;
                p.delay = delaySec;
                p.startPos = pos;
                p.element = elem;
                m_vPendingMonsterVFX.push_back(p);
            };

        auto QueueShake = [&](float delaySec, float intensity, float duration)
            {
                PendingMonsterVFX p;
                p.kind = PendingVFXKind::CameraShake;
                p.delay = delaySec;
                p.shakeIntensity = intensity;
                p.shakeDuration = duration;
                m_vPendingMonsterVFX.push_back(p);
            };

        // DarkLord 최종보스 패턴 라우팅.
 // 서버는 attackType/effectOption/effectPositions만 보내고,
 // 클라는 기존 DarkLord Behavior 연출만 재생한다.
 // 실제 데미지는 서버 권위.
        if (mt == 11)
        {
            EnemyComponent* pEC = pMonster->GetComponent<EnemyComponent>();

            if (!pEC)
            {
                WriteNetworkLog("[Network] DarkLord attack failed: EnemyComponent missing");
                return;
            }

            std::unique_ptr<IAttackBehavior> pBehavior;

            if (attackType == 40) // DarkLordSigilSlash
            {
                pBehavior = MakeNetworkDarkLordSigilSlash(effectOption);

                char buf[256];
                uint32 style = DecodeDarkLordStyle(effectOption);
                uint32 elemCode = effectOption % 10;

                sprintf_s(buf,
                    "[Network] DarkLordSigilSlash received monsterId=%llu effectOption=%u elem=%u style=%u",
                    monsterId,
                    effectOption,
                    elemCode,
                    style);
                WriteNetworkLog(buf);
            }
            else if (attackType == 41) // DarkLordSigilField
            {
                pBehavior = MakeNetworkDarkLordSigilField(effectOption, effectPositions);

                char buf[256];
                sprintf_s(buf,
                    "[Network] DarkLordSigilField received monsterId=%llu effectOption=%u count=%zu",
                    monsterId,
                    effectOption,
                    effectPositions.size());
                WriteNetworkLog(buf);
            }
            else if (attackType == 42) // DarkLordSwordRain
            {
                pBehavior = MakeNetworkDarkLordSwordRain(effectOption, effectPositions);

                char buf[256];
                sprintf_s(buf,
                    "[Network] DarkLordSwordRain received monsterId=%llu effectOption=%u count=%zu",
                    monsterId,
                    effectOption,
                    effectPositions.size());
                WriteNetworkLog(buf);
            }
            else if (attackType == 43) // DarkLordSwordSeal
            {
                pBehavior = MakeNetworkDarkLordSwordSeal(effectOption);

                uint32 style = DecodeDarkLordStyle(effectOption);
                uint32 elemCode = effectOption % 10;
                bool isFinalStyle = (style >= 1);

                char buf[256];
                sprintf_s(buf,
                    "[Network] DarkLordSwordSeal received monsterId=%llu effectOption=%u elem=%u style=%u final=%d",
                    monsterId,
                    effectOption,
                    elemCode,
                    style,
                    isFinalStyle ? 1 : 0);
                WriteNetworkLog(buf);
            }
            else
            {
                char buf[256];
                sprintf_s(buf,
                    "[Network] DarkLord unknown attackType=%u monsterId=%llu",
                    attackType,
                    monsterId);
                WriteNetworkLog(buf);
                return;
            }

            if (!pBehavior)
                return;

            // DarkLord는 EnemyComponent::UpdateAttack()이 직접 업데이트한다.
            // m_bAIPaused 상태여도 Attack 상태일 때는 Update가 허용되도록 EnemyComponent.cpp에서 처리.
            pEC->DebugForceSpecialAttack(std::move(pBehavior));

            return;
        }

        // 일반 몬스터 공격 라우팅
        // 서버 권위 일반 몬스터는 attackType 기준으로 클라 Behavior 연출만 실행한다.
        // 1~4: 기존 일반 공격, 36~39: B방식 추가 변종 공격
        bool bNormalMonster =
            (mt == 2 || mt == 3 || mt == 4 || mt == 5);

        bool bNormalMonsterAttack =
            (attackType == 1 ||
                attackType == 2 ||
                attackType == 3 ||
                attackType == 4 ||
                attackType == 36 ||
                attackType == 37 ||
                attackType == 38 ||
                attackType == 39);

        if (bNormalMonster && bNormalMonsterAttack)
        {
            PlayNetworkNormalMonsterAttackBehavior(
                pScene,
                pMonster,
                monsterId,
                mt,
                attackType,
                targetPlayerId
            );
            return;
        }

        // VFX 가 실제 스폰될 delay — 서버 windupSec 가 아닌 애니 release frame 기준 (sync 문제 해결).
        //   서버 windupSec 는 데미지 타이밍이고 클라 애니 peak 와 다름. 위 GetVfxStartDelay 참고.
        const float startDelay = GetVfxStartDelay(mt, attackType, windupSec);

        // [DEBUG] Red Dragon 공격 패턴 클라 수신 로그
        if (mt == 6)
        {
            char dbg[256];
            sprintf_s(dbg,
                "[CLIENT][RedDragonAttack RECV] monsterId=%llu attackType=%u targetPlayerId=%llu windup=%.2f startDelay=%.2f clip=%s pos=(%.1f,%.1f,%.1f) effectCount=%zu option=%u",
                monsterId,
                attackType,
                targetPlayerId,
                windupSec,
                startDelay,
                attackClip ? attackClip : "null",
                atkX, atkY, atkZ,
                effectPositions.size(),
                effectOption
            );
            WriteNetworkLog(dbg);
        }

        switch (attackType)
        {
        case 1:
            // 일반 근접 몬스터는 클라 오프라인 MeleeAttackBehavior를 연출 전용으로 실행한다.
            // 실제 데미지는 서버 권위이며, 클라는 원형 telegraph/애니메이션만 담당한다.
            if (mt == 2)
            {
                PlayNetworkNormalMonsterAttackBehavior(
                    pScene,
                    pMonster,
                    monsterId,
                    mt,
                    attackType,
                    targetPlayerId
                );
                return;
            }
            break;

        case 2:
            // RangedEnemy는 클라 오프라인 RangedAttackBehavior를 연출 전용으로 실행한다.
            // 실제 데미지는 서버 권위이며, 클라는 투사체 VFX만 담당한다.
            if (mt == 3)
            {
                PlayNetworkNormalMonsterAttackBehavior(
                    pScene,
                    pMonster,
                    monsterId,
                    mt,
                    attackType,
                    targetPlayerId
                );
                return;
            }
            break;

        case 3:
        case 4:
            // 일반 몬스터 Rush 계열은 클라 오프라인 AttackBehavior를 연출 전용으로 실행한다.
            // 실제 이동/데미지는 서버 권위이며, 클라는 telegraph/애니메이션만 담당한다.
            if (mt == 4 || mt == 5)
            {
                WriteNetworkLog("[Network] ProcessMonsterAttack normal rush case ENTER");

                PlayNetworkNormalMonsterAttackBehavior(
                    pScene,
                    pMonster,
                    monsterId,
                    mt,
                    attackType,
                    targetPlayerId
                );
                return;
            }
            break;

        case 5:   // Breath
            // Red Dragon (6): 5발 50° 큰 화염 — 전투 중 잘 보이게 scale 3.0
            // Kraken  (7):   10발 55° 잉크 다발 — 다발이라 개별 scale 1.5
            // BlueDragon(10): 5발 50° scale 2.5
            if (mt == 6)      QueueFan(5, 50.0f, 35.0f, 1.2f, 3.0f, 70.0f, startDelay, 0.8f);
            else if (mt == 7) QueueFan(10, 55.0f, 32.0f, 0.8f, 1.5f, 60.0f, startDelay, 1.1f);
            else              QueueFan(5, 50.0f, 35.0f, 1.0f, 2.5f, 70.0f, startDelay, 0.8f);
            break;

        case 6:   // MegaBreath — 옵션A: 클라 단독 9-phase 시퀀스 (오프라인 MegaBreathAttackBehavior 1:1)
            if (mt == 6)
            {
                // 중복 가드
                if (m_mapServerMegaBreathCutscenes.find(monsterId) != m_mapServerMegaBreathCutscenes.end())
                    break;

                ServerMegaBreathCutscene cs;
                cs.phase = MegaBreathPhase::TakeOff;
                cs.phaseTimer = 0.0f;

                // 보스 진입 직전 위치 (현재 보스 위치) — 복귀 목표
                cs.originalPos = XMFLOAT3{ atkX, atkY, atkZ };
                {
                    auto bIt = m_mapServerMonsters.find(monsterId);
                    if (bIt != m_mapServerMonsters.end() && bIt->second && bIt->second->GetTransform())
                        cs.originalPos = bIt->second->GetTransform()->GetPosition();
                }
                cs.phaseStartPos = cs.originalPos;

                // 보스룸 (Boss_Room) 고정 — game world 좌표 (오프라인 동일):
                //   center=(-30, 0, 6.71), bounds world X[-145, 85] Z[-115, 128.4], WALL_OFFSET=15(world units)
                int wallDir = static_cast<int>((monsterId * 7u + 2u) % 4u);
                XMFLOAT3 wallPos{ -30.0f, 0.0f, 6.71f };
                switch (wallDir)
                {
                case 0: wallPos.x = 75.95f; wallPos.z = 6.71f; break; // +X (84.95-9, 벽 쪽으로 당김)
                case 1: wallPos.x = -135.95f; wallPos.z = 6.71f; break; // -X (-144.95+9)
                case 2: wallPos.x = -30.0f;  wallPos.z = 119.40f; break; // +Z (128.40-9)
                default: wallPos.x = -30.0f; wallPos.z = -105.95f; break; // -Z (-114.95+9)
                }
                cs.wallPos = wallPos;
                cs.bossSpawnPos = XMFLOAT3{ -30.0f, 0.0f, 6.71f }; // 룸 중심 game world = cover/카메라 기준
                cs.active = true;
                m_mapServerMegaBreathCutscenes[monsterId] = std::move(cs);

                // MegaBreath 동안 Fire boss room 맵 기믹 메테오/용암기둥 정지.
                // 플레이어가 엄폐해야 하는 핵심 패턴 중 랜덤 기믹이 겹치지 않게 한다.
                if (pScene && pScene->GetCurrentRoom())
                {
                    pScene->GetCurrentRoom()->SetLavaGeyserEnabled(false);
                    WriteNetworkLog("[Network] LavaGeyser disabled during MegaBreath");
                }

                // 이륙 애니 — 오프라인 MegaBreath::Execute 동일
                {
                    auto bIt = m_mapServerMonsters.find(monsterId);
                    if (bIt != m_mapServerMonsters.end() && bIt->second)
                    {
                        if (auto* pAnim = bIt->second->GetComponent<AnimationComponent>())
                            pAnim->CrossFade("Take Off", 0.15f, false);
                    }
                }

                char cBuf[200];
                sprintf_s(cBuf, "[Network] MegaBreath OptionA START monsterId=%llu wallDir=%d wall=(%.1f,%.1f,%.1f)",
                    monsterId, wallDir, wallPos.x, wallPos.y, wallPos.z);
                WriteNetworkLog(cBuf);
            }
            else
            {
                QueueFan(7, 30.0f, 40.0f, 1.0f, 2.0f, 80.0f, startDelay, 2.5f);
            }
            break;

        case 7:   // JumpSlam — 보스 점프 + 착지 폭발
        {
            if (mt == 8)
            {
                PlayNetworkGolemAttackBehavior(
                    pScene,
                    pMonster,
                    monsterId,
                    attackType,
                    targetPlayerId,
                    effectPositions,
                    effectOption
                );
                return;
            }

            XMFLOAT3 impactPos{ atkX, atkY + 0.2f, atkZ };

            if (!effectPositions.empty())
            {
                impactPos = effectPositions.front();
                impactPos.y = atkY + 0.2f;
            }
            else if (mt == 10)
            {
                impactPos = targetPos;
                impactPos.y = atkY + 0.2f;
            }

            QueueExplosion(impactPos, startDelay);
            QueueShake(startDelay, 2.5f, 0.5f);

            {
                ServerBossAction act;
                act.kind = BossActionKind::Jump;
                act.timer = 0.0f;
                act.duration = startDelay > 0.f ? startDelay : 1.0f;
                act.peakHeight = 8.0f;
                m_mapServerBossActions[monsterId] = act;
            }

            break;
        }

        case 8:   // TailSweep
            if (mt == 6 || mt == 10)
            {
                // Dragon / BlueDragon — 보스 발 밑 폭발 + 좌우 호 폭발
                QueueExplosion(XMFLOAT3{ atkX, atkY + 0.2f, atkZ }, startDelay);
                if (auto* pT = pMonster->GetTransform())
                {
                    float yawRad = pT->GetRotation().y * (3.14159265f / 180.0f);
                    for (int i = 0; i < 4; ++i)
                    {
                        float angle = yawRad + (-1.5708f + (i / 3.0f) * 3.1416f);
                        float r = (mt == 10) ? 6.0f : 8.0f;
                        DirectX::XMFLOAT3 ep{ atkX + sinf(angle) * r, atkY + 0.2f, atkZ + cosf(angle) * r };
                        QueueExplosion(ep, startDelay + 0.05f * i);
                    }
                }
                QueueShake(startDelay, 2.0f, 0.4f);
            }
            else if (mt == 7)
            {
                // Kraken TailSweep — 전방으로 촉수 충격이 지나가는 느낌의 물 폭발 3개
                if (auto* pT = pMonster->GetTransform())
                {
                    float yawRad = pT->GetRotation().y * (3.14159265f / 180.0f);
                    for (int i = 1; i <= 3; ++i)
                    {
                        float d = 8.0f * static_cast<float>(i);
                        DirectX::XMFLOAT3 ep{ atkX + sinf(yawRad) * d, atkY + 0.2f, atkZ + cosf(yawRad) * d };
                        QueueExplosion(ep, startDelay + 0.08f * static_cast<float>(i - 1));
                    }
                }
                QueueShake(startDelay, 2.0f, 0.35f);
            }
            break;

        case 9:   // GroundRupture — windup 끝에 타겟 지점 폭발
            QueueExplosion(targetPos, startDelay);
            QueueShake(startDelay, 2.0f, 0.4f);
            break;

        case 10:  // FlyingBarrage — 비행 + 12발/1.5s 넓은 부채꼴
            QueueFan(12, 80.0f, 22.0f, 0.7f, 1.2f, 70.0f, startDelay, 1.5f);
            {
                ServerBossAction act;
                act.kind = BossActionKind::Flying;
                act.timer = 0.0f; act.duration = 3.5f; act.peakHeight = 18.0f;
                m_mapServerBossActions[monsterId] = act;
            }
            break;

        case 11: // KrakenCombo — 3연타 전방 충격
            if (mt == 7)
            {
                if (auto* pT = pMonster->GetTransform())
                {
                    float yawRad = pT->GetRotation().y * (3.14159265f / 180.0f);
                    const float delays[3] = { 0.0f, 0.35f, 0.70f };
                    const float dists[3] = { 10.0f, 16.0f, 22.0f };
                    for (int i = 0; i < 3; ++i)
                    {
                        DirectX::XMFLOAT3 ep{ atkX + sinf(yawRad) * dists[i], atkY + 0.2f, atkZ + cosf(yawRad) * dists[i] };
                        QueueExplosion(ep, startDelay + delays[i]);
                        QueueShake(startDelay + delays[i], 1.4f + 0.4f * i, 0.20f);
                    }
                }
            }
            break;

        case 12: // Kraken SideSmash
            if (mt == 7)
            {
                if (auto* pT = pMonster->GetTransform())
                {
                    float yawRad = pT->GetRotation().y * (3.14159265f / 180.0f);
                    DirectX::XMFLOAT3 ep{ atkX + sinf(yawRad) * 14.0f, atkY + 0.2f, atkZ + cosf(yawRad) * 14.0f };
                    QueueExplosion(ep, startDelay);
                }
                QueueShake(startDelay, 2.4f, 0.35f);
            }
            break;

        case 13: // Kraken WaterBurst — 360도 물 탄막
            if (mt == 7)
            {
                QueueFan(16, 360.0f, 28.0f, 0.8f, 1.1f, 60.0f, startDelay, 1.6f);
                QueueExplosion(XMFLOAT3{ atkX, atkY + 0.2f, atkZ }, startDelay);
                QueueShake(startDelay, 1.2f, 0.25f);
            }
            break;

            // Red Dragon 추가 패턴 — 클라 오프라인 VFX 미러
        case 14:  // LightCombo — 3-hit (8/8/12), 콘 90°, 1.8s 윈도우
            // 보스 발치 + 정면 ±70° 폭발 3회 (휘두르는 발톱 느낌)
            for (int i = 0; i < 3; ++i)
            {
                float t = i * 0.55f; // 0.0 / 0.55 / 1.10
                if (auto* pT = pMonster->GetTransform())
                {
                    float yawRad = pT->GetRotation().y * (3.14159265f / 180.0f);
                    float side = (i == 1) ? -0.7f : 0.7f; // 좌/우 휘두름
                    DirectX::XMFLOAT3 ep{
                        atkX + sinf(yawRad + side) * 4.0f,
                        atkY + 0.2f,
                        atkZ + cosf(yawRad + side) * 4.0f };
                    QueueExplosion(ep, startDelay + t);
                }
                QueueShake(startDelay + t, 1.2f, 0.15f);
            }
            break;

        case 15:  // HeavyCombo — 2-hit (20/30), 콘 120~150°, 2.0s 윈도우
            // 강타 2회 — 더 큰 폭발 + 강한 쉐이크
            for (int i = 0; i < 2; ++i)
            {
                float t = i * 0.7f;
                if (auto* pT = pMonster->GetTransform())
                {
                    float yawRad = pT->GetRotation().y * (3.14159265f / 180.0f);
                    DirectX::XMFLOAT3 ep{
                        atkX + sinf(yawRad) * 5.0f,
                        atkY + 0.2f,
                        atkZ + cosf(yawRad) * 5.0f };
                    QueueExplosion(ep, startDelay + t);
                }
                QueueShake(startDelay + t, 2.5f, 0.3f);
            }
            break;

        case 16:  // FuryCombo — 5-hit, 콘 70°, 0.9s 윈도우 (빠른 폭주)
            // 5연속 작은 폭발 — 좌우 번갈아
            for (int i = 0; i < 5; ++i)
            {
                float t = i * 0.18f;
                if (auto* pT = pMonster->GetTransform())
                {
                    float yawRad = pT->GetRotation().y * (3.14159265f / 180.0f);
                    float side = (i % 2 == 0) ? -0.4f : 0.4f;
                    DirectX::XMFLOAT3 ep{
                        atkX + sinf(yawRad + side) * 3.5f,
                        atkY + 0.2f,
                        atkZ + cosf(yawRad + side) * 3.5f };
                    QueueExplosion(ep, startDelay + t);
                }
                if (i == 0 || i == 4) QueueShake(startDelay + t, 1.5f, 0.12f);
            }
            break;

        case 17:  // FlyingStrafe — 비행 + 직선 사격
            QueueFan(5, 25.0f, 32.0f, 0.8f, 2.0f, 60.0f, startDelay, 0.5f);
            {
                ServerBossAction act;
                act.kind = BossActionKind::Flying;
                act.timer = 0.0f; act.duration = 2.5f; act.peakHeight = 14.0f;
                m_mapServerBossActions[monsterId] = act;
            }
            break;

        case 18:  // FlyingCircle — 비행 + 원형 분사
            QueueFan(8, 360.0f, 24.0f, 0.7f, 1.8f, 50.0f, startDelay, 1.0f);
            {
                ServerBossAction act;
                act.kind = BossActionKind::Flying;
                act.timer = 0.0f; act.duration = 3.0f; act.peakHeight = 16.0f;
                m_mapServerBossActions[monsterId] = act;
            }
            break;

        case 19:  // FlyingSweep — 비행 + 와이드 부채꼴
            QueueFan(6, 60.0f, 28.0f, 0.7f, 2.0f, 55.0f, startDelay, 0.8f);
            {
                ServerBossAction act;
                act.kind = BossActionKind::Flying;
                act.timer = 0.0f; act.duration = 2.5f; act.peakHeight = 14.0f;
                m_mapServerBossActions[monsterId] = act;
            }
            break;

        case 20:  // DiveBomb — 보스 공중 → 급강하
            QueueExplosion(targetPos, startDelay);
            QueueExplosion(XMFLOAT3{
                (atkX + targetPos.x) * 0.5f,
                atkY + 5.0f,
                (atkZ + targetPos.z) * 0.5f }, startDelay - 0.3f);
            QueueShake(startDelay, 3.5f, 0.6f);
            // 다이브: 비행 → 급강하. peakHeight 18u 까지 빠르게 올라가서 startDelay 시점 근처 착지
            {
                ServerBossAction act;
                act.kind = BossActionKind::Flying;
                act.timer = 0.0f; act.duration = startDelay + 0.5f; act.peakHeight = 18.0f;
                m_mapServerBossActions[monsterId] = act;
            }
            break;

        case 21:
        case 22:
        case 23:
        case 24:
        case 25:
        case 26:
            // Golem 전용 패턴은 클라 오프라인 AttackBehavior를 직접 실행하여
            // 실제 Rock mesh / 낙석 / 균열 / 순차 십자 연출을 재현한다.
            // 데미지는 서버 권위이므로 클라는 연출만 담당한다.
            if (mt == 8)
            {
                PlayNetworkGolemAttackBehavior(pScene, pMonster, monsterId, attackType, targetPlayerId, effectPositions, effectOption);
                return;
            }
            break;

        case 27:
        case 30:
        case 31:
        case 32:
        case 33:
            // Demon 전용 범위 패턴은 클라 오프라인 AttackBehavior를 직접 실행한다.
            // 데미지는 서버 권위이고, 클라는 장판/VFX/인디케이터 연출만 담당한다.
            if (mt == 9)
            {
                PlayNetworkDemonAttackBehavior(pScene, pMonster, monsterId, attackType, effectPositions, effectOption);
                return;
            }
            break;

        default:
            break;
        }
    }

    char buf[128];
    sprintf_s(buf, "[Network] MonsterAttack applied: monsterId=%llu clip=%s lock=%.2fs atkType=%u",
        monsterId, attackClip, lockDur, attackType);
    WriteNetworkLog(buf);
}

void NetworkManager::ProcessPlayerDamage(Scene* pScene, uint64 playerId, float damage,
    float currentHp, bool isDead, uint64 attackerMonsterId)
{
    if (!pScene) return;

    // 결산: 받은 데미지 + 사망 카운트 (몬스터 데미지만, 환경 데미지 attackerMonsterId==0 제외)
    if (attackerMonsterId != 0)
        StatOnPlayerDamage(playerId, damage, isDead);

    uint64 localId = GetLocalPlayerId();
    bool bIsLocal = (playerId == localId);

    if (bIsLocal)
    {
        // ── 로컬 플레이어: HP UI + 화면 쉐이크 + hit flash + 데미지 넘버 + 사망 ──
        GameObject* pPlayerGO = pScene->GetPlayer();
        if (!pPlayerGO) return;

        auto* pPC = pPlayerGO->GetComponent<PlayerComponent>();
        if (!pPC) return;

        pPC->SetCurrentHP(currentHp);
        pPC->TriggerHitFlash();

        if (damage > 0.0f)
        {
            XMFLOAT3 pos = pPlayerGO->GetTransform()->GetPosition();
            pos.y += 3.0f;
            DamageNumberManager::Get().AddNumber(pos, damage);
        }

        if (CCamera* pCam = pScene->GetCamera())
            pCam->StartShake(2.0f, 0.18f);

        if (isDead)
            pPC->OnServerDeath();

        // ABY_RVG 보복 — 서버는 READY 패킷 미발송, 같은 조건을 클라가 자체 검사해서 오라 활성화.
        //   조건: 환경 데미지(attackerMonsterId==0) 제외 + 사망 제외 + 4개 슬롯 중 ABY_RVG 장착 (서버 TryActivateRevengeRuneOnDamage 와 일치).
        //   서버도 자체 변수만 켜고 CONSUME 시점에 패킷 보내므로, 동일 조건 검사로 클라/서버 동기화 유지.
        if (!isDead && damage > 0.f && attackerMonsterId != 0)
        {
            if (auto* pSkill = pPlayerGO->GetComponent<SkillComponent>())
            {
                bool hasRvg = false;
                for (int s = 0; s < static_cast<int>(SkillSlot::Count); ++s)
                {
                    if (pSkill->HasRuneEquipped(static_cast<SkillSlot>(s), "ABY_RVG"))
                    {
                        hasRvg = true;
                        break;
                    }
                }
                // 이미 활성 상태(IsVengeancePrimed)면 서버가 중복 처리 안 하므로 클라도 중복 활성 skip
                if (hasRvg && !pPC->IsVengeancePrimed())
                    pPC->TriggerVengeance(10.f);
            }
        }
    }
    else
    {
        // ── 원격 플레이어: 데미지 넘버 + hit flash + 사망 애니 ──
        auto it = m_mapRemotePlayers.find(playerId);
        if (it == m_mapRemotePlayers.end())
        {
            char buf[128];
            sprintf_s(buf, "[Network] PlayerDamage: remote player %llu not found in map", playerId);
            WriteNetworkLog(buf);
            return;
        }
        GameObject* pRemoteGO = it->second;
        if (!pRemoteGO) return;

        // 데미지 넘버 — 맞은 사람 머리 위 (화면 쉐이크는 로컬에게만)
        if (damage > 0.0f)
        {
            XMFLOAT3 pos = pRemoteGO->GetTransform()->GetPosition();
            pos.y += 3.0f;
            DamageNumberManager::Get().AddNumber(pos, damage);
        }

        // Hit flash — 0.15s 동안 1.0→0 페이드. CheckRemotePlayerIdle 에서 매 프레임 tick
        m_mapRemotePlayerHitFlashTimer[playerId] = REMOTE_HIT_FLASH_DURATION;
        pRemoteGO->SetHitFlashAll(1.0f);

        // 원격 플레이어 보복 룬 READY 오라.
        // 서버는 보복 준비 패킷을 따로 안 보내므로 로컬과 같은 조건으로 원격도 표시한다.
        if (!isDead && damage > 0.f && attackerMonsterId != 0)
        {
            if (PlayerComponent* pPC = pRemoteGO->GetComponent<PlayerComponent>())
            {
                if (SkillComponent* pSkill = pRemoteGO->GetComponent<SkillComponent>())
                {
                    bool hasRvg = false;

                    for (int s = 0; s < static_cast<int>(SkillSlot::Count); ++s)
                    {
                        if (pSkill->HasRuneEquipped(static_cast<SkillSlot>(s), "ABY_RVG"))
                        {
                            hasRvg = true;
                            break;
                        }
                    }

                    if (hasRvg && !pPC->IsVengeancePrimed())
                    {
                        pPC->TriggerVengeance(10.f);
                    }
                }
            }
        }

        if (isDead)
        {
            // 데스 애니 (MageBlue_Anim.bin 의 "Death1" 사용)
            AnimationComponent* pAnim = pRemoteGO->GetComponent<AnimationComponent>();
            if (pAnim)
                pAnim->CrossFade("Death1", 0.15f, false, true);

            // 사망 리스트 등록 — 이후 MOVE/Idle 전환 skip
            m_setDeadRemotePlayers.insert(playerId);
            m_mapRemotePlayerMoveTime.erase(playerId);
        }
    }

    char buf[192];
    sprintf_s(buf, "[Network] PlayerDamage applied: id=%llu local=%d dmg=%.1f hp=%.1f dead=%d attacker=%llu",
        playerId, bIsLocal ? 1 : 0, damage, currentHp, isDead ? 1 : 0, attackerMonsterId);
    WriteNetworkLog(buf);
}

void NetworkManager::QueueMonsterDamage(uint64 monsterId, float damage, float currentHp, bool isDead,
    uint64 attackerPlayerId, int skillType)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::MonsterDamage;
    cmd.monsterId = monsterId;
    cmd.damage = damage;
    cmd.currentHp = currentHp;
    cmd.isDead = isDead;
    cmd.attackerPlayerId = attackerPlayerId;
    cmd.skillType = skillType;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueRoomCleared(uint32 stageIndex, uint32 roomIndex)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::RoomCleared;
    cmd.stageIndex = stageIndex;
    cmd.roomIndex = roomIndex;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueRoomRewardSpawn(uint32 stageIndex, uint32 roomIndex, const DirectX::XMFLOAT3& portalPos, bool hasSecondPortal, const DirectX::XMFLOAT3& secondPortalPos, const std::vector<NetworkRewardRuneObjectInfo>& runeObjects)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::RoomRewardSpawn;
    cmd.stageIndex = stageIndex;
    cmd.roomIndex = roomIndex;

    cmd.rewardPortalX = portalPos.x;
    cmd.rewardPortalY = portalPos.y;
    cmd.rewardPortalZ = portalPos.z;

    cmd.rewardHasSecondPortal = hasSecondPortal;
    cmd.rewardSecondPortalX = secondPortalPos.x;
    cmd.rewardSecondPortalY = secondPortalPos.y;
    cmd.rewardSecondPortalZ = secondPortalPos.z;

    cmd.rewardRuneObjects = runeObjects;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueRuneRewardPicked(uint64 ownerPlayerId)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::RuneRewardPicked;
    cmd.playerId = ownerPlayerId;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueRuneEquip(uint64 playerId, uint32 skillSlot, uint32 runeSlotIndex, const std::string& runeId, uint32 stackCount)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::RuneEquip;
    cmd.playerId = playerId;
    cmd.runeSkillSlot = skillSlot;
    cmd.runeSlotIndex = runeSlotIndex;
    cmd.runeId = runeId;
    cmd.runeStackCount = stackCount;

    m_vCommandQueue.push_back(cmd);
}

void NetworkManager::QueueRuneHomingTarget(uint64 playerId, int32 skillSlot, int32 skillType, uint64 targetMonsterId, const DirectX::XMFLOAT3& targetPos, const DirectX::XMFLOAT3& originPos)
{
    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::RuneHomingTarget;
    cmd.playerId = playerId;

    cmd.runeHomingSkillSlot = skillSlot;
    cmd.runeHomingSkillType = skillType;
    cmd.runeHomingTargetMonsterId = targetMonsterId;

    cmd.runeHomingTargetX = targetPos.x;
    cmd.runeHomingTargetY = targetPos.y;
    cmd.runeHomingTargetZ = targetPos.z;

    cmd.runeHomingOriginX = originPos.x;
    cmd.runeHomingOriginY = originPos.y;
    cmd.runeHomingOriginZ = originPos.z;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_vCommandQueue.push_back(cmd);
    }

    char buf[256];
    sprintf_s(buf,
        "[Network] QueueRuneHomingTarget: playerId=%llu skillSlot=%d skillType=%d targetMonsterId=%llu",
        playerId,
        skillSlot,
        skillType,
        targetMonsterId);

    WriteNetworkLog(buf);
}

void NetworkManager::QueueRuneTrigger(uint64 playerId, int32 skillSlot, int32 skillType,
    const std::string& runeId, int32 triggerType,
    uint64 targetMonsterId, uint64 targetPlayerId,
    uint64 objectId,
    const DirectX::XMFLOAT3& pos, float value1, float value2)
{
    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::RuneTrigger;
    cmd.playerId = playerId;
    cmd.runeId = runeId;

    cmd.x = pos.x;
    cmd.y = pos.y;
    cmd.z = pos.z;

    cmd.runeTriggerSkillSlot = skillSlot;
    cmd.runeTriggerSkillType = skillType;
    cmd.runeTriggerType = triggerType;
    cmd.runeTriggerTargetMonsterId = targetMonsterId;
    cmd.runeTriggerTargetPlayerId = targetPlayerId;
    cmd.runeTriggerObjectId = objectId;
    cmd.runeTriggerValue1 = value1;
    cmd.runeTriggerValue2 = value2;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_vCommandQueue.push_back(cmd);
    }

    char buf[256];
    sprintf_s(buf,
        "[Network] QueueRuneTrigger: playerId=%llu runeId=%s objectId=%llu targetMonsterId=%llu value1=%.2f value2=%.2f",
        playerId,
        runeId.c_str(),
        objectId,
        targetMonsterId,
        value1,
        value2);

    WriteNetworkLog(buf);
}

void NetworkManager::QueueBossEvent(uint64 monsterId, uint32 eventType, uint32 phaseIndex)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::BossEvent;
    cmd.monsterId = monsterId;
    cmd.bossEventType = eventType;
    cmd.phaseIndex = phaseIndex;

    m_vCommandQueue.push_back(cmd);
}

// (monsterType) → 보스 인트로/페이즈/사망용 포효 클립.
//   오프라인 EnemyComponent::UpdateBossIntro 가 "Scream" 등을 사용 — 같은 클립 매핑.
//   해당 모델에 클립이 없으면 nullptr → 폴백으로 attackClip 재생.
static const char* GetBossRoarClip(uint32 monsterType)
{
    switch (monsterType)
    {
    case 6:  return "Scream";        // Dragon
    case 10: return "Roar";          // BlueDragon (있으면 — 없으면 idle 로 폴백)
    case 7:  return "Unreal Take";   // Kraken
    case 9:  return "Rage";          // Demon — P2 진입 시 분노 변신 클립 (오프라인 m_strTransitionAnimation)
    case 8:  return "Golem_battle_stand_ge"; // Golem — 정지자세로 컷씬 대체
    default: return nullptr;
    }
}

void NetworkManager::ProcessBossEvent(Scene* pScene, uint64 monsterId, uint32 eventType, uint32 phaseIndex)
{
    if (!pScene) return;

    auto it = m_mapServerMonsters.find(monsterId);
    GameObject* pBoss = (it != m_mapServerMonsters.end()) ? it->second : nullptr;

    auto clipIt = m_mapServerMonsterClips.find(monsterId);
    uint32 mt = (clipIt != m_mapServerMonsterClips.end()) ? clipIt->second.monsterType : 0;
    const char* roarClip = GetBossRoarClip(mt);

    // DarkLord 사망은 바로 Ending UI로 넘기지 않고 Scene 사망 연출을 먼저 실행한다.
    if (mt == 11 && eventType == 3)
    {
        pScene->StartNetworkDarkLordDeath(pBoss, monsterId);

        char buf[160];
        sprintf_s(buf,
            "[Network] DarkLord death event received - death sequence start monsterId=%llu",
            monsterId);
        WriteNetworkLog(buf);

        return;
    }

    // DarkLord 인트로는 Scene에 구현된 전용 컷신을 사용한다.
    // RedDragon 공통 BossIntroState를 타면 안 된다.
    if (mt == 11 && eventType == 1)
    {
        if (pBoss)
        {
            SetCutscenePlaying(true);
            WriteNetworkLog("[Network] Cutscene lock ON - DarkLord intro");

            pScene->StartNetworkDarkLordIntro(pBoss, monsterId);
            WriteNetworkLog("[Network] DarkLord network intro requested");
        }
        else
        {
            WriteNetworkLog("[Network] DarkLord intro failed: boss not found");
        }

        return;
    }

    // Red Dragon phase transition MegaBreath.
    // 서버는 Dragon phase 2 / phase 3 진입 시 MegaBreath를 시작한다.
    // 클라 로컬 입력은 벽 이동/착지/엄폐물 등장 연출까지만 5.8초 잠근다.
    if (mt == 6 && eventType == 2 && (phaseIndex == 2 || phaseIndex == 3))
    {
        StartMegaBreathInputLock(5.8f);
        WriteNetworkLog("[Network] RedDragon MegaBreath input lock started: 5.8s");
    }

    // 카메라 쉐이크 — 이벤트 타입별로 강도/지속 다르게
    CCamera* pCam = pScene->GetCamera();

    switch (eventType)
    {
    case 1: // BOSS_EVENT_INTRO — 오프라인 EnemyComponent::StartBossIntro 와 동일 시퀀스
        // 중복 가드 — 다중 플레이어 broadcast 누적 방지
        if (m_mapServerBossIntros.find(monsterId) != m_mapServerBossIntros.end())
        {
            char dupBuf[128];
            sprintf_s(dupBuf, "[Network] BossIntro duplicate skipped monsterId=%llu", monsterId);
            WriteNetworkLog(dupBuf);
            break;
        }

        // BlueDragon 전용 가벼운 등장 연출
        // 카메라 컷신 없음.
        // RedDragon용 m_mapServerBossIntros에 넣지 않는다.
        // 그래야 UpdateServerBossIntros()의 StartCinematic/Landing/Roaring 로직을 타지 않는다.
        if (mt == 10)
        {
            if (pBoss)
            {
                ServerBossIntroState st;
                st.phase = BossIntroPhase::FlyingIn;
                st.phaseTimer = 0.0f;
                st.startHeight = 5.0f;
                st.active = true;

                if (auto* pT = pBoss->GetTransform())
                {
                    XMFLOAT3 p = pT->GetPosition();

                    st.bossX = p.x;
                    st.bossZ = p.z;
                    st.groundY = p.y;
                    st.curY = p.y + st.startHeight;

                    pT->SetPosition(p.x, st.curY, p.z);
                }

                m_mapServerBossIntros[monsterId] = st;


                if (auto* pAnim = pBoss->GetComponent<AnimationComponent>())
                    pAnim->CrossFade("Fly Glide", 0.15f, true, true);

                m_mapServerBossIntros[monsterId].flyAnimFired = true;

                WriteNetworkLog("[Network] BlueDragon light intro started - no camera");
            }

            break;
        }

        if (pBoss)
        {
            ServerBossIntroState st;
            st.phase = BossIntroPhase::FlyingIn;
            st.phaseTimer = 0.0f;
            st.startHeight = 25.0f;  // 오프라인 Scene.cpp 와 동일 (Red Dragon 25u)

            if (auto* pT = pBoss->GetTransform())
            {
                XMFLOAT3 p = pT->GetPosition();
                st.bossX = p.x;
                st.bossZ = p.z;
                st.groundY = p.y;            // 스폰 좌표 = 지면 y
                st.curY = p.y + st.startHeight; // 시작은 25u 위
                pT->SetPosition(p.x, st.curY, p.z);

                // Red Dragon intro 시작 시 정면이 보이도록 고정
                SetDragonVisualYaw(pT, 0.0f);
            }
            st.active = true;
            m_mapServerBossIntros[monsterId] = st;

            // Red Dragon intro 중에는 플레이어가 움직일 수 없으므로
                // Fire boss room 맵 기믹(메테오/용암기둥)을 잠시 정지한다.
            if (mt == 6 && pScene && pScene->GetCurrentRoom())
            {
                pScene->GetCurrentRoom()->SetLavaGeyserEnabled(false);
                WriteNetworkLog("[Network] LavaGeyser disabled during RedDragon intro");
            }

            // 강하 시작 — "Fly Glide" 애니 (오프라인 동일)
            if (auto* pAnim = pBoss->GetComponent<AnimationComponent>())
                pAnim->CrossFade("Fly Glide", 0.2f, true);
            m_mapServerBossIntros[monsterId].flyAnimFired = true;

            // ── 시네마틱 카메라 ON ── 오프라인 Scene.cpp 와 100% 동일 (dist 55, pitch 15, yaw 180)
            //   ground 위 보스를 정반대 시점에서 wide-angle 로 비춤 → 하늘에서 강하하는 보스가 정면 보임
            if (pCam)
            {
                XMFLOAT3 landPos{ st.bossX, st.groundY, st.bossZ };
                pCam->StartCinematic(landPos, 55.0f, 15.0f, 180.0f);
                pCam->StartShake(2.5f, 0.6f);
            }

            char introBuf[160];
            sprintf_s(introBuf, "[Network] BossIntro (offline-port) started monsterId=%llu startHeight=%.1f",
                monsterId, st.startHeight);
            WriteNetworkLog(introBuf);
        }
        else
        {
            if (pCam) pCam->StartShake(4.0f, 1.5f);
        }
        break;

    case 2: // BOSS_EVENT_PHASE_CHANGE
    {
        bool isKrakenPhase2 = (mt == 7 && phaseIndex == 2);

        if (pCam) pCam->StartShake(3.0f, 1.0f);

        if (pBoss)
        {
            // Kraken 2페이즈 등장 컷신은 오프라인처럼
            // 스폰 직후부터 Idle/촉수 애니가 자연스럽게 움직여야 하므로
            // 공통 Roar 클립 강제 재생을 건너뛴다.
            if (!isKrakenPhase2)
            {
                if (roarClip)
                {
                    if (auto* pAnim = pBoss->GetComponent<AnimationComponent>())
                        pAnim->CrossFade(roarClip, 0.2f, false, true);
                }
            }

            // 무적 페이즈 flash는 Kraken에도 적용 가능
            pBoss->SetHitFlashAll(1.0f);
            m_mapServerMonsterHitFlashTimer[monsterId] = 0.4f;

            // Demon Rage 전환: 2.4s "Rage" 클립 동안 walk/idle CrossFade 가 덮지 않도록
            //   attack timer 잠금 (오프라인 m_fTransitionDuration 와 동일).
            if (mt == 9)
            {
                m_mapServerMonsterAttackTimer[monsterId] = 2.4f;
                m_mapServerMonsterCurrentAnimClip[monsterId] = "Rage";
            }
        }

        if (isKrakenPhase2)
        {
            if (pBoss)
            {
                SetCutscenePlaying(true);
                WriteNetworkLog("[Network] Cutscene lock ON");

                pScene->StartNetworkKrakenCutscene(pBoss, monsterId);
                WriteNetworkLog("[Network] Kraken phase 2 cutscene requested");
            }
            else
            {
                WriteNetworkLog("[Network] Kraken phase 2 cutscene failed: monster not found");
            }
        }
        break;
    }

    case 3: // BOSS_EVENT_DEATH — 사망 컷씬: 가장 강한 쉐이크 (사망 애니는 S_MONSTER_DAMAGE 가 별도 처리)
        if (pCam) pCam->StartShake(5.0f, 2.0f);
        break;

    default:
        break;
    }

    char buf[192];
    sprintf_s(buf, "[Network] BossEvent processed: monsterId=%llu type=%u phase=%u clip=%s",
        monsterId, eventType, phaseIndex, roarClip ? roarClip : "(none)");
    WriteNetworkLog(buf);
}

// 몬스터 기절/그로기 처리
void NetworkManager::ProcessMonsterStagger(uint64 monsterId, float duration)
{
    // 1. 서버 몬스터 조회
    auto it = m_mapServerMonsters.find(monsterId);
    if (it == m_mapServerMonsters.end() || it->second == nullptr)
        return;

    GameObject* pMonster = it->second;

    // 2. Demon FixatedCharge 기둥 충돌 그로기 애니메이션 재생
    if (auto* pAnim = pMonster->GetComponent<AnimationComponent>())
    {
        pAnim->CrossFade("gethit3", 0.15f, true, true);
    }

    // 3. 기절 시간 동안 이동/공격 애니메이션이 바로 덮지 못하도록 락 타이머 설정
    m_mapServerMonsterAttackTimer[monsterId] = duration;
    m_mapServerMonsterMoveTime.erase(monsterId);

    // 4. 카메라 pull-back 해제 + 기둥 충돌 임팩트 shake
    if (auto* pApp = Dx12App::GetInstance())
    {
        if (auto* pScene = pApp->GetScene())
        {
            if (auto* pCam = pScene->GetCamera())
            {
                pCam->SetExtraOrbitDistanceTarget(0.0f);
                pCam->StartShake(1.6f, 0.55f);
            }
        }
    }

    // 5. 디버그 로그 출력
    char buf[160];
    sprintf_s(buf,
        "[Network] MonsterStagger applied: monsterId=%llu duration=%.2f",
        monsterId,
        duration);
    WriteNetworkLog(buf);
}

void NetworkManager::PlayNetworkGolemAttackBehavior(Scene* pScene, GameObject* pMonster, uint64 monsterId, uint32 attackType, uint64 targetPlayerId, const std::vector<DirectX::XMFLOAT3>& effectPositions, uint32 effectOption)
{
    if (pScene == nullptr || pMonster == nullptr)
        return;

    // Golem 전용 Behavior가 이미 진행 중이면 새 패턴 연출은 스킵한다.
    // 서버 공격 패킷이 너무 빨리 들어오면 RockFall/RockBarrage가
    // 바위 생성 전에 Reset되어 돌이 안 보이는 문제가 생김.
    if (!m_vNetworkGolemBehaviors.empty())
        return;

    // 1. 네트워크 Golem에 연출용 EnemyComponent가 없으면 추가
    //    데미지/사망 판정은 서버 권위이므로 클라에서는 AI를 멈춘 연출용으로만 사용한다.
    EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>();
    if (pEnemy == nullptr)
    {
        pEnemy = pMonster->AddComponent<EnemyComponent>();
        pEnemy->SetBoss(true);
        pEnemy->SetStationary(true);
        pEnemy->SetAIPaused(true);

        if (auto* pAnim = pMonster->GetComponent<AnimationComponent>())
            pEnemy->SetAnimationComponent(pAnim);

        if (CRoom* pRoom = pScene->GetCurrentRoom())
            pEnemy->SetRoom(pRoom);

        if (GameObject* pPlayer = pScene->GetPlayer())
            pEnemy->SetTarget(pPlayer);
    }
    else
    {
        // 2. 이미 EnemyComponent가 있어도 네트워크 몬스터는 서버 권위 유지
        pEnemy->SetBoss(true);
        pEnemy->SetStationary(true);
        pEnemy->SetAIPaused(true);

        if (GameObject* pPlayer = pScene->GetPlayer())
            pEnemy->SetTarget(pPlayer);

        if (CRoom* pRoom = pScene->GetCurrentRoom())
            pEnemy->SetRoom(pRoom);

        if (auto* pAnim = pMonster->GetComponent<AnimationComponent>())
            pEnemy->SetAnimationComponent(pAnim);
    }

    // 3. Golem 전용 attackType에 맞는 클라 오프라인 Behavior 생성
    std::unique_ptr<IAttackBehavior> behavior;

    switch (attackType)
    {
    case 7:
        // Golem Primary Slam - 기본 제자리 내려찍기
        // 서버 attackType 7 = JumpSlam 이지만, Golem은 이동 점프가 아니라 제자리 대형 슬램으로 연출한다.
        // 데미지는 서버 권위이므로 0.0f.
        behavior = std::make_unique<JumpSlamAttackBehavior>(
            0.0f,
            0.0f,
            0.25f,
            70.0f,
            3.35f,
            1.3f,
            false,
            3.4f,
            0.7f,
            "Golem_battle_attack01_ge",
            0.7f
        );
        break;

    case 21:
        // GolemJumpShock - 작은 원형 충격파
        behavior = std::make_unique<JumpSlamAttackBehavior>(
            0.0f,
            6.5f,
            1.8f,
            55.0f, // 원형 데미지 반경
            1.3f,
            0.7f,
            false,
            3.6f,
            0.7f,
            "Golem_jump_ge",
            0.5f
        );
        break;

    case 22:
        // GolemWideSlam - 큰 원형 광역 내려찍기
        behavior = std::make_unique<JumpSlamAttackBehavior>(
            0.0f,
            0.0f,
            0.3f,
            75.0f, // 원형 데미지 반경
            3.6f,
            1.8f,
            false,
            3.4f,
            0.65f,
            "Golem_battle_attack01_ge"
        );
        break;

    case 23:
    {
        // GolemRockBarrage
        auto rockBarrage = std::make_unique<RockBarrageAttackBehavior>(
            16,
            0.0f,
            4.0f,
            44.0f,
            22.0f,
            18.0f,
            2.6f,
            0.6f,
            0.16f,
            3.5f,
            2.2f,
            0.0f,
            0.0f,
            0.8f
        );

        // 서버가 정한 targetPlayerId에 해당하는 플레이어 GameObject 찾기
        GameObject* pTargetObj = nullptr;

        if (targetPlayerId == GetLocalPlayerId())
        {
            pTargetObj = pScene ? pScene->GetPlayer() : nullptr;
        }
        else
        {
            auto rIt = m_mapRemotePlayers.find(targetPlayerId);
            if (rIt != m_mapRemotePlayers.end())
                pTargetObj = rIt->second;
        }

        // 모든 클라가 같은 플레이어를 추적하도록 타겟 고정
        rockBarrage->SetNetworkTarget(pTargetObj);

        // 회전/스케일 등 시각 랜덤 동기화
        rockBarrage->SetNetworkEffectSeed(effectOption);

        behavior = std::move(rockBarrage);
        break;
    }

    case 24:
    {
        // GolemRockFall
        auto rockFall = std::make_unique<RockFallAttackBehavior>(
            10,
            0.0f,
            14.0f,
            20.0f,
            75.0f,
            2.6f,
            1.2f,
            4.0f,
            3.0f,
            0.5f
        );

        // 서버 낙석 위치가 있으면 클라 rand() 위치 생성을 막고 서버 위치를 사용한다.
        if (!effectPositions.empty())
        {
            rockFall->SetNetworkEffectData(effectPositions, effectOption);
        }

        behavior = std::move(rockFall);
        break;
    }

    case 25:
    {
        // GolemGroundRupture
        auto shape = (effectOption == 0)
            ? GroundRuptureAttackBehavior::RuptureShape::Cross
            : GroundRuptureAttackBehavior::RuptureShape::XDiag;

        auto groundRupture = std::make_unique<GroundRuptureAttackBehavior>(
            shape,
            0.0f,
            80.0f,
            6.0f,
            2.2f,
            0.4f,
            1.2f,
            3.0f,
            0.5f
        );

        // 서버 effectOption을 seed로 사용해서 균열에서 솟는 돌의 jitter / 회전 / 스케일을 동기화한다.
        groundRupture->SetNetworkEffectSeed(effectOption);

        behavior = std::move(groundRupture);
        break;
    }

    case 26:
    {
        // GolemSequentialCross
        auto sequentialCross = std::make_unique<SequentialCrossAttackBehavior>(
            0.0f,
            80.0f,
            12.0f,
            2.5f,
            0.65f,
            0.35f,
            1.4f,
            2.4f,
            0.45f
        );

        // 서버 effectOption을 seed로 사용해서 순차 십자 폭발의 돌 jitter / 회전 / 스케일을 동기화한다.
        sequentialCross->SetNetworkEffectSeed(effectOption);

        behavior = std::move(sequentialCross);
        break;
    }

    default:
        return;
    }

    if (!behavior)
        return;

    // 새 Golem 연출 실행
    behavior->Execute(pEnemy);

    NetworkGolemBehaviorEntry entry;
    entry.behavior = std::move(behavior);
    entry.owner = pEnemy;

    m_vNetworkGolemBehaviors.push_back(std::move(entry));

    char buf[160];
    sprintf_s(buf,
        "[Network] PlayNetworkGolemAttackBehavior monsterId=%llu attackType=%u",
        monsterId,
        attackType);
    WriteNetworkLog(buf);
}

void NetworkManager::PlayNetworkNormalMonsterAttackBehavior(
    Scene* pScene,
    GameObject* pMonster,
    uint64 monsterId,
    uint32 monsterType,
    uint32 attackType,
    uint64 targetPlayerId)
{
    WriteNetworkLog("[Network] PlayNetworkNormalMonsterAttackBehavior ENTER");

    if (pScene == nullptr || pMonster == nullptr)
        return;

    // 일반 몬스터는 여러 마리가 동시에 공격할 수 있으므로
    // Golem처럼 전체 vector가 비어있을 때만 허용하는 전역 가드는 걸지 않는다.

    // 1. 네트워크 일반 몬스터에 연출용 EnemyComponent가 없으면 추가
    //    데미지/이동 판정은 서버 권위이므로 클라에서는 AI를 멈춘 연출용으로만 사용한다.
    EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>();
    if (pEnemy == nullptr)
    {
        pEnemy = pMonster->AddComponent<EnemyComponent>();
        pEnemy->SetBoss(false);
        pEnemy->SetAIPaused(true);

        if (auto* pAnim = pMonster->GetComponent<AnimationComponent>())
            pEnemy->SetAnimationComponent(pAnim);

        if (CRoom* pRoom = pScene->GetCurrentRoom())
            pEnemy->SetRoom(pRoom);
    }
    else
    {
        // 2. 이미 EnemyComponent가 있어도 네트워크 몬스터는 서버 권위 유지
        pEnemy->SetBoss(false);
        pEnemy->SetAIPaused(true);

        if (auto* pAnim = pMonster->GetComponent<AnimationComponent>())
            pEnemy->SetAnimationComponent(pAnim);

        if (CRoom* pRoom = pScene->GetCurrentRoom())
            pEnemy->SetRoom(pRoom);
    }

    // 3. 서버가 지정한 targetPlayerId에 해당하는 클라 GameObject를 타겟으로 설정
    GameObject* pTargetObj = nullptr;

    if (targetPlayerId == GetLocalPlayerId())
    {
        pTargetObj = pScene->GetPlayer();
    }
    else
    {
        auto rIt = m_mapRemotePlayers.find(targetPlayerId);
        if (rIt != m_mapRemotePlayers.end())
            pTargetObj = rIt->second;
    }

    // targetPlayerId가 0이거나 원격 플레이어 못 찾으면 로컬 플레이어로 fallback
    if (pTargetObj == nullptr)
        pTargetObj = pScene->GetPlayer();

    if (pTargetObj)
    {
        pEnemy->SetTarget(pTargetObj);
    }
    else
    {
        WriteNetworkLog("[Network] NormalMonsterBehavior target is null");
        return;
    }

    // 4. 일반 몬스터 attackType에 맞는 클라 오프라인 Behavior 생성
    std::unique_ptr<IAttackBehavior> behavior;

    uint32 visualType = 0;
    bool isMiniBoss = false;

    auto clipIt = m_mapServerMonsterClips.find(monsterId);
    if (clipIt != m_mapServerMonsterClips.end())
    {
        visualType = clipIt->second.visualType;
        isMiniBoss = clipIt->second.isMiniBoss;
    }

    // 중간보스는 일반 몬스터 AttackBehavior를 그대로 쓰되,
    // 몸집이 커진 비율만큼 인디케이터 / 공격 범위를 함께 키운다.
    const float attackScale = GetNetworkMiniBossAttackScaleRatio(visualType);

    switch (attackType)
    {
    case 1:
    {
        // AirElemental / Melee
        // 클라 AirElemental preset과 동일 수치
        auto melee = std::make_unique<MeleeAttackBehavior>(
            15.0f,  // damage
            0.4f,   // windup
            0.2f,   // hit
            0.4f    // recovery
        );

        melee->SetHitRange(4.0f);

        // 서버 권위 모드: 클라 직접 데미지 금지
        melee->SetNetworkVisualOnly(true);

        behavior = std::move(melee);
        break;
    }
    case 2:
    {
        // RangedEnemy
        // 클라 RangedEnemy preset과 동일 수치
        ProjectileManager* pProjMgr = pScene ? pScene->GetProjectileManager() : nullptr;
        if (!pProjMgr)
            return;

        auto ranged = std::make_unique<RangedAttackBehavior>(
            pProjMgr,
            10.0f,  // damage
            20.0f,  // projectileSpeed
            0.5f,   // windup
            0.1f,   // shootTime
            0.5f    // recovery
        );

        // 서버 권위 모드: 클라 투사체 데미지 0
        ranged->SetNetworkVisualOnly(true);

        behavior = std::move(ranged);
        break;
    }

    case 3:
    {
        // RushAoEEnemy
        // 일반 몬스터 / 중간보스 모두 같은 RushAoEAttackBehavior를 사용한다.
        // 중간보스는 몸집 비율만큼 rushSpeed / aoeRadius만 키운다.
        auto rushAoE = std::make_unique<RushAoEAttackBehavior>(
            15.0f,                // damage
            15.0f * attackScale,  // rushSpeed
            0.5f,                 // rushDuration
            0.3f,                 // windupTime
            0.2f,                 // hitTime
            0.3f,                 // recoveryTime
            5.0f * attackScale,   // aoeRadius
            0.45f                 // telegraphTime
        );

        // 서버 권위 모드: 클라 직접 이동/데미지 금지
        rushAoE->SetNetworkVisualOnly(true);

        behavior = std::move(rushAoE);
        break;
    }

    case 4:
    {
        // RushFrontEnemy
        // 일반 몬스터 / 중간보스 모두 같은 RushFrontAttackBehavior를 사용한다.
        // 중간보스는 몸집 비율만큼 rushSpeed / hitRange만 키운다.
        auto rushFront = std::make_unique<RushFrontAttackBehavior>(
            20.0f,                // damage
            18.0f * attackScale,  // rushSpeed
            0.4f,                 // rushDuration
            0.2f,                 // windupTime
            0.2f,                 // hitTime
            0.3f,                 // recoveryTime
            4.0f * attackScale,   // hitRange
            90.0f,                // coneAngleDeg
            0.45f                 // telegraphTime
        );

        // 서버 권위 모드: 클라 직접 이동/데미지 금지
        rushFront->SetNetworkVisualOnly(true);

        behavior = std::move(rushFront);
        break;
    }

    case 36: // QuickJab
    {
        // QuickJab — 서버 권위 일반 몬스터 잽 3연타 연출
        // 실제 데미지는 서버 S_PLAYER_DAMAGE를 따르고, 클라는 모션 / 인디케이터만 재생한다.
        auto quickJab = std::make_unique<QuickJabAttackBehavior>(
            8.0f,                 // damage
            0.18f,                // windupTime
            0.18f,                // hitInterval
            3,                    // hitCount
            0.35f,                // recoveryTime
            3.2f * attackScale    // hitRange
        );

        quickJab->SetNetworkVisualOnly(true);

        behavior = std::move(quickJab);
        break;
    }

    case 37: // ChargedShot
    {
        // ChargedShot — 서버 권위 일반 몬스터 차징 직선 사격 연출
        // 투사체는 보이게 생성하되, 데미지는 0으로 두고 서버 판정을 따른다.
        ProjectileManager* pProjMgr = pScene ? pScene->GetProjectileManager() : nullptr;
        if (!pProjMgr)
            return;

        auto chargedShot = std::make_unique<ChargedShotAttackBehavior>(
            pProjMgr,
            28.0f,                // damage
            32.0f,                // projectileSpeed
            1.6f,                 // chargeTime
            0.8f,                 // recoveryTime
            34.0f * attackScale,  // indicatorLength
            1.2f * attackScale    // indicatorHalfW
        );

        chargedShot->SetNetworkVisualOnly(true);

        behavior = std::move(chargedShot);
        break;
    }

    case 38: // GrenadeThrow
    {
        // GrenadeThrow — 서버 권위 일반 몬스터 수류탄 투척 연출
        // 폭발 VFX / 카메라 쉐이크는 클라에서 재생하고, 실제 데미지는 서버 판정을 따른다.
        ProjectileManager* pProjMgr = pScene ? pScene->GetProjectileManager() : nullptr;
        if (!pProjMgr)
            return;

        auto grenadeThrow = std::make_unique<GrenadeThrowAttackBehavior>(
            pProjMgr,
            32.0f,                // damage
            4.5f * attackScale,   // aoeRadius
            0.5f,                 // windupTime
            1.1f,                 // airTime
            0.6f                  // recoveryTime
        );

        grenadeThrow->SetNetworkVisualOnly(true);

        behavior = std::move(grenadeThrow);
        break;
    }

    case 39: // SuicideExplode
    {
        // SuicideExplode — 서버 권위 일반 몬스터 자폭 연출
        // 폭발 VFX는 클라에서 재생하고, 데미지 / 자기 사망은 서버 S_MONSTER_DAMAGE 흐름을 따른다.
        ProjectileManager* pProjMgr = pScene ? pScene->GetProjectileManager() : nullptr;
        if (!pProjMgr)
            return;

        auto suicideExplode = std::make_unique<SuicideExplodeAttackBehavior>(
            pProjMgr,
            45.0f,                // damage
            4.5f * attackScale,   // aoeRadius
            1.0f                  // countdownTime
        );

        suicideExplode->SetNetworkVisualOnly(true);

        behavior = std::move(suicideExplode);
        break;
    }

    default:
        return;
    }

    if (!behavior)
        return;

    // 새 공격 연출 시작 전, 이전 일반 몬스터 인디케이터를 강제로 숨긴다.
    pEnemy->HideNetworkAttackIndicator();

    if (IAttackBehavior* pOldBehavior = pEnemy->GetAttackBehavior())
        pOldBehavior->Reset();

    // 5. 일반 몬스터 연출 실행
    // EnemyComponent에 behavior를 넣어 기존 ShowIndicators()가 pActive behavior를 읽을 수 있게 한다.
    pEnemy->SetAttackBehavior(std::move(behavior));

    IAttackBehavior* pBehavior = pEnemy->GetAttackBehavior();
    if (!pBehavior)
        return;

    // 인디케이터 타이머 초기화 후 연출 실행
    pEnemy->ResetNetworkAttackIndicator();
    pBehavior->Execute(pEnemy);

    // 같은 몬스터의 이전 일반 공격 연출 entry 제거
// 연속 공격 시 같은 EnemyComponent를 여러 번 Update하는 것을 방지한다.
    m_vNetworkNormalMonsterBehaviors.erase(
        std::remove_if(
            m_vNetworkNormalMonsterBehaviors.begin(),
            m_vNetworkNormalMonsterBehaviors.end(),
            [monsterId](const NetworkNormalMonsterBehaviorEntry& e)
            {
                return e.monsterId == monsterId;
            }),
        m_vNetworkNormalMonsterBehaviors.end()
    );

    NetworkNormalMonsterBehaviorEntry entry;
    entry.monsterId = monsterId;
    m_vNetworkNormalMonsterBehaviors.push_back(entry);

    char buf[240];
    sprintf_s(
        buf,
        sizeof(buf),
        "[Network] PlayNetworkNormalMonsterAttackBehavior monsterId=%llu monsterType=%u attackType=%u visualType=%u miniBoss=%d",
        monsterId,
        monsterType,
        attackType,
        visualType,
        isMiniBoss ? 1 : 0
    );
    WriteNetworkLog(buf);
}

void NetworkManager::ProcessMonsterDamage(Scene* pScene, uint64 monsterId, float damage,
    float currentHp, bool isDead,
    uint64 attackerPlayerId, int skillType)
{
    if (!pScene) return;

    // 결산: 가한 데미지 + 막타 (attackerPlayerId 가 있을 때만)
    if (attackerPlayerId != 0)
        StatOnMonsterDamage(attackerPlayerId, damage, isDead);

    auto it = m_mapServerMonsters.find(monsterId);
    if (it == m_mapServerMonsters.end())
    {
        char buf[128];
        sprintf_s(buf, "[Network] MonsterDamage: unknown monsterId=%llu (despawned?)", monsterId);
        WriteNetworkLog(buf);
        return;
    }

    GameObject* pMonster = it->second;
    if (!pMonster) return;

    auto clipIt = m_mapServerMonsterClips.find(monsterId);
    uint32 mt = (clipIt != m_mapServerMonsterClips.end())
        ? clipIt->second.monsterType
        : 0;

    // 혹시 BossEvent Death보다 S_MONSTER_DAMAGE isDead가 먼저 온 경우도 처리
    if (mt == 11 && isDead)
    {
        pScene->StartNetworkDarkLordDeath(pMonster, monsterId);
    }

    // 데미지 넘버 — 몬스터 머리 위
    if (damage > 0.0f && pMonster->GetTransform())
    {
        XMFLOAT3 pos = pMonster->GetTransform()->GetPosition();
        pos.y += 2.0f;  // EnemyComponent::TakeDamage 와 동일 오프셋
        DamageNumberManager::Get().AddNumber(pos, damage);
    }

    // Hit flash — 0.15s 페이드 (원격 플레이어와 동일 패턴)
    m_mapServerMonsterHitFlashTimer[monsterId] = SERVER_MONSTER_HIT_FLASH_DURATION;
    pMonster->SetHitFlashAll(1.0f);

    // ── 몬스터 피격 / 수비 / 사망 애니메이션 동기화 ──
    // 서버 S_MONSTER_DAMAGE 기준으로 모든 클라에서 같은 반응을 재생한다.
    AnimationComponent* pAnim = pMonster->GetComponent<AnimationComponent>();

    if (isDead)
    {
        // 사망 애니메이션은 최우선. 이후 Move/Attack/Idle 전환이 덮지 못하게 dead set에 등록한다.
        m_setDeadServerMonsters.insert(monsterId);
        m_mapServerMonsterMoveTime.erase(monsterId);
        m_mapServerMonsterAttackTimer.erase(monsterId);

        // 일반 몬스터 공격 인디케이터 정리
        // 사망한 몬스터의 telegraph는 더 이상 필요 없으므로 숨김이 아니라 삭제 예약한다.
        if (EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>())
        {
            pEnemy->DestroyIndicators(pScene);

            if (IAttackBehavior* pBehavior = pEnemy->GetAttackBehavior())
                pBehavior->Reset();
        }

        if (pAnim)
        {
            const char* deathClip =
                (clipIt != m_mapServerMonsterClips.end() && !clipIt->second.death.empty())
                ? clipIt->second.death.c_str()
                : "Death";

            pAnim->CrossFade(deathClip, 0.15f, false, true);
        }
    }
    else
    {
        bool isBoss = false;
        bool isMiniBoss = false;

        if (clipIt != m_mapServerMonsterClips.end())
        {
            isBoss = clipIt->second.isBoss;
            isMiniBoss = clipIt->second.isMiniBoss;
        }

        // 보스는 일반 피격 모션을 거의 재생하지 않는다.
        // 데미지 숫자와 hit flash만으로 피격 피드백을 준다.
        if (isBoss)
        {
            // 보스는 gethit으로 패턴/공격 모션이 끊기지 않게 한다.
        }
        else
        {
            // 공격 중인 몬스터는 공격 모션 유지.
            auto atkIt = m_mapServerMonsterAttackTimer.find(monsterId);
            bool bAttackLocked = (atkIt != m_mapServerMonsterAttackTimer.end() && atkIt->second > 0.0f);

            // 공격 모션 중이면 일반 피격 모션 생략
            if (!bAttackLocked)
            {
                // 일반몹/중간보스별 피격 모션 재생 간격
                float hitAnimCooldown = isMiniBoss ? 1.0f : 0.5f;

                // 너무 작은 연속 피해는 모션 없이 데미지 숫자 + flash만 보여준다.
                float minHitAnimDamage = isMiniBoss ? 15.0f : 5.0f;

                bool bCooldownActive =
                    (m_mapServerMonsterHitAnimCooldown.find(monsterId) != m_mapServerMonsterHitAnimCooldown.end());

                if (pAnim && !bCooldownActive && damage >= minHitAnimDamage)
                {
                    const char* hitClip = GetMonsterHitReactClip(mt);
                    pAnim->CrossFade(hitClip, 0.08f, false, true);
                    m_mapServerMonsterCurrentAnimClip[monsterId] = hitClip;

                    // 피격 모션이 바로 Walk/Idle로 덮이지 않도록 짧게 잠금
                    m_mapServerMonsterAttackTimer[monsterId] = 0.25f;
                    m_mapServerMonsterMoveTime.erase(monsterId);

                    // 데미지는 계속 들어와도 gethit 모션은 일정 간격으로만 재생한다.
                    m_mapServerMonsterHitAnimCooldown[monsterId] = hitAnimCooldown;
                }
            }
        }
    }

    // 투사체 기반 스킬 폭발 VFX — 서버 권위 데미지 적용 시점에 트리거되어 VFX 와 데미지 표시가 동기화.
    //   skillType == 4 (RC): Fireball / WaterOrb / EarthShard. Wind RC (WindShot) 는 관통이라 폭발 없음.
    //   skillType == 2 (E): Earth 만 — RockThrow.
    // 공격자 원소 lookup: 로컬 자신 → PlayerComponent, 원격 → m_mapRemotePlayerElement.
    ElementType attackerElement = ElementType::None;
    if (attackerPlayerId == GetLocalPlayerId())
    {
        if (auto* pLocalPlayer = pScene->GetPlayer())
        {
            if (auto* pPC = pLocalPlayer->GetComponent<PlayerComponent>())
                attackerElement = pPC->GetElementType();
        }
    }
    else
    {
        auto eIt = m_mapRemotePlayerElement.find(attackerPlayerId);
        if (eIt != m_mapRemotePlayerElement.end())
            attackerElement = eIt->second;
    }

    bool shouldExplode = false;
    if (skillType == 4 /* SKILL_TYPE_MOUSE_RIGHT */ &&
        attackerElement != ElementType::Wind &&
        attackerElement != ElementType::None)
    {
        shouldExplode = true;     // Fireball / WaterOrb / EarthShard
    }
    else if (skillType == 2 /* SKILL_TYPE_E */ && attackerElement == ElementType::Earth)
    {
        shouldExplode = true;     // RockThrow
    }

    if (shouldExplode)
    {
        ProjectileManager* pProj = pScene->GetProjectileManager();
        if (pProj && pMonster->GetTransform())
        {
            pProj->SpawnExplosionParticles(pMonster->GetTransform()->GetPosition(), attackerElement);
        }
    }

    char buf[192];
    sprintf_s(buf, "[Network] MonsterDamage applied: id=%llu dmg=%.1f hp=%.1f dead=%d attacker=%llu skill=%d",
        monsterId, damage, currentHp, isDead ? 1 : 0, attackerPlayerId, skillType);
    WriteNetworkLog(buf);
}

void NetworkManager::ProcessRoomCleared(Scene* pScene, uint32 stageIndex, uint32 roomIndex)
{
    if (!pScene)
        return;

    // S_ROOM_CLEARED는 방 상태만 Cleared로 맞춘다.
    // 포탈/룬 오브젝트 생성 위치는 서버가 S_ROOM_REWARD_SPAWN으로 따로 보내므로 여기서 생성하지 않는다.
    CRoom* pRoom = pScene->GetCurrentRoom();
    if (pRoom)
    {
        if (pRoom->GetState() != RoomState::Cleared)
            pRoom->SetState(RoomState::Cleared);
    }

    char buf[128];
    sprintf_s(buf,
        "[Network] RoomCleared applied: stage=%u room=%u",
        stageIndex,
        roomIndex);
    WriteNetworkLog(buf);
}

void NetworkManager::ProcessRoomStart(Scene* pScene, uint64 starterPlayerId)
{
    if (!pScene)
        return;

    // 서버가 방 시작을 확정했으므로 모든 클라에서 시작 포탈을 즉시 숨긴다.
    pScene->HideInteractionCubeByNetworkStart();

    char buf[160];
    sprintf_s(buf,
        "[Network] RoomStart processed: starterPlayerId=%llu",
        starterPlayerId);
    WriteNetworkLog(buf);
}

void NetworkManager::ProcessRoomRewardSpawn(Scene* pScene, uint32 stageIndex, uint32 roomIndex, const DirectX::XMFLOAT3& portalPos, bool hasSecondPortal, const DirectX::XMFLOAT3& secondPortalPos, const std::vector<NetworkRewardRuneObjectInfo>& runeObjects)
{
    if (!pScene)
        return;

    CRoom* pRoom = pScene->GetCurrentRoom();
    if (!pRoom)
        return;

    // 서버가 방 클리어 보상 생성을 확정했으므로,
    // 혹시 S_ROOM_CLEARED보다 먼저 처리되어도 방 상태를 Cleared로 맞춘다.
    if (pRoom->GetState() != RoomState::Cleared)
        pRoom->SetState(RoomState::Cleared);

    pRoom->ClearPortalCube();
    pRoom->ClearSecondPortal();
    pRoom->ClearRewardRuneObjects();

    // 서버가 계산한 공통 위치에 메인/보라 포탈 생성
    pRoom->SpawnPortalCubeAt(portalPos);

    // 서버가 보조 포탈을 내려준 경우 빨간 포탈도 생성
    if (hasSecondPortal)
    {
        pRoom->SpawnSecondPortalAt(secondPortalPos, []()
            {
                NetworkManager* pNet = NetworkManager::GetInstance();
                if (pNet && pNet->IsConnected())
                {
                    pNet->SendPortalInteract(1);
                }
            });
    }

    // 서버가 계산한 각 플레이어 앞 위치와 서버가 결정한 룬 3개를 함께 적용한다.
    for (const auto& info : runeObjects)
    {
        pRoom->SpawnRewardRuneObjectAt(info.ownerPlayerId, info.pos, info.runeIds);
    }

    char buf[320];
    sprintf_s(buf,
        "[Network] RoomRewardSpawn applied: stage=%u room=%u portal=(%.2f, %.2f, %.2f) hasSecondPortal=%d second=(%.2f, %.2f, %.2f) runeCount=%d",
        stageIndex,
        roomIndex,
        portalPos.x,
        portalPos.y,
        portalPos.z,
        hasSecondPortal ? 1 : 0,
        secondPortalPos.x,
        secondPortalPos.y,
        secondPortalPos.z,
        static_cast<int>(runeObjects.size()));
    WriteNetworkLog(buf);
}

void NetworkManager::ProcessRuneRewardPicked(Scene* pScene, uint64 ownerPlayerId)
{
    if (!pScene)
        return;

    CRoom* pRoom = pScene->GetCurrentRoom();
    if (!pRoom)
        return;

    // 서버가 특정 플레이어의 룬 선택 완료를 알렸으므로,
    // 모든 클라에서 해당 플레이어 앞의 룬 오브젝트를 숨긴다.
    pRoom->HideRewardRuneObject(ownerPlayerId);

    char buf[160];
    sprintf_s(buf,
        "[Network] RuneRewardPicked applied: ownerPlayerId=%llu",
        ownerPlayerId);
    WriteNetworkLog(buf);
}

void NetworkManager::ProcessRuneEquip(Scene* pScene, uint64 playerId, uint32 skillSlot, uint32 runeSlotIndex, const std::string& runeId, uint32 stackCount)
{
    if (!pScene)
        return;

    if (skillSlot >= static_cast<uint32>(SkillSlot::Count))
        return;

    if (runeSlotIndex >= RUNES_PER_SKILL)
        return;

    GameObject* pTargetPlayer = nullptr;

    // 내 플레이어면 로컬 플레이어에 적용
    if (playerId == GetLocalPlayerId())
    {
        pTargetPlayer = pScene->GetPlayer();
    }
    else
    {
        auto it = m_mapRemotePlayers.find(playerId);
        if (it != m_mapRemotePlayers.end())
            pTargetPlayer = it->second;
    }

    // 해당 플레이어의 SkillComponent에 룬 장착 정보 적용
    SkillComponent* pSkill = pTargetPlayer ? pTargetPlayer->GetComponent<SkillComponent>() : nullptr;
    if (pSkill)
    {
        pSkill->SetRuneSlot(
            static_cast<SkillSlot>(skillSlot),
            static_cast<int>(runeSlotIndex),
            runeId,
            static_cast<int>(stackCount));
    }
    {
        char rbuf[256];
        sprintf_s(rbuf,
            "[RuneDiag] RuneEquip playerId=%llu slot=%u idx=%u rune=%s targetFound=%d skillComp=%d (mapSize=%zu)",
            playerId, skillSlot, runeSlotIndex, runeId.c_str(),
            pTargetPlayer ? 1 : 0, pSkill ? 1 : 0, m_mapRemotePlayers.size());
        WriteNetworkLog(rbuf);
    }

    char buf[256];
    sprintf_s(buf,
        "[Network] RuneEquip applied: playerId=%llu skillSlot=%u runeSlotIndex=%u runeId=%s stack=%u",
        playerId,
        skillSlot,
        runeSlotIndex,
        runeId.c_str(),
        stackCount);
    WriteNetworkLog(buf);
}

void NetworkManager::ProcessRuneHomingTarget(Scene* pScene, uint64 playerId, int32 skillSlot, int32 skillType, uint64 targetMonsterId, const DirectX::XMFLOAT3& targetPos, const DirectX::XMFLOAT3& originPos)
{
    // 1. 서버가 선택한 유도 타겟 몬스터를 클라 오브젝트 맵에서 찾는다.
    GameObject* pTargetMonster = GetServerMonster(targetMonsterId);

    char buf[384];
    sprintf_s(buf,
        "[Network] ProcessRuneHomingTarget: playerId=%llu skillSlot=%d skillType=%d targetMonsterId=%llu hasMonster=%d target=(%.2f, %.2f, %.2f) origin=(%.2f, %.2f, %.2f)",
        playerId,
        skillSlot,
        skillType,
        targetMonsterId,
        pTargetMonster ? 1 : 0,
        targetPos.x,
        targetPos.y,
        targetPos.z,
        originPos.x,
        originPos.y,
        originPos.z);

    WriteNetworkLog(buf);

    if (!pScene)
        return;

    ProjectileManager* pProjectileManager = pScene->GetProjectileManager();

    if (!pProjectileManager)
        return;

    // 2. 서버의 skillSlot 값을 클라 SkillSlot enum으로 변환한다.
    SkillSlot clientSkillSlot = SkillSlot::Count;

    switch (skillSlot)
    {
    case 0:
        clientSkillSlot = SkillSlot::Q;
        break;
    case 1:
        clientSkillSlot = SkillSlot::E;
        break;
    case 2:
        clientSkillSlot = SkillSlot::R;
        break;
    case 3:
        clientSkillSlot = SkillSlot::RightClick;
        break;
    default:
        clientSkillSlot = SkillSlot::Count;
        break;
    }

    // 3. 최근 발사된 해당 스킬 투사체에 서버 유도 타겟을 붙인다.
    bool attached = pProjectileManager->AttachNetworkHomingTarget(
        clientSkillSlot,
        originPos,
        targetMonsterId,
        targetPos
    );

    char attachBuf[256];
    sprintf_s(attachBuf,
        "[Network] RuneHomingTarget attach projectile: playerId=%llu skillSlot=%d targetMonsterId=%llu attached=%d",
        playerId,
        skillSlot,
        targetMonsterId,
        attached ? 1 : 0);

    WriteNetworkLog(attachBuf);

    // 비투사체 유도 룬은 붙일 투사체가 없을 수 있으므로 텍스트로도 발동을 표시한다.
    if (!attached && skillSlot != 3)
    {
        GameObject* pCaster = nullptr;

        if (playerId == GetLocalPlayerId())
        {
            pCaster = pScene ? pScene->GetPlayer() : nullptr;
        }
        else
        {
            auto it = m_mapRemotePlayers.find(playerId);
            if (it != m_mapRemotePlayers.end())
                pCaster = it->second;
        }

        DirectX::XMFLOAT3 textPos = originPos;

        if (pCaster && pCaster->GetTransform())
            textPos = pCaster->GetTransform()->GetPosition();

        textPos.y += 3.0f;

        DamageNumberManager::Get().AddText(
            textPos,
            L"HOMING!",
            DirectX::XMFLOAT4(0.65f, 1.0f, 0.75f, 1.0f));
    }
}

// ─── ProcessRuneTrigger ─────────────────────────────────────────────────────
//   서버에서 권위 룬 발동이 결정될 때마다 S_RUNE_TRIGGER 가 전달된다.
//   클라는 데미지/회복/보호막 등 숫자를 다시 계산하지 않고 서버 값(value1/value2)을
//   그대로 UI/VFX 에 반영한다. 룬 별 분기는 runeId 기준.
void NetworkManager::ProcessRuneTrigger(Scene* pScene,
    uint64 playerId, int32 skillSlot, int32 skillType,
    const std::string& runeId, int32 triggerType,
    uint64 targetMonsterId, uint64 targetPlayerId,
    uint64 objectId,
    const DirectX::XMFLOAT3& pos, float value1, float value2)
{
    if (!pScene)
        return;

    // 대상 플레이어 GameObject 조회 (룬 효과는 playerId 또는 targetPlayerId 기준)
    auto FindPlayer = [&](uint64 id) -> GameObject*
        {
            if (id == 0) return nullptr;
            if (id == GetLocalPlayerId())
                return pScene->GetPlayer();
            auto it = m_mapRemotePlayers.find(id);
            if (it != m_mapRemotePlayers.end())
                return it->second;
            return nullptr;
        };

    GameObject* pCaster = FindPlayer(playerId);
    GameObject* pTargetPlayer = (targetPlayerId != 0) ? FindPlayer(targetPlayerId) : nullptr;

    // 룬 효과 표시 기준 위치 — targetPlayer/caster 의 transform 우선, fallback 으로 packet pos
    auto GetDisplayPos = [&](GameObject* pObj) -> DirectX::XMFLOAT3
        {
            if (pObj)
            {
                if (auto* t = pObj->GetComponent<TransformComponent>())
                {
                    DirectX::XMFLOAT3 p = t->GetPosition();
                    p.y += 2.0f; // 머리 위쪽
                    return p;
                }
            }
            return DirectX::XMFLOAT3(pos.x, pos.y + 1.5f, pos.z);
        };

    const bool bLocalCaster = (playerId == GetLocalPlayerId());

    // ─────────────────────────────────────────────
    // 네트워크 룬 공통 VFX 헬퍼
    // 서버 룬 패킷을 받은 모든 클라에서 같은 위치에 같은 VFX를 띄운다.
    // ─────────────────────────────────────────────
    DecalManager* pDecals = pScene ? pScene->GetDecalManager() : nullptr;

    auto GetCasterPos = [&]() -> DirectX::XMFLOAT3
        {
            DirectX::XMFLOAT3 out = pos;

            if (pCaster && pCaster->GetTransform())
                out = pCaster->GetTransform()->GetPosition();

            return out;
        };

    // 서버가 보낸 룬 이벤트의 타겟 몬스터 위치.
    // 몬스터가 이미 사라졌으면 패킷 좌표 pos로 fallback.
    auto GetMonsterPos = [&](DirectX::XMFLOAT3& outPos) -> bool
        {
            if (targetMonsterId != 0)
            {
                if (GameObject* pMon = GetServerMonster(targetMonsterId))
                {
                    if (auto* t = pMon->GetComponent<TransformComponent>())
                    {
                        outPos = t->GetPosition();
                        return true;
                    }
                }
            }

            outPos = pos;
            return false;
        };

    // 서버가 보낸 특정 몬스터 ID의 월드 위치를 직접 조회한다.
    // TRF_CHA / TRF_ECH에서는 objectId = sourceMonsterId,
    // targetMonsterId = targetMonsterId 이므로 둘을 따로 조회해야 한다.
    auto GetMonsterWorldPosById = [&](uint64 monsterId, DirectX::XMFLOAT3& outPos) -> bool
        {
            if (monsterId != 0)
            {
                if (GameObject* pMon = GetServerMonster(monsterId))
                {
                    if (auto* t = pMon->GetComponent<TransformComponent>())
                    {
                        outPos = t->GetPosition();
                        return true;
                    }
                }
            }

            outPos = pos;
            return false;
        };

    auto SpawnRuneBurstAt = [&](const DirectX::XMFLOAT3& basePos,
        const char* spriteName,
        float size,
        float life,
        const DirectX::XMFLOAT4& color,
        float spin)
        {
            DirectX::XMFLOAT3 vfxPos = basePos;
            vfxPos.y += 1.2f;

            VFXSpriteManager::Get().Spawn(
                spriteName,
                vfxPos,
                size,
                life,
                color,
                spin,
                VFXSpriteAnim::FadeOut);
        };

    auto SpawnRuneGroundPulseAt = [&](const DirectX::XMFLOAT3& basePos,
        DecalTexture decal,
        float radius,
        const DirectX::XMFLOAT4& color)
        {
            if (!pDecals)
                return;

            DirectX::XMFLOAT3 decalPos = basePos;
            decalPos.y += 0.05f;

            pDecals->Spawn(
                decal,
                decalPos,
                radius,
                0.0f,
                1.0f,
                color);
        };

    auto SpawnStatusSprite = [&](const std::string& tex, const DirectX::XMFLOAT3& origin,
        float size, float lifetime, DirectX::XMFLOAT4 color,
        float spin = 0.f)
        {
            DirectX::XMFLOAT3 p = origin;
            p.y += 1.5f;
            VFXSpriteManager::Get().Spawn(tex, p, size, lifetime, color, spin, VFXSpriteAnim::FadeOut);
        };

    // 서버 이벤트 코드는 float 로 오지만 정수 의미 — 안전한 라운드.
    const int eventCode = static_cast<int>(value1 + 0.5f);

    auto ShowRuneCastMarker = [&](const DirectX::XMFLOAT3& basePos,
        const wchar_t* text,
        const DirectX::XMFLOAT4& color,
        const char* spriteName = "twirl1",
        float size = 170.f)
        {
            SpawnRuneBurstAt(
                basePos,
                spriteName,
                size,
                0.45f,
                color,
                5.0f);

            DamageNumberManager::Get().AddText(
                basePos,
                text,
                color);
        };

    // 서버가 value1=0으로 보낸 것은 "상태이상 적용"이 아니라
    // 장착 룬 시전 표시용 이벤트다.
    if (eventCode == 0)
    {
        DirectX::XMFLOAT3 castPos = GetCasterPos();

        if (runeId.rfind("FIR_", 0) == 0)
        {
            ShowRuneCastMarker(castPos, L"FIRE RUNE",
                DirectX::XMFLOAT4(1.0f, 0.35f, 0.15f, 0.95f));
            return;
        }
        if (runeId.rfind("WAT_", 0) == 0)
        {
            ShowRuneCastMarker(castPos, L"WATER RUNE",
                DirectX::XMFLOAT4(0.35f, 0.85f, 1.0f, 0.95f));
            return;
        }
        if (runeId.rfind("WND_", 0) == 0)
        {
            ShowRuneCastMarker(castPos, L"WIND RUNE",
                DirectX::XMFLOAT4(0.55f, 1.0f, 0.55f, 0.95f));
            return;
        }
        if (runeId.rfind("ERT_", 0) == 0)
        {
            ShowRuneCastMarker(castPos, L"EARTH RUNE",
                DirectX::XMFLOAT4(1.0f, 0.75f, 0.35f, 0.95f));
            return;
        }
    }

    // ─── 🔥 FIR_1~FIR_4: 화속성 (화상 적용 / 틱 / 종료 / 업화) ─────────────────
    if (runeId.rfind("FIR_", 0) == 0)
    {
        DirectX::XMFLOAT3 monsterPos; GetMonsterPos(monsterPos);
        DirectX::XMFLOAT3 textPos = monsterPos; textPos.y += 2.0f;

        switch (eventCode)
        {
        case 1: // 화상 적용/갱신 — value2 = 현재 화상 중첩 수
            SpawnStatusSprite("fire1", monsterPos, 110.f, 0.55f,
                { 1.0f, 0.55f, 0.15f, 1.0f }, 0.8f);
            break;
        case 2: // 틱 피해 — value2 = 실제 틱 피해량
        {
            wchar_t buf[24];
            swprintf_s(buf, L"%.0f", value2);
            DamageNumberManager::Get().AddText(textPos, buf,
                { 1.0f, 0.55f, 0.15f, 1.0f });
            break;
        }
        case 3: // 화상 종료 — 별도 표시 없음 (오라가 자연 페이드)
            break;
        case 4: // 업화 폭발 — value2 = 실제 폭발 피해량
        {
            SpawnStatusSprite("flare1", monsterPos, 360.f, 0.65f,
                { 1.0f, 0.4f, 0.1f, 1.0f }, 4.0f);
            wchar_t buf[32];
            swprintf_s(buf, L"BURN! %.0f", value2);
            DamageNumberManager::Get().AddText(textPos, buf,
                { 1.0f, 0.35f, 0.1f, 1.0f });
            break;
        }
        default: break;
        }
        return;
    }

    // ─── 💧 WAT_1~WAT_4: 수속성 (냉기 / 빙결 / 종료 / 동상 / 빙폭) ──────────────
    if (runeId.rfind("WAT_", 0) == 0)
    {
        DirectX::XMFLOAT3 monsterPos; GetMonsterPos(monsterPos);
        DirectX::XMFLOAT3 textPos = monsterPos; textPos.y += 2.0f;

        switch (eventCode)
        {
        case 1: // 냉기 적용/갱신 — value2 = 현재 냉기 중첩 수
            SpawnStatusSprite("twirl1", monsterPos, 120.f, 0.6f,
                { 0.45f, 0.75f, 1.0f, 1.0f }, 2.5f);
            break;
        case 2: // 빙결 발생 — value2 = 빙결 지속시간 (실제 정지/UI 는 S_MONSTER_STAGGER 가 처리)
            SpawnStatusSprite("star_08", monsterPos, 220.f, 0.5f,
                { 0.7f, 0.9f, 1.0f, 1.0f }, 0.f);
            break;
        case 3: // 냉기 종료
            break;
        case 4: // 동상 추가 피해 — value2 = 실제 추가 피해량
        {
            wchar_t buf[32];
            swprintf_s(buf, L"FROST %.0f", value2);
            DamageNumberManager::Get().AddText(textPos, buf,
                { 0.6f, 0.85f, 1.0f, 1.0f });
            break;
        }
        case 5: // 빙폭 광역 폭발 — value2 = 빙폭 피해량 (월드 위치 기준)
        {
            DirectX::XMFLOAT3 burstPos = pos; burstPos.y += 0.5f;
            VFXSpriteManager::Get().Spawn("flare1", burstPos, 420.f, 0.65f,
                { 0.55f, 0.85f, 1.0f, 1.0f }, 3.0f, VFXSpriteAnim::FadeOut);
            wchar_t buf[32];
            swprintf_s(buf, L"FROST BURST %.0f", value2);
            DirectX::XMFLOAT3 t2 = burstPos; t2.y += 1.5f;
            DamageNumberManager::Get().AddText(t2, buf,
                { 0.6f, 0.9f, 1.0f, 1.0f });
            break;
        }
        default: break;
        }
        return;
    }

    // ─── 🌀 WND_1~WND_4: 풍속성 (풍압 / 공중경직 / 종료 / 칼바람 / 폭풍) ─────────
    if (runeId.rfind("WND_", 0) == 0)
    {
        DirectX::XMFLOAT3 monsterPos; GetMonsterPos(monsterPos);
        DirectX::XMFLOAT3 textPos = monsterPos; textPos.y += 2.0f;

        switch (eventCode)
        {
        case 1: // 풍압 적용/갱신 — value2 = 현재 풍압 중첩 수
            SpawnStatusSprite("twirl1", monsterPos, 130.f, 0.55f,
                { 0.75f, 1.0f, 0.7f, 1.0f }, 4.0f);
            break;
        case 2: // 공중 경직 / 회오리 — value2 = 경직 시간
            SpawnStatusSprite("twirl1", monsterPos, 240.f, 0.7f,
                { 0.85f, 1.0f, 0.85f, 1.0f }, 6.0f);
            break;
        case 3: // 풍압 종료
            break;
        case 4: // 칼바람 추가 피해 — value2 = 실제 추가 피해량
        {
            wchar_t buf[32];
            swprintf_s(buf, L"BLADE %.0f", value2);
            DamageNumberManager::Get().AddText(textPos, buf,
                { 0.8f, 1.0f, 0.75f, 1.0f });
            break;
        }
        case 5: // 폭풍 광역 폭발 — value2 = 폭풍 피해량
        {
            DirectX::XMFLOAT3 burstPos = pos; burstPos.y += 0.5f;
            VFXSpriteManager::Get().Spawn("flare1", burstPos, 440.f, 0.7f,
                { 0.7f, 1.0f, 0.7f, 1.0f }, 5.0f, VFXSpriteAnim::FadeOut);
            wchar_t buf[32];
            swprintf_s(buf, L"STORM %.0f", value2);
            DirectX::XMFLOAT3 t2 = burstPos; t2.y += 1.5f;
            DamageNumberManager::Get().AddText(t2, buf,
                { 0.75f, 1.0f, 0.75f, 1.0f });
            break;
        }
        default: break;
        }
        return;
    }

    // ─── 🪨 ERT_1~ERT_4: 토속성 (균열 / 경직 / 종료 / 추가피해 / 냉기연계 / 붕괴) ─
    if (runeId.rfind("ERT_", 0) == 0)
    {
        DirectX::XMFLOAT3 monsterPos; GetMonsterPos(monsterPos);
        DirectX::XMFLOAT3 textPos = monsterPos; textPos.y += 2.0f;

        switch (eventCode)
        {
        case 1: // 균열 적용/갱신 — value2 = 현재 균열 스택
            SpawnStatusSprite("magic3", monsterPos, 150.f, 0.6f,
                { 0.85f, 0.65f, 0.4f, 1.0f }, 1.2f);
            break;
        case 2: // 균열 경직 — value2 = 경직 시간
            SpawnStatusSprite("flare1", monsterPos, 220.f, 0.55f,
                { 0.85f, 0.7f, 0.4f, 1.0f }, 0.f);
            break;
        case 3: // 균열 종료
            break;
        case 4: // 균열 추가 피해 — value2 = 추가 피해량
        {
            wchar_t buf[32];
            swprintf_s(buf, L"CRACK %.0f", value2);
            DamageNumberManager::Get().AddText(textPos, buf,
                { 0.9f, 0.7f, 0.35f, 1.0f });
            break;
        }
        case 5: // ERT_2 냉기 연계 — value2 = 냉기 지속시간
            SpawnStatusSprite("twirl1", monsterPos, 110.f, 0.55f,
                { 0.5f, 0.8f, 1.0f, 1.0f }, 2.0f);
            break;
        case 6: // ERT_4 붕괴 피해 — value2 = 붕괴 피해량
        {
            VFXSpriteManager::Get().Spawn("flare1",
                DirectX::XMFLOAT3(monsterPos.x, monsterPos.y + 0.8f, monsterPos.z),
                380.f, 0.65f, { 0.95f, 0.55f, 0.25f, 1.0f }, 3.0f, VFXSpriteAnim::FadeOut);
            wchar_t buf[32];
            swprintf_s(buf, L"COLLAPSE %.0f", value2);
            DamageNumberManager::Get().AddText(textPos, buf,
                { 1.0f, 0.6f, 0.25f, 1.0f });
            break;
        }
        default: break;
        }
        return;
    }

    // ─── ABY_TIM: 시간 역행 — 적중 시 해당 skillSlot 쿨다운 value1 만큼 감소 ────
    if (runeId == "ABY_TIM")
    {
        // 실제 쿨타임 감소는 본인 클라에만 적용 (서버 권위 멀티에선 예측 보정).
        if (bLocalCaster && pCaster)
        {
            if (skillSlot >= 0 && skillSlot < static_cast<int32>(SkillSlot::Count))
            {
                if (SkillComponent* pSkill = pCaster->GetComponent<SkillComponent>())
                    pSkill->ReduceCooldown(static_cast<SkillSlot>(skillSlot), value1, false); // 시계는 아래서 직접 스폰
            }
        }

        // 시계 VFX — PlayerComponent 추적 슬롯으로 띄운다(시전자 구분 없이).
        //   ReduceCooldown 내부 시계는 applied(쿨다운 실제 감소)에 게이팅돼 멀티 로컬에선 안 떴고,
        //   매번 새 스프라이트를 직접 스폰하면 다중 적중 시 시계가 겹쳤다. TriggerTimeRewindVFX 는
        //   Stop+교체 추적이라 항상 하나만 플레이어를 따라다님 → 오프라인과 동일한 깔끔한 시계.
        if (pCaster)
        {
            if (PlayerComponent* pPlayer = pCaster->GetComponent<PlayerComponent>())
                pPlayer->TriggerTimeRewindVFX();
        }
        return;
    }

    // ─── ABY_INF: 무한 룬 — 적중 시 확률 발동, 해당 skillSlot 쿨다운 즉시 초기화 ──
    if (runeId == "ABY_INF")
    {
        if (bLocalCaster && pCaster)
        {
            if (SkillComponent* pSkill = pCaster->GetComponent<SkillComponent>())
            {
                if (skillSlot >= 0 && skillSlot < static_cast<int32>(SkillSlot::Count))
                {
                    pSkill->ResetCooldown(static_cast<SkillSlot>(skillSlot));
                }
            }
        }

        DirectX::XMFLOAT3 casterPos = GetCasterPos();
        casterPos.y += 1.5f;

        // 쿨초 발동 별 아이콘
        VFXSpriteManager::Get().Spawn(
            "star_08",
            casterPos,
            130.f,
            0.85f,
            DirectX::XMFLOAT4(1.0f, 0.95f, 0.35f, 1.0f),
            0.0f,
            VFXSpriteAnim::SkullPop);

        // 시간/순환 느낌 회전
        VFXSpriteManager::Get().Spawn(
            "twirl1",
            casterPos,
            230.f,
            0.65f,
            DirectX::XMFLOAT4(1.0f, 0.88f, 0.25f, 0.9f),
            9.0f,
            VFXSpriteAnim::FadeOut);

        DamageNumberManager::Get().AddText(
            GetDisplayPos(pCaster),
            L"INFINITE!",
            DirectX::XMFLOAT4(1.0f, 0.95f, 0.4f, 1.0f));

        return;
    }

    // ─── ABY_VMP: 흡혈 — value1 회복량, value2 회복 후 HP ────────────────────────
    if (runeId == "ABY_VMP")
    {
        if (pCaster)
        {
            if (PlayerComponent* pPlayer = pCaster->GetComponent<PlayerComponent>())
            {
                if (bLocalCaster)
                    pPlayer->SetCurrentHP(value2);

                // 흡혈 송곳니(fang) 펄스 — 오프라인과 동일. proc 시 항상 표시(풀피여도).
                pPlayer->TriggerLifestealVFX(value1);
            }
        }

        // (오프라인 클라는 송곳니만 띄운다 → 멀티도 일치시키기 위해 symbol_01/trace_05 잉여 연출 제거)

        // 실제 회복이 있을 때만 +HP 숫자 표시 (풀피 proc 은 송곳니만 → "+0 HP" 방지)
        if (value1 > 0.0f)
        {
            wchar_t buf[32];
            swprintf_s(buf, L"+%.0f HP", value1);

            DamageNumberManager::Get().AddText(
                GetDisplayPos(pCaster),
                buf,
                DirectX::XMFLOAT4(0.4f, 1.0f, 0.4f, 1.0f));
        }

        return;
    }

    // ─── TRF_MLT: 다연발 — 비투사체에서는 추가 타격 보정으로 적용됨 ─────────────
    if (runeId == "TRF_MLT")
    {
        DirectX::XMFLOAT3 casterPos = GetCasterPos();

        SpawnRuneBurstAt(
            casterPos,
            "twirl1",
            180.f,
            0.45f,
            DirectX::XMFLOAT4(0.75f, 0.95f, 1.0f, 0.9f),
            8.0f);

        DamageNumberManager::Get().AddText(
            GetDisplayPos(pCaster),
            L"MULTI!",
            DirectX::XMFLOAT4(0.75f, 0.95f, 1.0f, 1.0f));

        return;
    }

    // ─── TRF_PRC: 관통 — 비투사체에서는 관통 보정 피해로 적용됨 ────────────────
    if (runeId == "TRF_PRC")
    {
        DirectX::XMFLOAT3 hitPos;
        GetMonsterPos(hitPos);

        SpawnRuneBurstAt(
            hitPos,
            "flare1",
            190.f,
            0.35f,
            DirectX::XMFLOAT4(0.85f, 0.85f, 1.0f, 0.95f),
            0.0f);

        DamageNumberManager::Get().AddText(
            GetDisplayPos(pCaster),
            L"PIERCE!",
            DirectX::XMFLOAT4(0.85f, 0.85f, 1.0f, 1.0f));

        return;
    }

    // ─── ABY_RES: 원소 공명 — 공명 펄스 ───────────────────────────────────────
    if (runeId == "ABY_RES")
    {
        DirectX::XMFLOAT3 casterPos = GetCasterPos();

        SpawnRuneBurstAt(
            casterPos,
            "flare1",
            280.f,
            0.50f,
            DirectX::XMFLOAT4(1.0f, 0.85f, 0.25f, 1.0f),
            0.0f);

        SpawnRuneBurstAt(
            casterPos,
            "twirl1",
            210.f,
            0.65f,
            DirectX::XMFLOAT4(0.45f, 0.85f, 1.0f, 0.85f),
            5.5f);

        DamageNumberManager::Get().AddText(
            GetDisplayPos(pCaster),
            L"RESONANCE!",
            DirectX::XMFLOAT4(1.0f, 0.85f, 0.35f, 1.0f));

        return;
    }

    // ─── ABY_SHD: 보호막 — value1 생성/흡수량, value2 현재 보호막 ────────────────
    if (runeId == "ABY_SHD")
    {
        // 현재 서버는 보호막 대상이 시전자 본인이지만,
        // targetPlayerId를 우선 쓰면 나중에 아군 보호막 룬이 생겨도 그대로 대응 가능하다.
        GameObject* pShieldTarget = pTargetPlayer ? pTargetPlayer : pCaster;

        if (pShieldTarget)
        {
            PlayerComponent* pPlayer = pShieldTarget->GetComponent<PlayerComponent>();

            // 기존 원격 플레이어가 PlayerComponent 없이 생성돼 있었던 경우를 방어한다.
            if (!pPlayer)
            {
                pPlayer = pShieldTarget->AddComponent<PlayerComponent>();

                if (pPlayer)
                {
                    uint64 elemPlayerId = (targetPlayerId != 0) ? targetPlayerId : playerId;
                    pPlayer->SetElementType(GetPlayerElement(elemPlayerId));
                }
            }

            if (pPlayer)
            {
                // 서버가 보낸 현재 보호막 수치를 로컬/원격 모두 반영한다.
                pPlayer->SetShield(value2);

                // 보호막 흡수 이벤트는 skillSlot = -1로 들어온다.
                // 이때는 깨짐 펄스도 전체 클라에서 재생한다.
                if (skillSlot < 0)
                    pPlayer->TriggerShieldBreakVFX();

                // 생성 직후 바로 한 번 갱신해서 다음 프레임까지 기다리지 않고 오라를 띄운다.
                pPlayer->UpdateNetworkRuneVFX(0.0f);
            }
        }

        wchar_t buf[32];
        swprintf_s(buf, L"+%.0f SHIELD", value1);

        DamageNumberManager::Get().AddText(
            GetDisplayPos(pShieldTarget ? pShieldTarget : pCaster),
            buf,
            DirectX::XMFLOAT4(0.5f, 0.85f, 1.0f, 1.0f));

        return;
    }

    // ─── ABY_RVG: 보복 — value1 비율, value2 최종 데미지 ─────────────────────────
    //   현재 서버는 CONSUME 시점에만 ABY_RVG 패킷을 보내며 triggerType 을 세팅하지 않는다.
    //     (Room.cpp BroadcastRevengeRuneTrigger 주석: "나중에 proto에 RUNE_TRIGGER_REVENGE를 추가하면…")
    //   READY 활성화는 ProcessPlayerDamage 에서 클라가 자체 추적(서버와 동일 조건)으로 처리.
    //   따라서 ABY_RVG runeId 자체를 CONSUME 으로 해석한다. triggerType 명시되면 그쪽 우선.
    if (runeId == "ABY_RVG")
    {
        const bool bConsume = (triggerType == 31 /* VENGEANCE_CONSUME */)
            || (triggerType == 0  /* NONE — 현재 서버 동작 */);
        if (bConsume)
        {
            if (pCaster)
            {
                if (auto* pPC = pCaster->GetComponent<PlayerComponent>())
                    pPC->ConsumeVengeance();
            }

            DirectX::XMFLOAT3 casterPos = GetCasterPos();
            casterPos.y += 1.5f;

            // 반격 베기 이펙트
            VFXSpriteManager::Get().Spawn(
                "slash_02",
                casterPos,
                180.f,
                0.45f,
                DirectX::XMFLOAT4(1.0f, 0.55f, 0.2f, 1.0f),
                4.0f,
                VFXSpriteAnim::FadeOut);

            // 반격 스파크
            VFXSpriteManager::Get().Spawn(
                "spark_06",
                casterPos,
                140.f,
                0.35f,
                DirectX::XMFLOAT4(1.0f, 0.75f, 0.25f, 1.0f),
                6.0f,
                VFXSpriteAnim::FadeOut);

            DamageNumberManager::Get().AddText(
                GetDisplayPos(pCaster),
                L"REVENGE!",
                DirectX::XMFLOAT4(1.0f, 0.55f, 0.2f, 1.0f));
        }
        else if (triggerType == 30 /* VENGEANCE_READY — 서버가 향후 보내올 경우 대비 */)
        {
            if (pCaster)
                if (auto* pPC = pCaster->GetComponent<PlayerComponent>())
                    pPC->TriggerVengeance(10.f);
        }
        return;
    }

    // ─── ABY_OVL: 과열 — READY / CONSUME 모두 네트워크 VFX 표시 ─────────────────
    if (runeId == "ABY_OVL")
    {
        DirectX::XMFLOAT3 casterPos = GetCasterPos();

        if (triggerType == 40 /* RUNE_TRIGGER_OVERHEAT_READY */ || value1 < 0.0f)
        {
            // 과열 준비 — 시전자 주변에 붉은 예열 펄스
            SpawnRuneBurstAt(
                casterPos,
                "twirl1",
                210.f,
                0.55f,
                DirectX::XMFLOAT4(1.0f, 0.35f, 0.12f, 1.0f),
                7.5f);

            DamageNumberManager::Get().AddText(
                GetDisplayPos(pCaster),
                L"OVERHEAT READY",
                DirectX::XMFLOAT4(1.0f, 0.45f, 0.15f, 1.0f));
        }
        else
        {
            // 과열 소모 — 실제 강화타 순간. 서버가 데미지는 이미 반영했으므로 연출만 한다.
            SpawnRuneBurstAt(
                casterPos,
                "flare1",
                320.f,
                0.45f,
                DirectX::XMFLOAT4(1.0f, 0.28f, 0.08f, 1.0f),
                0.0f);

            SpawnRuneGroundPulseAt(
                casterPos,
                DecalTexture::MagicCircle,
                3.2f,
                DirectX::XMFLOAT4(1.0f, 0.25f, 0.05f, 1.0f));

            DamageNumberManager::Get().AddText(
                GetDisplayPos(pCaster),
                L"OVERHEAT!",
                DirectX::XMFLOAT4(1.0f, 0.4f, 0.15f, 1.0f));
        }

        return;
    }

    // ─── ABY_EXC: 처형자 — HP30%↓ 몬스터에 +50% 데미지. 처형 킬 마커. ────────────
    if (runeId == "ABY_EXC")
    {
        GameObject* pMonster = GetServerMonster(targetMonsterId);

        DirectX::XMFLOAT3 markPos = pos;
        if (pMonster)
        {
            if (auto* t = pMonster->GetComponent<TransformComponent>())
            {
                markPos = t->GetPosition();
                markPos.y += 0.05f;
            }
        }

        // 바닥 해골 데칼
        if (pDecals)
        {
            pDecals->Spawn(
                DecalTexture::Skull,
                markPos,
                2.7f,
                0.0f,
                1.6f,
                DirectX::XMFLOAT4(1.0f, 0.25f, 0.25f, 1.0f));
        }

        // 머리 위 해골 아이콘
        {
            DirectX::XMFLOAT3 iconPos = markPos;
            iconPos.y += 2.6f;

            VFXSpriteManager::Get().Spawn(
                "skull",
                iconPos,
                120.f,
                1.1f,
                DirectX::XMFLOAT4(1.0f, 0.25f, 0.25f, 1.0f),
                0.0f,
                VFXSpriteAnim::SkullPop);
        }

        // 처형 스파크
        {
            DirectX::XMFLOAT3 sparkPos = markPos;
            sparkPos.y += 1.7f;

            VFXSpriteManager::Get().Spawn(
                "spark_05",
                sparkPos,
                150.f,
                0.45f,
                DirectX::XMFLOAT4(1.0f, 0.35f, 0.25f, 1.0f),
                6.0f,
                VFXSpriteAnim::FadeOut);
        }

        DirectX::XMFLOAT3 textPos = markPos;
        textPos.y += 2.0f;

        DamageNumberManager::Get().AddText(
            textPos,
            L"EXECUTE!",
            DirectX::XMFLOAT4(1.0f, 0.25f, 0.25f, 1.0f));

        return;
    }

    // ─── ABY_ECO: 메아리 — ECHO_SCHEDULE(예약 마법진) / ECHO_FIRE(실제 추가타) ────
// triggerType 10 = ECHO_SCHEDULE, 11 = ECHO_FIRE
    if (runeId == "ABY_ECO")
    {
        GameObject* pMonster = GetServerMonster(targetMonsterId);

        DirectX::XMFLOAT3 ringPos = pos;
        if (pMonster)
        {
            if (auto* t = pMonster->GetComponent<TransformComponent>())
            {
                ringPos = t->GetPosition();
                ringPos.y += 0.05f;
            }
        }

        const bool bEchoFire =
            (triggerType == 11 /* ECHO_FIRE */) ||
            (triggerType == 0 && value1 >= 0.0f);

        if (bEchoFire)
        {
            // 실제 추가타 — 원본 스킬 VFX 50% 스케일
            ElementType casterElem = GetPlayerElement(playerId);
            uint32_t casterRuneFlags = RUNE_NONE;

            if (pCaster)
            {
                SkillSlot echoSlot = SkillSlot::Count;

                switch (skillSlot)
                {
                case 0: echoSlot = SkillSlot::Q;          break;
                case 1: echoSlot = SkillSlot::E;          break;
                case 2: echoSlot = SkillSlot::R;          break;
                case 3: echoSlot = SkillSlot::RightClick; break;
                default: break;
                }

                if (echoSlot != SkillSlot::Count)
                {
                    if (SkillComponent* pCasterSkill = pCaster->GetComponent<SkillComponent>())
                    {
                        RuneCombo combo = pCasterSkill->GetRuneCombo(echoSlot);
                        casterRuneFlags = ToRuneFlags(combo);
                    }
                }
            }

            SpawnEchoSkillVFX(pScene, skillType, casterElem, ringPos, casterRuneFlags);

            // 실제 발동 바닥 마법진
            if (pDecals)
            {
                pDecals->Spawn(
                    DecalTexture::Magic2,
                    ringPos,
                    3.0f,
                    0.0f,
                    0.8f,
                    DirectX::XMFLOAT4(0.85f, 0.55f, 1.0f, 1.0f));
            }

            // 실제 발동 표적 아이콘
            {
                DirectX::XMFLOAT3 iconPos = ringPos;
                iconPos.y += 2.3f;

                VFXSpriteManager::Get().Spawn(
                    "magic2",
                    iconPos,
                    135.f,
                    0.85f,
                    DirectX::XMFLOAT4(0.9f, 0.6f, 1.0f, 1.0f),
                    3.0f,
                    VFXSpriteAnim::FadeOut);
            }

            // 발동 회전 잔상
            {
                DirectX::XMFLOAT3 twirlPos = ringPos;
                twirlPos.y += 1.8f;

                VFXSpriteManager::Get().Spawn(
                    "twirl2",
                    twirlPos,
                    170.f,
                    0.65f,
                    DirectX::XMFLOAT4(0.75f, 0.45f, 1.0f, 0.9f),
                    7.0f,
                    VFXSpriteAnim::FadeOut);
            }

            DirectX::XMFLOAT3 textPos = ringPos;
            textPos.y += 2.0f;

            wchar_t buf[32];
            swprintf_s(buf, L"ECHO %.0f", value2);

            DamageNumberManager::Get().AddText(
                textPos,
                buf,
                DirectX::XMFLOAT4(0.85f, 0.6f, 1.0f, 1.0f));
        }
        else
        {
            // 예약 마법진
            if (pDecals)
            {
                pDecals->Spawn(
                    DecalTexture::Magic3,
                    ringPos,
                    3.5f,
                    0.0f,
                    2.0f,
                    DirectX::XMFLOAT4(0.7f, 0.5f, 1.0f, 1.0f),
                    DirectX::XM_PI,
                    0.3f);
            }

            // 예약 표적 아이콘
            {
                DirectX::XMFLOAT3 iconPos = ringPos;
                iconPos.y += 2.2f;

                VFXSpriteManager::Get().Spawn(
                    "magic3",
                    iconPos,
                    115.f,
                    1.6f,
                    DirectX::XMFLOAT4(0.7f, 0.5f, 1.0f, 0.95f),
                    2.5f,
                    VFXSpriteAnim::FadeOut);
            }

            // 예약 회전 표시
            {
                DirectX::XMFLOAT3 twirlPos = ringPos;
                twirlPos.y += 1.6f;

                VFXSpriteManager::Get().Spawn(
                    "twirl3",
                    twirlPos,
                    150.f,
                    1.4f,
                    DirectX::XMFLOAT4(0.6f, 0.45f, 1.0f, 0.85f),
                    5.5f,
                    VFXSpriteAnim::FadeOut);
            }
        }

        return;
    }

    // ─── TRF_CHA: 연쇄 정밀 처리 ─────────────────────────────────────
    // objectId = sourceMonsterId
    // targetMonsterId = chainTargetMonsterId
    if (runeId == "TRF_CHA")
    {
        DirectX::XMFLOAT3 sourcePos;
        DirectX::XMFLOAT3 targetPos2;

        GetMonsterWorldPosById(objectId, sourcePos);
        GetMonsterWorldPosById(targetMonsterId, targetPos2);

        SpawnRuneBurstAt(
            sourcePos,
            "twirl1",
            140.f,
            0.25f,
            DirectX::XMFLOAT4(0.55f, 0.85f, 1.0f, 0.85f),
            8.0f);

        SpawnRuneBurstAt(
            targetPos2,
            "flare1",
            230.f,
            0.40f,
            DirectX::XMFLOAT4(0.55f, 0.85f, 1.0f, 0.95f),
            8.0f);

        DamageNumberManager::Get().AddText(
            DirectX::XMFLOAT3(targetPos2.x, targetPos2.y + 2.0f, targetPos2.z),
            L"CHAIN!",
            DirectX::XMFLOAT4(0.55f, 0.85f, 1.0f, 1.0f));

        return;
    }

    // ─── TRF_ECH: 반향 정밀 처리 ─────────────────────────────────────
    // objectId = sourceMonsterId
    // targetMonsterId = echoTargetMonsterId
    if (runeId == "TRF_ECH")
    {
        DirectX::XMFLOAT3 sourcePos;
        DirectX::XMFLOAT3 targetPos2;

        GetMonsterWorldPosById(objectId, sourcePos);
        GetMonsterWorldPosById(targetMonsterId, targetPos2);

        SpawnRuneBurstAt(
            sourcePos,
            "twirl1",
            140.f,
            0.30f,
            DirectX::XMFLOAT4(0.75f, 0.55f, 1.0f, 0.85f),
            6.5f);

        SpawnRuneBurstAt(
            targetPos2,
            "flare1",
            230.f,
            0.42f,
            DirectX::XMFLOAT4(0.80f, 0.60f, 1.0f, 0.95f),
            6.5f);

        DamageNumberManager::Get().AddText(
            DirectX::XMFLOAT3(targetPos2.x, targetPos2.y + 2.0f, targetPos2.z),
            L"ECHO SHOT",
            DirectX::XMFLOAT4(0.8f, 0.6f, 1.0f, 1.0f));

        return;
    }

    // ─── TRF_DEP: 설치 룬 정밀 처리 ─────────────────────────────────────
    // objectId = 서버 trapId
    // targetMonsterId == 0 : 설치
    // targetMonsterId != 0 : 발동
    if (runeId == "TRF_DEP")
    {
        const bool bPlaced = (targetMonsterId == 0);
        const bool bTriggered = (targetMonsterId != 0);

        if (bPlaced)
        {
            // 같은 trapId가 다시 오면 기존 표식을 제거하고 새로 만든다.
            if (objectId != 0)
            {
                auto oldIt = m_mapNetworkTrapVFXByObjectId.find(objectId);
                if (oldIt != m_mapNetworkTrapVFXByObjectId.end())
                {
                    if (pDecals && oldIt->second.decalId >= 0)
                        pDecals->Stop(oldIt->second.decalId);

                    m_mapNetworkTrapVFXByObjectId.erase(oldIt);
                }
            }

            DirectX::XMFLOAT3 trapPos = pos;
            trapPos.y += 0.05f;

            int decalId = -1;

            if (pDecals)
            {
                decalId = pDecals->Spawn(
                    DecalTexture::Star08,
                    trapPos,
                    value2 > 0.0f ? value2 : 5.0f,
                    0.0f,
                    30.0f,
                    DirectX::XMFLOAT4(0.75f, 0.90f, 1.0f, 0.95f),
                    1.5f,
                    0.2f);
            }

            if (objectId != 0)
            {
                NetworkRuneTrapVFX trapState;
                trapState.decalId = decalId;
                trapState.pos = trapPos;
                trapState.ownerPlayerId = playerId;
                trapState.skillSlot = skillSlot;

                m_mapNetworkTrapVFXByObjectId[objectId] = trapState;
            }

            SpawnRuneBurstAt(
                trapPos,
                "twirl1",
                150.f,
                0.35f,
                DirectX::XMFLOAT4(0.75f, 0.90f, 1.0f, 0.95f),
                4.0f);

            DamageNumberManager::Get().AddText(
                DirectX::XMFLOAT3(trapPos.x, trapPos.y + 2.0f, trapPos.z),
                L"TRAP SET",
                DirectX::XMFLOAT4(0.75f, 0.90f, 1.0f, 1.0f));

            return;
        }

        if (bTriggered)
        {
            DirectX::XMFLOAT3 hitPos;
            GetMonsterWorldPosById(targetMonsterId, hitPos);

            // trapId 기준으로 설치 표식 제거
            if (objectId != 0)
            {
                auto it = m_mapNetworkTrapVFXByObjectId.find(objectId);
                if (it != m_mapNetworkTrapVFXByObjectId.end())
                {
                    if (pDecals && it->second.decalId >= 0)
                        pDecals->Stop(it->second.decalId);

                    SpawnRuneBurstAt(
                        it->second.pos,
                        "flare1",
                        260.f,
                        0.40f,
                        DirectX::XMFLOAT4(1.0f, 0.55f, 0.25f, 0.95f),
                        0.0f);

                    m_mapNetworkTrapVFXByObjectId.erase(it);
                }
            }

            SpawnRuneBurstAt(
                hitPos,
                "flare1",
                300.f,
                0.45f,
                DirectX::XMFLOAT4(1.0f, 0.55f, 0.25f, 0.95f),
                0.0f);

            DamageNumberManager::Get().AddText(
                DirectX::XMFLOAT3(hitPos.x, hitPos.y + 2.0f, hitPos.z),
                L"TRAP HIT",
                DirectX::XMFLOAT4(1.0f, 0.65f, 0.3f, 1.0f));

            return;
        }
    }

    // ─── TRF_EMP: 증강 룬 정밀 처리 ─────────────────────────────────────
    // objectId = 0 : 증강 버프 적용
    // objectId = 1 : 다음 공격에 증강 소모
    if (runeId == "TRF_EMP")
    {
        const bool bApply = (objectId == 0);
        const bool bConsume = (objectId == 1);

        DirectX::XMFLOAT3 casterPos = GetCasterPos();
        casterPos.y += 1.5f;

        FluidSkillVFXManager* pVFX = pScene ? pScene->GetFluidVFXManager() : nullptr;

        if (bApply)
        {
            if (pVFX)
            {
                auto oldIt = m_mapRemoteEnhanceVFXId.find(playerId);
                if (oldIt != m_mapRemoteEnhanceVFXId.end() && oldIt->second >= 0)
                    pVFX->StopEffect(oldIt->second);

                if (EffectRegistry::Get().HasEffect("charge_gather"))
                {
                    EffectDef def = EffectRegistry::Get().GetEffect("charge_gather");

                    for (auto& l : def.layers)
                    {
                        l.overrideColors = true;
                        l.coreColor = { 1.0f, 0.90f, 0.30f, 1.0f };
                        l.edgeColor = { 1.0f, 0.55f, 0.10f, 0.90f };
                        l.sizeScale *= 1.25f;
                    }

                    int vfxId = pVFX->SpawnEffectDef(
                        casterPos,
                        DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f),
                        def,
                        false);

                    m_mapRemoteEnhanceVFXId[playerId] = vfxId;

                    float duration = value2 > 0.0f ? value2 : 5.0f;
                    if (vfxId >= 0)
                        m_vTimedVFXKills.push_back({ vfxId, duration });
                }
            }

            DamageNumberManager::Get().AddText(
                GetDisplayPos(pCaster),
                L"ENHANCE READY",
                DirectX::XMFLOAT4(1.0f, 0.85f, 0.35f, 1.0f));

            return;
        }

        if (bConsume)
        {
            if (pVFX)
            {
                auto it = m_mapRemoteEnhanceVFXId.find(playerId);
                if (it != m_mapRemoteEnhanceVFXId.end())
                {
                    if (it->second >= 0)
                        pVFX->StopEffect(it->second);

                    m_mapRemoteEnhanceVFXId.erase(it);
                }
            }

            SpawnRuneBurstAt(
                casterPos,
                "flare1",
                270.f,
                0.45f,
                DirectX::XMFLOAT4(1.0f, 0.75f, 0.25f, 0.95f),
                0.0f);

            DamageNumberManager::Get().AddText(
                GetDisplayPos(pCaster),
                L"ENHANCE HIT",
                DirectX::XMFLOAT4(1.0f, 0.75f, 0.25f, 1.0f));

            return;
        }
    }

    // ─── TRF_ORB: 궤도 룬 정밀 처리 ─────────────────────────────────────
 // objectId = 서버 orbitalId
 // start: targetMonsterId == 0 && 0 < value1 < 1.0f
 // fire : value1 == 0.0f 또는 value1 >= 1.0f
    if (runeId == "TRF_ORB")
    {
        DirectX::XMFLOAT3 casterPos = GetCasterPos();
        casterPos.y += 1.5f;

        FluidSkillVFXManager* pVFX = pScene ? pScene->GetFluidVFXManager() : nullptr;

        const bool bStart =
            objectId != 0 &&
            targetMonsterId == 0 &&
            value1 > 0.0f &&
            value1 < 1.0f;

        const bool bFire = !bStart;

        if (bStart)
        {
            int vfxId = -1;

            if (pVFX && EffectRegistry::Get().HasEffect("sub_orbital_halo"))
            {
                EffectDef def = EffectRegistry::Get().GetEffect("sub_orbital_halo");

                vfxId = pVFX->SpawnEffectDef(
                    casterPos,
                    DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f),
                    def,
                    false);
            }
            else
            {
                SpawnRuneBurstAt(
                    casterPos,
                    "twirl1",
                    230.f,
                    0.60f,
                    DirectX::XMFLOAT4(0.65f, 0.75f, 1.0f, 0.95f),
                    9.0f);
            }

            NetworkOrbitalRuneVFX orbitalState;
            orbitalState.vfxId = vfxId;
            orbitalState.ownerPlayerId = playerId;

            m_mapNetworkOrbitalVFXByObjectId[objectId] = orbitalState;

            DamageNumberManager::Get().AddText(
                GetDisplayPos(pCaster),
                L"ORBIT READY",
                DirectX::XMFLOAT4(0.65f, 0.75f, 1.0f, 1.0f));

            return;
        }

        if (bFire)
        {
            if (objectId != 0)
            {
                auto it = m_mapNetworkOrbitalVFXByObjectId.find(objectId);
                if (it != m_mapNetworkOrbitalVFXByObjectId.end())
                {
                    if (pVFX && it->second.vfxId >= 0)
                        pVFX->StopEffect(it->second.vfxId);

                    m_mapNetworkOrbitalVFXByObjectId.erase(it);
                }
            }

            DirectX::XMFLOAT3 firePos = casterPos;

            if (targetMonsterId != 0)
                GetMonsterWorldPosById(targetMonsterId, firePos);

            SpawnRuneBurstAt(
                firePos,
                "flare1",
                260.f,
                0.45f,
                DirectX::XMFLOAT4(0.65f, 0.75f, 1.0f, 0.95f),
                9.0f);

            DamageNumberManager::Get().AddText(
                DirectX::XMFLOAT3(firePos.x, firePos.y + 2.0f, firePos.z),
                L"ORBIT FIRE",
                DirectX::XMFLOAT4(0.65f, 0.75f, 1.0f, 1.0f));

            return;
        }
    }

    // ─── TRF_CHG / TRF_CHN / TRF_HOM ─────────────────────────────
    if (runeId == "TRF_CHG" || runeId == "TRF_CHN" || runeId == "TRF_HOM")
    {
        DirectX::XMFLOAT3 casterPos = GetCasterPos();

        const wchar_t* text = L"TRANSFORM";
        DirectX::XMFLOAT4 color = DirectX::XMFLOAT4(0.75f, 0.75f, 1.0f, 0.95f);

        if (runeId == "TRF_CHG")
        {
            text = L"CHARGE";
            color = DirectX::XMFLOAT4(1.0f, 0.75f, 0.35f, 0.95f);
        }
        else if (runeId == "TRF_CHN")
        {
            text = L"CHANNEL";
            color = DirectX::XMFLOAT4(0.55f, 0.95f, 1.0f, 0.95f);
        }
        else if (runeId == "TRF_HOM")
        {
            text = L"HOMING";
            color = DirectX::XMFLOAT4(0.65f, 1.0f, 0.75f, 0.95f);
        }

        SpawnRuneBurstAt(
            casterPos,
            "twirl1",
            190.f,
            0.45f,
            color,
            6.0f);

        DamageNumberManager::Get().AddText(
            GetDisplayPos(pCaster),
            text,
            color);

        return;
    }

    // ─── AMP_/CH_ 패시브 룬 표시 ──────────────────────────────────
    if (runeId.rfind("AMP_", 0) == 0 || runeId.rfind("CH_", 0) == 0)
    {
        DirectX::XMFLOAT3 casterPos = GetCasterPos();

        const wchar_t* text = L"AMPLIFY";
        DirectX::XMFLOAT4 color = DirectX::XMFLOAT4(1.0f, 0.9f, 0.45f, 0.95f);

        if (runeId.rfind("AMP_DMG", 0) == 0)
            text = L"DMG AMP";
        else if (runeId.rfind("AMP_RAD", 0) == 0)
            text = L"RANGE AMP";
        else if (runeId.rfind("AMP_CD", 0) == 0)
            text = L"COOLDOWN";
        else if (runeId.rfind("AMP_DUR", 0) == 0)
            text = L"DURATION";
        else if (runeId.rfind("AMP_SPD", 0) == 0)
            text = L"SPEED AMP";
        else if (runeId.rfind("AMP_MCO", 0) == 0)
            text = L"MANA SAVE";
        else if (runeId.rfind("CH_", 0) == 0)
        {
            text = L"CHANNEL+";
            color = DirectX::XMFLOAT4(0.65f, 0.95f, 1.0f, 0.95f);
        }

        SpawnRuneBurstAt(
            casterPos,
            "twirl1",
            170.f,
            0.40f,
            color,
            5.0f);

        DamageNumberManager::Get().AddText(
            GetDisplayPos(pCaster),
            text,
            color);

        return;
    }

    // ─────────────────────────────────────────────
    // 최종 fallback
    // 전용 분기가 없는 서버 룬 트리거도 최소한 양쪽 클라에 공통 룬 발동 표시를 띄운다.
    // ─────────────────────────────────────────────
    {
        DirectX::XMFLOAT3 fallbackPos = GetCasterPos();

        if (targetMonsterId != 0)
            GetMonsterPos(fallbackPos);

        DirectX::XMFLOAT4 color = DirectX::XMFLOAT4(0.8f, 0.9f, 1.0f, 0.9f);
        const wchar_t* text = L"RUNE!";

        if (runeId.rfind("FIR_", 0) == 0)
        {
            color = DirectX::XMFLOAT4(1.0f, 0.35f, 0.15f, 0.95f);
            text = L"FIRE RUNE";
        }
        else if (runeId.rfind("WAT_", 0) == 0)
        {
            color = DirectX::XMFLOAT4(0.35f, 0.85f, 1.0f, 0.95f);
            text = L"WATER RUNE";
        }
        else if (runeId.rfind("WND_", 0) == 0)
        {
            color = DirectX::XMFLOAT4(0.55f, 1.0f, 0.55f, 0.95f);
            text = L"WIND RUNE";
        }
        else if (runeId.rfind("ERT_", 0) == 0)
        {
            color = DirectX::XMFLOAT4(1.0f, 0.75f, 0.35f, 0.95f);
            text = L"EARTH RUNE";
        }
        else if (runeId.rfind("TRF_", 0) == 0)
        {
            color = DirectX::XMFLOAT4(0.75f, 0.75f, 1.0f, 0.95f);
            text = L"TRANSFORM";
        }
        else if (runeId.rfind("AMP_", 0) == 0)
        {
            color = DirectX::XMFLOAT4(1.0f, 0.9f, 0.45f, 0.95f);

            if (runeId.rfind("AMP_DMG", 0) == 0)
                text = L"DMG AMP";
            else if (runeId.rfind("AMP_RAD", 0) == 0)
                text = L"RANGE AMP";
            else if (runeId.rfind("AMP_CD", 0) == 0)
                text = L"COOLDOWN";
            else if (runeId.rfind("AMP_DUR", 0) == 0)
                text = L"DURATION";
            else if (runeId.rfind("AMP_SPD", 0) == 0)
                text = L"SPEED AMP";
            else if (runeId.rfind("AMP_MCO", 0) == 0)
                text = L"AMPLIFY";
            else
                text = L"AMPLIFY";
        }
        else if (runeId.rfind("CH_", 0) == 0)
        {
            color = DirectX::XMFLOAT4(0.65f, 0.95f, 1.0f, 0.95f);

            if (runeId.rfind("CH_DUR", 0) == 0)
                text = L"CHANNEL+";
            else
                text = L"CHANNEL";
        }
        else if (runeId.rfind("ABY_", 0) == 0)
        {
            color = DirectX::XMFLOAT4(0.75f, 0.45f, 1.0f, 0.95f);
            text = L"ABYSS";
        }

        SpawnRuneBurstAt(
            fallbackPos,
            "twirl1",
            160.f,
            0.40f,
            color,
            5.0f);

        DamageNumberManager::Get().AddText(
            fallbackPos,
            text,
            color);
    }
}

ElementType NetworkManager::GetPlayerElementPublic(uint64 playerId) const
{
    return GetPlayerElement(playerId);
}

ElementType NetworkManager::GetPlayerElement(uint64 playerId) const
{
    if (playerId == m_nLocalPlayerId.load())
    {
        Dx12App* pApp = Dx12App::GetInstance();
        Scene* pScene = pApp ? pApp->GetScene() : nullptr;
        if (pScene)
        {
            if (GameObject* pPlayer = pScene->GetPlayer())
            {
                if (auto* pc = pPlayer->GetComponent<PlayerComponent>())
                    return pc->GetElementType();
            }
        }
        return ElementType::Water;
    }
    auto it = m_mapRemotePlayerElement.find(playerId);
    if (it != m_mapRemotePlayerElement.end())
        return it->second;
    return ElementType::Water;
}

void NetworkManager::SpawnEchoSkillVFX(Scene* pScene, int skillType, ElementType element, const DirectX::XMFLOAT3& pos, uint32_t runeFlags)
{
    if (!pScene) return;
    FluidSkillVFXManager* pVFXManager = pScene->GetFluidVFXManager();
    if (!pVFXManager) return;

    using DirectX::XMFLOAT3;
    XMFLOAT3 up = XMFLOAT3(0.0f, 1.0f, 0.0f);
    XMFLOAT3 down = XMFLOAT3(0.0f, -1.0f, 0.0f);
    XMFLOAT3 head = XMFLOAT3(pos.x, pos.y + 2.0f, pos.z);

    auto spawnScaled = [&](const char* effectName, const XMFLOAT3& origin, const XMFLOAT3& dir)
        {
            EffectDef def = EffectRegistry::Get().GetEffect(effectName, runeFlags);
            // 에코는 원본의 50% 위력 — 시각 스케일도 50% 로 축소
            for (auto& l : def.layers)
                l.sizeScale *= 0.5f;
            pVFXManager->SpawnEffectDef(origin, dir, def, true);
        };

    switch (skillType)
    {
    case 1: // Q
        switch (element)
        {
        case ElementType::Fire:  spawnScaled("Q_WaveSlash", head, up); break;
        case ElementType::Water:
            spawnScaled("Q_WaterFall", XMFLOAT3(pos.x, pos.y + 5.5f, pos.z), down);
            spawnScaled("Q_WaterPuddle", XMFLOAT3(pos.x, pos.y + 2.5f, pos.z), down);
            break;
        case ElementType::Wind:  spawnScaled("Q_WindCutter", head, up); break;
        case ElementType::Earth: spawnScaled("Q_StoneSpike", pos, up); break;
        default: break;
        }
        break;

    case 2: // E
        switch (element)
        {
        case ElementType::Fire:
            spawnScaled("E_FireBeam_Core", head, up);
            spawnScaled("E_FireBeam_Burst", head, up);
            break;
        case ElementType::Water:
            spawnScaled("E_WaterVortex", XMFLOAT3(pos.x, pos.y + 3.0f, pos.z), up);
            break;
        case ElementType::Wind:
            spawnScaled("E_GaleRush_Burst", pos, up);
            spawnScaled("E_GaleRush_Ring", pos, up);
            break;
        case ElementType::Earth:
            spawnScaled("E_EarthArmor_Burst", pos, up);
            spawnScaled("E_EarthArmor_Aura", pos, up);
            break;
        default: break;
        }
        break;

    case 3: // R
        switch (element)
        {
        case ElementType::Fire:
            spawnScaled("R_MeteorImpact", pos, up);
            spawnScaled("R_MeteorGroundFire", pos, up);
            break;
        case ElementType::Water:
            spawnScaled("R_TidalWave", pos, up);
            spawnScaled("R_TidalWave_Foam", pos, up);
            break;
        case ElementType::Wind:
            spawnScaled("R_TornadoPlayer", pos, up);
            break;
        case ElementType::Earth:
            spawnScaled("R_Earthquake_Burst", pos, up);
            spawnScaled("R_Earthquake_Ring", pos, up);
            break;
        default: break;
        }
        break;

    case 4: // RC — 단발 임팩트
        spawnScaled("R_MeteorSmallImpact", pos, up);
        break;

    default: break;
    }
}

void NetworkManager::ProcessMonsterDespawn(Scene* pScene, uint64 monsterId)
{
    // DarkLord 사망 연출 중에는 서버 despawn 패킷이 와도 바로 지우지 않는다.
    // 바로 지우면 사망 애니메이션을 보여줄 오브젝트가 사라진다.
    if (pScene && pScene->IsNetworkDarkLordDeathTarget(monsterId))
    {
        char buf[160];
        sprintf_s(buf,
            "[Network] DarkLord despawn deferred during death sequence monsterId=%llu",
            monsterId);
        WriteNetworkLog(buf);
        return;
    }

    auto it = m_mapServerMonsters.find(monsterId);
    if (it == m_mapServerMonsters.end())
        return;

    GameObject* pMonster = it->second;
    // 네트워크 일반 몬스터용 EnemyComponent 인디케이터 정리
    // Melee/Ranged/Rush 계열은 EnemyComponent 내부 indicator를 사용하므로
    // 몬스터 despawn 전에 숨겨서 방 클리어 후 잔상 방지
    if (EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>())
    {
        // Hide만 하면 indicator GameObject는 Scene 전역에 계속 남을 수 있다.
        // Despawn 시에는 몬스터가 들고 있던 공격 인디케이터를 실제 삭제 예약한다.
        pEnemy->DestroyIndicators(pScene);

        if (IAttackBehavior* pBehavior = pEnemy->GetAttackBehavior())
            pBehavior->Reset();
    }

    // 일반 몬스터 공격 연출 entry 정리
    // 몬스터가 despawn된 뒤 UpdateNetworkNormalMonsterBehaviors에서
    // 이미 삭제된 몬스터의 Behavior를 다시 Update하지 않도록 제거한다.
    m_vNetworkNormalMonsterBehaviors.erase(
        std::remove_if(
            m_vNetworkNormalMonsterBehaviors.begin(),
            m_vNetworkNormalMonsterBehaviors.end(),
            [monsterId](const NetworkNormalMonsterBehaviorEntry& e)
            {
                return e.monsterId == monsterId;
            }),
        m_vNetworkNormalMonsterBehaviors.end()
    );

    // Golem / Demon 전용 entry 는 raw EnemyComponent* owner 를 들고 있어,
    // pMonster 가 MarkForDeletion 으로 곧 파기되면 다음 프레임 Update*Behaviors 에서
    // dangling owner -> AnimationComponent UAF (예: Golem 의 "Golem_battle_stand_ge" CrossFade) 발생.
    // 같은 EnemyComponent 를 가진 entry 를 즉시 Reset + 제거한다.
    if (EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>())
    {
        m_vNetworkGolemBehaviors.erase(
            std::remove_if(
                m_vNetworkGolemBehaviors.begin(),
                m_vNetworkGolemBehaviors.end(),
                [pEnemy](NetworkGolemBehaviorEntry& e)
                {
                    if (e.owner == pEnemy)
                    {
                        if (e.behavior) e.behavior->Reset();
                        return true;
                    }
                    return false;
                }),
            m_vNetworkGolemBehaviors.end()
        );

        m_vNetworkDemonBehaviors.erase(
            std::remove_if(
                m_vNetworkDemonBehaviors.begin(),
                m_vNetworkDemonBehaviors.end(),
                [pEnemy](NetworkDemonBehaviorEntry& e)
                {
                    if (e.owner == pEnemy)
                    {
                        if (e.behavior) e.behavior->Reset();
                        return true;
                    }
                    return false;
                }),
            m_vNetworkDemonBehaviors.end()
        );
    }

    pScene->MarkForDeletion(pMonster);
    m_mapServerMonsters.erase(it);
    m_mapServerMonsterMoveTime.erase(monsterId);
    m_mapServerMonsterClips.erase(monsterId);
    m_mapServerMonsterTarget.erase(monsterId);
    m_mapServerMonsterSpawnEffects.erase(monsterId);
    m_mapServerMonsterCurrentAnimClip.erase(monsterId);
    m_mapServerMonsterAttackTimer.erase(monsterId);
    m_mapServerMonsterHitFlashTimer.erase(monsterId);
    m_setDeadServerMonsters.erase(monsterId);

    // 인디케이터 4개도 정리 — 보스가 아니면 애초에 등록 안 됐으므로 find 시 미스
    auto indIt = m_mapServerMonsterIndicators.find(monsterId);
    if (indIt != m_mapServerMonsterIndicators.end())
    {
        ServerMonsterIndicators& ind = indIt->second;
        if (ind.circleBorder) pScene->MarkForDeletion(ind.circleBorder);
        if (ind.circleFill)   pScene->MarkForDeletion(ind.circleFill);
        if (ind.boxBorder)    pScene->MarkForDeletion(ind.boxBorder);
        if (ind.boxFill)      pScene->MarkForDeletion(ind.boxFill);
        m_mapServerMonsterIndicators.erase(indIt);
    }

    wchar_t buf[128];
    swprintf_s(buf, L"[Network] Despawned NetMonster_%llu\n", monsterId);
    OutputDebugString(buf);
}

void NetworkManager::UpdateServerBossActions(Scene* pScene, float deltaTime)
{
    if (m_mapServerBossActions.empty()) return;

    for (auto it = m_mapServerBossActions.begin(); it != m_mapServerBossActions.end(); )
    {
        ServerBossAction& act = it->second;
        if (act.kind == BossActionKind::None) { it = m_mapServerBossActions.erase(it); continue; }

        act.timer += deltaTime;

        auto mIt = m_mapServerMonsters.find(it->first);
        if (mIt == m_mapServerMonsters.end()) { it = m_mapServerBossActions.erase(it); continue; }
        GameObject* pBoss = mIt->second;
        if (pBoss == nullptr || pBoss->GetTransform() == nullptr) { it = m_mapServerBossActions.erase(it); continue; }

        float yOffset = 0.0f;
        bool finished = false;

        if (act.kind == BossActionKind::Jump)
        {
            // 포물선: y = 4*peak * t * (1-t)  (t in 0..1)
            float t = (act.timer / act.duration);
            if (t >= 1.0f) { finished = true; yOffset = 0.0f; }
            else { yOffset = 4.0f * act.peakHeight * t * (1.0f - t); }
        }
        else if (act.kind == BossActionKind::Flying)
        {
            // TakeOff(0.6s) → Hover(중간) → Landing(0.6s)
            constexpr float TAKEOFF = 0.6f;
            constexpr float LANDING = 0.6f;
            float total = act.duration;
            float landingStart = total - LANDING;

            if (act.timer >= total) { finished = true; yOffset = 0.0f; }
            else if (act.timer < TAKEOFF)
            {
                float t = act.timer / TAKEOFF;
                float ease = 1.0f - (1.0f - t) * (1.0f - t); // ease-out
                yOffset = act.peakHeight * ease;
            }
            else if (act.timer < landingStart)
            {
                yOffset = act.peakHeight;
            }
            else
            {
                float t = (act.timer - landingStart) / LANDING;
                float ease = t * t; // ease-in
                yOffset = act.peakHeight * (1.0f - ease);
            }
        }

        if (finished)
        {
            // 종료 — 보스 y 를 서버 타겟 y 로 복귀
            auto tIt = m_mapServerMonsterTarget.find(it->first);
            float baseY = (tIt != m_mapServerMonsterTarget.end()) ? tIt->second.py : 0.0f;
            XMFLOAT3 p = pBoss->GetTransform()->GetPosition();
            pBoss->GetTransform()->SetPosition(p.x, baseY, p.z);
            it = m_mapServerBossActions.erase(it);
            continue;
        }

        // yOffset 적용 — InterpolateServerMonsters 가 이미 위치를 보간해놓았으므로
        // 그 위에 yOffset 만 추가
        auto tIt = m_mapServerMonsterTarget.find(it->first);
        float baseY = (tIt != m_mapServerMonsterTarget.end()) ? tIt->second.py : 0.0f;
        XMFLOAT3 p = pBoss->GetTransform()->GetPosition();
        pBoss->GetTransform()->SetPosition(p.x, baseY + yOffset, p.z);

        ++it;
    }
}

void NetworkManager::UpdateServerMegaBreathCutscenes(Scene* pScene, float deltaTime)
{
    if (m_mapServerMegaBreathCutscenes.empty()) return;

    // 오프라인 MegaBreathAttackBehavior phase 시간 (P2)
    constexpr float TAKEOFF_TIME = 0.9f;
    constexpr float MOVE_TIME = 3.0f;
    constexpr float LANDING_TIME = 0.7f;
    constexpr float COVER_TIME = 1.2f;
    constexpr float WINDUP_TIME = 5.5f;
    constexpr float BREATH_TIME = 6.5f;
    constexpr float RECOVERY_TIME = 1.2f;
    constexpr float RETTAKEOFF_TIME = 0.9f;
    constexpr float RETFLY_TIME = 3.0f;
    constexpr float RETLAND_TIME = 0.7f;
    constexpr float FLY_HEIGHT = 18.0f;
    constexpr float COVER_DIST = 57.5f;  // game world: 11.5(JSON) * MAP_SCALE(5) = 57.5
    constexpr float COVER_SCALE = 5.0f;
    // 보스 spawn 기준 — 각 cs.bossSpawnPos 로 결정 (이전 hardcoded fire 룸 좌표 폐기)

    for (auto it = m_mapServerMegaBreathCutscenes.begin(); it != m_mapServerMegaBreathCutscenes.end(); )
    {
        ServerMegaBreathCutscene& cs = it->second;
        if (!cs.active) { it = m_mapServerMegaBreathCutscenes.erase(it); continue; }

        auto bIt = m_mapServerMonsters.find(it->first);
        if (bIt == m_mapServerMonsters.end()) { it = m_mapServerMegaBreathCutscenes.erase(it); continue; }
        GameObject* pBoss = bIt->second;
        if (!pBoss || !pBoss->GetTransform()) { it = m_mapServerMegaBreathCutscenes.erase(it); continue; }
        TransformComponent* pT = pBoss->GetTransform();
        AnimationComponent* pAnim = pBoss->GetComponent<AnimationComponent>();
        CCamera* pCam = pScene ? pScene->GetCamera() : nullptr;

        cs.phaseTimer += deltaTime;

        // 보스룸 중심 (cover/wall reference) — 모든 phase 공통
        XMFLOAT3 roomCenter = cs.bossSpawnPos;

        // 로컬 플레이어 위치 (카메라 lookAt — 어깨 너머 숏)
        XMFLOAT3 playerPos = roomCenter; // fallback
        if (pScene)
        {
            if (auto* pLocal = pScene->GetPlayer())
                if (auto* pPT = pLocal->GetTransform())
                    playerPos = pPT->GetPosition();
        }

        // ── 매 프레임 카메라 블렌드 (오프라인 UpdateCinematicCamera 와 동일 패턴) ──
        auto BlendCamera = [&](const XMFLOAT3& tgtLookAt, float tgtDist, float tgtPitch, float tgtYaw, float kBlend)
            {
                if (!pCam) return;
                if (!cs.camInit)
                {
                    cs.camLookAt = tgtLookAt; cs.camDist = tgtDist; cs.camPitch = tgtPitch; cs.camYaw = tgtYaw;
                    pCam->StartCinematic(cs.camLookAt, cs.camDist, cs.camPitch, cs.camYaw);
                    cs.camInit = true; return;
                }
                float rate = 1.0f - expf(-deltaTime * kBlend);
                if (rate < 0.f) rate = 0.f; if (rate > 1.f) rate = 1.f;
                cs.camLookAt.x += (tgtLookAt.x - cs.camLookAt.x) * rate;
                cs.camLookAt.y += (tgtLookAt.y - cs.camLookAt.y) * rate;
                cs.camLookAt.z += (tgtLookAt.z - cs.camLookAt.z) * rate;
                cs.camDist += (tgtDist - cs.camDist) * rate;
                cs.camPitch += (tgtPitch - cs.camPitch) * rate;
                float yawDelta = tgtYaw - cs.camYaw;
                while (yawDelta > 180.f) yawDelta -= 360.f;
                while (yawDelta < -180.f) yawDelta += 360.f;
                cs.camYaw += yawDelta * rate;
                pCam->SetCinematicLookAt(cs.camLookAt);
                pCam->SetCinematicOrbit(cs.camDist, cs.camPitch, cs.camYaw);
            };

        XMFLOAT3 dragonPos = pT->GetPosition();

        switch (cs.phase)
        {
        case MegaBreathPhase::TakeOff:
        {
            float t = (std::min)(cs.phaseTimer / TAKEOFF_TIME, 1.0f);
            float ease = t * t; // easeIn
            XMFLOAT3 p = cs.phaseStartPos;
            p.y = cs.phaseStartPos.y + FLY_HEIGHT * ease;
            pT->SetPosition(p);

            // 카메라: 와이드 (오프라인 TakeOff/MoveToWall/Landing 공통)
            float dxc = p.x - roomCenter.x, dzc = p.z - roomCenter.z;
            float radius = sqrtf(dxc * dxc + dzc * dzc);
            float baseYaw = (radius > 0.5f) ? atan2f(dxc, dzc) * (180.f / 3.14159265f) : 0.f;
            float yawOffset = -15.0f * (cs.phaseTimer / (TAKEOFF_TIME + MOVE_TIME + LANDING_TIME));
            float flightYaw = baseYaw + 45.f + yawOffset;
            BlendCamera(XMFLOAT3{ p.x, p.y + 4.0f, p.z }, 48.f, 28.f, flightYaw, 2.0f);

            if (cs.phaseTimer >= TAKEOFF_TIME)
            {
                if (pAnim) pAnim->CrossFade("Fly Glide", 0.2f, true);
                cs.phase = MegaBreathPhase::MoveToWall;
                cs.phaseTimer = 0.f;
                cs.phaseStartPos = pT->GetPosition();
                cs.wallPos.y = cs.phaseStartPos.y; // 비행 고도 유지
                WriteNetworkLog("[Network] MegaBreath phase -> MoveToWall");
            }
            break;
        }
        case MegaBreathPhase::MoveToWall:
        {
            float t = (std::min)(cs.phaseTimer / MOVE_TIME, 1.0f);
            float ease = 1.0f - (1.0f - t) * (1.0f - t); // easeOut
            XMFLOAT3 p;
            p.x = cs.phaseStartPos.x + (cs.wallPos.x - cs.phaseStartPos.x) * ease;
            p.y = cs.phaseStartPos.y;
            p.z = cs.phaseStartPos.z + (cs.wallPos.z - cs.phaseStartPos.z) * ease;
            pT->SetPosition(p);
            // 벽으로 날아가는 동안에는 이동 방향을 바라보게 한다.
            SetDragonFaceTarget(pT, cs.wallPos, p);

            float dxc = p.x - roomCenter.x, dzc = p.z - roomCenter.z;
            float radius = sqrtf(dxc * dxc + dzc * dzc);
            float baseYaw = (radius > 0.5f) ? atan2f(dxc, dzc) * (180.f / 3.14159265f) : 0.f;
            float globalT = (TAKEOFF_TIME + cs.phaseTimer) / (TAKEOFF_TIME + MOVE_TIME + LANDING_TIME);
            float yawOffset = (globalT - 0.5f) * 30.0f;
            BlendCamera(XMFLOAT3{ p.x, p.y + 4.0f, p.z }, 48.f, 28.f, baseYaw + 45.f + yawOffset, 2.0f);

            if (cs.phaseTimer >= MOVE_TIME)
            {
                if (pAnim) pAnim->CrossFade("Land", 0.15f, false);
                cs.phase = MegaBreathPhase::Landing;
                cs.phaseTimer = 0.f;
                cs.phaseStartPos = pT->GetPosition();
                WriteNetworkLog("[Network] MegaBreath phase -> Landing");
            }
            break;
        }
        case MegaBreathPhase::Landing:
        {
            float t = (std::min)(cs.phaseTimer / LANDING_TIME, 1.0f);
            float ease = 1.0f - (1.0f - t) * (1.0f - t);
            XMFLOAT3 p = cs.phaseStartPos;
            p.y = cs.phaseStartPos.y - FLY_HEIGHT * ease;
            pT->SetPosition(p);

            // 카메라: Landing 동안 flightYaw → playerDir 으로 수렴
            float dxc = p.x - roomCenter.x, dzc = p.z - roomCenter.z;
            float baseYaw = atan2f(dxc, dzc) * (180.f / 3.14159265f);
            // playerDir 추정 — 방 중심 향함
            float ddx = roomCenter.x - p.x, ddz = roomCenter.z - p.z;
            float playerDirYaw = atan2f(ddx, ddz) * (180.f / 3.14159265f);
            float globalT = (TAKEOFF_TIME + MOVE_TIME + cs.phaseTimer) / (TAKEOFF_TIME + MOVE_TIME + LANDING_TIME);
            float flightYaw = baseYaw + 45.f + (globalT - 0.5f) * 30.f;
            float landingT = t;
            float diff = playerDirYaw - flightYaw;
            while (diff > 180.f) diff -= 360.f;
            while (diff < -180.f) diff += 360.f;
            float yaw = flightYaw + diff * landingT;
            BlendCamera(XMFLOAT3{ p.x, p.y + 4.0f, p.z }, 48.f, 28.f, yaw, 2.0f);

            if (cs.phaseTimer >= LANDING_TIME)
            {
                cs.phase = MegaBreathPhase::SpawnCover;
                cs.phaseTimer = 0.f;
                WriteNetworkLog("[Network] MegaBreath phase -> SpawnCover (4 columns)");
                // 4 cover 기둥 spawn (방 중심 cross +12)
                Dx12App* pApp = Dx12App::GetInstance();
                Shader* pShader = pScene ? pScene->GetDefaultShader() : nullptr;
                if (pApp && pScene && pShader)
                {
                    ID3D12Device* pDev = pApp->GetDevice();
                    ID3D12GraphicsCommandList* pCmd = pApp->GetCommandList();
                    XMFLOAT3 coverPos[4] = {
                        { roomCenter.x + COVER_DIST, 0.0f, roomCenter.z },
                        { roomCenter.x - COVER_DIST, 0.0f, roomCenter.z },
                        { roomCenter.x, 0.0f, roomCenter.z + COVER_DIST },
                        { roomCenter.x, 0.0f, roomCenter.z - COVER_DIST }
                    };
                    for (int i = 0; i < 4; ++i)
                    {
                        CRoom* pPrev = pScene->GetCurrentRoom();
                        pScene->SetCurrentRoom(nullptr);
                        GameObject* pCover = pScene->CreateGameObject(pDev, pCmd);
                        pScene->SetCurrentRoom(pPrev);
                        if (!pCover) continue;
                        if (auto* pCT = pCover->GetTransform())
                        {
                            pCT->SetPosition(coverPos[i].x, coverPos[i].y, coverPos[i].z);
                            pCT->SetScale(COVER_SCALE, COVER_SCALE, COVER_SCALE);
                            pCT->SetRotation(0.0f, 0.0f, 0.0f);
                        } // 명시적 회전 0 (역방향 mesh 방지)
                        Mesh* pMesh = MapLoader::LoadMesh("Assets/MapData/meshes/ColumnBig_001.obj", pDev, pCmd);
                        if (pMesh) { pMesh->AddRef(); pCover->SetMesh(pMesh); }
                        MATERIAL mat;
                        mat.m_cAmbient = XMFLOAT4(0.3f, 0.3f, 0.3f, 1.0f);
                        mat.m_cDiffuse = XMFLOAT4(0.7f, 0.7f, 0.7f, 1.0f);
                        mat.m_cSpecular = XMFLOAT4(0.2f, 0.2f, 0.2f, 8.0f);
                        mat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                        pCover->SetMaterial(mat);
                        auto* pRC = pCover->AddComponent<RenderComponent>();
                        if (pMesh) pRC->SetMesh(pMesh);
                        pShader->AddRenderComponent(pRC);

                        // MegaBreath 엄폐 기둥 충돌체.
                        // MapLoader의 벽/장애물과 같은 Wall 레이어로 둬서 플레이어가 통과하지 못하게 한다.
                        auto* pCol = pCover->AddComponent<ColliderComponent>();
                        pCol->SetExtents(1.5f, 6.0f, 1.5f);
                        pCol->SetCenter(0.0f, 3.0f, 0.0f);
                        pCol->SetLayer(CollisionLayer::Wall);
                        pCol->SetCollisionMask(CollisionMask::Wall);

                        cs.covers.push_back(pCover);
                    }
                }
            }
            break;
        }
        case MegaBreathPhase::SpawnCover:
        {
            // establishing wide shot — lookAt = 플레이어, yaw = (player - dragon) (오프라인 동일)
            float ddx = playerPos.x - dragonPos.x, ddz = playerPos.z - dragonPos.z;
            float yaw = atan2f(ddx, ddz) * (180.f / 3.14159265f);
            BlendCamera(XMFLOAT3{ playerPos.x, playerPos.y + 2.5f, playerPos.z }, 78.f, 50.f, yaw, 2.0f);

            if (cs.phaseTimer >= COVER_TIME)
            {
                cs.phase = MegaBreathPhase::Windup;
                cs.phaseTimer = 0.f;
                // 브레스 준비 자세가 반대로 보여서 시각 방향만 180도 반전
                float dxc = roomCenter.x - dragonPos.x, dzc = roomCenter.z - dragonPos.z;
                if (fabsf(dxc) + fabsf(dzc) > 0.01f)
                {
                    SetDragonFaceTarget(pT, roomCenter, dragonPos);
                }
            }
            break;
        }
        case MegaBreathPhase::Windup:
        {
            // over-shoulder 카메라 — lookAt = 플레이어, yaw = (player - dragon)
            float ddx = playerPos.x - dragonPos.x, ddz = playerPos.z - dragonPos.z;
            float yaw = atan2f(ddx, ddz) * (180.f / 3.14159265f);
            BlendCamera(XMFLOAT3{ playerPos.x, playerPos.y + 1.0f, playerPos.z }, 62.f, 55.f, yaw, 2.0f);

            // Windup 진입 시 1회 수렴 차지 VFX (오프라인 SpawnChargeVFX 와 동일 공용 헬퍼)
            if (cs.chargeVFXId < 0)
            {
                if (auto* pFluidVFX = pScene ? pScene->GetFluidVFXManager() : nullptr)
                {
                    XMFLOAT3 cFwd{ roomCenter.x - dragonPos.x, 0.0f, roomCenter.z - dragonPos.z };
                    float cLen = sqrtf(cFwd.x * cFwd.x + cFwd.z * cFwd.z);
                    if (cLen < 0.001f) cFwd = XMFLOAT3{ 0.0f, 0.0f, 1.0f };
                    else { cFwd.x /= cLen; cFwd.z /= cLen; }
                    // 입은 드래곤 바로 앞(+5u) — 분사 시작과 동일하게 당겨 빈 공간 방지
                    XMFLOAT3 cMouth{
                        dragonPos.x + cFwd.x * 5.0f,
                        dragonPos.y + 7.0f,
                        dragonPos.z + cFwd.z * 5.0f
                    };
                    const XMFLOAT3 roomExtents{ 114.95f, 0.0f, 121.675f };
                    cs.chargeVFXId = MegaBreathAttackBehavior::SpawnMegaBreathChargeVFX(
                        pFluidVFX, cMouth, cFwd, dragonPos.y,
                        roomCenter, roomExtents, WINDUP_TIME);
                }
            }

            if (cs.phaseTimer >= WINDUP_TIME)
            {
                cs.phase = MegaBreathPhase::Breath;
                cs.phaseTimer = 0.f;
                if (pAnim) pAnim->CrossFade("Flame Attack", 0.2f, true);
                if (pCam) pCam->StartShake(2.5f, BREATH_TIME);
                WriteNetworkLog("[Network] MegaBreath phase -> Breath (SPH flood spawn)");

                // SPH 화염 홍수 — 오프라인 SpawnFireWave 와 동일한 공용 헬퍼 사용 (원기둥 포텐셜 흐름까지 일치).
                if (auto* pFluidVFX = pScene ? pScene->GetFluidVFXManager() : nullptr)
                {
                    // 차지 VFX 정지 (분사 시작)
                    if (cs.chargeVFXId >= 0) { pFluidVFX->StopEffect(cs.chargeVFXId); cs.chargeVFXId = -1; }

                    // 빔 방향 = wall(보스) → roomCenter (모델 yaw 보정과 무관하게 분리)
                    XMFLOAT3 forward{ roomCenter.x - dragonPos.x, 0.0f, roomCenter.z - dragonPos.z };
                    float fLen = sqrtf(forward.x * forward.x + forward.z * forward.z);
                    if (fLen < 0.001f) forward = XMFLOAT3{ 0.0f, 0.0f, 1.0f };
                    else { forward.x /= fLen; forward.z /= fLen; }

                    // 입 위치 — 드래곤 바로 앞(+5u)으로 당겨 분사가 드래곤에서 시작 (이전 +17은 앞이 비어 보였음)
                    XMFLOAT3 mouth{
                        dragonPos.x + forward.x * 5.0f,
                        dragonPos.y + 7.0f,
                        dragonPos.z + forward.z * 5.0f
                    };

                    // 엄폐 기둥 4개 → 수직 원기둥 장애물 (클라가 스폰한 cs.covers 좌표)
                    XMFLOAT4 obstacles[4];
                    int oc = 0;
                    for (GameObject* pCover : cs.covers)
                    {
                        if (oc >= 4) break;
                        if (!pCover || !pCover->GetTransform()) continue;
                        XMFLOAT3 cp = pCover->GetTransform()->GetPosition();
                        obstacles[oc] = { cp.x, cp.z, 3.0f, 0.0f };
                        ++oc;
                    }

                    // 보스룸 game-world AABB 반폭 (SpawnCover 주석 기준 X[-145,85] Z[-115,128.4])
                    const XMFLOAT3 roomExtents{ 114.95f, 0.0f, 121.675f };
                    cs.beamVFXIds[0] = MegaBreathAttackBehavior::SpawnMegaBreathFloodVFX(
                        pFluidVFX, mouth, forward, dragonPos.y,
                        roomCenter, roomExtents, obstacles, oc, BREATH_TIME, nullptr);
                    for (int i = 1; i < 5; ++i) cs.beamVFXIds[i] = -1;
                }
            }
            break;
        }
        case MegaBreathPhase::Breath:
        {
            // over-shoulder 유지 — lookAt = 플레이어, yaw = (player - dragon) (오프라인 동일)
            float ddx = playerPos.x - dragonPos.x, ddz = playerPos.z - dragonPos.z;
            float yaw = atan2f(ddx, ddz) * (180.f / 3.14159265f);
            BlendCamera(XMFLOAT3{ playerPos.x, playerPos.y + 1.0f, playerPos.z }, 62.f, 55.f, yaw, 2.0f);

            if (cs.phaseTimer >= BREATH_TIME)
            {
                cs.phase = MegaBreathPhase::Recovery;
                cs.phaseTimer = 0.f;
                if (pCam) pCam->StopShake();
                // beam stop
                if (auto* pFluidVFX = pScene ? pScene->GetFluidVFXManager() : nullptr)
                {
                    for (int i = 0; i < 5; ++i)
                        if (cs.beamVFXIds[i] >= 0) { pFluidVFX->StopEffect(cs.beamVFXIds[i]); cs.beamVFXIds[i] = -1; }
                }
                // cover despawn (Recovery 진입 즉시)
                Shader* pShader = pScene ? pScene->GetDefaultShader() : nullptr;
                for (GameObject* pCover : cs.covers)
                {
                    if (!pCover) continue;
                    if (pShader)
                        if (auto* pRC = pCover->GetComponent<RenderComponent>())
                            pShader->RemoveRenderComponent(pRC);
                    if (auto* pCT = pCover->GetTransform())
                        pCT->SetPosition(0.f, -1000.f, 0.f);
                    if (pScene) pScene->MarkForDeletion(pCover);
                }
                cs.covers.clear();
            }
            break;
        }
        case MegaBreathPhase::Recovery:
        {
            // 기본 orbit 으로 블렌드 (오프라인 동일) — lookAt = 플레이어
            BlendCamera(XMFLOAT3{ playerPos.x, playerPos.y + 1.0f, playerPos.z }, 50.f, 60.f, 45.f, 5.5f);

            if (cs.phaseTimer >= RECOVERY_TIME - 0.15f)
            {
                if (pCam) pCam->StopCinematic();
                cs.camInit = false;
            }
            if (cs.phaseTimer >= RECOVERY_TIME)
            {
                if (pAnim) pAnim->CrossFade("Take Off", 0.15f, false);
                cs.phase = MegaBreathPhase::ReturnTakeOff;
                cs.phaseTimer = 0.f;
                cs.phaseStartPos = pT->GetPosition();
            }
            break;
        }
        case MegaBreathPhase::ReturnTakeOff:
        {
            float t = (std::min)(cs.phaseTimer / RETTAKEOFF_TIME, 1.0f);
            float ease = t * t;
            XMFLOAT3 p = cs.phaseStartPos;
            p.y = cs.phaseStartPos.y + FLY_HEIGHT * ease;
            pT->SetPosition(p);
            if (cs.phaseTimer >= RETTAKEOFF_TIME)
            {
                if (pAnim) pAnim->CrossFade("Fly Glide", 0.2f, true);
                cs.phase = MegaBreathPhase::ReturnFly;
                cs.phaseTimer = 0.f;
                cs.phaseStartPos = pT->GetPosition();
            }
            break;
        }
        case MegaBreathPhase::ReturnFly:
        {
            float t = (std::min)(cs.phaseTimer / RETFLY_TIME, 1.0f);
            float ease = 1.0f - (1.0f - t) * (1.0f - t);
            XMFLOAT3 targetAir = cs.originalPos;
            targetAir.y = cs.phaseStartPos.y;
            XMFLOAT3 p;
            p.x = cs.phaseStartPos.x + (targetAir.x - cs.phaseStartPos.x) * ease;
            p.y = cs.phaseStartPos.y + (targetAir.y - cs.phaseStartPos.y) * ease;
            p.z = cs.phaseStartPos.z + (targetAir.z - cs.phaseStartPos.z) * ease;
            pT->SetPosition(p);
            float dx = targetAir.x - p.x, dz = targetAir.z - p.z;
            if (fabsf(dx) + fabsf(dz) > 0.01f)
            {
                SetDragonFaceTarget(pT, targetAir, p);
            }

            if (cs.phaseTimer >= RETFLY_TIME)
            {
                if (pAnim) pAnim->CrossFade("Land", 0.15f, false);
                cs.phase = MegaBreathPhase::ReturnLand;
                cs.phaseTimer = 0.f;
                cs.phaseStartPos = pT->GetPosition();
            }
            break;
        }
        case MegaBreathPhase::ReturnLand:
        {
            float t = (std::min)(cs.phaseTimer / RETLAND_TIME, 1.0f);
            float ease = 1.0f - (1.0f - t) * (1.0f - t);
            XMFLOAT3 p = cs.phaseStartPos;
            p.y = cs.phaseStartPos.y - FLY_HEIGHT * ease;
            pT->SetPosition(p);
            if (cs.phaseTimer >= RETLAND_TIME)
            {
                pT->SetPosition(cs.originalPos);
                cs.phase = MegaBreathPhase::Done;
            }
            break;
        }
        case MegaBreathPhase::Done:
        default:
        {
            // 보간 타겟 동기화 → 정상 보간 복귀
            auto tIt = m_mapServerMonsterTarget.find(it->first);
            if (tIt != m_mapServerMonsterTarget.end())
            {
                tIt->second.px = cs.originalPos.x; tIt->second.py = cs.originalPos.y; tIt->second.pz = cs.originalPos.z;
            }

            // MegaBreath 종료 후 맵 기믹 재개
            if (pScene && pScene->GetCurrentRoom())
            {
                pScene->GetCurrentRoom()->SetLavaGeyserEnabled(true);
                WriteNetworkLog("[Network] LavaGeyser re-enabled after MegaBreath");
            }

            it = m_mapServerMegaBreathCutscenes.erase(it);
            continue;
        }
        }

        ++it;
    }
    // 옵션A end — phase machine 안에서 모든 처리 완결
}

void NetworkManager::UpdateServerBossIntros(Scene* pScene, float deltaTime)
{
    if (m_mapServerBossIntros.empty()) return;

    // 오프라인 EnemyComponent::UpdateBossIntro 와 동일 흐름:
    //   FlyingIn   — y 8u/s 등속 강하, "Fly Glide" 유지, 플레이어 향해 회전
    //   Landing 1.5s — "Land" 애니, 위치 정지
    //   Roaring 2.0s — "Scream" 애니, 위치 정지
    //   Done       — StopCinematic, 정상 보간 복귀

    for (auto it = m_mapServerBossIntros.begin(); it != m_mapServerBossIntros.end(); )
    {
        ServerBossIntroState& st = it->second;
        if (!st.active) { it = m_mapServerBossIntros.erase(it); continue; }
        st.phaseTimer += deltaTime;

        auto mIt = m_mapServerMonsters.find(it->first);
        if (mIt == m_mapServerMonsters.end()) { it = m_mapServerBossIntros.erase(it); continue; }
        GameObject* pBoss = mIt->second;
        if (pBoss == nullptr || pBoss->GetTransform() == nullptr) { it = m_mapServerBossIntros.erase(it); continue; }

        auto* pAnim = pBoss->GetComponent<AnimationComponent>();
        auto* pCam = pScene->GetCamera();
        constexpr float DESCEND_SPEED = 8.0f; // 오프라인 EnemyComponent::UpdateBossIntro 와 동일

        switch (st.phase)
        {
        case BossIntroPhase::FlyingIn:
        {
            auto clipItSpeed = m_mapServerMonsterClips.find(it->first);
            uint32 mtSpeed = (clipItSpeed != m_mapServerMonsterClips.end()) ? clipItSpeed->second.monsterType : 0;

            // BlueDragon은 카메라 컷신 없이 보이므로 조금 천천히 내려오게 한다.
            float descendSpeed = (mtSpeed == 10) ? 8.0f : DESCEND_SPEED;

            st.curY -= descendSpeed * deltaTime;

            if (st.curY <= st.groundY + 0.5f)
            {
                st.curY = st.groundY;
                pBoss->GetTransform()->SetPosition(st.bossX, st.curY, st.bossZ);
                SetDragonVisualYaw(pBoss->GetTransform(), 0.0f);

                auto clipIt = m_mapServerMonsterClips.find(it->first);
                uint32 mt = (clipIt != m_mapServerMonsterClips.end()) ? clipIt->second.monsterType : 0;

                // BlueDragon은 RedDragon처럼 Landing/Roaring 컷신까지 가지 않고,
                // 가볍게 날아와 착지한 뒤 바로 전투 상태로 넘긴다.
                st.phase = BossIntroPhase::Landing;
                st.phaseTimer = 0.0f;

                if (pAnim && !st.landAnimFired)
                {
                    pAnim->CrossFade("Land", 0.15f, false);
                    st.landAnimFired = true;
                }
                // Landing 카메라 — 오프라인 Scene.cpp:1249 와 100% 동일
                if (mt != 10 && pCam)
                {
                    pCam->StartCinematic(XMFLOAT3{ st.bossX, 2.0f, st.bossZ }, 60.0f, 25.0f, 200.0f);
                    pCam->StartShake(0.4f, 1.5f);
                }
            }
            else
            {
                pBoss->GetTransform()->SetPosition(st.bossX, st.curY, st.bossZ);
                SetDragonVisualYaw(pBoss->GetTransform(), 0.0f);
            }

            auto clipItCam = m_mapServerMonsterClips.find(it->first);
            uint32 mtCam = (clipItCam != m_mapServerMonsterClips.end()) ? clipItCam->second.monsterType : 0;

            if (mtCam != 10 && pCam)
            {
                float focusY = st.curY * 0.45f + 3.0f;
                pCam->StartCinematic(XMFLOAT3{ st.bossX, focusY, st.bossZ }, 95.0f, 22.0f, 185.0f);
            }

            break;
        }
        case BossIntroPhase::Landing:
        {
            // 1.5s 정지 (오프라인 동일)
            pBoss->GetTransform()->SetPosition(st.bossX, st.groundY, st.bossZ);
            SetDragonVisualYaw(pBoss->GetTransform(), 0.0f);
            // 카메라는 phase 전환 시 한 번만 설정 (오프라인 동일) — 매 프레임 변경 X
            if (st.phaseTimer >= 1.5f)
            {
                st.phase = BossIntroPhase::Roaring;
                st.phaseTimer = 0.0f;
                if (pAnim && !st.roarAnimFired)
                {
                    auto clipIt = m_mapServerMonsterClips.find(it->first);
                    uint32 mt = (clipIt != m_mapServerMonsterClips.end()) ? clipIt->second.monsterType : 0;
                    const char* roarClip = GetBossRoarClip(mt);
                    if (roarClip) pAnim->CrossFade(roarClip, 0.15f, false);
                    st.roarAnimFired = true;
                }
                // Roaring 카메라 — 오프라인 Scene.cpp:1254 와 100% 동일
                auto clipItRoarCam = m_mapServerMonsterClips.find(it->first);
                uint32 mtRoarCam = (clipItRoarCam != m_mapServerMonsterClips.end())
                    ? clipItRoarCam->second.monsterType : 0;

                if (mtRoarCam != 10 && pCam)
                {
                    pCam->StartCinematic(XMFLOAT3{ st.bossX, 4.0f, st.bossZ }, 42.0f, 30.0f, 230.0f);
                    pCam->StartShake(2.2f, 2.2f);
                }
            }
            break;
        }
        case BossIntroPhase::Roaring:
        {
            // 2.0s 포효 — 카메라 phase 전환 시 한 번만 (오프라인 동일)
            pBoss->GetTransform()->SetPosition(st.bossX, st.groundY, st.bossZ);
            SetDragonVisualYaw(pBoss->GetTransform(), 0.0f);
            if (st.phaseTimer >= 2.0f)
            {
                st.phase = BossIntroPhase::Done; // 오프라인은 즉시 StopCinematic
                st.phaseTimer = 0.0f;
            }
            break;
        }
        case BossIntroPhase::Outro:
        {
            // 사용 안 함 (오프라인은 즉시 StopCinematic) — 안전 폴백으로 즉시 Done
            st.phase = BossIntroPhase::Done;
            st.phaseTimer = 0.0f;
            break;
        }
        case BossIntroPhase::Done:
        default:
        {
            auto clipItDone = m_mapServerMonsterClips.find(it->first);
            uint32 mtDone = (clipItDone != m_mapServerMonsterClips.end()) ? clipItDone->second.monsterType : 0;

            if (mtDone != 10 && pCam)
                pCam->StopCinematic();

            // Red Dragon intro 종료 후 Fire boss room 맵 기믹 재개
            if (mtDone == 6 && pScene && pScene->GetCurrentRoom())
            {
                pScene->GetCurrentRoom()->SetLavaGeyserEnabled(true);
                WriteNetworkLog("[Network] LavaGeyser re-enabled after RedDragon intro");
            }

            // 보간 타겟을 ground 위치(스폰)로 동기화 — 인트로 종료 후 보스가 다른 위치로 튀는 것 방지
            auto tIt = m_mapServerMonsterTarget.find(it->first);
            if (tIt != m_mapServerMonsterTarget.end())
            {
                tIt->second.px = st.bossX;
                tIt->second.py = st.groundY;
                tIt->second.pz = st.bossZ;
            }
            // 인트로 종료 — Idle 애니로 명시적 복귀
            if (pAnim)
            {
                auto clipIt = m_mapServerMonsterClips.find(it->first);
                const char* idleClip = (clipIt != m_mapServerMonsterClips.end())
                    ? clipIt->second.idle.c_str() : "Idle01";
                pAnim->CrossFade(idleClip, 0.2f, true);
            }
            it = m_mapServerBossIntros.erase(it);
            continue;
        }
        }

        ++it;
    }
}

// Demon 네트워크 범위 패턴 실행
void NetworkManager::PlayNetworkDemonAttackBehavior(Scene* pScene, GameObject* pMonster, uint64 monsterId, uint32 attackType, const std::vector<DirectX::XMFLOAT3>& effectPositions, uint32 effectOption)
{
    if (!pScene || !pMonster) return;

    // 동시에 여러 Demon Behavior가 겹치지 않도록 방지
    if (!m_vNetworkDemonBehaviors.empty())
        return;

    EnemyComponent* pEnemy = pMonster->GetComponent<EnemyComponent>();
    if (!pEnemy)
    {
        // 서버 몬스터에 연출용 EnemyComponent가 없으면 임시 추가
        pEnemy = pMonster->AddComponent<EnemyComponent>();
    }

    // 데미지/AI는 서버 권위, 클라는 연출만 담당
    pEnemy->SetBoss(true);
    pEnemy->SetAIPaused(true);

    if (auto* pAnim = pMonster->GetComponent<AnimationComponent>())
    {
        pEnemy->SetAnimationComponent(pAnim);
    }

    if (auto* pRoom = pScene->GetCurrentRoom())
    {
        pEnemy->SetRoom(pRoom);
    }

    if (auto* pPlayer = pScene->GetPlayer())
    {
        pEnemy->SetTarget(pPlayer);
    }

    std::unique_ptr<IAttackBehavior> behavior;

    switch (attackType)
    {
    case 27: // DemonSpinDash
        behavior = std::make_unique<SpinDashAttackBehavior>(
            18.0f, 0.22f,
            18.0f, 1.1f,
            0.25f, 0.55f,
            7.0f
        );
        break;

    case 28: // DemonShortRush
    {
        // 오프라인 EnemySpawner P1 수치 (damage 55, speed 28, dur 0.85, cone 75°).
        // 데미지는 서버 권위 → SetNetworkVisualOnly 로 클라 측 위치 이동 + 데미지 차단.
        auto rush = std::make_unique<RushFrontAttackBehavior>(
            55.0f,
            28.0f, 0.85f,
            0.25f, 0.15f, 1.0f,
            8.5f, 75.0f
        );
        rush->SetNetworkVisualOnly(true);
        behavior = std::move(rush);
        break;
    }

    case 29: // DemonLongRush
    {
        // 오프라인 EnemySpawner P1 수치 (damage 70, speed 34, dur 1.2, cone 95°).
        auto rush = std::make_unique<RushFrontAttackBehavior>(
            70.0f,
            34.0f, 1.2f,
            0.30f, 0.20f, 1.2f,
            10.5f, 95.0f
        );
        rush->SetNetworkVisualOnly(true);
        behavior = std::move(rush);
        break;
    }

    case 34: // DemonJumpSlam
    {
        // 오프라인 EnemySpawner P2 수치 (damage 130, jumpH 18, dur 1.1, slamR 16).
        // 데미지는 서버. 클라는 점프 아크 + 슬램 인디케이터만.
        auto slam = std::make_unique<JumpSlamAttackBehavior>(
            130.0f,
            18.0f, 1.1f,
            16.0f,
            0.35f, 1.0f,
            true,
            3.0f, 0.5f,
            "attack4", 0.0f
        );
        slam->SetNetworkVisualOnly(true);
        behavior = std::move(slam);
        break;
    }

    case 30: // DemonFixatedCharge
        behavior = std::make_unique<FixatedChargeAttackBehavior>(
            85.0f,
            3.0f, 0.2f,
            58.0f, 110.0f,
            6.5f, 8.0f,
            6.0f,
            4.5f, 0.9f
        );
        break;

    case 31: // TornadoField
    {
        // Demon 토네이도 장판
        // 서버가 보내준 effectPositions가 있으면 해당 좌표를 사용해서
        // 모든 클라가 같은 위치에 토네이도를 생성한다.
        auto tornadoBehavior = std::make_unique<TornadoFieldAttackBehavior>(
            4, 18.0f, 0.45f, 5.0f,
            12.0f, 28.0f,
            1.8f, 4.0f, 1.0f,
            1.0f, 0.4f
        );

        tornadoBehavior->SetServerPositions(effectPositions);

        behavior = std::move(tornadoBehavior);
        break;
    }

    case 32: // GaleSlash
    {
        // Demon 돌풍 베기 모양 동기화
        // 서버가 정한 effectOption을 사용해서 모든 클라가 같은 모양으로 생성한다.
        auto shape = (effectOption == 0)
            ? GaleSlashAttackBehavior::SlashShape::Cross
            : GaleSlashAttackBehavior::SlashShape::XDiag;

        behavior = std::make_unique<GaleSlashAttackBehavior>(
            shape,
            75.0f, 30.0f, 3.5f,
            1.4f, 0.4f, 1.2f,
            1.5f, 0.35f
        );
        break;
    }

    case 33: // ShockwaveRing
        behavior = std::make_unique<ShockwaveRingAttackBehavior>(
            85.0f, 35.0f, 4.0f,
            1.6f, 1.0f, 1.0f,
            2.0f, 0.5f
        );
        break;

    default:
        return;
    }

    if (!behavior) return;

    // 실제 장판/VFX/인디케이터 생성
    behavior->Execute(pEnemy);

    NetworkDemonBehaviorEntry entry;
    entry.behavior = std::move(behavior);
    entry.owner = pEnemy;
    entry.timer = 0.0f;

    m_vNetworkDemonBehaviors.push_back(std::move(entry));
}

// Demon 네트워크 Behavior 업데이트
void NetworkManager::UpdateNetworkDemonBehaviors(float deltaTime)
{
    for (auto it = m_vNetworkDemonBehaviors.begin(); it != m_vNetworkDemonBehaviors.end(); )
    {
        it->timer += deltaTime;

        // 비정상 잔존 방지용 안전 타임아웃
        if (!it->behavior || !it->owner || it->timer > 8.0f)
        {
            if (it->behavior)
                it->behavior->Reset();

            it = m_vNetworkDemonBehaviors.erase(it);
            continue;
        }

        it->behavior->Update(deltaTime, it->owner);

        // 완료된 Behavior는 Reset 후 제거
        if (it->behavior->IsFinished())
        {
            it->behavior->Reset();
            it = m_vNetworkDemonBehaviors.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void NetworkManager::InterpolateServerMonsters(float deltaTime)
{
    // 서버 MOVE 패킷은 일정 간격으로 들어오고,
    // 렌더 프레임은 그보다 촘촘하게 돈다.
    // 따라서 일반적인 이동은 절대 스냅하지 않고 항상 보간한다.
    // 큰 거리 차이는 방 전환 / 컷신 / 비정상 위치 보정으로 보고 즉시 스냅한다.
    // 너무 높이면 패킷 간 보간이 끝까지 따라잡고 다음 패킷까지 정지 → 보스가
    //   "달리다 멈춤" 을 반복해 덜덜 떨림. 10 으로 맞추면 패킷 간 lerp 가 항상
    //   진행 중이라 부드러움. 너무 낮으면 lag 처럼 느려져 trade-off.
    constexpr float POS_SMOOTH_RATE = 10.0f;
    constexpr float YAW_SMOOTH_RATE = 10.0f;
    constexpr float BOSS_JUMP_POS_SMOOTH_RATE = 3.0f;

    // 너무 멀리 벌어진 경우만 텔레포트성 보정으로 스냅
    constexpr float TELEPORT_SNAP_DIST_SQ = 100.0f; // 10m 이상

    // 거의 같은 위치면 미세 떨림 방지용으로만 정확히 맞춘다
    constexpr float TINY_SNAP_DIST_SQ = 0.0001f; // 1cm 수준

    const float posAlpha = 1.0f - expf(-POS_SMOOTH_RATE * deltaTime);
    const float yawAlpha = 1.0f - expf(-YAW_SMOOTH_RATE * deltaTime);

    for (auto& kv : m_mapServerMonsterTarget)
    {
        uint64 monsterId = kv.first;
        const ServerMonsterTarget& tgt = kv.second;

        if (!tgt.hasTarget)
            continue;

        Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
        if (pScene && pScene->IsNetworkKrakenCutsceneTarget(monsterId))
            continue;

        // DarkLord 사망 연출 중에는 위치 보간 X — 보스 transform 이 서버 target 으로
        //   계속 lerp 되면 사망 포즈가 옆으로 미끄러지듯 이동해 "다시 살아나 움직이는"
        //   인상을 준다. 사망 시점 위치 그대로 고정.
        if (pScene && pScene->IsNetworkDarkLordDeathTarget(monsterId))
            continue;

        // 죽은 일반 몬스터도 위치 보간 skip
        if (m_setDeadServerMonsters.find(monsterId) != m_setDeadServerMonsters.end())
            continue;

        // 스폰 포탈 / 낙하 연출 중에는 UpdateServerMonsterSpawnEffects가 위치를 직접 제어한다.
        // 여기서 보간하면 공중 낙하 위치와 서버 ground target이 서로 싸워서 떨릴 수 있다.
        if (m_mapServerMonsterSpawnEffects.find(monsterId) != m_mapServerMonsterSpawnEffects.end())
            continue;

        // 인트로 / 메가브레스 컷신 진행 중인 보스는 보간 스킵
        if (m_mapServerBossIntros.find(monsterId) != m_mapServerBossIntros.end())
            continue;

        if (m_mapServerMegaBreathCutscenes.find(monsterId) != m_mapServerMegaBreathCutscenes.end())
            continue;

        auto mIt = m_mapServerMonsters.find(monsterId);
        if (mIt == m_mapServerMonsters.end())
            continue;

        TransformComponent* pT = mIt->second->GetTransform();
        if (!pT)
            continue;

        XMFLOAT3 cur = pT->GetPosition();

        float dx = tgt.px - cur.x;
        float dy = tgt.py - cur.y;
        float dz = tgt.pz - cur.z;
        float distSq = dx * dx + dy * dy + dz * dz;

        bool bBossJumpAction = false;
        auto actIt = m_mapServerBossActions.find(monsterId);
        if (actIt != m_mapServerBossActions.end() &&
            actIt->second.kind == BossActionKind::Jump)
        {
            bBossJumpAction = true;
        }

        float localPosAlpha = posAlpha;
        if (bBossJumpAction)
        {
            localPosAlpha = 1.0f - expf(-BOSS_JUMP_POS_SMOOTH_RATE * deltaTime);
        }

        // 1. 비정상적으로 많이 벌어진 경우만 스냅
        // 단, Boss JumpSlam 연출 중에는 10m 이상 벌어져도 스냅하지 않는다.
        // 서버는 JumpSlam 때 타겟 위치로 즉시 이동시키므로, 여기서 스냅하면 순간이동처럼 보인다.
        if (distSq >= TELEPORT_SNAP_DIST_SQ && !bBossJumpAction)
        {
            pT->SetPosition(tgt.px, tgt.py, tgt.pz);
        }
        else if (distSq <= TINY_SNAP_DIST_SQ)
        {
            pT->SetPosition(tgt.px, tgt.py, tgt.pz);
        }
        else
        {
            XMFLOAT3 next;
            next.x = cur.x + dx * localPosAlpha;
            next.y = cur.y + dy * localPosAlpha;
            next.z = cur.z + dz * localPosAlpha;

            pT->SetPosition(next);
        }

        // yaw 보간 — 360도 경계 넘어갈 때 최단 경로 선택
        XMFLOAT3 rot = pT->GetRotation();

        float delta = tgt.yaw - rot.y;
        while (delta > 180.0f)
            delta -= 360.0f;
        while (delta < -180.0f)
            delta += 360.0f;

        // yaw도 아주 작은 변화면 정확히 맞추고, 그 외에는 보간
        float nextYaw = rot.y;
        if (fabsf(delta) < 0.05f)
        {
            nextYaw = tgt.yaw;
        }
        else
        {
            nextYaw = rot.y + delta * yawAlpha;
        }

        pT->SetRotation(rot.x, nextYaw, rot.z);
    }
}

// ─── 결산 통계 누적 헬퍼 ─────────────────────────────────────────────
//   ProcessPlayerDamage / ProcessMonsterDamage / ProcessSkill 에서 호출.
//   DarkLord 처치 후 SummaryStatsScreen 이 GetGameClearStats() 로 조회.

static double s_NowSec()
{
    return static_cast<double>(GetTickCount64()) / 1000.0;
}

static NetworkManager::GameClearStat& EnsureStat(
    std::unordered_map<uint64, NetworkManager::GameClearStat>& m, uint64 playerId)
{
    auto it = m.find(playerId);
    if (it == m.end())
    {
        NetworkManager::GameClearStat fresh;
        fresh.playerId = playerId;
        fresh.runStartTime = s_NowSec();
        it = m.emplace(playerId, fresh).first;
    }
    return it->second;
}

void NetworkManager::StatOnPlayerDamage(uint64 victimPlayerId, float damage, bool isDead)
{
    if (m_bGameClearStatsFrozen) return;
    if (victimPlayerId == 0) return;

    GameClearStat& s = EnsureStat(m_mapGameClearStats, victimPlayerId);
    if (damage > 0.0f)
        s.totalDamageTaken += damage;
    if (isDead)
    {
        s.deathCount++;
        s.survivalTime = static_cast<float>(s_NowSec() - s.runStartTime);
    }
}

void NetworkManager::StatOnMonsterDamage(uint64 attackerPlayerId, float damage, bool isDead)
{
    if (m_bGameClearStatsFrozen) return;
    if (attackerPlayerId == 0) return;

    GameClearStat& s = EnsureStat(m_mapGameClearStats, attackerPlayerId);
    if (damage > 0.0f)
    {
        s.totalDamageDealt += damage;
        s.hitsLanded++;
        if (damage > s.maxSingleHit)
            s.maxSingleHit = damage;
    }
    if (isDead)
        s.monstersKilled++;
}

void NetworkManager::StatOnSkillUse(uint64 casterPlayerId, int32 skillType)
{
    if (m_bGameClearStatsFrozen) return;
    if (casterPlayerId == 0) return;

    int slot = -1;
    switch (skillType)
    {
    case 1: slot = 0; break; // Q
    case 2: slot = 1; break; // E
    case 3: slot = 2; break; // R
    case 4: slot = 3; break; // MOUSE_RIGHT
    default: return;
    }

    GameClearStat& s = EnsureStat(m_mapGameClearStats, casterPlayerId);
    s.skillUseCounts[slot]++;
}

void NetworkManager::StatOnGameClear()
{
    if (m_bGameClearStatsFrozen) return;
    double now = s_NowSec();
    for (auto& kv : m_mapGameClearStats)
    {
        auto& s = kv.second;
        if (s.deathCount == 0)
            s.survivalTime = static_cast<float>(now - s.runStartTime);
    }
    m_bGameClearStatsFrozen = true;
}

void NetworkManager::StatReset()
{
    m_mapGameClearStats.clear();
    m_bGameClearStatsFrozen = false;
}

