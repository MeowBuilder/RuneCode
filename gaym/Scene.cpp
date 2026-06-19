#include "stdafx.h"
#include "Scene.h"
#include "RenderComponent.h"
#include "Shader.h"
#include "RotatorComponent.h"
#include "ColliderComponent.h"
#include "TransformComponent.h"
#include "InputSystem.h" // Added for InputSystem
#include "HitStopSystem.h"
#include "SlashCue.h"   // ImpactEffectName / ChargeAuraEffectName (Sanctum 원소별 spawn)
#include "WhiteFlashOverlay.h"  // Dominion 시그니처 발동 시 화이트플래시
#include "ScreenSplitOverlay.h" // Sever 화면 베기 후 분리 슬라이드
#include "LeafSystem.h"         // Wind 테마 잎새 시스템
#include "EnemyComponent.h"
#include "MegaBreathAttackBehavior.h"
#include "Room.h"
#include "PlayerComponent.h"
#include "AnimationComponent.h"
#include "CollisionManager.h"
#include "CollisionLayer.h"
#include "SkillComponent.h"
#include "FireballBehavior.h"
#include "WaveSlashBehavior.h"
#include "FireBeamBehavior.h"
#include "MeteorBehavior.h"
#include "NetworkManager.h"
// 물결술사
#include "WaterPuddleBehavior.h"
#include "WaterVortexBehavior.h"
#include "TidalWaveBehavior.h"
#include "WaterOrbBehavior.h"
// 바람술사
#include "WindCutterBehavior.h"
#include "GaleRushBehavior.h"
#include "TornadoBehavior.h"
#include "WindShotBehavior.h"
// 대지술사
#include "StoneSpikesBehavior.h"
#include "EarthArmorBehavior.h"
#include "EarthquakeBehavior.h"
#include "EarthShardBehavior.h"
#include "ProjectileManager.h"
#include "DropItemComponent.h"
#include "InteractableComponent.h"
#include "EnemyComponent.h"
#include "DarkLordSwordSeal.h"
#include "MathUtils.h"
#include "LavaGeyserManager.h"
#include "EffectRegistry.h"
#include "VFXSpriteManager.h"
#include <functional> // Added for std::function
#include "MapLoader.h"
#include "WICTextureLoader12.h"
#include "D3dx12.h"

// ServerPacketHandler.cpp에 정의된 파일 로그 함수 (network_log.txt append)
extern void WriteNetworkLog(const std::string& msg);

Scene::Scene()
{
    m_pCamera = std::make_unique<CCamera>();
    m_pCollisionManager = std::make_unique<CollisionManager>();
    m_pEnemySpawner = std::make_unique<EnemySpawner>();
    m_pProjectileManager = std::make_unique<ProjectileManager>();
    m_pVFXManager     = std::make_unique<VFXManager>();
    m_pSSF                 = std::make_unique<ScreenSpaceFluid>();
    m_pDebugRenderer = std::make_unique<DebugRenderer>();
    m_pTorchSystem = std::make_unique<TorchSystem>();
    m_pDecalManager = std::make_unique<DecalManager>();
}

Scene::~Scene()
{
    // ── 명시적 destruction 순서 강제 ──────────────────────────────────────────
    //   Scene 멤버 선언 순서상 m_vGameObjects(366) / m_vRooms(368) 가
    //   m_pVFXManager(561) / m_vShaders 보다 먼저 선언 → reverse 순서 destruct 시
    //   VFXManager / Shader 가 먼저 죽고 Room / GameObject Component 가 dangling
    //   포인터로 FluidParticleSystem 슬롯 Clear() 또는 Shader 의 iterator 를 건드려 UAF.
    //   해결: 본격 멤버 destruction 시작 전에 의존 관계 큰 컨테이너를 수동으로 비움.
    //     1) 일회성 전투 이펙트 (투사체, 플라잉 보스 탄막) 정리
    //     2) Pending deletion 큐 flush
    //     3) Room 비우기 (LavaGeyser/Component 가 VFXManager 참조)
    //     4) Global GameObject 비우기 (Player/Drop Component 가 VFXManager 참조)
    //     5) Shader 의 RenderComponent 리스트 비우기 — 이후 m_vShaders 자체는 자동 destruct.
    //        시점 상 모든 GameObject / Component 가 이미 destruct → 남은 raw 포인터는 dangling 이므로
    //        리스트를 비워야 함. 비우지 않으면 m_vShaders 멤버 destruct 시 미사용 dangling 잔존.
    //     6) VFXManager 모든 슬롯 graceful stop
    //   이 시점에 m_pVFXManager / m_pDecalManager / m_vShaders 는 아직 alive 한 상태라 safe.

    ClearTransientCombatEffects();
    ProcessPendingDeletions();

    m_pCurrentRoom = nullptr;
    m_vRooms.clear();
    m_vGameObjects.clear();

    for (auto& pShader : m_vShaders)
    {
        if (pShader) pShader->ClearRenderComponents();
    }
    if (m_pVFXManager) m_pVFXManager->ClearAll();

    if (m_pd3dcbPass) m_pd3dcbPass->Unmap(0, NULL);
}

// ── 로딩 화면 워밍업 ──────────────────────────────────────────────────────
//   런타임 hitch 방지: vector capacity 예약 (push_back realloc 0).
//   호출 위치: Dx12App::InitSceneWithElement, Scene::Init 완료 직후.
void Scene::PerformWarmup()
{
    // ── Tier 2-A/B: vector capacity 예약 ──
    // 글로벌 GameObject — Player + 원격 플레이어(최대 4) + 룬 드랍 등.
    m_vGameObjects.reserve(64);
    m_vPendingDeletions.reserve(256);            // 임시 VFX 메쉬 (검기/crescent) 빈번 marking
    m_vGrassClumpObjects.reserve(128);
    m_vAmbientWindIds.reserve(16);
    m_FlightBossBullets.reserve(64);

    // 방 별 capacity — 일반 방 ~30 오브젝트, 보스방 ~100. 안전하게 96 / 24.
    for (auto& room : m_vRooms)
    {
        if (room) room->ReserveCapacity(96, 24);
    }

    // CollisionManager — 글로벌 + 방 컬라이더 합산 추정. 256.
    if (m_pCollisionManager) m_pCollisionManager->ReserveCapacity(256);
}

#include "MeshLoader.h"
#include "MapLoader.h"
#include "Dx12App.h"

void Scene::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList)
{
    // Create Descriptor Heap
    m_pDescriptorHeap = std::make_unique<CDescriptorHeap>();
    m_pDescriptorHeap->Create(pDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16384, true);

    // Create Pass Constant Buffer
    UINT nConstantBufferSize = (sizeof(PassConstants) + 255) & ~255;
    m_pd3dcbPass = CreateBufferResource(pDevice, pCommandList, NULL, nConstantBufferSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
    m_pd3dcbPass->Map(0, NULL, (void**)&m_pcbMappedPass);

    // Set up camera projection
    m_pCamera->SetLens(XMConvertToRadians(60.0f), (float)kWindowWidth / (float)kWindowHeight, 0.1f, 500.0f);

    // Create Shader
    auto pShader = std::make_unique<Shader>();
    pShader->Build(pDevice);

    // --------------------------------------------------------------------------
    // 1. Create default room (will be overwritten by MapLoader if map.json exists)
    // --------------------------------------------------------------------------
    auto pRoom = std::make_unique<CRoom>();
    pRoom->SetState(RoomState::Inactive);
    pRoom->SetBoundingBox(BoundingBox(XMFLOAT3(0, 0, 0), XMFLOAT3(100.0f, 100.0f, 100.0f)));
    m_vRooms.push_back(std::move(pRoom));


    // --------------------------------------------------------------------------
    // 2. Load Global Objects (Player)
    // --------------------------------------------------------------------------
    m_pCurrentRoom = nullptr;

    const CharacterData& charData = GetCharacterData(m_eSelectedElement);
    const char* playerMeshPath = charData.meshPath;
    const char* playerAnimPath = charData.animPath;

    GameObject* pPlayer = MeshLoader::LoadGeometryFromFile(this, pDevice, pCommandList, NULL, playerMeshPath);
    if (pPlayer)
    {
        OutputDebugString(L"Player model loaded successfully!\n");
        pPlayer->GetTransform()->SetPosition(0.0f, 0.0f, 0.0f);
        pPlayer->GetTransform()->SetScale(5.0f, 5.0f, 5.0f);
        auto* pPlayerComp = pPlayer->AddComponent<PlayerComponent>();
        pPlayerComp->SetElementType(m_eSelectedElement);
        m_pPlayerGameObject = pPlayer;

        auto* pAnim = pPlayer->AddComponent<AnimationComponent>();
        pAnim->LoadAnimation(playerAnimPath);
        pAnim->Play("Idle", true);
        pAnim->SetCullEnabled(false);  // 플레이어는 항상 풀 애니메이션 (frustum/phase skip 면제)

        // Add Collider Component for Player
        auto* pPlayerCollider = pPlayer->AddComponent<ColliderComponent>();
        pPlayerCollider->SetExtents(1.0f, 2.0f, 1.0f);  // Player-sized box
        pPlayerCollider->SetCenter(0.0f, 2.0f, 0.0f);   // Center at player's midsection
        pPlayerCollider->SetLayer(CollisionLayer::Player);
        pPlayerCollider->SetCollisionMask(CollisionMask::Player);
        pPlayerCollider->SetOnCollisionEnter([](ColliderComponent* pOther) {});
        pPlayerCollider->SetOnCollisionExit([](ColliderComponent* pOther) {});

        // Add Skill Component
        auto* pSkillComponent = pPlayer->AddComponent<SkillComponent>();

        // Q/E/R 스킬 VFXManager 연결은 VFXManager::Init 이후에 수행 (아래 참고)
        if (m_eSelectedElement == ElementType::Fire)
        {
            auto q = std::make_unique<WaveSlashBehavior>(); q->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::Q, std::move(q));

            auto e = std::make_unique<FireBeamBehavior>(); e->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::E, std::move(e));

            auto r = std::make_unique<MeteorBehavior>(); r->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::R, std::move(r));

            auto rc = std::make_unique<FireballBehavior>();
            rc->SetProjectileManager(m_pProjectileManager.get());
            pSkillComponent->EquipSkill(SkillSlot::RightClick, std::move(rc));

            OutputDebugString(L"[Scene] Skills: Fire — Q:WaveSlash, E:FireBeam, R:Meteor, RC:Fireball\n");
        }
        else if (m_eSelectedElement == ElementType::Water)
        {
            auto q = std::make_unique<WaterPuddleBehavior>(); q->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::Q, std::move(q));

            auto e = std::make_unique<WaterVortexBehavior>(); e->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::E, std::move(e));

            auto r = std::make_unique<TidalWaveBehavior>(); r->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::R, std::move(r));

            auto rc = std::make_unique<WaterOrbBehavior>();
            rc->SetProjectileManager(m_pProjectileManager.get());
            pSkillComponent->EquipSkill(SkillSlot::RightClick, std::move(rc));

            OutputDebugString(L"[Scene] Skills: Water — Q:WaterWave, E:WaterVortex, R:TidalWave, RC:WaterOrb\n");
        }
        else if (m_eSelectedElement == ElementType::Wind)
        {
            auto q = std::make_unique<WindCutterBehavior>(); q->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::Q, std::move(q));

            auto e = std::make_unique<GaleRushBehavior>(); e->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::E, std::move(e));

            auto r = std::make_unique<TornadoBehavior>(); r->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::R, std::move(r));

            auto rc = std::make_unique<WindShotBehavior>();
            rc->SetProjectileManager(m_pProjectileManager.get());
            pSkillComponent->EquipSkill(SkillSlot::RightClick, std::move(rc));

            OutputDebugString(L"[Scene] Skills: Wind — Q:WindCutter, E:GaleRush, R:Tornado, RC:WindShot\n");
        }
        else  // Earth
        {
            auto q = std::make_unique<StoneSpikesBehavior>(); q->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::Q, std::move(q));

            auto e = std::make_unique<EarthArmorBehavior>(); e->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::E, std::move(e));

            auto r = std::make_unique<EarthquakeBehavior>(); r->SetScene(this);
            pSkillComponent->EquipSkill(SkillSlot::R, std::move(r));

            auto rc = std::make_unique<EarthShardBehavior>();
            rc->SetProjectileManager(m_pProjectileManager.get());
            pSkillComponent->EquipSkill(SkillSlot::RightClick, std::move(rc));

            OutputDebugString(L"[Scene] Skills: Earth — Q:StoneSpikes, E:EarthArmor, R:Earthquake, RC:EarthShard\n");
        }

        AddRenderComponentsToHierarchy(pDevice, pCommandList, pPlayer, pShader.get(), true);  // Player casts shadow

        // 플레이어 머티리얼 — 엘리먼트별 영웅 톤. .bin 디폴트(회색) 덮어써서
        //   회색 톤 잠식 해소 + 적 카테고리 색과 자동 분리 (outline 색도 baseColor 기반이라 같이 차별화).
        XMFLOAT4 playerColor;
        switch (m_eSelectedElement)
        {
        case ElementType::Fire:  playerColor = XMFLOAT4(1.00f, 0.55f, 0.30f, 1.0f); break; // 주황빨강
        case ElementType::Water: playerColor = XMFLOAT4(0.35f, 0.85f, 0.95f, 1.0f); break; // 청록
        case ElementType::Wind:  playerColor = XMFLOAT4(0.65f, 0.95f, 0.55f, 1.0f); break; // 라임
        case ElementType::Earth: playerColor = XMFLOAT4(0.95f, 0.75f, 0.40f, 1.0f); break; // 골드
        default:                 playerColor = XMFLOAT4(0.85f, 0.90f, 1.00f, 1.0f); break; // 화이트
        }
        std::function<void(GameObject*)> applyPlayerColor = [&](GameObject* go) {
            if (!go) return;
            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(playerColor.x*0.30f, playerColor.y*0.30f, playerColor.z*0.30f, 1.0f);
            mat.m_cDiffuse  = playerColor;
            // specular 0 — 카툰 룩에서 highlight 가 진해지면 플라스틱처럼 빤딱이게 됨
            mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 32.0f);
            // emissive 제거 — ambient+diffuse+floor 누적으로 발광체처럼 보이던 문제
            mat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            go->SetMaterial(mat);
            applyPlayerColor(go->m_pChild);
            applyPlayerColor(go->m_pSibling);
        };
        applyPlayerColor(pPlayer);
    }
    else
    { // Fallback
        pPlayer = CreateGameObject(pDevice, pCommandList);
        pPlayer->GetTransform()->SetPosition(0.0f, 0.0f, 20.0f);
        pPlayer->AddComponent<PlayerComponent>();
        m_pPlayerGameObject = pPlayer;
    }
    m_pCamera->SetTarget(m_pPlayerGameObject);

    // Set current room
    m_pCurrentRoom = m_vRooms[0].get();

    // --------------------------------------------------------------------------
    // Initialize Enemy Spawner
    // --------------------------------------------------------------------------
    m_pEnemySpawner->Init(pDevice, pCommandList, this, pShader.get());

    // --------------------------------------------------------------------------
    // 영속 리소스(Particles, Projectiles, Interaction Cube)를 먼저 초기화하여
    // 디스크립터 힙의 앞부분에 고정 배치합니다.
    // 이 이후에 맵 오브젝트(MapLoader)가 오도록 순서를 맞춤으로써,
    // 맵 전환 시 m_nPersistentDescriptorEnd 이후 슬롯만 재활용할 수 있습니다.
    // --------------------------------------------------------------------------

    // (구) ParticleSystem 환경 파티클(Ember/Dust/Sandstorm)은 LightEmitterSystem
    //  마이그레이션 과정에서 제거됨 — 향후 ambient 효과는 LightEmitterSystem
    //  Sphere/Cone 레이어로 재구현 가능.

    // VFXManager — 통합 파사드 (플레이어 SSF + 적 빌보드)
    // 내부에서 두 개의 FluidSkillVFXManager를 생성하며, 각 매니저가 슬롯당 2개
    // 디스크립터를 사용한다(SPH SRV + LightEmitter SRV).
    m_pVFXManager->Init(pDevice, pCommandList, m_pDescriptorHeap.get(), m_nNextDescriptorIndex);
    OutputDebugString(L"[Scene] VFXManager initialized\n");
    m_pEnemySpawner->SetVFXManager(m_pVFXManager->GetEnemyVFX());

    // VFXManager 초기화 이후 스킬 행동 클래스에 플레이어 VFX 매니저 연결
    // (Init 전에는 m_pPlayerVFX가 nullptr이므로 반드시 여기서 설정)
    if (m_pPlayerGameObject)
    {
        if (auto* pSC = m_pPlayerGameObject->GetComponent<SkillComponent>())
        {
            FluidSkillVFXManager* pPlayerVFX = m_pVFXManager->GetPlayerVFX();
            pSC->SetVFXManager(pPlayerVFX);
            // ISkillBehavior::SetVFXManager(default: no-op) — 각 Behavior가 override로 처리
            for (int s = 0; s < (int)SkillSlot::Count; ++s)
            {
                if (auto* pBeh = pSC->GetSkill(static_cast<SkillSlot>(s)))
                    pBeh->SetVFXManager(pPlayerVFX);
            }
        }
    }

    // TorchSystem (횃불 조명 및 불꽃 빌보드)
    UINT nTorchDescStart = m_nNextDescriptorIndex;
    m_nNextDescriptorIndex += 2;  // 1 for flame texture SRV, 1 for instance buffer SRV
    m_pTorchSystem->Init(pDevice, pCommandList, this, pShader.get(), m_pDescriptorHeap.get(), nTorchDescStart);
    OutputDebugString(L"[Scene] TorchSystem initialized\n");

    // EffectRegistry 초기화 (모든 VFX 이펙트 등록)
    EffectRegistry::Get().Initialize();
    OutputDebugString(L"[Scene] EffectRegistry initialized\n");

    // Screen-Space Fluid Renderer 초기화
    if (auto* pApp = Dx12App::GetInstance())
    {
        m_pSSF->Init(pDevice, pApp->GetWindowWidth(), pApp->GetWindowHeight());
        OutputDebugString(L"[Scene] ScreenSpaceFluid initialized\n");
    }

    // FluidSkillEffect (구형 Enhance ring 효과) 제거됨.
    // 동일한 효과를 EffectRegistry/VFXManager 경로로 재구현하려면
    // EffectDef를 등록한 뒤 m_pVFXManager->Spawn(...)으로 스폰한다.

    // Projectile Manager (64 reserved slots)
    UINT nProjectileDescriptorStart = m_nNextDescriptorIndex;
    m_nNextDescriptorIndex += 64;
    m_pProjectileManager->Init(this, pDevice, pCommandList, m_pDescriptorHeap.get(), nProjectileDescriptorStart);
    OutputDebugString(L"[Scene] Projectile system initialized\n");

    // Decal Manager
    m_pDecalManager->Init(
        pDevice,
        pCommandList,
        m_pDescriptorHeap.get(),
        m_nNextDescriptorIndex,
        pShader.get());

    // 실제 PNG 파일명 기준으로 로드
    m_pDecalManager->LoadTexture(
        pDevice, pCommandList, m_pDescriptorHeap.get(), m_nNextDescriptorIndex,
        DecalTexture::MagicCircle,
        L"Assets/Textures/VFX/MagicCircle.png");

    m_pDecalManager->LoadTexture(
        pDevice, pCommandList, m_pDescriptorHeap.get(), m_nNextDescriptorIndex,
        DecalTexture::Skull,
        L"Assets/Textures/VFX/human-skull.png");

    m_pDecalManager->LoadTexture(
        pDevice, pCommandList, m_pDescriptorHeap.get(), m_nNextDescriptorIndex,
        DecalTexture::Magic2,
        L"Assets/Textures/VFX/magic_02.png");

    m_pDecalManager->LoadTexture(
        pDevice, pCommandList, m_pDescriptorHeap.get(), m_nNextDescriptorIndex,
        DecalTexture::Magic3,
        L"Assets/Textures/VFX/magic_03.png");

    m_pDecalManager->LoadTexture(
        pDevice, pCommandList, m_pDescriptorHeap.get(), m_nNextDescriptorIndex,
        DecalTexture::Scorch1,
        L"Assets/Textures/VFX/scorch_01.png");

    m_pDecalManager->LoadTexture(
        pDevice, pCommandList, m_pDescriptorHeap.get(), m_nNextDescriptorIndex,
        DecalTexture::Scorch2,
        L"Assets/Textures/VFX/scorch_02.png");

    m_pDecalManager->LoadTexture(
        pDevice, pCommandList, m_pDescriptorHeap.get(), m_nNextDescriptorIndex,
        DecalTexture::Scorch3,
        L"Assets/Textures/VFX/scorch_03.png");

    m_pDecalManager->LoadTexture(
        pDevice, pCommandList, m_pDescriptorHeap.get(), m_nNextDescriptorIndex,
        DecalTexture::Star08,
        L"Assets/Textures/VFX/star_08.png");

    OutputDebugString(L"[Scene] DecalManager initialized\n");

    // ─────────────────────────────────────────────
// VFXSpriteManager 텍스처 등록
// VFXSpriteManager::Spawn("fire1", ...) 같은 호출은
// 여기서 texId가 등록되어 있어야 실제로 화면에 렌더된다.
// ─────────────────────────────────────────────
    auto RegisterRuneSpriteTexture = [&](const std::string& id, const wchar_t* path)
        {
            ComPtr<ID3D12Resource> tex;
            std::unique_ptr<uint8_t[]> decodedData;
            D3D12_SUBRESOURCE_DATA subresource{};

            HRESULT hr = DirectX::LoadWICTextureFromFile(
                pDevice,
                path,
                tex.GetAddressOf(),
                decodedData,
                subresource);

            if (FAILED(hr) || !tex)
            {
                wchar_t wbuf[512];
                swprintf_s(wbuf, L"[VFXSpriteManager] Texture load failed: id=%hs path=%s\n",
                    id.c_str(), path);
                OutputDebugString(wbuf);
                return;
            }

            UINT64 uploadSize = GetRequiredIntermediateSize(tex.Get(), 0, 1);

            ComPtr<ID3D12Resource> upload = CreateBufferResource(
                pDevice,
                pCommandList,
                nullptr,
                uploadSize,
                D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr);

            if (!upload)
            {
                wchar_t wbuf[512];
                swprintf_s(wbuf, L"[VFXSpriteManager] Upload buffer create failed: id=%hs path=%s\n",
                    id.c_str(), path);
                OutputDebugString(wbuf);
                return;
            }

            UpdateSubresources(
                pCommandList,
                tex.Get(),
                upload.Get(),
                0,
                0,
                1,
                &subresource);

            D3D12_RESOURCE_BARRIER barrier =
                CD3DX12_RESOURCE_BARRIER::Transition(
                    tex.Get(),
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            pCommandList->ResourceBarrier(1, &barrier);

            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
            AllocateDescriptor(&cpuHandle, &gpuHandle);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = tex->GetDesc().Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = tex->GetDesc().MipLevels;

            pDevice->CreateShaderResourceView(tex.Get(), &srvDesc, cpuHandle);

            D3D12_RESOURCE_DESC desc = tex->GetDesc();

            VFXSpriteManager::Get().RegisterTex(
                id,
                gpuHandle,
                static_cast<UINT>(desc.Width),
                static_cast<UINT>(desc.Height));

            // SRV가 참조하는 실제 텍스처/업로드 리소스 수명 유지
            m_vRuneSpriteTextures.push_back(tex);
            m_vRuneSpriteUploads.push_back(upload);

            wchar_t wbuf[512];
            swprintf_s(wbuf, L"[VFXSpriteManager] Registered: id=%hs path=%s\n",
                id.c_str(), path);
            OutputDebugString(wbuf);
        };

    // ─────────────────────────────────────────────
    // 실제 PNG 파일명 기준 등록
    // ─────────────────────────────────────────────

    // 예전 코드 호환 alias
    RegisterRuneSpriteTexture("fire1", L"Assets/Textures/VFX/fire_01.png");
    RegisterRuneSpriteTexture("fire2", L"Assets/Textures/VFX/fire_02.png");
    RegisterRuneSpriteTexture("flare1", L"Assets/Textures/VFX/flare_01.png");
    RegisterRuneSpriteTexture("twirl1", L"Assets/Textures/VFX/twirl_01.png");
    RegisterRuneSpriteTexture("twirl2", L"Assets/Textures/VFX/twirl_02.png");
    RegisterRuneSpriteTexture("twirl3", L"Assets/Textures/VFX/twirl_03.png");
    RegisterRuneSpriteTexture("magic2", L"Assets/Textures/VFX/magic_02.png");
    RegisterRuneSpriteTexture("magic3", L"Assets/Textures/VFX/magic_03.png");
    RegisterRuneSpriteTexture("skull", L"Assets/Textures/VFX/human-skull.png");

    // 실제 파일명 id
    RegisterRuneSpriteTexture("fire_01", L"Assets/Textures/VFX/fire_01.png");
    RegisterRuneSpriteTexture("fire_02", L"Assets/Textures/VFX/fire_02.png");
    RegisterRuneSpriteTexture("flare_01", L"Assets/Textures/VFX/flare_01.png");

    RegisterRuneSpriteTexture("twirl_01", L"Assets/Textures/VFX/twirl_01.png");
    RegisterRuneSpriteTexture("twirl_02", L"Assets/Textures/VFX/twirl_02.png");
    RegisterRuneSpriteTexture("twirl_03", L"Assets/Textures/VFX/twirl_03.png");

    RegisterRuneSpriteTexture("magic_01", L"Assets/Textures/VFX/magic_01.png");
    RegisterRuneSpriteTexture("magic_02", L"Assets/Textures/VFX/magic_02.png");
    RegisterRuneSpriteTexture("magic_03", L"Assets/Textures/VFX/magic_03.png");
    RegisterRuneSpriteTexture("magic_04", L"Assets/Textures/VFX/magic_04.png");
    RegisterRuneSpriteTexture("magic_05", L"Assets/Textures/VFX/magic_05.png");

    RegisterRuneSpriteTexture("human-skull", L"Assets/Textures/VFX/human-skull.png");

    RegisterRuneSpriteTexture("slash_01", L"Assets/Textures/VFX/slash_01.png");
    RegisterRuneSpriteTexture("slash_02", L"Assets/Textures/VFX/slash_02.png");
    RegisterRuneSpriteTexture("slash_03", L"Assets/Textures/VFX/slash_03.png");
    RegisterRuneSpriteTexture("slash_04", L"Assets/Textures/VFX/slash_04.png");

    RegisterRuneSpriteTexture("trace_01", L"Assets/Textures/VFX/trace_01.png");
    RegisterRuneSpriteTexture("trace_02", L"Assets/Textures/VFX/trace_02.png");
    RegisterRuneSpriteTexture("trace_03", L"Assets/Textures/VFX/trace_03.png");
    RegisterRuneSpriteTexture("trace_04", L"Assets/Textures/VFX/trace_04.png");
    RegisterRuneSpriteTexture("trace_05", L"Assets/Textures/VFX/trace_05.png");
    RegisterRuneSpriteTexture("trace_06", L"Assets/Textures/VFX/trace_06.png");
    RegisterRuneSpriteTexture("trace_07", L"Assets/Textures/VFX/trace_07.png");

    RegisterRuneSpriteTexture("spark_01", L"Assets/Textures/VFX/spark_01.png");
    RegisterRuneSpriteTexture("spark_02", L"Assets/Textures/VFX/spark_02.png");
    RegisterRuneSpriteTexture("spark_03", L"Assets/Textures/VFX/spark_03.png");
    RegisterRuneSpriteTexture("spark_04", L"Assets/Textures/VFX/spark_04.png");
    RegisterRuneSpriteTexture("spark_05", L"Assets/Textures/VFX/spark_05.png");
    RegisterRuneSpriteTexture("spark_06", L"Assets/Textures/VFX/spark_06.png");
    RegisterRuneSpriteTexture("spark_07", L"Assets/Textures/VFX/spark_07.png");

    RegisterRuneSpriteTexture("smoke_01", L"Assets/Textures/VFX/smoke_01.png");
    RegisterRuneSpriteTexture("smoke_02", L"Assets/Textures/VFX/smoke_02.png");
    RegisterRuneSpriteTexture("smoke_03", L"Assets/Textures/VFX/smoke_03.png");
    RegisterRuneSpriteTexture("smoke_04", L"Assets/Textures/VFX/smoke_04.png");
    RegisterRuneSpriteTexture("smoke_05", L"Assets/Textures/VFX/smoke_05.png");
    RegisterRuneSpriteTexture("smoke_06", L"Assets/Textures/VFX/smoke_06.png");
    RegisterRuneSpriteTexture("smoke_07", L"Assets/Textures/VFX/smoke_07.png");
    RegisterRuneSpriteTexture("smoke_08", L"Assets/Textures/VFX/smoke_08.png");
    RegisterRuneSpriteTexture("smoke_09", L"Assets/Textures/VFX/smoke_09.png");
    RegisterRuneSpriteTexture("smoke_10", L"Assets/Textures/VFX/smoke_10.png");

    RegisterRuneSpriteTexture("star_01", L"Assets/Textures/VFX/star_01.png");
    RegisterRuneSpriteTexture("star_02", L"Assets/Textures/VFX/star_02.png");
    RegisterRuneSpriteTexture("star_03", L"Assets/Textures/VFX/star_03.png");
    RegisterRuneSpriteTexture("star_04", L"Assets/Textures/VFX/star_04.png");
    RegisterRuneSpriteTexture("star_05", L"Assets/Textures/VFX/star_05.png");
    RegisterRuneSpriteTexture("star_06", L"Assets/Textures/VFX/star_06.png");
    RegisterRuneSpriteTexture("star_07", L"Assets/Textures/VFX/star_07.png");
    RegisterRuneSpriteTexture("star_08", L"Assets/Textures/VFX/star_08.png");
    RegisterRuneSpriteTexture("star_09", L"Assets/Textures/VFX/star_09.png");

    RegisterRuneSpriteTexture("symbol_01", L"Assets/Textures/VFX/symbol_01.png");
    RegisterRuneSpriteTexture("symbol_02", L"Assets/Textures/VFX/symbol_02.png");

    // ABY_TIM에서 "clock"을 쓰고 있으므로 우선 star_03에 alias 연결.
    // 나중에 전용 시계 PNG가 생기면 path만 바꾸면 됨.
    RegisterRuneSpriteTexture("clock", L"Assets/Textures/VFX/star_03.png");

    // ABY_VMP에서 "fang"을 쓰고 있으므로 우선 slash_03에 alias 연결.
    // 송곳니 느낌이 더 필요하면 별도 fang.png 추가 후 path 교체.
    RegisterRuneSpriteTexture("fang", L"Assets/Textures/VFX/slash_03.png");

    OutputDebugString(L"[Scene] VFXSpriteManager rune textures registered\n");

    // Debug Renderer (no descriptors)
    m_pDebugRenderer->Init(pDevice, pCommandList);
    OutputDebugString(L"[Scene] Debug renderer initialized (F1 to toggle)\n");

    // SpotLight parameters
    m_pcbMappedPass->m_SpotLight.m_xmf4SpotLightColor = XMFLOAT4(0.5f, 0.0f, 0.0f, 1.0f);
    m_pcbMappedPass->m_SpotLight.m_fSpotLightRange = 100.0f;
    m_pcbMappedPass->m_SpotLight.m_fSpotLightInnerCone = cosf(XMConvertToRadians(20.0f));
    m_pcbMappedPass->m_SpotLight.m_fSpotLightOuterCone = cosf(XMConvertToRadians(30.0f));
    m_pcbMappedPass->m_SpotLight.m_fPad5 = 0.0f;
    m_pcbMappedPass->m_SpotLight.m_fPad6 = 0.0f;

    // Store the shader (needed before Interaction Cube creation)
    m_vShaders.push_back(std::move(pShader));

    // Initialize global GameObjects (player hierarchy)
    for (auto& gameObject : m_vGameObjects)
        gameObject->Init(pDevice, pCommandList);

    // --------------------------------------------------------------------------
    // Create Interaction Cube (Blue Cube) – global object, permanent slot
    // --------------------------------------------------------------------------
    {
        CRoom* pTempRoom = m_pCurrentRoom;
        m_pCurrentRoom = nullptr;  // global object, not in any room
        m_pInteractionCube = CreateGameObject(pDevice, pCommandList);
        m_pCurrentRoom = pTempRoom;
    }

    m_pInteractionCube->GetTransform()->SetPosition(0.0f, 0.0f, 0.0f);  // repositioned after MapLoader
    // 포탈 비주얼: 플레이어 스폰 원형 베이스 위에 깔리는 바닥 마법진. 반경 9u (베이스 크기 매칭).
    m_pInteractionCube->GetTransform()->SetScale(9.0f, 9.0f, 9.0f);
    m_pInteractionCube->GetTransform()->SetRotation(0.0f, 0.0f, 0.0f);

    {
        // 채워진 디스크 (innerRadius=0) — 포탈 표면, 색이 차 있는 느낌
        RingMesh* pPortalDisc = new RingMesh(pDevice, pCommandList, 1.0f, 0.0f, 64);
        m_pInteractionCube->SetMesh(pPortalDisc);

        // 포탈 머티리얼 — 보라 포탈 추천값 (rim 밝게, core 어둡게 = 포탈 깊이감)
        MATERIAL portalMat;
        portalMat.m_cAmbient  = XMFLOAT4(0.00f, 0.00f, 0.00f, 1.0f);
        portalMat.m_cDiffuse  = XMFLOAT4(0.75f, 0.42f, 1.00f, 1.0f);   // 밝은 보라 림
        portalMat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        portalMat.m_cEmissive = XMFLOAT4(0.08f, 0.01f, 0.18f, 1.0f);   // 깊은 심연 — 거의 검정 바이올렛
        m_pInteractionCube->SetMaterial(portalMat);

        // 포탈 vortex body 텍스처 — bIsPortal 분기에서 폴라 UV로 샘플 (소용돌이/구름 형태 강조)
        m_pInteractionCube->SetTextureName("Assets/Textures/VFX/Portal/portal_vortex_body.png");
        D3D12_CPU_DESCRIPTOR_HANDLE portalCpuHandle;
        D3D12_GPU_DESCRIPTOR_HANDLE portalGpuHandle;
        AllocateDescriptor(&portalCpuHandle, &portalGpuHandle);
        m_pInteractionCube->LoadTexture(pDevice, pCommandList, portalCpuHandle);
        m_pInteractionCube->SetSrvGpuDescriptorHandle(portalGpuHandle);

        m_pInteractionCube->AddComponent<RenderComponent>()->SetMesh(pPortalDisc);
        m_pInteractionCube->SetPortal(true);  // bIsPortal — 셰이더 와류/블랙홀 분기 활성
        m_vShaders[0]->AddRenderComponent(m_pInteractionCube->GetComponent<RenderComponent>());

        auto* pInteractable = m_pInteractionCube->AddComponent<InteractableComponent>();
        pInteractable->SetPromptText(L"[F] Interact");
        // 포탈 disc 가 세로 반경 3 → 플레이어 발(y=0) ↔ disc 중심(y=3) 수직 거리 만큼 여유 필요
        pInteractable->SetInteractionDistance(7.0f);
        // 포탈 — 중력/bobbing 비활성화 (Scene 이 위치 직접 결정, 떨어지면 안 됨)
        pInteractable->DisablePhysics();
        pInteractable->SetOnInteract([this](InteractableComponent* pComp) {
            WriteNetworkLog("[Scene] InteractionCube OnInteract fired");

            if (!m_pCurrentRoom)
            {
                WriteNetworkLog("[Scene] OnInteract aborted: no current room");
                return;
            }
            if (m_pCurrentRoom->GetState() != RoomState::Inactive)
            {
                WriteNetworkLog("[Scene] OnInteract aborted: room already active/cleared");
                return;
            }

            NetworkManager* pNet = NetworkManager::GetInstance();
            bool bOnline = (pNet && pNet->IsConnected());
            WriteNetworkLog(bOnline ? "[Scene] OnInteract path: ONLINE → SendTorchInteract"
                                    : "[Scene] OnInteract path: OFFLINE → local SetState(Active)");

            if (bOnline)
            {
                pNet->SendTorchInteract();
                OutputDebugString(L"[Scene] Torch interact requested to server\n");
            }
            else
            {
                m_pCurrentRoom->SetState(RoomState::Active);
                OutputDebugString(L"[Scene] Room activated locally (offline)\n");
            }
            pComp->Hide();
        });
    }
    m_bInteractionCubeActive = true;
    m_bEnemiesSpawned = false;

    // --------------------------------------------------------------------------
    // 용암 바닥 배치 (타일 아래에 큰 평면 하나) — 워터마크 이전에 생성해 영속 슬롯 확보
    // --------------------------------------------------------------------------
    {
        CRoom* pTempRoom = m_pCurrentRoom;
        m_pCurrentRoom = nullptr;  // m_vGameObjects에 등록 (룸에 속하지 않음)
        m_pLavaPlane = CreateGameObject(pDevice, pCommandList);
        m_pCurrentRoom = pTempRoom;

        if (m_pLavaPlane)
        {
            // 하나의 큰 평면 메쉬 (타일 아래 전체를 덮음)
            CubeMesh* pPlaneMesh = new CubeMesh(pDevice, pCommandList, 1.0f, 0.1f, 1.0f);
            m_pLavaPlane->SetMesh(pPlaneMesh);

            // 타일보다 약간 아래에 배치, 맵 + 화산 외곽까지 충분히 덮음
            m_pLavaPlane->GetTransform()->SetPosition(0.0f, -3.5f, -200.0f);
            m_pLavaPlane->GetTransform()->SetScale(2000.0f, 1.0f, 2000.0f);

            m_pLavaPlane->SetLava(true);

            // 용암 머티리얼 (텍스쳐 원본 색상 유지)
            MATERIAL lavaMat;
            lavaMat.m_cAmbient  = XMFLOAT4(0.25f, 0.25f, 0.25f, 1.0f);
            lavaMat.m_cDiffuse  = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
            lavaMat.m_cSpecular = XMFLOAT4(0.85f, 0.85f, 0.85f, 8.0f);  // smoothness 0.85
            lavaMat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            m_pLavaPlane->SetMaterial(lavaMat);

            // 텍스쳐 로드
            m_pLavaPlane->SetTextureName("Assets/MapData/meshes/textures/lava-texture.png");
            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
            AllocateDescriptor(&cpuHandle, &gpuHandle);
            m_pLavaPlane->LoadTexture(pDevice, pCommandList, cpuHandle);
            m_pLavaPlane->SetSrvGpuDescriptorHandle(gpuHandle);

            auto* pRC = m_pLavaPlane->AddComponent<RenderComponent>();
            pRC->SetMesh(pPlaneMesh);
            pRC->SetCastsShadow(false);
            m_vShaders[0]->AddRenderComponent(pRC);
        }
        OutputDebugString(L"[Scene] Lava floor plane placed under tiles\n");
    }

    // --------------------------------------------------------------------------
    // 바위 바닥 배치 (Earth 스테이지 전용 — Init 시 생성 후 평소엔 숨김)
    // 텍스처 없이 머티리얼 + 셰이더 균열 발광으로 시각화 → 디스크립터 절약
    // --------------------------------------------------------------------------
    {
        CRoom* pTempRoom = m_pCurrentRoom;
        m_pCurrentRoom = nullptr;
        m_pRockPlane = CreateGameObject(pDevice, pCommandList);
        m_pCurrentRoom = pTempRoom;

        if (m_pRockPlane)
        {
            CubeMesh* pPlaneMesh = new CubeMesh(pDevice, pCommandList, 1.0f, 0.1f, 1.0f);
            m_pRockPlane->SetMesh(pPlaneMesh);

            // 기본은 숨김 — Earth 스테이지 진입 시 표시 위치로 이동
            m_pRockPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
            m_pRockPlane->GetTransform()->SetScale(2000.0f, 1.0f, 2000.0f);

            m_pRockPlane->SetRocky(true);

            MATERIAL rockMat;
            rockMat.m_cAmbient  = XMFLOAT4(0.20f, 0.18f, 0.16f, 1.0f);
            rockMat.m_cDiffuse  = XMFLOAT4(0.32f, 0.29f, 0.26f, 1.0f);  // 어두운 회갈색
            rockMat.m_cSpecular = XMFLOAT4(0.18f, 0.18f, 0.18f, 4.0f);  // 거친 표면
            rockMat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            m_pRockPlane->SetMaterial(rockMat);

            auto* pRC = m_pRockPlane->AddComponent<RenderComponent>();
            pRC->SetMesh(pPlaneMesh);
            pRC->SetCastsShadow(false);
            m_vShaders[0]->AddRenderComponent(pRC);
        }
        OutputDebugString(L"[Scene] Rock floor plane created (hidden until Earth stage)\n");
    }

    // --------------------------------------------------------------------------
    // Volcano 장식 메쉬 배치 제거됨 (사용자 요청 — 맵 외곽 배경으로 쓰던 대형 화산 오브젝트들)
    // --------------------------------------------------------------------------

    // --------------------------------------------------------------------------
    // 영속 디스크립터 워터마크 기록
    // 이 시점 이후의 슬롯(맵 오브젝트·적·포탈 등)은 맵 전환 시 재활용됩니다.
    // --------------------------------------------------------------------------
    m_nPersistentDescriptorEnd = m_nNextDescriptorIndex;

    // --------------------------------------------------------------------------
    // Map pool – 여기에 맵 JSON 경로를 추가하세요
    // --------------------------------------------------------------------------
    // rooms.json manifest가 있으면 그 목록을 pool로 사용, 없으면 map.json 폴백
    {
        JsonVal manifest = JsonVal::parseFile("Assets/MapData/rooms.json");
        if (!manifest.isNull() && manifest.has("rooms"))
        {
            const JsonVal& roomFiles = manifest["rooms"];
            for (size_t i = 0; i < roomFiles.size(); i++)
                m_vMapPool.push_back(roomFiles[i].str);
        }
        if (!manifest.isNull() && manifest.has("bossRoom"))
            m_strBossMap = manifest["bossRoom"].str;
        if (m_vMapPool.empty())
            m_vMapPool.push_back("Assets/MapData/map.json");
        if (m_strBossMap.empty())
            m_strBossMap = "Assets/MapData/map.json";
    }

    // --------------------------------------------------------------------------
    // Load map from JSON (recyclable slots from m_nPersistentDescriptorEnd onward)
    // --------------------------------------------------------------------------
    m_strCurrentMap = m_vMapPool[0];
    bool bMapLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());

    if (!bMapLoaded) {
        OutputDebugString(L"[Scene] Map load failed – using default test room\n");
        m_pCurrentRoom->SetEnemySpawner(m_pEnemySpawner.get());
        m_pCurrentRoom->SetPlayerTarget(m_pPlayerGameObject);
        m_pCurrentRoom->SetScene(this);
    }

    // 일반 맵: 적 스폰은 인터랙션 큐브 활성화 후 Room::SetState(Active)에서 처리됨
    OutputDebugString(L"[Scene] Normal map loaded - enemies will spawn on room activation\n");

    // 맵 정적 오브젝트의 상수 버퍼를 한 번 초기화
    // (CRoom::Update는 Inactive 상태에서 스킵하므로, 맵 로드 직후 딱 한 번 강제 갱신)
    if (m_pCurrentRoom)
    {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects())
            pGO->Update(0.0f);
    }

    // 인터랙션 큐브(포탈)를 플레이어 스폰 원형 베이스 정중앙에 배치 (MapLoader 가 플레이어 위치 결정 후).
    // 포탈은 베이스 단(stand) 표면 위에 깔린 마법진 → y=1.5 (베이스 표면 위, Z-fight 회피).
    if (m_pPlayerGameObject)
    {
        XMFLOAT3 playerSpawn = m_pPlayerGameObject->GetTransform()->GetPosition();
        m_pInteractionCube->GetTransform()->SetPosition(
            playerSpawn.x, 2.5f, playerSpawn.z);

        // 포탈 진입 연출 — 플레이어를 공중에서 시작해 Levitating 자유낙하 → Landing.
        //   착지 높이는 MapLoader 가 결정한 playerSpawn.y (포탈 베이스 표면) — 베이스 통과 방지.
        //   standRadius: 베이스 영역 (XZ 반경). 영역 밖으로 이동하면 자유낙하 (부양 방지).
        m_pPlayerGameObject->GetTransform()->SetPosition(playerSpawn.x, playerSpawn.y + 22.0f, playerSpawn.z);
        if (auto* pc = m_pPlayerGameObject->GetComponent<PlayerComponent>())
        {
            pc->StartIntroFly(3.0f, playerSpawn.y,
                XMFLOAT3(playerSpawn.x, playerSpawn.y, playerSpawn.z), 5.0f);

            // 네트워크 연출 액션 — 포탈 Intro Fly 동기화
            if (NetworkManager* pNetMgr = NetworkManager::GetInstance())
            {
                if (pNetMgr->IsConnected())
                {
                    pNetMgr->SendPlayerAction(
                        PLAYER_ACTION_PORTAL_INTRO_FLY,
                        playerSpawn.x, playerSpawn.y, playerSpawn.z,
                        0.0f, 0.0f, 1.0f);
                }
            }
        }
    }

    // --------------------------------------------------------------------------
    // 8. Initialize LavaGeyser Manager for current room (화염 맵 전용 기믹)
    // --------------------------------------------------------------------------
    if (m_pCurrentRoom && m_eCurrentTheme == StageTheme::Fire)
    {
        UINT nGeyserDescStart = m_nNextDescriptorIndex;
        m_nNextDescriptorIndex += 1;  // FluidParticleSystem uses 1 descriptor slot

        m_pCurrentRoom->InitLavaGeyserManager(
            pDevice, pCommandList, m_vShaders[0].get(),
            m_pDescriptorHeap.get(), nGeyserDescStart);

        OutputDebugString(L"[Scene] LavaGeyserManager initialized for current room\n");
    }

    // --------------------------------------------------------------------------
    // 9. Initialize Rockfall Manager for current room (땅 맵 전용 기믹)
    // --------------------------------------------------------------------------
    if (m_pCurrentRoom && m_eCurrentTheme == StageTheme::Earth)
    {
        m_pCurrentRoom->InitRockfallManager(
            pDevice, pCommandList, m_vShaders[0].get());

        OutputDebugString(L"[Scene] RockfallManager initialized for current room\n");
    }

    OutputDebugString(L"[Scene] Enemy spawn system initialized\n");

    OutputDebugString(L"[Scene] Interaction cube created\n");
}

void Scene::LoadSceneFromFile(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, const char* pstrFileName)
{
}

CRoom* Scene::CreateRoomFromBounds(const XMFLOAT3& center, const XMFLOAT3& extents)
{
    auto pRoom = std::make_unique<CRoom>();
    pRoom->SetState(RoomState::Inactive);
    pRoom->SetBoundingBox(BoundingBox(center, extents));
    CRoom* pRaw = pRoom.get();
    m_vRooms.push_back(std::move(pRoom));
    // First room from map becomes the current room
    if (!m_pCurrentRoom)
        m_pCurrentRoom = pRaw;
    return pRaw;
}

void Scene::AddRenderComponentsToHierarchy(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
	GameObject* pGameObject, Shader* pShader, bool bCastsShadow)
{
	if (!pGameObject)
	{
		OutputDebugString(L"AddRenderComponentsToHierarchy called with a NULL game object!\n");
		return;
	}

	if (pGameObject->GetMesh())
	{
		auto* pRenderComp = pGameObject->AddComponent<RenderComponent>();
		pRenderComp->SetMesh(pGameObject->GetMesh());
		pRenderComp->SetCastsShadow(bCastsShadow);
		pShader->AddRenderComponent(pRenderComp);
	}

	if (pGameObject->m_pChild)
	{
		AddRenderComponentsToHierarchy(pDevice, pCommandList, pGameObject->m_pChild, pShader, bCastsShadow);
	}
	if (pGameObject->m_pSibling)
	{
		AddRenderComponentsToHierarchy(pDevice, pCommandList, pGameObject->m_pSibling, pShader, bCastsShadow);
	}
}

void Scene::Update(float deltaTime, InputSystem* pInputSystem)
{
    // ── Hit-Stop: 검기 임팩트 등에서 Request 된 dt 정지를 게임 로직에만 반영 ──
    //   카메라/플래시/VFX 는 m_fRawDeltaTime (정지 영향 X) 사용
    //   게임 로직 (이동·애니·물리) 은 deltaTime (effective) 사용
    m_fRawDeltaTime = deltaTime;
    deltaTime       = HitStopSystem::Get().Tick(deltaTime);

    m_fLastDeltaTime = deltaTime;

    // LOD 용 전역 프레임 카운터 — AnimationComponent 의 phase offset 분산에 사용
    AnimationComponent::TickGlobalFrame();

    // ── Sandstorm (Earth) — 로컬 cycleTimer 가 주기적으로 TriggerSandstorm 호출 ──
    //   서버 권위화 후 이 블록 전체 삭제 + 패킷 수신 시 TriggerSandstorm() 만 남김.
    if (m_eCurrentTheme == StageTheme::Earth)
    {
        if (!m_bSandstormActive)
        {
            m_fSandstormCycleTimer += deltaTime;
            if (m_fSandstormCycleTimer >= kSandstormCycleSec)
                TriggerSandstorm(kSandstormDefaultSec);
        }
        else
        {
            m_fSandstormPhaseTimer += deltaTime;
            const float ramp = kSandstormRampSec;
            const float dur  = m_fSandstormDuration;
            const float t    = m_fSandstormPhaseTimer;
            auto sstep = [](float a, float b, float x) {
                float u = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
                return u * u * (3.0f - 2.0f * u);
            };
            // attack-release envelope: smoothstep ramp in, plateau, smoothstep ramp out.
            float attack  = (ramp > 0.0f) ? sstep(0.0f, ramp, t) : 1.0f;
            float release = (ramp > 0.0f) ? (1.0f - sstep(dur - ramp, dur, t)) : 1.0f;
            m_fSandstormStrength = std::clamp(attack * release, 0.0f, 1.0f);
            if (t >= dur)
            {
                m_bSandstormActive    = false;
                m_fSandstormStrength  = 0.0f;
                m_fSandstormPhaseTimer = 0.0f;
                m_fSandstormCycleTimer = 0.0f;
            }
        }
    }
    else if (m_fSandstormStrength != 0.0f || m_bSandstormActive)
    {
        // Earth 외 테마로 전환 시 상태 깨끗하게 reset.
        m_bSandstormActive     = false;
        m_fSandstormStrength   = 0.0f;
        m_fSandstormCycleTimer = 0.0f;
        m_fSandstormPhaseTimer = 0.0f;
    }

    // ── Wind gust (Grass) — Sandstorm 과 동일 패턴 ──
    if (m_eCurrentTheme == StageTheme::Grass)
    {
        if (!m_bWindGustActive)
        {
            m_fWindGustCycleTimer += deltaTime;
            if (m_fWindGustCycleTimer >= kWindGustCycleSec)
                TriggerWindGust(kWindGustDefaultSec);
        }
        else
        {
            m_fWindGustPhaseTimer += deltaTime;
            const float ramp = kWindGustRampSec;
            const float dur  = m_fWindGustDuration;
            const float t    = m_fWindGustPhaseTimer;
            auto sstep = [](float a, float b, float x) {
                float u = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
                return u * u * (3.0f - 2.0f * u);
            };
            float attack  = (ramp > 0.0f) ? sstep(0.0f, ramp, t) : 1.0f;
            float release = (ramp > 0.0f) ? (1.0f - sstep(dur - ramp, dur, t)) : 1.0f;
            m_fWindGustStrength = std::clamp(attack * release, 0.0f, 1.0f);
            if (t >= dur)
            {
                m_bWindGustActive    = false;
                m_fWindGustStrength  = 0.0f;
                m_fWindGustPhaseTimer = 0.0f;
                m_fWindGustCycleTimer = 0.0f;
            }
        }
    }
    else if (m_fWindGustStrength != 0.0f || m_bWindGustActive)
    {
        m_bWindGustActive     = false;
        m_fWindGustStrength   = 0.0f;
        m_fWindGustCycleTimer = 0.0f;
        m_fWindGustPhaseTimer = 0.0f;
    }

    // ── InteractionCube 포탈 회오리 VFX 관리 ──────────────────────────────────
    //   큐브가 활성(보임) 동안 Demon_Tornado 회오리를 큐브 위치에 매 프레임 추적.
    //   인터랙트되어 Hide 되면 stop. 다시 Show 되면 재 spawn.
    //   최종 보스방(Dark) 에선 포탈 회오리 일체 금지 — 입장 직후 VFX 정리 후에도
    //   큐브가 잠깐 active 로 남는 frame 동안 다시 spawn 되는 잔존 회오리 차단.
    if (m_pInteractionCube && m_pVFXManager && m_eCurrentTheme != StageTheme::Dark)
    {
        bool bActive = m_bInteractionCubeActive;
        if (auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>())
            bActive = bActive && pInteractable->IsActive();

        if (bActive)
        {
            DirectX::XMFLOAT3 cubePos = m_pInteractionCube->GetTransform()->GetPosition();
            // 포탈이 바닥에 눕혀짐(XZ 평면) → Ring 평면 normal = Y. Beam도 위로 솟으므로 동일 Y.
            DirectX::XMFLOAT3 vfxNormal{ 0.0f, 1.0f, 0.0f };
            DirectX::XMFLOAT3 beamNormal{ 0.0f, 1.0f, 0.0f };

            // Portal_Ring / Suction / Beam — 각 이미터는 1회 spawn 후 입자 lifetime 만큼 살다 죽으므로
            // 주기적(1.5s)으로 stop + 재 spawn 해서 continuous 한 시각 유지
            constexpr float PORTAL_RING_RESPAWN_INTERVAL = 1.5f;
            m_fPortalRingRespawnTimer += deltaTime;

            bool bNeedSpawn = (m_nInteractionCubePortalRingVFXId < 0)
                           || (m_fPortalRingRespawnTimer >= PORTAL_RING_RESPAWN_INTERVAL);

            if (bNeedSpawn)
            {
                if (m_nInteractionCubePortalRingVFXId >= 0)
                    m_pVFXManager->Stop(m_nInteractionCubePortalRingVFXId);
                if (m_nInteractionCubePortalSuctionVFXId >= 0)
                    m_pVFXManager->Stop(m_nInteractionCubePortalSuctionVFXId);
                if (m_nInteractionCubePortalBeamVFXId >= 0)
                    m_pVFXManager->Stop(m_nInteractionCubePortalBeamVFXId);
                m_nInteractionCubePortalRingVFXId    = m_pVFXManager->Spawn("Portal_Ring",    cubePos, vfxNormal, 0u, false);
                m_nInteractionCubePortalSuctionVFXId = m_pVFXManager->Spawn("Portal_Suction", cubePos, vfxNormal, 0u, false);
                m_nInteractionCubePortalBeamVFXId    = m_pVFXManager->Spawn("Portal_Beam",    cubePos, beamNormal, 0u, false);
                m_fPortalRingRespawnTimer = 0.0f;
            }
            else
            {
                if (m_nInteractionCubePortalRingVFXId    >= 0) m_pVFXManager->Track(m_nInteractionCubePortalRingVFXId,    cubePos, vfxNormal);
                if (m_nInteractionCubePortalSuctionVFXId >= 0) m_pVFXManager->Track(m_nInteractionCubePortalSuctionVFXId, cubePos, vfxNormal);
                if (m_nInteractionCubePortalBeamVFXId    >= 0) m_pVFXManager->Track(m_nInteractionCubePortalBeamVFXId,    cubePos, beamNormal);
            }
        }
        else
        {
            if (m_nInteractionCubePortalRingVFXId >= 0)
            {
                m_pVFXManager->Stop(m_nInteractionCubePortalRingVFXId);
                m_nInteractionCubePortalRingVFXId = -1;
            }
            if (m_nInteractionCubePortalSuctionVFXId >= 0)
            {
                m_pVFXManager->Stop(m_nInteractionCubePortalSuctionVFXId);
                m_nInteractionCubePortalSuctionVFXId = -1;
            }
            if (m_nInteractionCubePortalBeamVFXId >= 0)
            {
                m_pVFXManager->Stop(m_nInteractionCubePortalBeamVFXId);
                m_nInteractionCubePortalBeamVFXId = -1;
            }
            m_fPortalRingRespawnTimer = 0.0f;
        }
    }

    // ── Kraken emergence cinematic ──────────────────────────────────────────
    // Trigger: death callback sets m_bPendingKrakenSpawn
    if (m_bPendingKrakenSpawn && m_pPreloadedKraken)
    {
        m_bPendingKrakenSpawn = false;

        GameObject* pKrakenObj = m_pPreloadedKraken->GetOwner();
        if (pKrakenObj)
        {
            XMFLOAT3 emergePos = m_xmf3PendingKrakenPos;
            emergePos.y = -5.0f;
            pKrakenObj->GetTransform()->SetPosition(emergePos);
            pKrakenObj->GetTransform()->SetScale(0.05f, 0.05f, 0.05f);
        }

        m_eKrakenStage = KrakenCutsceneStage::Rumble;
        m_fKrakenEmergeTimer = 0.0f;

        // 수면 시각 범위를 낙하 안전존과 동일하게 축소 — "보이는 물 = 걸을 수 있는 영역"
        // 낙하존이 rb.Extents × 1.6 이므로 수면 plane 전체 너비 = 2 × 1.6 × extent
        if (m_pWaterPlane && m_pCurrentRoom)
        {
            const BoundingBox& rb = m_pCurrentRoom->GetBoundingBox();
            constexpr float kSafeMul = 1.6f;
            float scaleX = rb.Extents.x * kSafeMul * 2.0f;
            float scaleZ = rb.Extents.z * kSafeMul * 2.0f;
            m_pWaterPlane->GetTransform()->SetScale(scaleX, 1.0f, scaleZ);
            // 수면 중심을 방 중심(XZ)에 정렬 — Y는 기존값(차오름 단계에서 갱신) 유지
            XMFLOAT3 wp = m_pWaterPlane->GetTransform()->GetPosition();
            wp.x = rb.Center.x;
            wp.z = rb.Center.z;
            m_pWaterPlane->GetTransform()->SetPosition(wp);
        }

        // Lock camera on emergence point + rumble shake
        XMFLOAT3 camFocus = m_xmf3PendingKrakenPos;
        camFocus.y = 0.0f;
        m_pCamera->StartCinematic(camFocus, 45.0f, 25.0f, m_pCamera->IsFreeCam() ? 45.0f : 200.0f);
        // 초반 "땅 치는" 쉐이크 제거 — 물에서 치는 임팩트(Burst/Slam)만 남김

        OutputDebugString(L"[Scene] Kraken cutscene: RUMBLE\n");
    }

    // Kraken 컷신 대상 오브젝트 선택
    // 오프라인 모드: m_pPreloadedKraken(EnemyComponent*) 사용
    // 온라인 모드: 서버가 스폰한 m_pNetworkKrakenCutsceneObject(GameObject*) 사용
    GameObject* pKrakenObj = nullptr;

    if (m_pNetworkKrakenCutsceneObject)
    {
        // 온라인/네트워크 Kraken 컷신 대상
        pKrakenObj = m_pNetworkKrakenCutsceneObject;
    }
    else if (m_pPreloadedKraken)
    {
        // 오프라인 Kraken 컷신 대상
        pKrakenObj = m_pPreloadedKraken->GetOwner();
    }

    if (m_eKrakenStage != KrakenCutsceneStage::None && pKrakenObj)
    {
        m_fKrakenEmergeTimer += deltaTime;
        float T = m_fKrakenEmergeTimer;

        auto easeOutCubic = [](float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); };
        auto easeInOutQuad = [](float t) { return t < 0.5f ? 2*t*t : 1 - (-2*t+2)*(-2*t+2)/2; };
        auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };

        // ── Stage: Rumble (0 ~ KRAKEN_T_RUMBLE) ─────────────────────────────
        if (m_eKrakenStage == KrakenCutsceneStage::Rumble)
        {
            // Kraken barely stirs underground
            if (pKrakenObj)
            {
                float t = T / KRAKEN_T_RUMBLE;
                float s = lerp(0.05f, 0.12f, easeOutCubic(t));
                pKrakenObj->GetTransform()->SetScale(s, s, s);
                XMFLOAT3 pos = m_xmf3PendingKrakenPos; pos.y = -5.0f;
                pKrakenObj->GetTransform()->SetPosition(pos);
            }

            if (T >= KRAKEN_T_RUMBLE)
            {
                m_eKrakenStage = KrakenCutsceneStage::Rise;
                // Zoom in closer for the rise
                XMFLOAT3 camFocus = m_xmf3PendingKrakenPos; camFocus.y = 1.0f;
                m_pCamera->StartCinematic(camFocus, 30.0f, 20.0f, 210.0f);
                // 지면 진동 쉐이크 제거 — Burst/Slam의 수면 임팩트만 카메라 쉐이크 유지
                OutputDebugString(L"[Scene] Kraken cutscene: RISE\n");
            }
        }
        // ── Stage: Rise (KRAKEN_T_RUMBLE ~ KRAKEN_T_RISE) ───────────────────
        else if (m_eKrakenStage == KrakenCutsceneStage::Rise)
        {
            float t = (T - KRAKEN_T_RUMBLE) / (KRAKEN_T_RISE - KRAKEN_T_RUMBLE);
            if (t > 1.0f) t = 1.0f;
            float e = easeInOutQuad(t);

            if (pKrakenObj)
            {
                float s = lerp(0.12f, 0.55f, e);
                pKrakenObj->GetTransform()->SetScale(s, s, s);
                XMFLOAT3 pos = m_xmf3PendingKrakenPos;
                pos.y = lerp(-5.0f, -1.0f, e);
                pKrakenObj->GetTransform()->SetPosition(pos);
            }

            if (T >= KRAKEN_T_RISE)
            {
                m_eKrakenStage = KrakenCutsceneStage::Burst;
                // Pull back dramatically for the burst
                XMFLOAT3 camFocus = m_xmf3PendingKrakenPos; camFocus.y = 2.0f;
                m_pCamera->StartCinematic(camFocus, 50.0f, 30.0f, 210.0f);
                m_pCamera->StartShake(1.2f, KRAKEN_T_BURST - KRAKEN_T_RISE);  // Burst 톤다운 (2.5 → 1.2)
                OutputDebugString(L"[Scene] Kraken cutscene: BURST\n");
            }
        }
        // ── Stage: Burst (KRAKEN_T_RISE ~ KRAKEN_T_BURST) ───────────────────
        else if (m_eKrakenStage == KrakenCutsceneStage::Burst)
        {
            float t = (T - KRAKEN_T_RISE) / (KRAKEN_T_BURST - KRAKEN_T_RISE);
            if (t > 1.0f) t = 1.0f;
            float e = easeOutCubic(t);

            if (pKrakenObj)
            {
                float s = lerp(0.55f, KRAKEN_SCALE, e);
                pKrakenObj->GetTransform()->SetScale(s, s, s);
                XMFLOAT3 pos = m_xmf3PendingKrakenPos;
                pos.y = lerp(-1.0f, 0.0f, e);
                pKrakenObj->GetTransform()->SetPosition(pos);
            }

            if (T >= KRAKEN_T_BURST)
            {
                m_eKrakenStage = KrakenCutsceneStage::Reveal;
                // Wide reveal shot (앞모습 보이도록 yaw +180°: 210 → 30)
                XMFLOAT3 camFocus = m_xmf3PendingKrakenPos; camFocus.y = 3.0f;
                m_pCamera->StartCinematic(camFocus, 65.0f, 35.0f, 30.0f);
                OutputDebugString(L"[Scene] Kraken cutscene: REVEAL\n");
            }
        }
        // ── Stage: Reveal (KRAKEN_T_BURST ~ KRAKEN_T_REVEAL) ────────────────
        else if (m_eKrakenStage == KrakenCutsceneStage::Reveal)
        {
            float t = (T - KRAKEN_T_BURST) / (KRAKEN_T_REVEAL - KRAKEN_T_BURST);
            if (t > 1.0f) t = 1.0f;

            // Slowly zoom out further during reveal (yaw 앞모습: 30°)
            float dist = lerp(65.0f, 75.0f, t);
            m_pCamera->SetCinematicOrbit(dist, 35.0f, 30.0f);

            if (T >= KRAKEN_T_REVEAL)
            {
                // Reveal 완료 → 포효 단계로
                m_eKrakenStage = KrakenCutsceneStage::Roar;
                // 포효: 앞모습(yaw 0°) + 너무 가깝지 않도록 dist 55
                XMFLOAT3 camFocus = m_xmf3PendingKrakenPos; camFocus.y = 4.0f;
                m_pCamera->StartCinematic(camFocus, 55.0f, 25.0f, 0.0f);

                // Unreal Take 애니메이션 재생 (포효)
                if (pKrakenObj)
                {
                    auto* pAnim = pKrakenObj->GetComponent<AnimationComponent>();
                    if (pAnim) pAnim->CrossFade("Unreal Take", 0.15f, false);
                }
                m_bKrakenRoarFadedToIdle = false;
                OutputDebugString(L"[Scene] Kraken cutscene: ROAR\n");
            }
        }
        // ── Stage: Roar (KRAKEN_T_REVEAL ~ KRAKEN_T_ROAR) ────────────────────
        else if (m_eKrakenStage == KrakenCutsceneStage::Roar)
        {
            // Unreal Take 클립 길이 2.0s — 끝난 뒤 Idle 루프로 부드럽게 전환해 정지 방지
            const float kUnrealTakeDuration = 2.0f;
            float stageT = T - KRAKEN_T_REVEAL;
            if (!m_bKrakenRoarFadedToIdle && stageT >= kUnrealTakeDuration && pKrakenObj)
            {
                auto* pAnim = pKrakenObj->GetComponent<AnimationComponent>();
                if (pAnim) pAnim->CrossFade("Idle", 0.25f, true);
                m_bKrakenRoarFadedToIdle = true;
            }

            if (T >= KRAKEN_T_ROAR)
            {
                m_eKrakenStage = KrakenCutsceneStage::Jump;

                // 점프 시작(현재 위치) → 착지(맵 바깥 수면 위 Slam 지점)
                if (pKrakenObj)
                    m_xmf3KrakenJumpStart = pKrakenObj->GetTransform()->GetPosition();
                m_xmf3KrakenJumpEnd = {
                    m_xmf3PendingKrakenPos.x + KRAKEN_SLAM_OFFSET_X,
                    KRAKEN_LAND_Y,
                    m_xmf3PendingKrakenPos.z + KRAKEN_SLAM_OFFSET_Z
                };

                // 점프 추적 카메라 (앞모습: yaw 200 → 20)
                XMFLOAT3 camFocus = {
                    (m_xmf3KrakenJumpStart.x + m_xmf3KrakenJumpEnd.x) * 0.5f,
                    4.0f,
                    (m_xmf3KrakenJumpStart.z + m_xmf3KrakenJumpEnd.z) * 0.5f
                };
                m_pCamera->StartCinematic(camFocus, 80.0f, 30.0f, 20.0f);

                // 점프 동안은 애니 전환 없이 Roar 에서 넘어온 Idle 포즈를 그대로 유지.
                // 착지 순간 Attack_Forward 를 Slam 단계에서 CrossFade → 임팩트 프레임 즉시 시작.

                // 착지 방향으로 yaw 정렬 — 모델 forward가 +Z 축이라 180° 플립 불필요
                if (pKrakenObj)
                {
                    XMFLOAT3 d = {
                        m_xmf3KrakenJumpEnd.x - m_xmf3KrakenJumpStart.x, 0.0f,
                        m_xmf3KrakenJumpEnd.z - m_xmf3KrakenJumpStart.z
                    };
                    if (d.x*d.x + d.z*d.z > 0.001f)
                    {
                        float yawDeg = atan2f(d.x, d.z) * (180.0f / XM_PI);
                        XMFLOAT3 rot = pKrakenObj->GetTransform()->GetRotation();
                        rot.y = yawDeg;
                        pKrakenObj->GetTransform()->SetRotation(rot);
                    }
                }
                OutputDebugString(L"[Scene] Kraken cutscene: JUMP\n");
            }
        }
        // ── Stage: Jump (KRAKEN_T_ROAR ~ KRAKEN_T_JUMP) — 포물선 점프 ─────────
        else if (m_eKrakenStage == KrakenCutsceneStage::Jump)
        {
            float t = (T - KRAKEN_T_ROAR) / (KRAKEN_T_JUMP - KRAKEN_T_ROAR);
            if (t > 1.0f) t = 1.0f;

            if (pKrakenObj)
            {
                // Phase split:
                //   0.00 ~ 0.18 : Anticipation — 잠깐 주저앉음 (무게감 연출)
                //   0.18 ~ 1.00 : Launch       — 포물선 비행 + 전방 피치(내리찍는 모션)
                const float kAnticip = 0.18f;

                XMFLOAT3 pos;
                float pitchDeg = 0.0f;

                if (t < kAnticip)
                {
                    float a = t / kAnticip;
                    float crouch = sinf(XM_PI * a) * 2.0f;  // 0→2→0 아래로 움푹
                    pos.x = m_xmf3KrakenJumpStart.x;
                    pos.z = m_xmf3KrakenJumpStart.z;
                    pos.y = m_xmf3KrakenJumpStart.y - crouch;
                    pitchDeg = 8.0f * a;  // 살짝 앞으로 기울며 웅크림
                }
                else
                {
                    float u = (t - kAnticip) / (1.0f - kAnticip);
                    pos.x = lerp(m_xmf3KrakenJumpStart.x, m_xmf3KrakenJumpEnd.x, u);
                    pos.z = lerp(m_xmf3KrakenJumpStart.z, m_xmf3KrakenJumpEnd.z, u);
                    float yBase = lerp(m_xmf3KrakenJumpStart.y, m_xmf3KrakenJumpEnd.y, u);
                    // 상승 ease-out / 하강 ease-in → 정점에서 살짝 멈춘 듯한 무게감
                    float arc;
                    if (u < 0.5f) { float r = u * 2.0f; arc = 1.0f - (1.0f - r) * (1.0f - r); }
                    else          { float r = (u - 0.5f) * 2.0f; arc = 1.0f - r * r; }
                    pos.y = yBase + KRAKEN_JUMP_PEAK_DY * arc;
                    // 공중 피치 — Attack_Forward 애니가 이미 내려찍는 자세라 과한 추가 기울임은 오히려 어색함.
                    // 상승 시 살짝 젖힘(-6) → 하강 시 완만하게 앞으로(+10).
                    pitchDeg = (u < 0.5f) ? -6.0f * (u * 2.0f)
                                          :  10.0f * ((u - 0.5f) * 2.0f);
                }
                pKrakenObj->GetTransform()->SetPosition(pos);

                // pitch 적용 (yaw는 Roar→Jump 전이 시 고정됨)
                XMFLOAT3 rot = pKrakenObj->GetTransform()->GetRotation();
                rot.x = pitchDeg;
                pKrakenObj->GetTransform()->SetRotation(rot);
            }

            if (T >= KRAKEN_T_JUMP)
            {
                m_eKrakenStage = KrakenCutsceneStage::Slam;
                m_bSlamShakeTriggered = false;

                // 착지 직후 pitch 원상복구
                if (pKrakenObj)
                {
                    XMFLOAT3 rot = pKrakenObj->GetTransform()->GetRotation();
                    rot.x = 0.0f;
                    pKrakenObj->GetTransform()->SetRotation(rot);
                }

                // 슬램 임팩트용 카메라 (앞모습 yaw 20°)
                XMFLOAT3 camFocus = m_xmf3KrakenJumpEnd;
                camFocus.y = 2.0f;
                m_pCamera->StartCinematic(camFocus, 50.0f, 30.0f, 20.0f);

                // 착지 순간 내려찍기 애니 재생 시작 — 임팩트 프레임부터 깔끔하게 진입.
                if (pKrakenObj)
                {
                    auto* pAnim = pKrakenObj->GetComponent<AnimationComponent>();
                    if (pAnim) pAnim->CrossFade("Attack_Forward", 0.05f, false, true);
                }
                OutputDebugString(L"[Scene] Kraken cutscene: SLAM\n");
            }
        }
        // ── Stage: Slam (KRAKEN_T_JUMP ~ KRAKEN_T_SLAM) ────────────────────
        else if (m_eKrakenStage == KrakenCutsceneStage::Slam)
        {
            // 진입 시 한 번 강한 카메라 쉐이크 (수면 쾅)
            if (!m_bSlamShakeTriggered)
            {
                m_pCamera->StartShake(3.5f, 0.7f);
                m_bSlamShakeTriggered = true;
            }

            if (T >= KRAKEN_T_SLAM)
            {
                m_eKrakenStage = KrakenCutsceneStage::WaterRise;

                // 카메라 / 조작 권한 플레이어에게 반환 — WaterRise는 일반 게임플레이
                m_pCamera->StopCinematic();

				// 온라인 모드: 컷신용 Kraken 오브젝트 제거 (서버가 스폰한 GameObject이므로 클라이언트에서 직접 삭제)
                NetworkManager::GetInstance()->SetCutscenePlaying(false);
                WriteNetworkLog("[Network] Cutscene lock OFF");

                NetworkManager::GetInstance()->SendBossCutsceneEnd(
                    m_nNetworkKrakenCutsceneMonsterId,
                    static_cast<uint32>(Protocol::BOSS_EVENT_PHASE_CHANGE),
                    2
                );

                // 크라켄이 플레이어 타겟팅 시작 (슬램 지점에서 전투 재개, WaterRise 중에도 물 따라 부상)
                if (m_pPreloadedKraken)
                    m_pPreloadedKraken->SetTarget(m_pPlayerGameObject);

                // 플레이어 낙사존 활성화 — 방 XZ 바운드 밖 = 물 아래로 낙하
                if (m_pPlayerGameObject && m_pCurrentRoom)
                {
                    auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
                    const BoundingBox& rb = m_pCurrentRoom->GetBoundingBox();
                    if (pPC)
                    {
                        // 수면 시각 범위 대비 안전존을 넉넉히 — 방 extents × 1.6배
                        XMFLOAT3 safeExt = { rb.Extents.x * 1.6f, rb.Extents.y, rb.Extents.z * 1.6f };
                        pPC->EnableFallZone(rb.Center, safeExt);
                    }
                }

                OutputDebugString(L"[Scene] Kraken cutscene: WATER RISE (control returned)\n");
            }
        }
        // ── Stage: WaterRise — 물 서서히 차오름, 플레이어 자유 조작 ──────────
        else if (m_eKrakenStage == KrakenCutsceneStage::WaterRise)
        {
            float t = (T - KRAKEN_T_SLAM) / (KRAKEN_T_WATER_RISE - KRAKEN_T_SLAM);
            if (t > 1.0f) t = 1.0f;
            float e = easeInOutQuad(t);

            // 물 표면 Y: 천천히 상승 (-4 → 15) — 기존 타일(Y=0)보다 높은 곳까지
            float waterY = lerp(KRAKEN_WATER_Y_START, KRAKEN_WATER_Y_END, e);
            if (m_pWaterPlane)
            {
                XMFLOAT3 wp = m_pWaterPlane->GetTransform()->GetPosition();
                wp.y = waterY;
                m_pWaterPlane->GetTransform()->SetPosition(wp);
            }

            // 크라켄도 물 따라 부상 (슬램 XZ 유지, Y만 수면 살짝 아래)
            if (pKrakenObj)
            {
                XMFLOAT3 pos = pKrakenObj->GetTransform()->GetPosition();
                pos.y = waterY - 1.0f;
                pKrakenObj->GetTransform()->SetPosition(pos);
            }

            // PlayerComponent에 현재 수면 높이 알림 (safe zone 안에서는 수면에 뜸)
            if (m_pPlayerGameObject)
            {
                auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
                if (pPC) pPC->SetFallZoneWaterY(waterY);
            }

            if (T >= KRAKEN_T_WATER_RISE)
            {
                m_eKrakenStage = KrakenCutsceneStage::None;
                m_pPreloadedKraken = nullptr;
                m_pNetworkKrakenCutsceneObject = nullptr;
                m_nNetworkKrakenCutsceneMonsterId = 0;

                OutputDebugString(L"[Scene] Water rise complete — battle at upper water level\n");
            }
        }
    }

    // F2: FreeCam 토글 (테스트용 자유 시점)
    if (pInputSystem && pInputSystem->IsKeyPressed(VK_F2))
    {
        m_pCamera->ToggleFreeCam();
    }

    // F6: Flight 프로토타입 토글 (4스테이지 바람 보스 레일 슈팅 Step1)
    if (pInputSystem && pInputSystem->IsKeyPressed(VK_F6))
    {
        ToggleFlightMode(Dx12App::GetInstance()->GetDevice(),
                         Dx12App::GetInstance()->GetCommandList());
    }

    // F7: Toon shading 토글 (원신풍 셀 셰이딩 ON/OFF — 적용 전후 비교용)
    if (pInputSystem && pInputSystem->IsKeyPressed(VK_F7))
    {
        m_bToonEnabled = !m_bToonEnabled;
        OutputDebugString(m_bToonEnabled
            ? L"[Toon] Cel shading ON (Genshin style)\n"
            : L"[Toon] Cel shading OFF (original Phong)\n");
    }

    // 비행 모드 활성 시 보스 전진 + 연출 갱신
    if (m_pFlightBossDummy && m_pPlayerGameObject)
    {
        auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
        if (pPC && pPC->IsFlightMode())
        {
            UpdateFlightBoss(deltaTime);
            UpdateFlightFX(deltaTime, pInputSystem);
        }
    }

    // 보스 피격 플래시
    if (m_fFlightBossHitFlashTimer > 0.0f && m_pFlightBossDummy)
    {
        m_fFlightBossHitFlashTimer -= deltaTime;
        float ratio = max(0.0f, m_fFlightBossHitFlashTimer / kFlightHitFlashDuration);
        m_pFlightBossDummy->SetHitFlashAll(ratio);
        if (m_fFlightBossHitFlashTimer <= 0.0f)
        {
            m_pFlightBossDummy->SetHitFlashAll(0.0f);
            m_fFlightBossHitFlashTimer = 0.0f;
        }
    }

    // Toggle debug collider visualization with F1
    if (pInputSystem && pInputSystem->IsKeyPressed(VK_F1))
    {
        m_pDebugRenderer->Toggle();
        OutputDebugString(m_pDebugRenderer->IsEnabled() ? L"[Debug] Colliders ON\n" : L"[Debug] Colliders OFF\n");
    }

    // F3: Toggle static bind pose (no skinning) — plane stays = mesh/material bug, disappears = skinning bug
    if (pInputSystem && pInputSystem->IsKeyPressed(VK_F3))
    {
        AnimationComponent::s_bDebugStaticPose = !AnimationComponent::s_bDebugStaticPose;
        OutputDebugString(AnimationComponent::s_bDebugStaticPose
            ? L"[Debug] Static pose ON (skinning disabled)\n"
            : L"[Debug] Static pose OFF (skinning enabled)\n");
    }

    // F4: Toggle no-texture mode — shows raw geometry with material color only
    if (pInputSystem && pInputSystem->IsKeyPressed(VK_F4))
    {
        GameObject::s_bDebugNoTexture = !GameObject::s_bDebugNoTexture;
        OutputDebugString(GameObject::s_bDebugNoTexture
            ? L"[Debug] No-texture ON (solid color)\n"
            : L"[Debug] No-texture OFF (textures enabled)\n");
    }

    // F5: 모든 스킬 쿨타임 즉시 초기화
    if (pInputSystem && pInputSystem->IsKeyPressed(VK_F5))
    {
        if (m_pPlayerGameObject)
        {
            auto* pSkill = m_pPlayerGameObject->GetComponent<SkillComponent>();
            if (pSkill) pSkill->ResetAllCooldowns();
        }
    }

    // F12: [DEBUG] 플레이어 풀힐 — 보스 검기 데미지 확인용 (원콤 후 재시도)
    if (pInputSystem && pInputSystem->IsKeyPressed(VK_F12))
    {
        if (m_pPlayerGameObject)
        {
            auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
            if (pPC)
            {
                pPC->SetCurrentHP(pPC->GetMaxHP());
                OutputDebugString(L"[Debug] Player HP fully restored\n");
            }
        }
    }

    // Home: [DEBUG] DarkLord 강제 검의 봉인 발동 — 페이즈/쿨다운 무시.
    if (pInputSystem && pInputSystem->IsKeyPressed(VK_HOME))
    {
        if (m_pCurrentRoom)
        {
            const auto& vEnemies = m_pCurrentRoom->GetEnemies();
            EnemyComponent* pBoss = nullptr;
            for (EnemyComponent* pE : vEnemies)
                if (pE && pE->IsBoss()) { pBoss = pE; break; }
            if (pBoss)
            {
                pBoss->DebugForceSpecialAttack(std::make_unique<DarkLordSwordSeal>(
                    ElementType::Fire, 45.0f, 7.0f, 26.0f, 65.0f, 4.0f, 21.0f, 4));
                OutputDebugString(L"[Debug] DarkLord SwordSeal force-trigger\n");
            }
            else
            {
                OutputDebugString(L"[Debug] No boss in current room\n");
            }
        }
    }

    // F8: [DEBUG] 현재 방 살아있는 몬스터 전체 즉사 (서버 권위) — 온라인 모드만 의미 있음
    // F11 은 전체화면 토글과 겹쳐서 F8 사용
    if (pInputSystem && pInputSystem->IsKeyPressed(VK_F8))
    {
        NetworkManager* pNet = NetworkManager::GetInstance();
        if (pNet && pNet->IsConnected())
        {
            pNet->SendDebugKillAll();
            OutputDebugString(L"[Scene] F8 - C_DEBUG_KILL_ALL requested\n");
        }
        else
        {
            OutputDebugString(L"[Scene] F8 ignored - not online (offline 즉사 미구현)\n");
        }
    }

    // F9 / F10: [DEBUG] 현재 방의 보스 애니메이션 클립 사이클러.
    //   안 쓰던 클립이 실제로 어떤 모션인지 in-game 으로 확인하기 위한 도구.
    //   F9 = 다음 클립, F10 = 이전 클립. 보스 1마리 가정 (DarkLord 방).
    //   클립명은 DarkKnight2_skin3 익스포트 기준. 새 보스 추가 시 클립명 별도 관리 필요.
    {
        static const std::vector<std::string> s_vBossClips = {
            "fightidle", "Idle1", "Idle2", "Idle3",
            "walk1", "walkback", "run",
            "attack1", "attack2", "attack3", "attack4",
            "Attack5", "Attack6", "attack7", "attack8", "attack9", "Attack10",
            "gethit1", "gethit2", "gethit3",
            "death1", "death2",
        };
        static int s_nClipIdx = 0;

        auto cycleBossClip = [&](int delta)
        {
            if (!m_pCurrentRoom) return;
            const auto& vEnemies = m_pCurrentRoom->GetEnemies();
            EnemyComponent* pBoss = nullptr;
            for (EnemyComponent* pE : vEnemies)
            {
                if (pE && pE->IsBoss()) { pBoss = pE; break; }
            }
            if (!pBoss) { OutputDebugString(L"[ClipCycler] no boss in current room\n"); return; }
            AnimationComponent* pAnim = pBoss->GetAnimationComponent();
            if (!pAnim) { OutputDebugString(L"[ClipCycler] boss has no AnimationComponent\n"); return; }

            s_nClipIdx = (s_nClipIdx + delta + (int)s_vBossClips.size()) % (int)s_vBossClips.size();
            const std::string& clip = s_vBossClips[s_nClipIdx];
            // bForceRestart=true — 같은 클립 재선택 시에도 처음부터 재생되게 함
            pAnim->CrossFade(clip, 0.10f, true, true);

            char msg[160];
            snprintf(msg, sizeof(msg), "[ClipCycler] (%d/%zu) %s\n",
                     s_nClipIdx + 1, s_vBossClips.size(), clip.c_str());
            OutputDebugStringA(msg);
        };

        if (pInputSystem && pInputSystem->IsKeyPressed(VK_F9))  cycleBossClip(+1);
        if (pInputSystem && pInputSystem->IsKeyPressed(VK_F10)) cycleBossClip(-1);
    }

    static uint64 s_lastDebugRoomActionTick = 0;
    const uint64 nowDebugRoomActionTick = GetTickCount64();

    auto CanSendDebugRoomAction = [&]() -> bool
        {
            if (nowDebugRoomActionTick - s_lastDebugRoomActionTick < 2000)
            {
                OutputDebugString(L"[Scene] Debug room action blocked: cooldown\n");
                return false;
            }

            s_lastDebugRoomActionTick = nowDebugRoomActionTick;
            return true;
        };

    // B 키: 현재 스테이지 보스방 이동
    if (pInputSystem && pInputSystem->IsKeyPressed('B'))
    {
        NetworkManager* pNet = NetworkManager::GetInstance();

        if (pNet && pNet->IsConnected())
        {
            if (CanSendDebugRoomAction())
            {
                // 온라인에서는 서버가 모든 플레이어를 함께 보스방으로 보낸다.
                pNet->SendDebugRoomAction(0); // 0 = DEBUG_ROOM_ACTION_GO_BOSS
                OutputDebugString(L"[Scene] B key - C_DEBUG_ROOM_ACTION GO_BOSS requested\n");
            }
        }
        else
        {
            switch (m_eCurrentTheme)
            {
            case StageTheme::Water:
                OutputDebugString(L"[Scene] B key - Water boss (Kraken)\n");
                TransitionToWaterBossRoom();
                break;

            case StageTheme::Earth:
                OutputDebugString(L"[Scene] B key - Earth boss (Golem)\n");
                TransitionToEarthBossRoom();
                break;

            case StageTheme::Grass:
                OutputDebugString(L"[Scene] B key - Grass boss (Demon)\n");
                TransitionToGrassBossRoom();
                break;

            default:
                OutputDebugString(L"[Scene] B key - Fire boss (Dragon)\n");
                TransitionToBossRoom();
                break;
            }
        }
    }
    
    // N 키: 다음 스테이지 이동
    if (pInputSystem && pInputSystem->IsKeyPressed('N'))
    {
        NetworkManager* pNet = NetworkManager::GetInstance();

        if (pNet && pNet->IsConnected())
        {
            if (CanSendDebugRoomAction())
            {
                // 온라인에서는 서버가 모든 플레이어를 함께 다음 스테이지로 보낸다.
                pNet->SendDebugRoomAction(1); // 1 = DEBUG_ROOM_ACTION_NEXT_STAGE
                OutputDebugString(L"[Scene] N key - C_DEBUG_ROOM_ACTION NEXT_STAGE requested\n");
            }
        }
        else
        {
            switch (m_eCurrentTheme)
            {
            case StageTheme::Fire:
                OutputDebugString(L"[Scene] N key - Fire -> Water\n");
                TransitionToWaterStage();
                break;

            case StageTheme::Water:
                OutputDebugString(L"[Scene] N key - Water -> Earth\n");
                TransitionToEarthStage();
                break;

            case StageTheme::Earth:
                OutputDebugString(L"[Scene] N key - Earth -> Grass\n");
                TransitionToGrassStage();
                break;

            case StageTheme::Grass:
                OutputDebugString(L"[Scene] N key - Grass -> Fire boss\n");
                TransitionToBossRoom();
                break;

            default:
                break;
            }
        }
    }

    // L 키: 보스 메가 브레스 강제 실행 (테스트용)
    if (pInputSystem && pInputSystem->IsKeyPressed('L'))
    {
        if (m_pCurrentRoom)
        {
            const auto& gameObjects = m_pCurrentRoom->GetGameObjects();
            for (const auto& pObj : gameObjects)
            {
                if (!pObj) continue;
                EnemyComponent* pEnemy = pObj->GetComponent<EnemyComponent>();
                if (pEnemy && pEnemy->IsBoss() && !pEnemy->IsDead())
                {
                    OutputDebugString(L"[Scene] Debug key 'L' pressed - Forcing Mega Breath!\n");
                    // 즉시 메가 브레스 주입 및 실행
                    auto megaBreath = std::make_unique<MegaBreathAttackBehavior>();
                    megaBreath->Execute(pEnemy); // 즉시 초기화 및 애니메이션 시작
                    pEnemy->SetAttackBehavior(std::move(megaBreath));
                    pEnemy->ChangeState(EnemyState::Attack); // 공격 상태로 강제 전이
                }
            }
        }
    }

    // 0 키: 다음 방 / 9 키: 이전 방 (개발용 직접 이동)
    if (pInputSystem && !m_vMapPool.empty())
    {
        int poolSize = (int)m_vMapPool.size();
        if (pInputSystem->IsKeyPressed('0'))
            TransitionToRoomByIndex((m_nCurrentPoolIndex + 1) % poolSize);
        else if (pInputSystem->IsKeyPressed('9'))
            TransitionToRoomByIndex((m_nCurrentPoolIndex - 1 + poolSize) % poolSize);
    }

    // Update camera
    if (m_pCamera && pInputSystem)
    {
        bool bFreeCam = m_pCamera->IsFreeCam();
        m_pCamera->Update(
            pInputSystem->GetMouseDeltaX(),
            pInputSystem->GetMouseDeltaY(),
            pInputSystem->GetMouseWheelDelta(),
            deltaTime,
            bFreeCam && pInputSystem->IsKeyDown('W'),
            bFreeCam && pInputSystem->IsKeyDown('S'),
            bFreeCam && pInputSystem->IsKeyDown('A'),
            bFreeCam && pInputSystem->IsKeyDown('D'),
            bFreeCam && pInputSystem->IsKeyDown('E'),
            bFreeCam && pInputSystem->IsKeyDown('Q')
        );
    }

    // Update Pass Constants
    XMMATRIX mView = XMLoadFloat4x4(&m_pCamera->GetViewMatrix());
    XMMATRIX mProjection = XMLoadFloat4x4(&m_pCamera->GetProjectionMatrix());
    XMMATRIX mViewProj = mView * mProjection;
    DirectX::XMStoreFloat4x4(&m_pcbMappedPass->m_xmf4x4ViewProj, XMMatrixTranspose(mViewProj));

    // Set lighting parameters based on current theme
    XMVECTOR lightDir;
    switch (m_eCurrentTheme)
    {
    case StageTheme::Water:
        m_pcbMappedPass->m_xmf4LightColor = XMFLOAT4(2.0f, 1.9f, 1.7f, 1.0f);
        lightDir = XMVector3Normalize(XMVectorSet(-0.8f, -0.3f, 0.5f, 0.0f));
        break;
    case StageTheme::Earth:
        // 따뜻한 황토빛 태양
        m_pcbMappedPass->m_xmf4LightColor = XMFLOAT4(1.9f, 1.7f, 1.2f, 1.0f);
        lightDir = XMVector3Normalize(XMVectorSet(-0.5f, -0.6f, 0.4f, 0.0f));
        break;
    case StageTheme::Grass:
        // 밝은 낮 햇빛 (청량한 하늘)
        m_pcbMappedPass->m_xmf4LightColor = XMFLOAT4(2.1f, 2.0f, 1.8f, 1.0f);
        lightDir = XMVector3Normalize(XMVectorSet(-0.4f, -0.8f, 0.3f, 0.0f));
        break;
    case StageTheme::Dark:
        // 차가운 어두운 라이트 — 다크나이트 아레나, 강도 낮춤 + cool 톤
        //   warm Fire 라이트가 Skin1 청-회색 텍스처를 황금톤으로 보이게 하던 문제 해소.
        m_pcbMappedPass->m_xmf4LightColor = XMFLOAT4(1.0f, 1.1f, 1.5f, 1.0f);
        lightDir = XMVector3Normalize(XMVectorSet(-0.5f, -0.7f, 0.4f, 0.0f));
        // 입장 컷씬 동안 sanctum(vivid 4원소) 톤 ↔ dark 톤 lerp.
        //   m_fSanctumBlend = 0 → 완전 sanctum, 1 → 완전 dark.
        if (m_eDarkLordIntroStage != DarkLordIntroStage::None)
        {
            // Sanctum 팔레트 — 4원소 평균 vivid 톤 (warm gold + cool cyan 블렌드, 밝게).
            XMFLOAT4 sanctumLight(2.0f, 1.75f, 1.45f, 1.0f);
            XMVECTOR sanctumLightDir = XMVector3Normalize(XMVectorSet(-0.3f, -0.85f, 0.3f, 0.0f));
            float b = m_fSanctumBlend;   // 0 → sanctum, 1 → dark
            XMFLOAT4& dst = m_pcbMappedPass->m_xmf4LightColor;
            dst.x = sanctumLight.x + (dst.x - sanctumLight.x) * b;
            dst.y = sanctumLight.y + (dst.y - sanctumLight.y) * b;
            dst.z = sanctumLight.z + (dst.z - sanctumLight.z) * b;
            lightDir = XMVectorLerp(sanctumLightDir, lightDir, b);
        }
        break;
    default: // Fire
        m_pcbMappedPass->m_xmf4LightColor = XMFLOAT4(2.0f, 1.3f, 0.8f, 1.0f);
        lightDir = XMVector3Normalize(XMVectorSet(-0.6f, -0.7f, 0.3f, 0.0f));
        break;
    }
    DirectX::XMStoreFloat3(&m_pcbMappedPass->m_xmf3LightDirection, lightDir);

    // Calculate Light View-Projection for Shadow Mapping
    {
        // Get player position as shadow focus center
        XMFLOAT3 shadowCenter = XMFLOAT3(0.0f, 0.0f, 0.0f);
        if (m_pPlayerGameObject)
        {
            shadowCenter = m_pPlayerGameObject->GetTransform()->GetPosition();
        }

        // Light position: center + opposite of light direction * distance
        float lightDistance = 50.0f;  // 가까워짐
        XMVECTOR vShadowCenter = XMLoadFloat3(&shadowCenter);
        XMVECTOR vLightPos = vShadowCenter - lightDir * lightDistance;

        // Light View Matrix (look at shadow center from light position)
        XMVECTOR vUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        // If light is nearly vertical, use different up vector
        if (fabsf(XMVectorGetY(lightDir)) > 0.99f)
        {
            vUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        }
        XMMATRIX mLightView = XMMatrixLookAtLH(vLightPos, vShadowCenter, vUp);

        // Orthographic Projection for directional light shadow
        float shadowOrthoSize = 160.0f;  // 넓은 그림자 영역
        float nearZ = 0.1f;
        float farZ = 150.0f;
        XMMATRIX mLightProj = XMMatrixOrthographicLH(shadowOrthoSize, shadowOrthoSize, nearZ, farZ);

        XMMATRIX mLightViewProj = mLightView * mLightProj;
        DirectX::XMStoreFloat4x4(&m_pcbMappedPass->m_xmf4x4LightViewProj, XMMatrixTranspose(mLightViewProj));
    }
    m_pcbMappedPass->m_fPad0 = 0.0f; // Padding for directional light

    m_pcbMappedPass->m_xmf4PointLightColor = XMFLOAT4(0.7f, 0.5f, 0.3f, 1.0f); // Subtle warm point light
    m_pcbMappedPass->m_xmf3PointLightPosition = XMFLOAT3(10.0f, 5.0f, -10.0f); // Off to the side
    m_pcbMappedPass->m_fPad1 = 0.0f; // Padding for point light position

    m_pcbMappedPass->m_fPointLightRange = 50.0f; // Smaller range
    m_pcbMappedPass->m_fPad2 = 0.0f; // Padding
    m_pcbMappedPass->m_fPad3 = 0.0f; // Padding
    m_pcbMappedPass->m_fPad4 = 0.0f; // Padding

    switch (m_eCurrentTheme)
    {
    case StageTheme::Water:
        m_pcbMappedPass->m_xmf4AmbientLight = XMFLOAT4(0.6f, 0.65f, 0.75f, 1.0f);
        break;
    case StageTheme::Earth:
        m_pcbMappedPass->m_xmf4AmbientLight = XMFLOAT4(0.5f, 0.42f, 0.3f, 1.0f);
        break;
    case StageTheme::Grass:
        m_pcbMappedPass->m_xmf4AmbientLight = XMFLOAT4(0.5f, 0.6f, 0.4f, 1.0f);
        break;
    default: // Fire
        m_pcbMappedPass->m_xmf4AmbientLight = XMFLOAT4(0.35f, 0.2f, 0.1f, 1.0f);
        break;
    }

    // DarkLord 입장 컷씬 중 sanctum 톤 ↔ dark(=Fire fallback) 톤 lerp — light color 와 동일 규칙.
    if (m_eCurrentTheme == StageTheme::Dark &&
        m_eDarkLordIntroStage != DarkLordIntroStage::None)
    {
        XMFLOAT4 sanctumAmbient(0.55f, 0.55f, 0.55f, 1.0f);   // 밝은 중립 — vivid 4원소 평균
        XMFLOAT4& dst = m_pcbMappedPass->m_xmf4AmbientLight;
        float b = m_fSanctumBlend;
        dst.x = sanctumAmbient.x + (dst.x - sanctumAmbient.x) * b;
        dst.y = sanctumAmbient.y + (dst.y - sanctumAmbient.y) * b;
        dst.z = sanctumAmbient.z + (dst.z - sanctumAmbient.z) * b;
    }

    // Set Camera Position for Specular Calculation
    XMFLOAT3 cameraPosition = m_pCamera->GetPosition();
    m_pcbMappedPass->m_xmf3CameraPosition = cameraPosition;
    m_pcbMappedPass->m_fPadCam = 0.0f; // Padding

    // Update time for lava animation
    m_fTotalTime += deltaTime;
    m_pcbMappedPass->m_fTime = m_fTotalTime;

    // 스테이지 테마 (셰이더 caustics/fog 분기용)
    m_pcbMappedPass->m_nStageTheme = static_cast<int>(m_eCurrentTheme);
    m_pcbMappedPass->m_nToonEnabled = m_bToonEnabled ? 1 : 0;
    m_pcbMappedPass->m_fStormStrength = m_fSandstormStrength;
    m_pcbMappedPass->m_fGustStrength  = m_fWindGustStrength;

    // Update SpotLight parameters based on player position
    if (m_pPlayerGameObject)
    {
        XMFLOAT3 playerPosition = m_pPlayerGameObject->GetTransform()->GetPosition();
        XMVECTOR playerForward = m_pPlayerGameObject->GetTransform()->GetLook();
        XMVECTOR spotlightOffset = XMVectorScale(playerForward, 5.0f); // 5 units in front of the player
        XMVECTOR spotlightPosition = XMLoadFloat3(&playerPosition) + spotlightOffset;
        DirectX::XMStoreFloat3(&m_pcbMappedPass->m_SpotLight.m_xmf3SpotLightPosition, spotlightPosition);
    }
    else
    {
        // Fallback to camera position if player not available
        m_pcbMappedPass->m_SpotLight.m_xmf3SpotLightPosition = cameraPosition;
    }
    XMVECTOR look = m_pCamera->GetLookDirection();
    DirectX::XMStoreFloat3(&m_pcbMappedPass->m_SpotLight.m_xmf3SpotLightDirection, look);


    // 1. Update Global Components (Player, etc.)
    for (auto& gameObject : m_vGameObjects)
    {
        gameObject->Update(deltaTime);
    }

    // 2. Update Current Room
    if (m_pCurrentRoom)
    {
        m_pCurrentRoom->Update(deltaTime);
        // 보스 클리어 → 포탈이 Room::CheckClearCondition에서 스폰됨.
        // 플레이어가 포탈 F 상호작용하면 TransitionToNextRoom에서
        // m_bInBossRoom 플래그를 보고 다음 스테이지로 넘김.

        // Grass(4스테이지) 보스 클리어 시 분기 포탈 추가 spawn:
        //   기존 포탈 = 1스테이지 루프(파밍, 사이클++)
        //   보조 포탈 = 최종 보스(DarkLord)
        // 오프라인 한정. m_bInBossRoom 가 true 인 동안 한 번만 동작.
        NetworkManager* pNetForPortal = NetworkManager::GetInstance();
        bool bOfflinePortal = !(pNetForPortal && pNetForPortal->IsConnected());
        if (bOfflinePortal
            && m_bInBossRoom
            && m_eCurrentTheme == StageTheme::Grass
            && !m_bBranchPortalsSpawned
            && m_pCurrentRoom->HasPortalCube())
        {
            GameObject* pMain = m_pCurrentRoom->GetPortalCube();
            XMFLOAT3 basePos = pMain ? pMain->GetTransform()->GetPosition() : XMFLOAT3(0.0f, 1.5f, 0.0f);
            // 메인 포탈에서 X축으로 떨어진 위치에 보조 포탈 배치 (시각적으로 분리)
            XMFLOAT3 bossPortalPos = { basePos.x + 12.0f, basePos.y, basePos.z };

            m_pCurrentRoom->SpawnSecondPortalAt(bossPortalPos, [this]() {
                OutputDebugString(L"[Scene] Boss-branch portal interacted → DarkLord room\n");
                m_bInBossRoom = false;          // 보스방 플래그 해제 (다음 보스방 진입 가드)
                m_bBranchPortalsSpawned = false; // 다음 사이클을 위해 reset (DarkLord 클리어 시점 별개)
                TransitionToDarkLordRoom();
            });

            m_bBranchPortalsSpawned = true;
            OutputDebugString(L"[Scene] Grass boss cleared → two portals offered (farm loop / final boss)\n");
        }

        // 최종 보스(DarkLord) 클리어 → 게임 클리어 처리
        //   포탈은 자동 spawn 되므로 즉시 숨겨 추가 전환 방지.
        //   본격적인 엔딩 UI 는 Dx12App 측에서 m_bGameClear 플래그를 보고 처리.
        if (m_bInBossRoom
            && m_eCurrentTheme == StageTheme::Dark
            && !m_bGameClear
            && m_pCurrentRoom->HasPortalCube())
        {
            m_bGameClear = true;
            m_pCurrentRoom->ClearPortalCube();
            OutputDebugString(L"[Scene] ========== GAME CLEAR — DarkLord defeated! ==========\n");
        }
    }

    // ── Dragon boss intro cutscene ──────────────────────────────────────────
    if (m_pDragonIntroEnemy)
    {
        if (m_pDragonIntroEnemy->IsInIntro())
        {
            BossIntroPhase phase = m_pDragonIntroEnemy->GetIntroPhase();
            GameObject* pDragonObj = m_pDragonIntroEnemy->GetOwner();
            XMFLOAT3 dragonPos = pDragonObj ? pDragonObj->GetTransform()->GetPosition() : XMFLOAT3(0,0,0);

            if (phase != m_eLastDragonPhase)
            {
                m_eLastDragonPhase = phase;
                switch (phase)
                {
                case BossIntroPhase::Landing:
                    // Mid shot — watch the dragon touch down
                    m_pCamera->StartCinematic({ dragonPos.x, 2.0f, dragonPos.z }, 60.0f, 25.0f, 200.0f);
                    m_pCamera->StartShake(0.4f, 1.5f);
                    break;
                case BossIntroPhase::Roaring:
                    // Dramatic side angle — strong shake, dragon fills frame
                    m_pCamera->StartCinematic({ dragonPos.x, 4.0f, dragonPos.z }, 42.0f, 30.0f, 230.0f);
                    m_pCamera->StartShake(2.2f, 2.2f);
                    break;
                default: break;
                }
            }

            // FlyingIn: wide overhead shot tracking the dragon as it descends
            // Player can move freely during this phase
            if (phase == BossIntroPhase::FlyingIn && pDragonObj)
            {
                float focusY = dragonPos.y * 0.45f + 3.0f;
                m_pCamera->StartCinematic({ dragonPos.x, focusY, dragonPos.z }, 95.0f, 22.0f, 185.0f);
            }
        }
        else
        {
            // Intro done — return camera to player
            m_pCamera->StopCinematic();
            m_eLastDragonPhase = BossIntroPhase::None;
            m_pDragonIntroEnemy = nullptr;
            OutputDebugString(L"[Scene] Dragon intro complete - combat begins\n");
        }
    }

    // ── DarkLord 입장 컷씬 driver — Sanctum→Dread→Sever→Devour→Dominion ───────
    //   m_eDarkLordIntroStage 가 None 이면 즉시 return. cutscene 동안 입력/네트워크 차단.
    UpdateDarkLordIntro(deltaTime);

    // ── 컷씬 종료 후 보스 공격 grace period 카운트다운 ───────────────────────
    //   Dominion 종료 시점에 m_fBossGracePeriodRemain 이 3초로 세팅됨. 이 시간 동안 보스는 정지·무적.
    //   0 도달 시점에 한 번만 AI/무적 풀어 전투 시작.
    if (m_fBossGracePeriodRemain > 0.0f)
    {
        m_fBossGracePeriodRemain -= deltaTime;
        if (m_fBossGracePeriodRemain <= 0.0f)
        {
            m_fBossGracePeriodRemain = 0.0f;
            if (m_pDarkLordCutsceneObject)
            {
                if (auto* pEC = m_pDarkLordCutsceneObject->GetComponent<EnemyComponent>())
                {
                    pEC->SetAIPaused(false);
                    pEC->SetInvincible(false);
                }
            }
            OutputDebugString(L"[Scene] DarkLord grace period END — combat starts\n");
        }
    }

    // Player input block during boss cutscenes.
    // Dragon intro는 FlyingIn/Landing/Roaring 전체 입력 차단.
    // Kraken 컷씬은 WaterRise를 제외하고 입력 차단.
    // MegaBreath는 벽 이동/착지/엄폐물 등장 연출 5.8초 동안만 입력 차단.
    // DarkLord 입장은 전체 5 페이즈 모두 입력 차단 (~11s).
    bool bKrakenBlocking =
        (m_eKrakenStage != KrakenCutsceneStage::None) &&
        (m_eKrakenStage != KrakenCutsceneStage::WaterRise);
    bool bDarkLordIntroBlocking = IsDarkLordIntroPlaying();

    // Dragon intro는 m_pDragonIntroEnemy가 살아있는 동안 전체 입력 차단.
    // m_eLastDragonPhase는 시작 직후 None일 수 있으므로 조건에 넣지 않는다.
    bool bDragonIntroBlocking =
        (m_pDragonIntroEnemy != nullptr);

    NetworkManager* pNet = NetworkManager::GetInstance();

    bool bNetworkDragonIntroBlocking =
        (pNet != nullptr) &&
        pNet->IsBlockingServerBossIntroActive();

    bool bMegaBreathWallBlocking =
        (pNet != nullptr) &&
        pNet->IsMegaBreathInputLockActive();

    bool bBlockInput =
        bKrakenBlocking ||
        bDragonIntroBlocking ||
        bNetworkDragonIntroBlocking ||
        bMegaBreathWallBlocking ||
        bDarkLordIntroBlocking;

    // 네트워크 패킷 전송도 같이 차단.
    // SendMove / SendSkill / SendPlayerAttack 내부에서 m_bCutscenePlaying을 검사한다.
    if (pNet)
    {
        pNet->SetCutscenePlaying(bBlockInput);
    }

    if (m_pPlayerGameObject)
    {
        PlayerComponent* pPlayerComp =
            m_pPlayerGameObject->GetComponent<PlayerComponent>();

        if (pPlayerComp)
        {
            // 보스 인트로 때문에 입력은 막혀도,
            // 방 전환 직후 포탈 낙하 인트로는 계속 업데이트해야 한다.
            bool bAllowPortalIntroFlyUpdate = pPlayerComp->IsIntroFlyPlaying();

            if (!bBlockInput || bAllowPortalIntroFlyUpdate)
            {
                pPlayerComp->PlayerUpdate(
                    deltaTime,
                    pInputSystem,
                    m_pCamera.get()
                );
            }
        }
    }

    // Update Projectile System
    if (m_pProjectileManager)
    {
        m_pProjectileManager->Update(deltaTime);
    }


    // (구) ParticleSystem 기반 환경 파티클 업데이트 블록은 마이그레이션과 함께
    //  제거되었습니다. 동일 효과 필요 시 LightEmitterSystem 으로 재구현하세요.

    // Update VFX Manager (player + enemy 슬롯 풀 동시 업데이트)
    if (m_pVFXManager)
        m_pVFXManager->Update(deltaTime);

    if (m_pDecalManager)
        m_pDecalManager->Update(deltaTime);

    // 룬/상태이상 2D 월드 스프라이트 VFX 갱신
    VFXSpriteManager::Get().Update(deltaTime);

    // 디버그 wind VFX 영구 재스폰 — 90s 마다 자동 재시작 (sub_wind 페이즈 99s 직전)
    //   m_bInBossRoom 일 때만 동작. 보스방 벗어나면 정리.
    if (m_bInBossRoom && m_pVFXManager && m_nDebugWindVFXId >= 0)
    {
        m_fDebugWindVFXTimer += deltaTime;
        // 위치 추적(이동 X 지만 매 프레임 Track 호출하면 슬롯이 살아있는 동안 정상)
        m_pVFXManager->Track(m_nDebugWindVFXId, m_xmf3DebugWindPos, XMFLOAT3(0.0f, 1.0f, 0.0f));
        if (m_fDebugWindVFXTimer >= 90.0f)
        {
            m_pVFXManager->Stop(m_nDebugWindVFXId);
            m_nDebugWindVFXId = m_pVFXManager->Spawn(
                "Demon_Tornado", m_xmf3DebugWindPos, XMFLOAT3(0.0f, 1.0f, 0.0f),
                0u, false);
            m_fDebugWindVFXTimer = 0.0f;
        }
    }
    else if (!m_bInBossRoom && m_nDebugWindVFXId >= 0 && m_pVFXManager)
    {
        m_pVFXManager->Stop(m_nDebugWindVFXId);
        m_nDebugWindVFXId = -1;
    }

    // 테마 변경 감지 — sky color 자동 갱신 (Update 단일 진입점에서 처리)
    if (m_eCurrentTheme != m_eLastAppliedTheme)
    {
        m_eLastAppliedTheme = m_eCurrentTheme;
        ApplyThemeSkyColor();
        if (m_eCurrentTheme != StageTheme::Grass) CleanupWindAmbient();

        // Wind 테마 잎새 시스템 토글
        if (auto* pApp = Dx12App::GetInstance())
        {
            if (auto* pLeaves = pApp->GetLeafSystem())
            {
                bool bWind = (m_eCurrentTheme == StageTheme::Grass);
                if (bWind && m_pCurrentRoom)
                {
                    const BoundingBox& bb = m_pCurrentRoom->GetBoundingBox();
                    XMFLOAT3 center = bb.Center; center.y = 5.0f;
                    XMFLOAT3 half(bb.Extents.x * 0.95f, 18.0f, bb.Extents.z * 0.95f);
                    pLeaves->SetSpawnArea(center, half);
                    pLeaves->SetWind(XMFLOAT3(0.5f, 0.0f, 0.3f), 4.5f);  // 2.5 → 4.5 (수평 drift ↑)
                }
                pLeaves->SetEnabled(bWind);
            }
        }
    }

    // 4스테이지 바람 ambient — Grass 테마: 주기적 큰 토네이도 + 경고 인디케이터 + 데미지 트랩
    //   사이클: Idle(6s) → Warning(2s, 경고링) → Active(6s, 토네이도+트랩) → Cooldown(1.5s)
    if (m_pVFXManager && m_pCurrentRoom && m_eCurrentTheme == StageTheme::Grass)
    {
        // 강풍 burst — 풀 군집 위치에서 주기적으로 꽃가루 터짐. 셰이더 gust 와 손맞춤은 X
        //   (CPU/GPU 동기 비용 큼 → 그냥 비슷한 주기로 자연스럽게 보임).
        //   1.8 ~ 3.2s 사이 랜덤 인터벌 → 풀숲 곳곳에서 불규칙 터짐.
        m_fGustBurstTimer += deltaTime;
        if (m_fGustBurstTimer >= 2.5f && !m_vGrassClumpObjects.empty())
        {
            int idx = rand() % static_cast<int>(m_vGrassClumpObjects.size());
            GameObject* pClump = m_vGrassClumpObjects[idx];
            if (pClump)
            {
                XMFLOAT3 cp = pClump->GetTransform()->GetPosition();
                cp.y = 1.5f;  // 풀 중간 높이에서 터짐
                m_pVFXManager->Spawn("Wind_GustBurst", cp,
                                     XMFLOAT3(0.0f, 1.0f, 0.0f), 0u, false);
            }
            // 다음 인터벌 랜덤화 — timer 를 음수로 살짝 밀어주면 다음 burst 까지 1.8~3.2 사이
            m_fGustBurstTimer = -((rand() % 1400) / 1000.0f);  // -1.4 ~ 0
        }

        m_fPeriodicTornadoTimer += deltaTime;

        // Active 페이즈: 모든 플레이어에 데미지/석션/트랩 적용
        if (m_eTornadoPhase == TornadoEventPhase::Active)
        {
            const float kSuctionRadius = 7.5f;       // 이 안에 들어오면 끌림
            const float kTrapRadius    = 3.5f;       // 이 안에 들어오면 트랩 발동
            const float kPullStrength  = 12.0f;      // 끌리는 속도 (units/s)
            const float kInitialDamage = 10.0f;      // 트랩 진입 시 즉시 데미지
            const float kTickDamage    = 6.0f;       // 트랩 중 0.5s 마다 tick
            const float kTickInterval  = 0.5f;

            m_fTornadoDamageTickTimer += deltaTime;
            bool bTickFires = (m_fTornadoDamageTickTimer >= kTickInterval);
            if (bTickFires) m_fTornadoDamageTickTimer = 0.0f;

            const auto& players = GetAllPlayers();
            for (GameObject* pP : players)
            {
                if (!pP) continue;
                auto* pPC = pP->GetComponent<PlayerComponent>();
                auto* pPT = pP->GetTransform();
                if (!pPC || !pPT) continue;

                XMFLOAT3 ppos = pPT->GetPosition();
                float dx = ppos.x - m_xmf3TornadoEventPos.x;
                float dz = ppos.z - m_xmf3TornadoEventPos.z;
                float dist = sqrtf(dx*dx + dz*dz);

                if (pPC->IsTornadoTrapped())
                {
                    if (bTickFires) pPC->TakeDamage(kTickDamage);
                }
                else if (dist <= kTrapRadius)
                {
                    // 트랩 발동
                    pPC->EnterTornadoTrap(m_xmf3TornadoEventPos);
                    pPC->TakeDamage(kInitialDamage);
                }
                else if (dist <= kSuctionRadius && dist > 0.001f)
                {
                    // 끌림: 중심 방향으로 위치 이동
                    float pullDist = kPullStrength * deltaTime;
                    ppos.x -= (dx / dist) * pullDist;
                    ppos.z -= (dz / dist) * pullDist;
                    pPT->SetPosition(ppos);
                }
            }
        }

        // 페이즈 전환
        switch (m_eTornadoPhase)
        {
        case TornadoEventPhase::Idle:
            if (!m_bUseNetworkTornadoEvent && m_fPeriodicTornadoTimer >= 6.0f)
            {
                // 새 위치 선택
                const BoundingBox& rb = m_pCurrentRoom->GetBoundingBox();
                float halfX = rb.Extents.x * 0.55f;
                float halfZ = rb.Extents.z * 0.55f;
                float rx = ((rand() % 1000) / 1000.0f - 0.5f) * 2.0f * halfX;
                float rz = ((rand() % 1000) / 1000.0f - 0.5f) * 2.0f * halfZ;
                m_xmf3TornadoEventPos = XMFLOAT3(rb.Center.x + rx, 0.0f, rb.Center.z + rz);

                // 경고 링 spawn (Y=0.1 지면 살짝 위)
                XMFLOAT3 warnPos = m_xmf3TornadoEventPos; warnPos.y = 0.1f;
                m_nTornadoWarningVFXId = m_pVFXManager->Spawn(
                    "Wind_TornadoWarning", warnPos, XMFLOAT3(0.0f, 1.0f, 0.0f), 0u, false);

                m_eTornadoPhase = TornadoEventPhase::Warning;
                m_fPeriodicTornadoTimer = 0.0f;
            }
            break;

        case TornadoEventPhase::Warning:
            if (m_fPeriodicTornadoTimer >= 2.0f)
            {
                // 경고 종료 (자동 fade 되지만 안전하게 stop)
                if (m_nTornadoWarningVFXId >= 0) m_pVFXManager->Stop(m_nTornadoWarningVFXId);
                m_nTornadoWarningVFXId = -1;

                // 토네이도 본체 spawn
                XMFLOAT3 tornadoPos = m_xmf3TornadoEventPos; tornadoPos.y = 0.5f;
                m_nPeriodicTornadoId = m_pVFXManager->Spawn(
                    "Demon_Tornado_Big", tornadoPos, XMFLOAT3(0.0f, 1.0f, 0.0f), 0u, false);

                m_eTornadoPhase = TornadoEventPhase::Active;
                m_fPeriodicTornadoTimer = 0.0f;
                m_fTornadoDamageTickTimer = 0.0f;
            }
            break;

        case TornadoEventPhase::Active:
            if (m_fPeriodicTornadoTimer >= 6.0f)
            {
                // 토네이도 종료 + 트랩된 플레이어 모두 해제 (자연 낙하)
                if (m_nPeriodicTornadoId >= 0) m_pVFXManager->Stop(m_nPeriodicTornadoId);
                m_nPeriodicTornadoId = -1;

                for (GameObject* pP : GetAllPlayers())
                {
                    if (!pP) continue;
                    if (auto* pPC = pP->GetComponent<PlayerComponent>())
                        pPC->ExitTornadoTrap();
                }

                m_eTornadoPhase = TornadoEventPhase::Cooldown;
                m_fPeriodicTornadoTimer = 0.0f;
            }
            break;

        case TornadoEventPhase::Cooldown:
            if (m_fPeriodicTornadoTimer >= 1.5f)
            {
                m_eTornadoPhase = TornadoEventPhase::Idle;
                m_fPeriodicTornadoTimer = 0.0f;
            }
            break;
        }
    }
    else
    {
        // grass 이탈 시 정리
        if (m_nPeriodicTornadoId >= 0 && m_pVFXManager)
        {
            m_pVFXManager->Stop(m_nPeriodicTornadoId);
            m_nPeriodicTornadoId = -1;
        }
        if (m_nTornadoWarningVFXId >= 0 && m_pVFXManager)
        {
            m_pVFXManager->Stop(m_nTornadoWarningVFXId);
            m_nTornadoWarningVFXId = -1;
        }
        // 트랩된 플레이어 해제
        for (GameObject* pP : GetAllPlayers())
        {
            if (pP)
                if (auto* pPC = pP->GetComponent<PlayerComponent>())
                    pPC->ExitTornadoTrap();
        }
        m_eTornadoPhase = TornadoEventPhase::Idle;
        m_fPeriodicTornadoTimer = 0.0f;
    }

    // Update Torch System (flickering effect)
    if (m_pTorchSystem)
    {
        m_pTorchSystem->Update(deltaTime);
        m_pTorchSystem->FillLightData(m_pcbMappedPass);
    }

    // 2. Check for collisions
    if (m_pCollisionManager)
    {
        // Collect colliders from global objects
        std::vector<ColliderComponent*> globalColliders;
        for (auto& gameObject : m_vGameObjects)
        {
            CollectColliders(gameObject.get(), globalColliders);
        }

        // Collect colliders from current room
        std::vector<ColliderComponent*> roomColliders;
        if (m_pCurrentRoom)
        {
            const auto& roomObjects = m_pCurrentRoom->GetGameObjects();
            for (const auto& obj : roomObjects)
            {
                CollectColliders(obj.get(), roomColliders);
            }
        }

        // Run collision detection
        m_pCollisionManager->Update(globalColliders, roomColliders);
    }

    // Process pending deletions at end of frame
    ProcessPendingDeletions();
}

void Scene::UpdateRenderList()
{
    // 1. Clear previous frame's render list from all shaders
    for (auto& shader : m_vShaders)
    {
        shader->ClearRenderComponents();
    }

    // 2. Register Global Objects (Player, etc.)
    Shader* pMainShader = m_vShaders[0].get();

    // Helper vector for traversal to avoid recursion
    std::vector<GameObject*> stack;
    stack.reserve(64);

    // Process Global Objects
    for (auto& gameObject : m_vGameObjects)
    {
        stack.push_back(gameObject.get());
        while (!stack.empty())
        {
            GameObject* pObj = stack.back();
            stack.pop_back();

            if (pObj->GetComponent<RenderComponent>())
            {
                pMainShader->AddRenderComponent(pObj->GetComponent<RenderComponent>());
            }

            if (pObj->m_pSibling) stack.push_back(pObj->m_pSibling);
            if (pObj->m_pChild) stack.push_back(pObj->m_pChild);
        }
    }

    // 3. Register Current Room Objects
    if (m_pCurrentRoom)
    {
        const auto& roomObjects = m_pCurrentRoom->GetGameObjects();
        for (const auto& obj : roomObjects)
        {
            stack.push_back(obj.get());
            while (!stack.empty())
            {
                GameObject* pObj = stack.back();
                stack.pop_back();

                if (pObj->GetComponent<RenderComponent>())
                {
                    pMainShader->AddRenderComponent(pObj->GetComponent<RenderComponent>());
                }

                if (pObj->m_pSibling) stack.push_back(pObj->m_pSibling);
                if (pObj->m_pChild) stack.push_back(pObj->m_pChild);
            }
        }
    }
}

void Scene::RenderShadowPass(ID3D12GraphicsCommandList* pCommandList)
{
    // Update render list before shadow pass (ensures correct objects are rendered)
    UpdateRenderList();

    // Set the descriptor heap
    ID3D12DescriptorHeap* ppHeaps[] = { m_pDescriptorHeap->GetHeap() };
    pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    // Render shadow casters
    for (auto& shader : m_vShaders)
    {
        shader->RenderShadowPass(pCommandList, GetPassCBVAddress());
    }
}

void Scene::Render(ID3D12GraphicsCommandList* pCommandList, D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle,
                   D3D12_CPU_DESCRIPTOR_HANDLE mainRTV, D3D12_CPU_DESCRIPTOR_HANDLE mainDSV,
                   ID3D12Resource* pMainRTBuffer)
{
    // Set the descriptor heap
    ID3D12DescriptorHeap* ppHeaps[] = { m_pDescriptorHeap->GetHeap() };
    pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    // Note: UpdateRenderList() is already called in RenderShadowPass() before this

    // Iterate through shaders (groups) and render
    for (auto& shader : m_vShaders)
    {
        shader->Render(pCommandList, GetPassCBVAddress(), shadowSrvHandle,
                       m_d3dWaterNormal2GpuHandle, m_d3dWaterHeight2GpuHandle,
                       m_d3dFoamOpacityGpuHandle, m_d3dFoamDiffuseGpuHandle);
    }

    // Terrain 렌더 (불투명, 전용 힙 사용) — Water stage에만 사용 (다른 stage에선 흰 면 버그)
    if (m_pTerrain && m_pTerrain->IsLoaded() && m_eCurrentTheme == StageTheme::Water)
    {
        m_pTerrain->Render(pCommandList, GetPassCBVAddress());

        // Terrain 전용 힙 사용 후 → 메인 힙 복구
        ID3D12DescriptorHeap* mainHeaps[] = { m_pDescriptorHeap->GetHeap() };
        pCommandList->SetDescriptorHeaps(1, mainHeaps);
    }

    // Render projectiles (after main rendering, pipeline state is already set)
    if (m_pProjectileManager)
    {
        m_pProjectileManager->Render(pCommandList);
    }

    // Render ground decals (회전 마법진 등 — 투명 패스, 바닥에 눕힌 쿼드)
    if (m_pDecalManager)
    {
        m_pDecalManager->Render(pCommandList, GetPassCBVAddress());
    }

    // Render skill geometry meshes (e.g. meteor rock)
    if (m_pPlayerGameObject)
    {
        auto* pSkill = m_pPlayerGameObject->GetComponent<SkillComponent>();
        if (pSkill)
        {
            for (int s = 0; s < static_cast<int>(SkillSlot::Count); ++s)
            {
                ISkillBehavior* pBehavior = pSkill->GetSkill(static_cast<SkillSlot>(s));
                if (pBehavior) pBehavior->Render(pCommandList);
            }
        }
    }

    // 구 ParticleSystem.Render 호출은 LightEmitterSystem 통합으로 제거됨.

    // ---------- Screen-Space Fluid 렌더링 (VFXManager 통합 경로) ----------
    bool bHasFluid = (m_pVFXManager != nullptr); // 매니저 있으면 SSF 시도

    if (m_pSSF && m_pSSF->IsInitialized() && bHasFluid)
    {
        // 행렬 준비
        XMMATRIX mView = XMLoadFloat4x4(&m_pCamera->GetViewMatrix());
        XMMATRIX mProj = XMLoadFloat4x4(&m_pCamera->GetProjectionMatrix());
        XMFLOAT3 camRight = { XMVectorGetX(mView.r[0]), XMVectorGetX(mView.r[1]), XMVectorGetX(mView.r[2]) };
        XMFLOAT3 camUp    = { XMVectorGetY(mView.r[0]), XMVectorGetY(mView.r[1]), XMVectorGetY(mView.r[2]) };

        XMFLOAT4X4 viewProjT, viewT;
        DirectX::XMStoreFloat4x4(&viewProjT, XMMatrixTranspose(mView * mProj));
        DirectX::XMStoreFloat4x4(&viewT, XMMatrixTranspose(mView));

        XMFLOAT4X4 projRaw;
        DirectX::XMStoreFloat4x4(&projRaw, mProj);

        float projA = projRaw._33;
        float projB = projRaw._43;

        // GPU SPH dispatch (BeginDepthPass 전에) — 두 매니저 동시 처리
        m_pVFXManager->DispatchSPH(pCommandList, m_fLastDeltaTime);

        // 조명 방향 (공통)
        XMFLOAT3 lightDirWorld = { -0.5f, -0.8f, -0.3f };
        XMVECTOR lightV = XMVector3TransformNormal(XMLoadFloat3(&lightDirWorld), mView);
        XMFLOAT3 lightDirVS;
        DirectX::XMStoreFloat3(&lightDirVS, XMVector3Normalize(lightV));

        auto GetFluidColors = [&](bool blurOnly) -> std::pair<XMFLOAT4, XMFLOAT4>
        {
            XMFLOAT4 outer = { 0.95f, 0.15f, 0.0f, 0.9f };
            XMFLOAT4 inner = { 1.0f,  0.88f, 0.25f, 1.0f };
            FluidElementColor colors = m_pVFXManager->GetDominantFluidColors(blurOnly);
            if (colors.coreColor.w > 0.01f)
            {
                outer   = colors.edgeColor;
                outer.w = (std::max)(outer.w, 0.6f);
                inner   = colors.coreColor;
            }
            return { outer, inner };
        };

        // ── 패스 A: blur 없는 플레이어 이펙트 ──
        bool bHasNonBlur = m_pVFXManager->HasActiveSlots(false);
        if (bHasNonBlur)
        {
            m_pSSF->BeginDepthPass(pCommandList);
            m_pVFXManager->RenderDepth(pCommandList, viewProjT, viewT, camRight, camUp, projA, projB, m_pSSF.get(), false);

            m_pSSF->BeginThicknessPass(pCommandList);
            m_pVFXManager->RenderThicknessOnly(pCommandList, m_pSSF.get(), false);

            m_pSSF->EndDepthPass(pCommandList);
            m_pSSF->SetBlurEnabled(false);

            auto [outerA, innerA] = GetFluidColors(false);

            if (auto* pApp = Dx12App::GetInstance())
            {
                D3D12_VIEWPORT vp = { 0, 0, (FLOAT)pApp->GetWindowWidth(), (FLOAT)pApp->GetWindowHeight(), 0.0f, 1.0f };
                pCommandList->RSSetViewports(1, &vp);
                D3D12_RECT sr = { 0, 0, (LONG)pApp->GetWindowWidth(), (LONG)pApp->GetWindowHeight() };
                pCommandList->RSSetScissorRects(1, &sr);
            }
            m_pSSF->SmoothAndComposite(pCommandList, mainRTV, mainDSV, projRaw, lightDirVS, outerA, innerA);

            pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
            pCommandList->OMSetRenderTargets(1, &mainRTV, FALSE, &mainDSV);
        }

        // ── 패스 B: blur 이펙트 (Q 파도 등) ──
        bool bHasBlur = m_pVFXManager->HasActiveSlots(true);
        if (bHasBlur)
        {
            m_pSSF->BeginDepthPass(pCommandList);
            m_pVFXManager->RenderDepth(pCommandList, viewProjT, viewT, camRight, camUp, projA, projB, m_pSSF.get(), true);

            m_pSSF->BeginThicknessPass(pCommandList);
            m_pVFXManager->RenderThicknessOnly(pCommandList, m_pSSF.get(), true);

            m_pSSF->EndDepthPass(pCommandList);
            m_pSSF->SetBlurEnabled(true);

            auto [outerB, innerB] = GetFluidColors(true);

            if (auto* pApp = Dx12App::GetInstance())
            {
                D3D12_VIEWPORT vp = { 0, 0, (FLOAT)pApp->GetWindowWidth(), (FLOAT)pApp->GetWindowHeight(), 0.0f, 1.0f };
                pCommandList->RSSetViewports(1, &vp);
                D3D12_RECT sr = { 0, 0, (LONG)pApp->GetWindowWidth(), (LONG)pApp->GetWindowHeight() };
                pCommandList->RSSetScissorRects(1, &sr);
            }
            m_pSSF->SmoothAndComposite(pCommandList, mainRTV, mainDSV, projRaw, lightDirVS, outerB, innerB);

            pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
            pCommandList->OMSetRenderTargets(1, &mainRTV, FALSE, &mainDSV);
        }

        // ── 적 투사체 빌보드 렌더 (SSF 완료 후, 적 슬롯 풀 전용) ──
        m_pVFXManager->RenderEnemyEffects(pCommandList, viewProjT, camRight, camUp);

        // ── 플레이어 LightEmitter 빌보드 렌더 (SSF 미사용 효과: Trail, Impact 등) ──
        // SPH 플레이어 효과는 SSF로 처리되었으므로 LightEmitter 슬롯만 별도 렌더
        m_pVFXManager->RenderPlayerLightEmitters(pCommandList, viewProjT, camRight, camUp);
    }
    else
    {
        // Fallback: SSF 없이 빌보드 렌더링
        XMMATRIX mView2 = XMLoadFloat4x4(&m_pCamera->GetViewMatrix());
        XMFLOAT3 camRight2 = { XMVectorGetX(mView2.r[0]), XMVectorGetX(mView2.r[1]), XMVectorGetX(mView2.r[2]) };
        XMFLOAT3 camUp2    = { XMVectorGetY(mView2.r[0]), XMVectorGetY(mView2.r[1]), XMVectorGetY(mView2.r[2]) };

        XMMATRIX mViewProj2 = mView2 * XMLoadFloat4x4(&m_pCamera->GetProjectionMatrix());
        XMFLOAT4X4 viewProj2;
        DirectX::XMStoreFloat4x4(&viewProj2, XMMatrixTranspose(mViewProj2));

        if (m_pVFXManager)
            m_pVFXManager->Render(pCommandList, viewProj2, camRight2, camUp2);
    }

    // Render lava geyser particles (Room 기반 맵 기믹)
    if (m_pCurrentRoom)
    {
        LavaGeyserManager* pGeyserManager = m_pCurrentRoom->GetLavaGeyserManager();
        if (pGeyserManager && pGeyserManager->IsActive())
        {
            XMMATRIX mView3 = XMLoadFloat4x4(&m_pCamera->GetViewMatrix());
            XMFLOAT3 camRight3 = { XMVectorGetX(mView3.r[0]), XMVectorGetX(mView3.r[1]), XMVectorGetX(mView3.r[2]) };
            XMFLOAT3 camUp3    = { XMVectorGetY(mView3.r[0]), XMVectorGetY(mView3.r[1]), XMVectorGetY(mView3.r[2]) };

            XMMATRIX mViewProj3 = mView3 * XMLoadFloat4x4(&m_pCamera->GetProjectionMatrix());
            XMFLOAT4X4 viewProj3;
            DirectX::XMStoreFloat4x4(&viewProj3, XMMatrixTranspose(mViewProj3));

            pGeyserManager->Render(pCommandList, viewProj3, camRight3, camUp3);
        }
    }

    // Render torch flame billboards
    if (m_pTorchSystem && m_pTorchSystem->GetTorchCount() > 0)
    {
        XMMATRIX mView4 = XMLoadFloat4x4(&m_pCamera->GetViewMatrix());
        XMFLOAT3 camRight4 = { XMVectorGetX(mView4.r[0]), XMVectorGetY(mView4.r[0]), XMVectorGetZ(mView4.r[0]) };
        XMFLOAT3 camUp4    = { XMVectorGetX(mView4.r[1]), XMVectorGetY(mView4.r[1]), XMVectorGetZ(mView4.r[1]) };

        XMMATRIX mViewProj4 = mView4 * XMLoadFloat4x4(&m_pCamera->GetProjectionMatrix());
        XMFLOAT4X4 viewProj4;
        DirectX::XMStoreFloat4x4(&viewProj4, XMMatrixTranspose(mViewProj4));

        m_pTorchSystem->Render(pCommandList, viewProj4, camRight4, camUp4);
    }

    // Render debug colliders (F1 to toggle)
    if (m_pDebugRenderer && m_pDebugRenderer->IsEnabled())
    {
        std::vector<ColliderComponent*> allColliders;

        // Collect from global objects
        for (auto& gameObject : m_vGameObjects)
        {
            CollectColliders(gameObject.get(), allColliders);
        }

        // Collect from current room
        if (m_pCurrentRoom)
        {
            const auto& roomObjects = m_pCurrentRoom->GetGameObjects();
            for (const auto& obj : roomObjects)
            {
                CollectColliders(obj.get(), allColliders);
            }
        }

        m_pDebugRenderer->Render(pCommandList, GetPassCBVAddress(), allColliders);
    }
}

void Scene::OnResizeSSF(UINT width, UINT height)
{
    if (m_pSSF && m_pSSF->IsInitialized())
    {
        if (auto* pApp = Dx12App::GetInstance())
        {
            m_pSSF->OnResize(pApp->GetDevice(), width, height);
        }
    }
}

GameObject* Scene::CreateGameObject(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList)
{
    // Note: We use raw pointer here, but ownership is transferred to unique_ptr below
    GameObject* newGameObject = new GameObject();

    UINT slot = m_nNextDescriptorIndex;
    bool bRecyclable = (slot >= m_nPersistentDescriptorEnd);

    auto cacheIt = bRecyclable ? m_vCBCache.find(slot) : m_vCBCache.end();
    bool bReused = (cacheIt != m_vCBCache.end());

    // 슬롯 충돌 진단 로그
    wchar_t dbgBuf[256];
    swprintf_s(dbgBuf, L"[Scene] CreateObject: Slot=%u, Reused=%s, PersistentEnd=%u\n", 
              slot, (bReused ? L"TRUE" : L"FALSE"), m_nPersistentDescriptorEnd);
    OutputDebugString(dbgBuf);

    if (bReused)
    {
        // 같은 슬롯 번호에 이미 생성된 리소스 재사용
        ObjectConstants* pMapped = nullptr;
        cacheIt->second->Map(0, nullptr, (void**)&pMapped);
        newGameObject->ReuseConstantBuffer(cacheIt->second, pMapped);

        // 다른 방 방문 시 AllocateDescriptor(SRV)가 이 슬롯을 덮어썼을 수 있으므로
        // CBV 뷰를 항상 재생성하여 슬롯 타입 충돌 버그를 방지
        UINT nCBSize = (sizeof(ObjectConstants) + 255) & ~255;
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
        cbvDesc.BufferLocation = cacheIt->second->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes    = nCBSize;
        pDevice->CreateConstantBufferView(&cbvDesc, m_pDescriptorHeap->GetCPUHandle(slot));
    }
    else
    {
        // 처음 사용하는 슬롯 — 리소스 + CBV 생성
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_pDescriptorHeap->GetCPUHandle(slot);
        newGameObject->CreateConstantBuffer(pDevice, pCommandList, sizeof(ObjectConstants), cpuHandle);
        if (bRecyclable)
            m_vCBCache[slot] = newGameObject->GetConstantBufferResource();
    }

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_pDescriptorHeap->GetGPUHandle(slot);
    newGameObject->SetGpuDescriptorHandle(gpuHandle);

    m_nNextDescriptorIndex++;

    // Add to Room or Scene
    if (m_pCurrentRoom)
    {
        m_pCurrentRoom->AddGameObject(std::unique_ptr<GameObject>(newGameObject));
    }
    else
    {
        m_vGameObjects.push_back(std::unique_ptr<GameObject>(newGameObject));
    }

    return newGameObject;
}

void Scene::PrintHierarchy(GameObject* pGameObject, int nDepth)
{
	if (!pGameObject) return;

	// Indent for hierarchy visualization
	std::wstring indent(nDepth * 2, ' ');

	// Prepare debug string
	wchar_t buffer[256];
	swprintf_s(buffer, 256, L"%sFrame: %hs, Has Mesh: %s, Has RenderComponent: %s\n",
		indent.c_str(),
		pGameObject->m_pstrFrameName,
		pGameObject->GetMesh() ? L"Yes" : L"No",
		pGameObject->GetComponent<RenderComponent>() ? L"Yes" : L"No"
	);

	OutputDebugString(buffer);

	// Recurse for children and siblings
	if (pGameObject->m_pChild)
	{
		PrintHierarchy(pGameObject->m_pChild, nDepth + 1);
	}
	if (pGameObject->m_pSibling)
	{
		PrintHierarchy(pGameObject->m_pSibling, nDepth);
	}
}

void Scene::CollectColliders(GameObject* pGameObject, std::vector<ColliderComponent*>& outColliders)
{
    if (!pGameObject) return;

    // Check if this object has a ColliderComponent
    ColliderComponent* pCollider = pGameObject->GetComponent<ColliderComponent>();
    if (pCollider && pCollider->IsEnabled())
    {
        outColliders.push_back(pCollider);
    }

    // Recurse for children and siblings
    if (pGameObject->m_pChild)
    {
        CollectColliders(pGameObject->m_pChild, outColliders);
    }
    if (pGameObject->m_pSibling)
    {
        CollectColliders(pGameObject->m_pSibling, outColliders);
    }
}

bool Scene::IsNearInteractionCube() const
{
    if (!m_pInteractionCube || !m_pPlayerGameObject)
        return false;

    auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>();
    if (!pInteractable || !pInteractable->IsActive())
        return false;

    return pInteractable->IsPlayerInRange(m_pPlayerGameObject);
}

void Scene::TriggerInteraction()
{
    if (!IsNearInteractionCube())
        return;

    auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>();
    if (pInteractable)
    {
        pInteractable->Interact();
        m_bInteractionCubeActive = false;
        m_bEnemiesSpawned = true;
    }
}

void Scene::HideInteractionCubeByNetworkStart()
{
    if (!m_pInteractionCube)
        return;

    auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>();

    // 이미 숨겨진 상태면 중복 처리하지 않는다.
    if (!m_bInteractionCubeActive && (!pInteractable || !pInteractable->IsActive()))
        return;

    // 다른 클라가 F를 눌러도 서버 몬스터 스폰을 받는 순간 내 포탈도 숨긴다.
    if (pInteractable)
        pInteractable->Hide();

    // Scene::Update의 포탈 VFX 추적도 같이 멈추도록 상태값을 맞춘다.
    m_bInteractionCubeActive = false;
    m_bEnemiesSpawned = true;

    WriteNetworkLog("[Scene] InteractionCube hidden by network room start");
}

bool Scene::IsNearDropItem() const
{
    if (!m_pCurrentRoom || !m_pPlayerGameObject)
        return false;

    GameObject* pDropItem = m_pCurrentRoom->GetDropItem();
    if (!pDropItem)
        return false;

    DropItemComponent* pDropComp = pDropItem->GetComponent<DropItemComponent>();
    if (!pDropComp || !pDropComp->IsActive())
        return false;

    XMFLOAT3 playerPos = m_pPlayerGameObject->GetTransform()->GetPosition();
    XMFLOAT3 dropPos = pDropItem->GetTransform()->GetPosition();

    return MathUtils::Distance3D(playerPos, dropPos) <= m_fDropInteractionDistance;
}

void Scene::StartDropInteraction()
{
    if (m_eDropState != DropInteractionState::None && m_eDropState != DropInteractionState::NearDrop)
        return;

    if (!IsNearDropItem())
        return;

    m_pCurrentDropItem = m_pCurrentRoom->GetDropItem();
    m_eDropState = DropInteractionState::SelectingRune;

    OutputDebugString(L"[Scene] Started drop interaction - selecting rune\n");
}

void Scene::SelectRune(int choice)
{
    if (m_eDropState != DropInteractionState::SelectingRune)
        return;

    if (choice < 0 || choice >= 3)
        return;

    if (!m_pCurrentDropItem)
    {
        CancelDropInteraction();
        return;
    }

    DropItemComponent* pDropComp = m_pCurrentDropItem->GetComponent<DropItemComponent>();
    if (!pDropComp)
    {
        CancelDropInteraction();
        return;
    }

    // Get the selected rune
    EquippedRune selectedRune = pDropComp->GetRuneOption(choice);

    // Apply to player's skill component (slot Q, runeIndex 0 — legacy path)
    if (m_pPlayerGameObject && !selectedRune.IsEmpty())
    {
        SkillComponent* pSkill = m_pPlayerGameObject->GetComponent<SkillComponent>();
        if (pSkill)
        {
            pSkill->SetRuneSlot(SkillSlot::Q, 0, selectedRune.runeId, selectedRune.stackCount);
            wchar_t buffer[128];
            swprintf_s(buffer, L"[Scene] Rune selected (legacy): %hs\n", selectedRune.runeId.c_str());
            OutputDebugString(buffer);
        }
    }

    // 온라인 모드에서는 룬 선택 완료를 서버에 알린다.
    // 서버는 S_RUNE_REWARD_PICKED를 브로드캐스트하고,
    // 모든 클라가 해당 플레이어의 룬 오브젝트를 같이 숨긴다.
    if (NetworkManager* pNet = NetworkManager::GetInstance())
    {
        if (pNet->IsConnected())
            pNet->SendRuneRewardPick();
    }

    // Deactivate and hide the drop item
    pDropComp->SetActive(false);
    m_pCurrentDropItem->GetTransform()->SetPosition(0.0f, -1000.0f, 0.0f);

    // Clear room's drop reference
    if (m_pCurrentRoom)
    {
        m_pCurrentRoom->ClearDropItem();
    }

    // Reset state
    m_pCurrentDropItem = nullptr;
    m_eDropState = DropInteractionState::None;
}

void Scene::CancelDropInteraction()
{
    DropInteractionState prevState = m_eDropState;

    // 룬 선택창을 보고 나온 경우에는 서버에 완료 알림을 보낸다.
    // 룬을 장착하지 않았어도 "선택창 종료" 기준으로 포탈 잠금을 풀기 위함.
    if (prevState == DropInteractionState::SelectingRune ||
        prevState == DropInteractionState::SelectingSkill)
    {
        if (NetworkManager* pNet = NetworkManager::GetInstance())
        {
            if (pNet->IsConnected())
            {
                pNet->SendRuneRewardPick();
            }
        }
    }

    m_eDropState = DropInteractionState::None;
    m_pCurrentDropItem = nullptr;
    m_sSelectedRuneId.clear();
    m_nSelectedRuneOptionIndex = -1;

    OutputDebugString(L"[Scene] Drop interaction cancelled\n");
}

void Scene::SelectRuneByClick(int runeIndex)
{
    if (m_eDropState != DropInteractionState::SelectingRune)
        return;

    if (runeIndex < 0 || runeIndex >= 3)
        return;

    if (!m_pCurrentDropItem)
    {
        CancelDropInteraction();
        return;
    }

    DropItemComponent* pDropComp = m_pCurrentDropItem->GetComponent<DropItemComponent>();
    if (!pDropComp)
    {
        CancelDropInteraction();
        return;
    }

    // Store selected rune ID and selected option index, then move to skill selection state.
    // 서버에는 runeId를 직접 보내지 않고 rewardOptionIndex만 보낸다.
    EquippedRune selected = pDropComp->GetRuneOption(runeIndex);
    m_sSelectedRuneId = selected.runeId;
    m_nSelectedRuneOptionIndex = runeIndex;
    m_eDropState = DropInteractionState::SelectingSkill;

    wchar_t buffer[160];
    swprintf_s(buffer,
        L"[Scene] Rune clicked: index=%d rune=%hs - Now select skill slot\n",
        runeIndex,
        m_sSelectedRuneId.c_str());
    OutputDebugString(buffer);
}

void Scene::SelectSkillSlot(SkillSlot slot, int runeSlotIndex)
{
    if (m_eDropState != DropInteractionState::SelectingSkill)
        return;

    if (m_sSelectedRuneId.empty())
    {
        CancelDropInteraction();
        return;
    }

    // Apply rune to player's skill slot
    if (m_pPlayerGameObject)
    {
        SkillComponent* pSkill = m_pPlayerGameObject->GetComponent<SkillComponent>();
        if (pSkill)
        {
            pSkill->SetRuneSlot(slot, runeSlotIndex, m_sSelectedRuneId);

            const wchar_t* slotNames[] = { L"Q", L"E", L"R", L"RMB" };
            wchar_t buffer[128];
            swprintf_s(buffer, L"[Scene] Rune %hs assigned to %s slot %d\n",
                m_sSelectedRuneId.c_str(), slotNames[static_cast<int>(slot)], runeSlotIndex + 1);
            OutputDebugString(buffer);
        }
    }

    // 온라인 모드에서는 룬 장착 요청을 서버에 보낸다.
    // 클라는 runeId를 직접 보내지 않고, 이번 보상 3개 중 몇 번째를 골랐는지만 보낸다.
    // 서버는 pendingRewardRunes[rewardOptionIndex]로 실제 runeId를 검증한 뒤 장착/DB 저장한다.
    if (NetworkManager* pNet = NetworkManager::GetInstance())
    {
        if (pNet->IsConnected())
        {
            if (m_nSelectedRuneOptionIndex >= 0 && m_nSelectedRuneOptionIndex < 3)
            {
                pNet->SendRuneEquip(
                    static_cast<uint32>(m_nSelectedRuneOptionIndex),
                    static_cast<uint32>(slot),
                    static_cast<uint32>(runeSlotIndex));
            }
        }
    }

    // Deactivate and hide the drop item
    if (m_pCurrentDropItem)
    {
        DropItemComponent* pDropComp = m_pCurrentDropItem->GetComponent<DropItemComponent>();
        if (pDropComp)
            pDropComp->SetActive(false);
        m_pCurrentDropItem->GetTransform()->SetPosition(0.0f, -1000.0f, 0.0f);
    }

    // Clear room's drop reference
    if (m_pCurrentRoom)
    {
        m_pCurrentRoom->ClearDropItem();
    }

    // Reset state
    m_pCurrentDropItem = nullptr;
    m_sSelectedRuneId.clear();
    m_nSelectedRuneOptionIndex = -1;
    m_eDropState = DropInteractionState::None;
}

bool Scene::IsNearPortalCube() const
{
    if (!m_pCurrentRoom || !m_pPlayerGameObject)
        return false;

    auto checkPortal = [this](GameObject* pPortal) -> bool {
        if (!pPortal) return false;
        auto* pInteractable = pPortal->GetComponent<InteractableComponent>();
        if (!pInteractable || !pInteractable->IsActive()) return false;
        return pInteractable->IsPlayerInRange(m_pPlayerGameObject);
    };

    if (checkPortal(m_pCurrentRoom->GetPortalCube()))   return true;
    if (checkPortal(m_pCurrentRoom->GetSecondPortal())) return true;
    return false;
}

void Scene::TriggerPortalInteraction()
{
    if (!m_pCurrentRoom || !m_pPlayerGameObject)
        return;

    auto tryInteract = [this](GameObject* pPortal) -> bool {
        if (!pPortal) return false;
        auto* pInteractable = pPortal->GetComponent<InteractableComponent>();
        if (!pInteractable || !pInteractable->IsActive()) return false;
        if (!pInteractable->IsPlayerInRange(m_pPlayerGameObject)) return false;
        pInteractable->Interact();
        return true;
    };

    // 보조(최종 보스) 포탈을 먼저 시도 — 두 포탈이 겹치면 의도적으로 분기 우선
    if (tryInteract(m_pCurrentRoom->GetSecondPortal())) return;
    tryInteract(m_pCurrentRoom->GetPortalCube());
}

void Scene::ReAddRenderComponentsToShader(GameObject* pGO)
{
    if (!pGO) return;
    auto* pRC = pGO->GetComponent<RenderComponent>();
    if (pRC) m_vShaders[0]->AddRenderComponent(pRC);
    ReAddRenderComponentsToShader(pGO->m_pChild);
    ReAddRenderComponentsToShader(pGO->m_pSibling);
}

void Scene::TransitionToNextRoom()
{
    OutputDebugString(L"[Scene] Transitioning to next room...\n");
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드
    CleanupWindAmbient();   // 다음 방으로 가기 전 ambient 정리 (다음 방에서 다시 spawn)

    // 보스방에서 포탈 탄 경우 → 다음 스테이지로 (방 카운트 증가 X)
    if (m_bInBossRoom)
    {
        m_bInBossRoom = false;
        switch (m_eCurrentTheme)
        {
        case StageTheme::Fire:  TransitionToWaterStage();  return;
        case StageTheme::Water: TransitionToEarthStage();  return;
        case StageTheme::Earth: TransitionToGrassStage();  return;
        case StageTheme::Grass:
            // 풀 보스 클리어 후 메인 포탈 = 파밍 루프. 1스테이지(Fire) 부터 다시 시작.
            m_nCycleCount++;
            m_bBranchPortalsSpawned = false;
            {
                wchar_t buf[96];
                swprintf_s(buf, L"[Scene] Farm loop → cycle %d (back to Fire stage)\n", m_nCycleCount);
                OutputDebugString(buf);
            }
            TransitionToFireStage();
            return;
        default:
            return;
        }
    }

    m_nRoomCount++;

    // 5방 클리어 후 현재 테마의 보스방 진입
    if (m_nRoomCount >= 5)
    {
        OutputDebugString(L"[Scene] 5 rooms cleared - entering boss room!\n");
        switch (m_eCurrentTheme)
        {
        case StageTheme::Water: TransitionToWaterBossRoom();  return;
        case StageTheme::Earth: TransitionToEarthBossRoom();  return;
        case StageTheme::Grass: TransitionToGrassBossRoom();  return;
        default:                TransitionToBossRoom();       return;  // Fire (Dragon)
        }
    }

    if (m_vMapPool.empty())
    {
        OutputDebugString(L"[Scene] Map pool is empty – cannot transition\n");
        return;
    }

    // ── 1. 셰이더 RC 목록 전체 클리어 (룸 오브젝트 RC가 댕글링 포인터가 되기 전에)
    m_vShaders[0]->ClearRenderComponents();

    // ── 1b. 이번 프레임에 처리 대기 중인 삭제 요청을 미리 처리
    //        (적 사망 등이 방 파기와 같은 프레임에 발생하면, m_vRooms.clear() 이후
    //         ProcessPendingDeletions 에서 해제된 메모리에 접근 → 새 타일 오브젝트를
    //         잘못 삭제하는 버그 방지)
    ProcessPendingDeletions();

    // ── 2. 기존 룸 전체 파기 (룸 오브젝트, 적, 맵 메시 등)
    m_vRooms.clear();
    m_pCurrentRoom = nullptr;

    // ── 2b. 디스크립터 인덱스를 워터마크로 리셋 (맵 슬롯 재활용)
    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    // 이전 스테이지의 CBV 리소스 재사용 캐시 클리어 — 스테이지별 슬롯 타입 패턴이
    // 달라 SRV가 CBV 슬롯을 덮어쓰는 충돌 방지. 뷰는 항상 새로 생성한다.
    m_vCBCache.clear();

    // ── 2c. 횃불 시스템 클리어 (새 맵에서 다시 배치)
    if (m_pTorchSystem) m_pTorchSystem->Clear();

    // ── 3. 인터랙션 큐브 숨기기 (다음 맵에서 다시 보여야 하는 시작 큐브)
    if (m_pInteractionCube)
    {
        auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pInteractable) pInteractable->SetActive(true);
        m_bInteractionCubeActive = true;
    }

    // ── 4. 영속 오브젝트의 RC를 셰이더에 다시 등록
    //       (플레이어 계층 전체, 인터랙션 큐브)
    for (auto& pGO : m_vGameObjects)
        ReAddRenderComponentsToShader(pGO.get());

    // ── 5. 풀에서 랜덤 맵 선택 (현재 맵과 다른 것 우선)
    std::string nextMap = m_strCurrentMap;
    if (m_vMapPool.size() > 1)
    {
        // 현재 맵이 아닌 것 중에서 랜덤 선택
        std::vector<std::string> candidates;
        for (const auto& path : m_vMapPool)
            if (path != m_strCurrentMap) candidates.push_back(path);
        nextMap = candidates[rand() % candidates.size()];
    }
    // (풀에 맵이 1개뿐이면 같은 맵 재로드)
    m_strCurrentMap = nextMap;

    // ── 6. 새 맵 로드
    ID3D12Device*               pDevice      = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList*  pCommandList = Dx12App::GetInstance()->GetCommandList();

    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());

    if (!bLoaded)
    {
        OutputDebugString(L"[Scene] Map load failed during transition – keeping current state\n");
        return;
    }

    // ── 6b. 사막 스테이지 데코 산포 — 방 이동 후에도 새 방에 prop 배치 유지
    if (m_eCurrentTheme == StageTheme::Earth)
    {
        MapLoader::ScatterPropsOnFloorTiles(
            m_strCurrentMap.c_str(),
            "Assets/MapData/desert_props.json",
            this, pDevice, pCommandList, m_vShaders[0].get());
    }

    // ── 6c. 풀 스테이지 wind ambient 재설정 — 다음 방에서도 풀/토네이도/잎 보이게.
    //        (CleanupWindAmbient 는 이미 앞에서 호출됨)
    if (m_eCurrentTheme == StageTheme::Grass && m_pCurrentRoom)
    {
        SetupWindAmbient(m_pCurrentRoom->GetBoundingBox());
    }

    // ── 7. 맵 정적 오브젝트 상수 버퍼 초기화 (Inactive 상태에서는 Update가 스킵되므로)
    if (m_pCurrentRoom)
    {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects())
            pGO->Update(0.0f);
    }

    // ── 7b. LavaGeyser Manager 초기화 (화염 맵 전용 기믹)
    if (m_pCurrentRoom && m_eCurrentTheme == StageTheme::Fire)
    {
        UINT nGeyserDescStart = m_nNextDescriptorIndex;
        m_nNextDescriptorIndex += 1;

        m_pCurrentRoom->InitLavaGeyserManager(
            pDevice, pCommandList, m_vShaders[0].get(),
            m_pDescriptorHeap.get(), nGeyserDescStart);

        OutputDebugString(L"[Scene] LavaGeyserManager initialized for new room\n");
    }

    // ── 8. 포탈을 통해 입장하면 즉시 몬스터 스폰 (인터랙션 큐브 단계 없이)
    if (m_pCurrentRoom)
        m_pCurrentRoom->SetState(RoomState::Active);

    // ── 9. 인터랙션 큐브는 이후 맵에서 숨김 (첫 맵 전용)
    if (m_pInteractionCube)
    {
        auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pInteractable) pInteractable->Hide();
        m_bInteractionCubeActive = false;
    }

    // ── 10. 플레이어 intro fly — 다음 방 진입 시에도 첫 방과 동일하게 베이스 안착 처리.
    //         착지점은 MapLoader 가 정한 playerSpawn.y. standCenter+radius 도 첫 방과 동일.
    if (m_pPlayerGameObject)
    {
        XMFLOAT3 playerSpawn = m_pPlayerGameObject->GetTransform()->GetPosition();
        m_pPlayerGameObject->GetTransform()->SetPosition(playerSpawn.x, playerSpawn.y + 22.0f, playerSpawn.z);
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>())
            pPC->StartIntroFly(3.0f, playerSpawn.y,
                               XMFLOAT3(playerSpawn.x, playerSpawn.y, playerSpawn.z), 5.0f);
    }

    wchar_t buffer[128];
    swprintf_s(buffer, L"[Scene] Transitioned to map: %hs (room #%d)\n",
               m_strCurrentMap.c_str(), m_nRoomCount + 1);
    OutputDebugString(buffer);
}

void Scene::TransitionToRoomByIndex(int index)
{
    if (m_vMapPool.empty()) return;
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드

    int poolSize = (int)m_vMapPool.size();
    index = ((index % poolSize) + poolSize) % poolSize; // 안전한 wrapping
    m_nCurrentPoolIndex = index;

    wchar_t dbg[128];
    swprintf_s(dbg, L"[Scene] Dev nav → room index %d: %hs\n", index, m_vMapPool[index].c_str());
    OutputDebugString(dbg);

    // ── 공통 정리 단계 (TransitionToNextRoom과 동일)
    CleanupWindAmbient();   // 이전 방 ambient VFX 정리 (방마다 재스폰)
    m_vShaders[0]->ClearRenderComponents();
    ProcessPendingDeletions();

    m_vRooms.clear();
    m_pCurrentRoom = nullptr;

    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    // 이전 스테이지의 CBV 리소스 재사용 캐시 클리어 — 스테이지별 슬롯 타입 패턴이
    // 달라 SRV가 CBV 슬롯을 덮어쓰는 충돌 방지. 뷰는 항상 새로 생성한다.
    m_vCBCache.clear();

    // 횃불 시스템 클리어
    if (m_pTorchSystem) m_pTorchSystem->Clear();

    if (m_pInteractionCube)
    {
        auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pInteractable) pInteractable->SetActive(true);
        m_bInteractionCubeActive = true;
    }

    for (auto& pGO : m_vGameObjects)
        ReAddRenderComponentsToShader(pGO.get());

    // ── 지정 인덱스 맵 로드
    m_strCurrentMap = m_vMapPool[index];

    ID3D12Device*              pDevice      = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = Dx12App::GetInstance()->GetCommandList();

    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());

    if (!bLoaded)
    {
        OutputDebugString(L"[Scene] TransitionToRoomByIndex: map load failed\n");
        return;
    }

    // 사막 스테이지 데코 산포 — dev nav 로 점프해도 prop 배치 유지
    if (m_eCurrentTheme == StageTheme::Earth)
    {
        MapLoader::ScatterPropsOnFloorTiles(
            m_strCurrentMap.c_str(),
            "Assets/MapData/desert_props.json",
            this, pDevice, pCommandList, m_vShaders[0].get());
    }

    if (m_pCurrentRoom)
    {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects())
            pGO->Update(0.0f);

        if (m_eCurrentTheme == StageTheme::Fire)
        {
            UINT nGeyserDescStart = m_nNextDescriptorIndex;
            m_nNextDescriptorIndex += 1;
            m_pCurrentRoom->InitLavaGeyserManager(
                pDevice, pCommandList, m_vShaders[0].get(),
                m_pDescriptorHeap.get(), nGeyserDescStart);
        }
        else if (m_eCurrentTheme == StageTheme::Earth)
        {
            m_pCurrentRoom->InitRockfallManager(
                pDevice, pCommandList, m_vShaders[0].get());
        }
        else if (m_eCurrentTheme == StageTheme::Grass)
        {
            // 바람 ambient — 새 방의 bounding box 기반 spawn (이전 ambient 는 caller 가 정리)
            SetupWindAmbient(m_pCurrentRoom->GetBoundingBox());
        }

        m_pCurrentRoom->SetState(RoomState::Active);
    }

    if (m_pInteractionCube)
    {
        auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pInteractable) pInteractable->Hide();
        m_bInteractionCubeActive = false;
    }

    if (m_pPlayerGameObject)
    {
        auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
        if (pPC) pPC->ResetGroundY();
    }
}

void Scene::TransitionToBossRoom()
{
    OutputDebugString(L"[Scene] ========== BOSS ROOM ==========\n");
    if (!IsReadyForTransition()) return;
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드

    if (m_pPlayerGameObject)
    {
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>())
            pPC->DisableFallZone();
    }

    // ── 1. 셰이더 RC 목록 전체 클리어
    m_vShaders[0]->ClearRenderComponents();
    ProcessPendingDeletions();

    // ── 2. 기존 룸 전체 파기
    m_vRooms.clear();
    m_pCurrentRoom = nullptr;

    // ── 3. 디스크립터 인덱스를 워터마크로 리셋
    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    // 이전 스테이지의 CBV 리소스 재사용 캐시 클리어 — 스테이지별 슬롯 타입 패턴이
    // 달라 SRV가 CBV 슬롯을 덮어쓰는 충돌 방지. 뷰는 항상 새로 생성한다.
    m_vCBCache.clear();

    // ── 3b. 횃불 시스템 클리어
    if (m_pTorchSystem) m_pTorchSystem->Clear();

    // ── 4. 영속 오브젝트의 RC를 셰이더에 다시 등록
    for (auto& pGO : m_vGameObjects)
        ReAddRenderComponentsToShader(pGO.get());

    // ── 5. 보스 맵 로드 (일반 맵과 동일)
    ID3D12Device*               pDevice      = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList*  pCommandList = Dx12App::GetInstance()->GetCommandList();

    m_strCurrentMap = m_strBossMap;

    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());

    if (!bLoaded)
    {
        OutputDebugString(L"[Scene] Boss map load failed!\n");
        return;
    }

    // ── 6. 맵 정적 오브젝트 상수 버퍼 초기화
    if (m_pCurrentRoom)
    {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects())
            pGO->Update(0.0f);
    }

    // ── 7. LavaGeyser Manager 초기화 (화염 보스전 전용)
    if (m_pCurrentRoom && m_eCurrentTheme == StageTheme::Fire)
    {
        UINT nGeyserDescStart = m_nNextDescriptorIndex;
        m_nNextDescriptorIndex += 1;

        m_pCurrentRoom->InitLavaGeyserManager(
            pDevice, pCommandList, m_vShaders[0].get(),
            m_pDescriptorHeap.get(), nGeyserDescStart);
    }

    // ── 8. 보스(드래곤) 스폰 - 일반 적 스폰 설정 제거
    if (m_pCurrentRoom && m_pEnemySpawner)
    {
        // 일반 적 스폰 설정 제거
        RoomSpawnConfig emptyConfig;
        m_pCurrentRoom->SetSpawnConfig(emptyConfig);

        NetworkManager* pNet = NetworkManager::GetInstance();
        bool bOnline = (pNet && pNet->IsConnected());

        if (bOnline)
        {
            // 온라인 모드에서는 서버가 S_MONSTER_SPAWN으로 보스를 생성함
            // 따라서 클라에서 로컬 Dragon을 직접 생성하면 보스가 2마리 생기므로 스폰하지 않음
            OutputDebugString(L"[Scene] Online mode - skip local Dragon boss spawn\n");

            // 방 상태만 활성화
            m_pCurrentRoom->SetState(RoomState::Active);
        }
        else
        {
            // 오프라인/싱글 모드에서만 클라가 직접 Dragon 보스를 생성
            OutputDebugString(L"[Scene] Offline mode - Spawning Dragon boss\n");

            XMFLOAT3 dragonPos = XMFLOAT3(0.0f, 0.0f, 20.0f);
            if (m_pPlayerGameObject)
            {
                XMFLOAT3 playerPos = m_pPlayerGameObject->GetTransform()->GetPosition();
                dragonPos = XMFLOAT3(playerPos.x, playerPos.y, playerPos.z + 15.0f);
            }

            GameObject* pDragon = m_pEnemySpawner->SpawnEnemy(
                m_pCurrentRoom,
                "Dragon",
                dragonPos,
                m_pPlayerGameObject
            );

            // 보스 인트로 컷씬 시작
            if (pDragon)
            {
                // 보스는 애니 LOD(phase/frustum skip) 면제 — 찍기 등 긴 모션의 끊김 방지
                if (auto* pA = pDragon->GetComponent<AnimationComponent>())
                    pA->SetCullEnabled(false);

                EnemyComponent* pEnemy = pDragon->GetComponent<EnemyComponent>();
                if (pEnemy)
                {
                    pEnemy->StartBossIntro(25.0f);  // 25유닛 위에서 착지

                    // Camera cinematic: wide-angle shot looking up at the sky where dragon appears
                    XMFLOAT3 landPos = dragonPos;
                    landPos.y = 0.0f;
                    if (m_pCamera)
                        m_pCamera->StartCinematic(landPos, 55.0f, 15.0f, 180.0f);
                    m_pDragonIntroEnemy = pEnemy;
                    m_eLastDragonPhase = BossIntroPhase::None;
                }
            }

            // 방 활성화
            m_pCurrentRoom->SetState(RoomState::Active);
        }
    }

    // ── 9. 인터랙션 큐브 숨김
    if (m_pInteractionCube)
    {
        auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pInteractable) pInteractable->Hide();
        m_bInteractionCubeActive = false;
    }

    // ── 10. 플레이어 위치는 MapLoader가 세팅한 playerSpawn 높이를 유지한다.
    // 온라인에서는 NetworkManager::ProcessRoomTransition()이 이 위치를 groundPos로 읽어서
    // Portal Intro Fly를 시작하므로 여기서 y=0으로 강제하면 단상 안으로 파묻힌다.
    if (m_pPlayerGameObject)
    {
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>())
        {
            pPC->DisableFallZone();
            // ResetGroundY는 호출하지 않는다.
            // StartIntroFly가 곧 호출되면서 groundY/standCenter/standRadius를 다시 세팅한다.
        }
    }

    m_bInBossRoom = true;
    OutputDebugString(L"[Scene] Boss room ready - Dragon spawned!\n");
}

void Scene::MarkForDeletion(GameObject* pGameObject)
{
    if (!pGameObject) return;

    // Avoid duplicates
    for (auto* pObj : m_vPendingDeletions)
    {
        if (pObj == pGameObject) return;
    }

    m_vPendingDeletions.push_back(pGameObject);
    OutputDebugString(L"[Scene] GameObject marked for deletion\n");
}

// Helper function to recursively unregister colliders from hierarchy
static void UnregisterCollidersRecursive(GameObject* pObj, CollisionManager* pCollisionMgr)
{
    if (!pObj || !pCollisionMgr) return;

    auto* pCollider = pObj->GetComponent<ColliderComponent>();
    if (pCollider)
    {
        pCollisionMgr->UnregisterCollider(pCollider);
    }

    if (pObj->m_pChild) UnregisterCollidersRecursive(pObj->m_pChild, pCollisionMgr);
    if (pObj->m_pSibling) UnregisterCollidersRecursive(pObj->m_pSibling, pCollisionMgr);
}

// Helper function to collect all child/sibling GameObjects for deletion
static void CollectHierarchyForDeletion(GameObject* pObj, std::vector<GameObject*>& outList)
{
    if (!pObj) return;
    outList.push_back(pObj);
    if (pObj->m_pChild) CollectHierarchyForDeletion(pObj->m_pChild, outList);
    if (pObj->m_pSibling) CollectHierarchyForDeletion(pObj->m_pSibling, outList);
}

void Scene::ProcessPendingDeletions()
{
    if (m_vPendingDeletions.empty()) return;

    // Collect all objects to delete (including children/siblings)
    std::vector<GameObject*> allToDelete;
    for (GameObject* pObj : m_vPendingDeletions)
    {
        if (!pObj) continue;

        // Collect this object and all its children/siblings
        // (MeshLoader creates each Frame as separate GameObject in Room)
        if (pObj->m_pChild) CollectHierarchyForDeletion(pObj->m_pChild, allToDelete);
        // Note: Don't add siblings of root - they are separate entities
        allToDelete.push_back(pObj);
    }

    for (GameObject* pObj : allToDelete)
    {
        if (!pObj) continue;

        // Unregister colliders from this object
        auto* pCollider = pObj->GetComponent<ColliderComponent>();
        if (pCollider && m_pCollisionManager)
        {
            m_pCollisionManager->UnregisterCollider(pCollider);
        }

        // Try to remove from Scene's global objects
        bool bFound = false;
        for (auto it = m_vGameObjects.begin(); it != m_vGameObjects.end(); ++it)
        {
            if (it->get() == pObj)
            {
                m_vGameObjects.erase(it);
                bFound = true;
                OutputDebugString(L"[Scene] Deleted GameObject from Scene\n");
                break;
            }
        }

        // If not found in Scene, try current room
        if (!bFound && m_pCurrentRoom)
        {
            m_pCurrentRoom->RemoveGameObject(pObj);
        }
    }

    m_vPendingDeletions.clear();
}

void Scene::ClearTransientCombatEffects()
{
    // 1. 투사체 정리
    // ProjectileManager::Clear() 내부에서 projectile trail / extra VFX / sub VFX까지 Stop한다.
    if (m_pProjectileManager)
    {
        m_pProjectileManager->Clear();
    }

    // 2. 비행 보스 탄막 정리
    // m_FlightBossBullets는 Scene이 직접 들고 있으므로 여기서 따로 정리한다.
    if (m_pVFXManager)
    {
        for (auto& bullet : m_FlightBossBullets)
        {
            if (bullet.fluidId >= 0)
            {
                m_pVFXManager->Stop(bullet.fluidId);
                bullet.fluidId = -1;
            }
        }
    }
    m_FlightBossBullets.clear();

    // 3. 통합 VFX 슬롯 전체 정리
    // ProjectileManager가 추적하지 않는 LightEmitter burst, 스킬 장판, 보스 발사 이펙트까지 정리한다.
    if (m_pVFXManager)
    {
        m_pVFXManager->ClearAll();
    }

    WriteNetworkLog("[Scene] ClearTransientCombatEffects done");
}

// 모든 플레이어 목록 반환 (로컬 + 원격)
std::vector<GameObject*> Scene::GetAllPlayers() const
{
    std::vector<GameObject*> players;

    // 로컬 플레이어 추가
    if (m_pPlayerGameObject)
    {
        players.push_back(m_pPlayerGameObject);
    }

    // 원격 플레이어 추가 (NetworkManager에서 가져옴)
    // TODO: 멀티플레이어 구현 시 NetworkManager::GetRemotePlayers() 연동
    // NetworkManager* pNetMgr = NetworkManager::GetInstance();
    // if (pNetMgr)
    // {
    //     for (const auto& pair : pNetMgr->GetRemotePlayers())
    //     {
    //         if (pair.second) players.push_back(pair.second);
    //     }
    // }

    return players;
}

// ================================================================
// Terrain 로드
// ================================================================
void Scene::LoadTerrain(const char* configJsonPath, int subdivisionStep)
{
    ID3D12Device*              pDevice      = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = Dx12App::GetInstance()->GetCommandList();

    m_pTerrain = std::make_unique<Terrain>();
    if (!m_pTerrain->Load(pDevice, pCommandList, configJsonPath, subdivisionStep))
    {
        OutputDebugString(L"[Scene] Terrain load failed!\n");
        m_pTerrain.reset();
    }
    else
    {
        wchar_t msg[256];
        swprintf_s(msg, L"[Scene] Terrain loaded: %hs\n", configJsonPath);
        OutputDebugString(msg);
    }
}

// 적에게 모든 플레이어 등록 (어그로 시스템용)
void Scene::RegisterPlayersToEnemy(EnemyComponent* pEnemy)
{
    if (!pEnemy) return;

    std::vector<GameObject*> players = GetAllPlayers();
    pEnemy->RegisterAllPlayers(players);
}

void Scene::TransitionToFireStage(int roomIndex)
{
    OutputDebugString(L"[Scene] ========== FIRE STAGE (cycle restart) ==========\n");
    if (!IsReadyForTransition()) return;
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드

    CleanupWindAmbient();

    // 플레이어 Y 복원 & 비행 모드 해제
    if (m_pPlayerGameObject)
    {
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>())
            pPC->DisableFallZone();
    }
    if (m_pCamera)
    {
        m_pCamera->SetFlightMode(false);
        m_pCamera->SetFovDegrees(m_pCamera->GetBaseFovDeg());
    }
    m_pFlightBossDummy = nullptr;
    m_fFlightFovOffsetCur = 0.0f;
    m_eKrakenStage = KrakenCutsceneStage::None;
    m_bPendingKrakenSpawn = false;

    // 테마 변경 + 방 카운트 리셋
    m_eCurrentTheme = StageTheme::Fire;
    m_nRoomCount = 0;

    // 셰이더 RC / 디스크립터 / 룸 / CB 캐시 리셋
    m_vShaders[0]->ClearRenderComponents();
    ProcessPendingDeletions();
    m_vRooms.clear();
    m_pCurrentRoom = nullptr;
    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    m_vCBCache.clear();
    if (m_pTorchSystem) m_pTorchSystem->Clear();

    // 평면 정렬: 용암 복귀, 물·바위 숨김
    if (m_pLavaPlane)  m_pLavaPlane->GetTransform()->SetPosition(0.0f, -3.5f, -200.0f);
    if (m_pWaterPlane) m_pWaterPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    if (m_pRockPlane)  m_pRockPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);

    // 영속 오브젝트 RC 재등록 (LavaPlane 포함)
    for (auto& pGO : m_vGameObjects)
        ReAddRenderComponentsToShader(pGO.get());

    // 맵 로드 — 풀에서 인덱스 선택
    int safeIdx = roomIndex;
    if (safeIdx < 0) safeIdx = 0;
    if (m_vMapPool.empty())
    {
        OutputDebugString(L"[Scene] Fire stage: map pool empty\n");
        return;
    }
    if (safeIdx >= static_cast<int>(m_vMapPool.size()))
        safeIdx = static_cast<int>(m_vMapPool.size()) - 1;
    m_strCurrentMap = m_vMapPool[safeIdx];

    ID3D12Device* pDevice = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = Dx12App::GetInstance()->GetCommandList();

    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());

    if (!bLoaded)
    {
        OutputDebugString(L"[Scene] Fire stage: map load failed\n");
        return;
    }

    // 맵 정적 오브젝트 상수 버퍼 초기화 + 룸 활성화 (적 스폰)
    if (m_pCurrentRoom)
    {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects())
            pGO->Update(0.0f);
        m_pCurrentRoom->SetState(RoomState::Active);
    }

    // 첫 방용 LavaGeyser 매니저 셋업 (Fire 전용 기믹)
    if (m_pCurrentRoom)
    {
        UINT nGeyserDescStart = m_nNextDescriptorIndex;
        m_nNextDescriptorIndex += 1;
        m_pCurrentRoom->InitLavaGeyserManager(
            pDevice, pCommandList, m_vShaders[0].get(),
            m_pDescriptorHeap.get(), nGeyserDescStart);
    }

    // 인터랙션 큐브 숨김 + groundY 리셋
    if (m_pInteractionCube)
    {
        auto* pI = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pI) pI->Hide();
        m_bInteractionCubeActive = false;
    }
    if (m_pPlayerGameObject)
    {
        auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
        if (pPC) pPC->ResetGroundY();
    }

    // 파도 진폭 0 (불 스테이지에선 물 셰이더 안 씀)
    if (m_pcbMappedPass)
    {
        for (int i = 0; i < 5; i++)
            m_pcbMappedPass->m_Waves[i].m_fAmplitude = 0.0f;
    }

    OutputDebugString(L"[Scene] Fire stage ready!\n");
}

void Scene::TransitionToWaterStage(int roomIndex)
{
    OutputDebugString(L"[Scene] ========== WATER STAGE ==========\n");
    if (!IsReadyForTransition()) return;
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드

    // ── 0. 플레이어 Y 복원 (이전 스테이지에서 리프트 됐을 가능성 방어)
    //    MapLoader가 playerSpawn 정의하면 이후 덮어씀
    if (m_pPlayerGameObject)
    {
        XMFLOAT3 pp = m_pPlayerGameObject->GetTransform()->GetPosition();
        pp.y = 0.0f;
        m_pPlayerGameObject->GetTransform()->SetPosition(pp);
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>()) pPC->DisableFallZone();
    }
    // 크라켄 cutscene 상태도 초기화 (잔여 상승 효과 방지)
    m_eKrakenStage = KrakenCutsceneStage::None;
    m_bPendingKrakenSpawn = false;

    // ── 1. 테마 변경 (조명 색상에 영향)
    m_eCurrentTheme = StageTheme::Water;
    m_nRoomCount = 0;  // 새 스테이지 진입 → 방 카운트 리셋

    // 기존 WaterPlane 정리
    if (m_pWaterPlane)
    {
        MarkForDeletion(m_pWaterPlane);
        m_pWaterPlane = nullptr;
    }

    // ── 2. 셰이더 RC 목록 전체 클리어
    m_vShaders[0]->ClearRenderComponents();
    ProcessPendingDeletions();

    // ── 3. 기존 룸 전체 파기
    m_vRooms.clear();
    m_pCurrentRoom = nullptr;

    // ── 4. 디스크립터 인덱스를 워터마크로 리셋
    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    // 이전 스테이지의 CBV 리소스 재사용 캐시 클리어 — 스테이지별 슬롯 타입 패턴이
    // 달라 SRV가 CBV 슬롯을 덮어쓰는 충돌 방지. 뷰는 항상 새로 생성한다.
    m_vCBCache.clear();

    // ── 5. 횃불 시스템 클리어
    if (m_pTorchSystem) m_pTorchSystem->Clear();

    // ── 6. 용암·바위 평면 숨기기 (렌더링에서 제외)
    if (m_pLavaPlane)
    {
        m_pLavaPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);  // 화면 밖으로 이동
    }
    if (m_pRockPlane) m_pRockPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);

    // ── 7. 영속 오브젝트의 RC를 셰이더에 다시 등록 (용암 제외)
    for (auto& pGO : m_vGameObjects)
    {
        // 용암은 제외
        if (pGO.get() == m_pLavaPlane)
            continue;

        if (pGO.get() == m_pWaterPlane)
            continue;

        if (pGO.get() == m_pRockPlane)
            continue;

        ReAddRenderComponentsToShader(pGO.get());
    }

    ID3D12Device*              pDevice      = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = Dx12App::GetInstance()->GetCommandList();

    // ── 7. 서버 roomIndex 기준으로 물 스테이지 맵 로드
// roomIndex: 0=1번방, 1=2번방, 2=3번방 ...
    int safeRoomIndex = roomIndex;

    if (safeRoomIndex < 0)
        safeRoomIndex = 0;

    if (safeRoomIndex >= static_cast<int>(m_vMapPool.size()))
        safeRoomIndex = static_cast<int>(m_vMapPool.size()) - 1;

    m_strCurrentMap = m_vMapPool[safeRoomIndex];

    char mapLog[256];
    sprintf_s(mapLog, "[Scene] Water stage load roomIndex=%d map=%s",
        safeRoomIndex, m_strCurrentMap.c_str());
    WriteNetworkLog(mapLog);

    WriteNetworkLog("[Scene] WaterStage before LoadIntoScene");
    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());
    WriteNetworkLog("[Scene] WaterStage after LoadIntoScene");

    WriteNetworkLog("[Scene] WaterStage before WaterPlane create");
    // ── 8. 물 바닥 평면 생성 (용암 대신 물)
    {
        CRoom* pTempRoom = m_pCurrentRoom;
        m_pCurrentRoom = nullptr;

        m_pWaterPlane = CreateGameObject(pDevice, pCommandList);
        m_pCurrentRoom = pTempRoom;

        if (m_pWaterPlane)
        {
            // 세분화된 평면 메쉬 (256x256 그리드 = 66049 정점, 정점 변위용)
            GridPlaneMesh* pPlaneMesh = new GridPlaneMesh(pDevice, pCommandList, 1.0f, 1.0f, 256, 256);
            m_pWaterPlane->SetMesh(pPlaneMesh);

            // Y=-4: 탑뷰에서 물 표면 잘 보이는 위치. 셰이더 소프트 캡(2.5)으로 피크 제한.
            // 최대 피크 = -4 + 2.5 = -1.5 (플레이어 발 Y=0 아래로 마진 1.5)
            m_pWaterPlane->GetTransform()->SetPosition(0.0f, -4.0f, -200.0f);
            m_pWaterPlane->GetTransform()->SetScale(2000.0f, 1.0f, 2000.0f);

            m_pWaterPlane->SetWater(true);

            // 물 머티리얼 (파란색, 높은 광택)
            MATERIAL waterMat;
            waterMat.m_cAmbient  = XMFLOAT4(0.1f, 0.15f, 0.25f, 1.0f);
            waterMat.m_cDiffuse  = XMFLOAT4(0.8f, 0.9f, 1.0f, 1.0f);
            waterMat.m_cSpecular = XMFLOAT4(0.95f, 0.95f, 0.95f, 64.0f);
            waterMat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            m_pWaterPlane->SetMaterial(waterMat);

            // 물 텍스쳐 로드 (Base Color)
            m_pWaterPlane->SetTextureName("Assets/Stylize Water Texture/Textures/Vol_36_5_Base_Color.png");
            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
            AllocateDescriptor(&cpuHandle, &gpuHandle);
            m_pWaterPlane->LoadTexture(pDevice, pCommandList, cpuHandle);
            m_pWaterPlane->SetSrvGpuDescriptorHandle(gpuHandle);

            // 물 노말맵 로드
            m_pWaterPlane->SetNormalMapName("Assets/Stylize Water Texture/Textures/Vol_36_5_Normal.png");
            D3D12_CPU_DESCRIPTOR_HANDLE normalCpuHandle;
            D3D12_GPU_DESCRIPTOR_HANDLE normalGpuHandle;
            AllocateDescriptor(&normalCpuHandle, &normalGpuHandle);
            m_pWaterPlane->LoadNormalMap(pDevice, pCommandList, normalCpuHandle);
            m_pWaterPlane->SetNormalMapSrvGpuHandle(normalGpuHandle);

            // 물 높이맵 로드
            m_pWaterPlane->SetHeightMapName("Assets/Stylize Water Texture/Textures/Vol_36_5_Height.png");
            D3D12_CPU_DESCRIPTOR_HANDLE heightCpuHandle;
            D3D12_GPU_DESCRIPTOR_HANDLE heightGpuHandle;
            AllocateDescriptor(&heightCpuHandle, &heightGpuHandle);
            m_pWaterPlane->LoadHeightMap(pDevice, pCommandList, heightCpuHandle);
            m_pWaterPlane->SetHeightMapSrvGpuHandle(heightGpuHandle);

            // 물 AO 맵 로드 (스타일라이즈드 패턴 깊이감)
            m_pWaterPlane->SetAOMapName("Assets/Stylize Water Texture/Textures/Vol_36_5_Ambient_Occlusion.png");
            D3D12_CPU_DESCRIPTOR_HANDLE aoCpuHandle;
            D3D12_GPU_DESCRIPTOR_HANDLE aoGpuHandle;
            AllocateDescriptor(&aoCpuHandle, &aoGpuHandle);
            m_pWaterPlane->LoadAOMap(pDevice, pCommandList, aoCpuHandle);
            m_pWaterPlane->SetAOMapSrvGpuHandle(aoGpuHandle);

            // 물 Roughness 맵 로드 (날카로운 스펙큘러)
            m_pWaterPlane->SetRoughnessMapName("Assets/Stylize Water Texture/Textures/Vol_36_5_Roughness.png");
            D3D12_CPU_DESCRIPTOR_HANDLE roughCpuHandle;
            D3D12_GPU_DESCRIPTOR_HANDLE roughGpuHandle;
            AllocateDescriptor(&roughCpuHandle, &roughGpuHandle);
            m_pWaterPlane->LoadRoughnessMap(pDevice, pCommandList, roughCpuHandle);
            m_pWaterPlane->SetRoughnessMapSrvGpuHandle(roughGpuHandle);

            // 물 Emissive 맵 로드 (빛나는 물결 효과)
            m_pWaterPlane->SetEmissiveTextureName("Assets/Stylize Water Texture/Textures/Vol_36_5_Emissive.png");
            D3D12_CPU_DESCRIPTOR_HANDLE emissiveCpuHandle;
            D3D12_GPU_DESCRIPTOR_HANDLE emissiveGpuHandle;
            AllocateDescriptor(&emissiveCpuHandle, &emissiveGpuHandle);
            m_pWaterPlane->LoadEmissiveTexture(pDevice, pCommandList, emissiveCpuHandle);
            m_pWaterPlane->SetEmissiveSrvGpuDescriptorHandle(emissiveGpuHandle);
            m_pWaterPlane->SetHasEmissiveTexture(true);

            // ── 추가 물 텍스처 로드 (Water_6 + foam4) ──

            // water_normal_01.png (t7)
            {
                std::wstring normalPath2 = L"Assets/Stylize Water Texture/water_normal_01.png";
                std::unique_ptr<uint8_t[]> decodedData;
                D3D12_SUBRESOURCE_DATA subresource;

                HRESULT hr = DirectX::LoadWICTextureFromFile(pDevice, normalPath2.c_str(), m_pd3dWaterNormal2.GetAddressOf(), decodedData, subresource);
                if (SUCCEEDED(hr))
                {
                    UINT64 nBytes = GetRequiredIntermediateSize(m_pd3dWaterNormal2.Get(), 0, 1);
                    m_pd3dWaterNormal2Upload = CreateBufferResource(pDevice, pCommandList, NULL, nBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
                    UpdateSubresources(pCommandList, m_pd3dWaterNormal2.Get(), m_pd3dWaterNormal2Upload.Get(), 0, 0, 1, &subresource);

                    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pd3dWaterNormal2.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    pCommandList->ResourceBarrier(1, &barrier);

                    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
                    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
                    AllocateDescriptor(&cpuHandle, &gpuHandle);

                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.Format = m_pd3dWaterNormal2->GetDesc().Format;
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    pDevice->CreateShaderResourceView(m_pd3dWaterNormal2.Get(), &srvDesc, cpuHandle);

                    m_d3dWaterNormal2GpuHandle = gpuHandle;  // Store GPU handle
                    OutputDebugString(L"[Scene] water_normal_01.png loaded (t7)\n");
                }
                else
                {
                    OutputDebugString(L"[Scene] Failed to load water_normal_01.png\n");
                }
            }

            // water_height_01.png (t8)
            {
                std::wstring heightPath2 = L"Assets/Stylize Water Texture/water_height_01.png";
                std::unique_ptr<uint8_t[]> decodedData;
                D3D12_SUBRESOURCE_DATA subresource;

                HRESULT hr = DirectX::LoadWICTextureFromFile(pDevice, heightPath2.c_str(), m_pd3dWaterHeight2.GetAddressOf(), decodedData, subresource);
                if (SUCCEEDED(hr))
                {
                    UINT64 nBytes = GetRequiredIntermediateSize(m_pd3dWaterHeight2.Get(), 0, 1);
                    m_pd3dWaterHeight2Upload = CreateBufferResource(pDevice, pCommandList, NULL, nBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
                    UpdateSubresources(pCommandList, m_pd3dWaterHeight2.Get(), m_pd3dWaterHeight2Upload.Get(), 0, 0, 1, &subresource);

                    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pd3dWaterHeight2.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    pCommandList->ResourceBarrier(1, &barrier);

                    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
                    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
                    AllocateDescriptor(&cpuHandle, &gpuHandle);

                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.Format = m_pd3dWaterHeight2->GetDesc().Format;
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    pDevice->CreateShaderResourceView(m_pd3dWaterHeight2.Get(), &srvDesc, cpuHandle);

                    m_d3dWaterHeight2GpuHandle = gpuHandle;  // Store GPU handle
                    OutputDebugString(L"[Scene] water_height_01.png loaded (t8)\n");
                }
                else
                {
                    OutputDebugString(L"[Scene] Failed to load water_height_01.png\n");
                }
            }

            // water_normal_02.png (t9) - detail normal layer
            {
                std::wstring foamOpacityPath = L"Assets/Stylize Water Texture/water_normal_02.png";
                std::unique_ptr<uint8_t[]> decodedData;
                D3D12_SUBRESOURCE_DATA subresource;

                HRESULT hr = DirectX::LoadWICTextureFromFile(pDevice, foamOpacityPath.c_str(), m_pd3dFoamOpacity.GetAddressOf(), decodedData, subresource);
                if (SUCCEEDED(hr))
                {
                    UINT64 nBytes = GetRequiredIntermediateSize(m_pd3dFoamOpacity.Get(), 0, 1);
                    m_pd3dFoamOpacityUpload = CreateBufferResource(pDevice, pCommandList, NULL, nBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
                    UpdateSubresources(pCommandList, m_pd3dFoamOpacity.Get(), m_pd3dFoamOpacityUpload.Get(), 0, 0, 1, &subresource);

                    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pd3dFoamOpacity.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    pCommandList->ResourceBarrier(1, &barrier);

                    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
                    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
                    AllocateDescriptor(&cpuHandle, &gpuHandle);

                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.Format = m_pd3dFoamOpacity->GetDesc().Format;
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    pDevice->CreateShaderResourceView(m_pd3dFoamOpacity.Get(), &srvDesc, cpuHandle);

                    m_d3dFoamOpacityGpuHandle = gpuHandle;  // Store GPU handle
                    OutputDebugString(L"[Scene] water_normal_02.png loaded (t9)\n");
                }
                else
                {
                    OutputDebugString(L"[Scene] Failed to load water_normal_02.png\n");
                }
            }

            // water_height_02.png (t10) - detail height layer
            {
                std::wstring foamDiffusePath = L"Assets/Stylize Water Texture/water_height_02.png";
                std::unique_ptr<uint8_t[]> decodedData;
                D3D12_SUBRESOURCE_DATA subresource;

                HRESULT hr = DirectX::LoadWICTextureFromFile(pDevice, foamDiffusePath.c_str(), m_pd3dFoamDiffuse.GetAddressOf(), decodedData, subresource);
                if (SUCCEEDED(hr))
                {
                    UINT64 nBytes = GetRequiredIntermediateSize(m_pd3dFoamDiffuse.Get(), 0, 1);
                    m_pd3dFoamDiffuseUpload = CreateBufferResource(pDevice, pCommandList, NULL, nBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
                    UpdateSubresources(pCommandList, m_pd3dFoamDiffuse.Get(), m_pd3dFoamDiffuseUpload.Get(), 0, 0, 1, &subresource);

                    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pd3dFoamDiffuse.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    pCommandList->ResourceBarrier(1, &barrier);

                    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
                    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
                    AllocateDescriptor(&cpuHandle, &gpuHandle);

                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.Format = m_pd3dFoamDiffuse->GetDesc().Format;
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    pDevice->CreateShaderResourceView(m_pd3dFoamDiffuse.Get(), &srvDesc, cpuHandle);

                    m_d3dFoamDiffuseGpuHandle = gpuHandle;  // Store GPU handle
                    OutputDebugString(L"[Scene] water_height_02.png loaded (t10)\n");
                }
                else
                {
                    OutputDebugString(L"[Scene] Failed to load water_height_02.png\n");
                }
            }

            auto* pRC = m_pWaterPlane->AddComponent<RenderComponent>();
            pRC->SetMesh(pPlaneMesh);
            pRC->SetCastsShadow(false);
            pRC->SetTransparent(true);  // 물 투명 렌더링 활성화
            m_vShaders[0]->AddRenderComponent(pRC);
        }
        OutputDebugString(L"[Scene] Water floor plane placed (with water_normal_01/02 + water_height_01/02)\n");
    }
    WriteNetworkLog("[Scene] WaterStage after WaterPlane create");

    WriteNetworkLog("[Scene] WaterStage before CurrentRoom update");
    // ── 9. 맵 정적 오브젝트 상수 버퍼 초기화
    if (m_pCurrentRoom)
    {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects())
            pGO->Update(0.0f);

        // 룸 활성화 (적 스폰)
        m_pCurrentRoom->SetState(RoomState::Active);
    }
    WriteNetworkLog("[Scene] WaterStage after CurrentRoom update");

    // ── 10. 인터랙션 큐브 숨김
    if (m_pInteractionCube)
    {
        auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pInteractable) pInteractable->Hide();
        m_bInteractionCubeActive = false;
    }

    // ── 11. 플레이어 groundY 리셋
    if (m_pPlayerGameObject)
    {
        auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
        if (pPC) pPC->ResetGroundY();
    }

    // ── 12. Gerstner Waves (균형: 뾰족하지 않으면서 명확한 파도)
    if (m_pcbMappedPass)
    {
        // Wave 1: 메인 파동 (큰 파도)
        m_pcbMappedPass->m_Waves[0].m_fWavelength = 70.0f;
        m_pcbMappedPass->m_Waves[0].m_fAmplitude = 10.0f;      // 6 → 10
        m_pcbMappedPass->m_Waves[0].m_fSteepness = 0.35f;
        m_pcbMappedPass->m_Waves[0].m_fSpeed = 4.5f;
        m_pcbMappedPass->m_Waves[0].m_xmf2Direction = XMFLOAT2(1.0f, 0.3f);
        m_pcbMappedPass->m_Waves[0].m_fFadeSpeed = 0.1f;

        // Wave 2: 부 파동 (교차)
        m_pcbMappedPass->m_Waves[1].m_fWavelength = 45.0f;
        m_pcbMappedPass->m_Waves[1].m_fAmplitude = 6.5f;       // 4 → 6.5
        m_pcbMappedPass->m_Waves[1].m_fSteepness = 0.3f;
        m_pcbMappedPass->m_Waves[1].m_fSpeed = 6.0f;
        m_pcbMappedPass->m_Waves[1].m_xmf2Direction = XMFLOAT2(-0.7f, 0.7f);
        m_pcbMappedPass->m_Waves[1].m_fFadeSpeed = 0.12f;

        // Wave 3: 중간 파동 (복잡도 추가)
        m_pcbMappedPass->m_Waves[2].m_fWavelength = 28.0f;
        m_pcbMappedPass->m_Waves[2].m_fAmplitude = 4.0f;       // 2.5 → 4
        m_pcbMappedPass->m_Waves[2].m_fSteepness = 0.28f;
        m_pcbMappedPass->m_Waves[2].m_fSpeed = 8.0f;
        m_pcbMappedPass->m_Waves[2].m_xmf2Direction = XMFLOAT2(0.6f, -0.8f);
        m_pcbMappedPass->m_Waves[2].m_fFadeSpeed = 0.15f;

        // Wave 4-5: 작은 디테일
        m_pcbMappedPass->m_Waves[3].m_fWavelength = 30.0f;
        m_pcbMappedPass->m_Waves[3].m_fAmplitude = 1.8f;       // 1.0 → 1.8
        m_pcbMappedPass->m_Waves[3].m_fSteepness = 0.2f;
        m_pcbMappedPass->m_Waves[3].m_fSpeed = 9.0f;
        m_pcbMappedPass->m_Waves[3].m_xmf2Direction = XMFLOAT2(0.5f, 0.9f);
        m_pcbMappedPass->m_Waves[3].m_fFadeSpeed = 0.0f;

        m_pcbMappedPass->m_Waves[4].m_fWavelength = 22.0f;
        m_pcbMappedPass->m_Waves[4].m_fAmplitude = 1.0f;       // 0.6 → 1.0
        m_pcbMappedPass->m_Waves[4].m_fSteepness = 0.18f;
        m_pcbMappedPass->m_Waves[4].m_fSpeed = 11.0f;
        m_pcbMappedPass->m_Waves[4].m_xmf2Direction = XMFLOAT2(-0.9f, 0.4f);
        m_pcbMappedPass->m_Waves[4].m_fFadeSpeed = 0.0f;

        OutputDebugString(L"[Scene] Balanced ocean waves (visible + natural)\n");
    }

    // ── 13. 장식용 터레인 로드 (파일이 있으면)
    {
        const char* terrainConfig = "Assets/Terrain/terrain_config.json";
        // 파일 존재 여부 확인 후 로드
        FILE* f = nullptr;
        fopen_s(&f, terrainConfig, "r");
        if (f)
        {
            fclose(f);
            LoadTerrain(terrainConfig, 4);
        }
        else
        {
            OutputDebugString(L"[Scene] terrain_config.json not found – skipping terrain load\n");
        }
    }

    WriteNetworkLog("[Scene] WaterStage ready");
    OutputDebugString(L"[Scene] Water stage ready!\n");
}

void Scene::TransitionToWaterBossRoom()
{
    OutputDebugString(L"[Scene] ========== WATER BOSS ROOM (KRAKEN) ==========\n");
    if (!IsReadyForTransition()) return;
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드

    // 플레이어 Y 복원 (이전 리프트 상태 방어)
    if (m_pPlayerGameObject)
    {
        XMFLOAT3 pp = m_pPlayerGameObject->GetTransform()->GetPosition();
        pp.y = 0.0f;
        m_pPlayerGameObject->GetTransform()->SetPosition(pp);
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>()) pPC->DisableFallZone();
    }

    // ── 1. 테마를 Water로 유지
    m_eCurrentTheme = StageTheme::Water;

    // ── 2. 셰이더 RC 목록 전체 클리어
    m_vShaders[0]->ClearRenderComponents();
    ProcessPendingDeletions();

    // ── 3. 기존 룸 전체 파기
    m_vRooms.clear();
    m_pCurrentRoom = nullptr;

    // ── 4. 디스크립터 인덱스를 워터마크로 리셋
    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    // 이전 스테이지의 CBV 리소스 재사용 캐시 클리어 — 스테이지별 슬롯 타입 패턴이
    // 달라 SRV가 CBV 슬롯을 덮어쓰는 충돌 방지. 뷰는 항상 새로 생성한다.
    m_vCBCache.clear();

    // ── 5. 횃불 시스템 클리어
    if (m_pTorchSystem) m_pTorchSystem->Clear();

    // ── 6. 용암·바위 평면 숨기기
    if (m_pLavaPlane)
        m_pLavaPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    if (m_pRockPlane)
        m_pRockPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);

    // ── 7. 물 평면 제거 (재진입 시 중복 방지)
    GameObject* pOldWaterPlane = m_pWaterPlane;
    m_pWaterPlane = nullptr;

    // ── 8. 영속 오브젝트 RC 재등록 (용암/이전 물 제외)
    for (auto& pGO : m_vGameObjects)
    {
        if (pGO.get() != m_pLavaPlane && pGO.get() != pOldWaterPlane)
            ReAddRenderComponentsToShader(pGO.get());
    }

    // 이전 평면을 m_vGameObjects에서 제거 (고아 RC 방지)
    m_vGameObjects.erase(
        std::remove_if(m_vGameObjects.begin(), m_vGameObjects.end(),
            [pOldWaterPlane](const std::unique_ptr<GameObject>& p) { return p.get() == pOldWaterPlane; }),
        m_vGameObjects.end());

    ID3D12Device*              pDevice      = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = Dx12App::GetInstance()->GetCommandList();

    // ── 9. 보스 공용 맵 로드 — 모든 보스전이 Red Dragon 과 동일한 맵 사용 (rooms.json 의 bossRoom)
    m_strCurrentMap = m_strBossMap;
    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());
    if (!bLoaded)
    {
        OutputDebugString(L"[Scene] Water boss map load failed!\n");
        return;
    }

    // ── 10. 물 바닥 평면 생성 (TransitionToWaterStage와 동일)
    {
        CRoom* pTempRoom = m_pCurrentRoom;
        m_pCurrentRoom = nullptr;

        // FIX: 기존 WaterPlane이 있으면 재사용 (고아 WaterPlane이 GrassStage에서 흰 면으로 보이는 버그 방지)
        bool bCreateNewWater = (m_pWaterPlane == nullptr);
        if (bCreateNewWater)
            m_pWaterPlane = CreateGameObject(pDevice, pCommandList);
        m_pCurrentRoom = pTempRoom;

        if (m_pWaterPlane && bCreateNewWater)
        {
            GridPlaneMesh* pPlaneMesh = new GridPlaneMesh(pDevice, pCommandList, 1.0f, 1.0f, 256, 256);
            m_pWaterPlane->SetMesh(pPlaneMesh);
            // Y=-4: 탑뷰에서 물 표면 잘 보이는 위치. 셰이더 소프트 캡(2.5)으로 피크 제한.
            // 최대 피크 = -4 + 2.5 = -1.5 (플레이어 발 Y=0 아래로 마진 1.5)
            m_pWaterPlane->GetTransform()->SetPosition(0.0f, -4.0f, -200.0f);
            m_pWaterPlane->GetTransform()->SetScale(2000.0f, 1.0f, 2000.0f);
            m_pWaterPlane->SetWater(true);

            MATERIAL waterMat;
            waterMat.m_cAmbient  = XMFLOAT4(0.1f, 0.15f, 0.25f, 1.0f);
            waterMat.m_cDiffuse  = XMFLOAT4(0.8f, 0.9f, 1.0f, 1.0f);
            waterMat.m_cSpecular = XMFLOAT4(0.95f, 0.95f, 0.95f, 64.0f);
            waterMat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
            m_pWaterPlane->SetMaterial(waterMat);

            m_pWaterPlane->SetTextureName("Assets/Stylize Water Texture/Textures/Vol_36_5_Base_Color.png");
            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle; D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
            AllocateDescriptor(&cpuHandle, &gpuHandle);
            m_pWaterPlane->LoadTexture(pDevice, pCommandList, cpuHandle);
            m_pWaterPlane->SetSrvGpuDescriptorHandle(gpuHandle);

            m_pWaterPlane->SetNormalMapName("Assets/Stylize Water Texture/Textures/Vol_36_5_Normal.png");
            D3D12_CPU_DESCRIPTOR_HANDLE normalCpu; D3D12_GPU_DESCRIPTOR_HANDLE normalGpu;
            AllocateDescriptor(&normalCpu, &normalGpu);
            m_pWaterPlane->LoadNormalMap(pDevice, pCommandList, normalCpu);
            m_pWaterPlane->SetNormalMapSrvGpuHandle(normalGpu);

            m_pWaterPlane->SetHeightMapName("Assets/Stylize Water Texture/Textures/Vol_36_5_Height.png");
            D3D12_CPU_DESCRIPTOR_HANDLE heightCpu; D3D12_GPU_DESCRIPTOR_HANDLE heightGpu;
            AllocateDescriptor(&heightCpu, &heightGpu);
            m_pWaterPlane->LoadHeightMap(pDevice, pCommandList, heightCpu);
            m_pWaterPlane->SetHeightMapSrvGpuHandle(heightGpu);

            auto* pRC = m_pWaterPlane->AddComponent<RenderComponent>();
            pRC->SetMesh(pPlaneMesh);
            pRC->SetCastsShadow(false);
            pRC->SetTransparent(true);
            m_vShaders[0]->AddRenderComponent(pRC);
        }
        OutputDebugString(L"[Scene] Water boss room floor placed\n");
    }

    // ── 11. 맵 정적 오브젝트 상수 버퍼 초기화
    if (m_pCurrentRoom)
    {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects())
            pGO->Update(0.0f);
    }

    // ── 12. 2페이즈 보스 스폰: Phase 1 = Blue Dragon, Phase 2 = Kraken (pre-loaded hidden)
    m_pPreloadedKraken = nullptr;
    m_bPendingKrakenSpawn = false;

    if (m_pCurrentRoom && m_pEnemySpawner)
    {
        RoomSpawnConfig emptyConfig;
        m_pCurrentRoom->SetSpawnConfig(emptyConfig);

        NetworkManager* pNet = NetworkManager::GetInstance();
        bool bOnline = (pNet && pNet->IsConnected());

        if (bOnline)
        {
            // 온라인 모드에서는 서버가 S_MONSTER_SPAWN으로 Kraken 보스를 생성함
            // 클라가 BlueDragon/Kraken 을 직접 스폰하면 보스가 중복 생성되므로 스킵
            OutputDebugString(L"[Scene] Online mode - skip local Water boss spawn (BlueDragon/Kraken)\n");
            m_pCurrentRoom->SetState(RoomState::Active);
        }
        else
        {
            // 오프라인/싱글 모드 — 클라가 2페이즈 보스 직접 관리
            // 보스 스폰 위치 = 맵 중앙 (공용 보스 맵 기준)
            const BoundingBox& bossBB = m_pCurrentRoom->GetBoundingBox();
            XMFLOAT3 bossPos = XMFLOAT3(bossBB.Center.x, 0.0f, bossBB.Center.z);

            // Pre-spawn Kraken hidden underground (no target) to avoid mid-combat GPU upload lag
            // Y=-10000: 시야/프러스텀 밖으로 완전히 숨김 (다른 숨긴 오브젝트들과 동일 깊이)
            XMFLOAT3 hidePos = XMFLOAT3(bossPos.x, -10000.0f, bossPos.z);
            GameObject* pKraken = m_pEnemySpawner->SpawnEnemy(m_pCurrentRoom, "Kraken", hidePos, nullptr);
            if (pKraken)
            {
                pKraken->GetTransform()->SetScale(0.05f, 0.05f, 0.05f);
                if (auto* pA = pKraken->GetComponent<AnimationComponent>()) pA->SetCullEnabled(false);
                m_pPreloadedKraken = pKraken->GetComponent<EnemyComponent>();
            }

            OutputDebugString(L"[Scene] Offline mode - Spawning Blue Dragon (Phase 1)\n");
            GameObject* pDragon = m_pEnemySpawner->SpawnEnemy(m_pCurrentRoom, "BlueDragon", bossPos, m_pPlayerGameObject);

            if (pDragon)
            {
                if (auto* pA = pDragon->GetComponent<AnimationComponent>()) pA->SetCullEnabled(false);
                EnemyComponent* pDragonEnemy = pDragon->GetComponent<EnemyComponent>();
                if (pDragonEnemy)
                {
                    pDragonEnemy->StartBossIntro(5.0f);

                    CRoom* pRoom = m_pCurrentRoom;
                    pDragonEnemy->SetOnDeathCallback([this, pRoom](EnemyComponent* pDeadEnemy)
                    {
                        if (pRoom)
                            pRoom->OnEnemyDeath(pDeadEnemy);

                        OutputDebugString(L"[Scene] Blue Dragon defeated - Kraken emerges! (Phase 2)\n");
                        m_xmf3PendingKrakenPos = { 0.0f, 0.0f, 0.0f };
                        if (pDeadEnemy && pDeadEnemy->GetOwner())
                            m_xmf3PendingKrakenPos = pDeadEnemy->GetOwner()->GetTransform()->GetPosition();
                        m_bPendingKrakenSpawn = true;
                    });
                }
            }

            m_pCurrentRoom->SetState(RoomState::Active);
        }
    }

    // ── 13. 인터랙션 큐브 숨김
    if (m_pInteractionCube)
    {
        auto* pInteractable = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pInteractable) pInteractable->Hide();
        m_bInteractionCubeActive = false;
    }

    // ── 14. 플레이어 groundY 리셋
    if (m_pPlayerGameObject)
    {
        auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
        if (pPC) pPC->ResetGroundY();
    }

    // ── 15. Gerstner Waves (보스전용 거친 파도)
    if (m_pcbMappedPass)
    {
        m_pcbMappedPass->m_Waves[0].m_fWavelength = 70.0f;
        m_pcbMappedPass->m_Waves[0].m_fAmplitude  = 12.0f;
        m_pcbMappedPass->m_Waves[0].m_fSteepness  = 0.4f;
        m_pcbMappedPass->m_Waves[0].m_fSpeed      = 5.0f;
        m_pcbMappedPass->m_Waves[0].m_xmf2Direction = XMFLOAT2(1.0f, 0.3f);
        m_pcbMappedPass->m_Waves[0].m_fFadeSpeed  = 0.1f;

        m_pcbMappedPass->m_Waves[1].m_fWavelength = 45.0f;
        m_pcbMappedPass->m_Waves[1].m_fAmplitude  = 8.0f;
        m_pcbMappedPass->m_Waves[1].m_fSteepness  = 0.35f;
        m_pcbMappedPass->m_Waves[1].m_fSpeed      = 7.0f;
        m_pcbMappedPass->m_Waves[1].m_xmf2Direction = XMFLOAT2(-0.7f, 0.7f);
        m_pcbMappedPass->m_Waves[1].m_fFadeSpeed  = 0.12f;

        m_pcbMappedPass->m_Waves[2].m_fWavelength = 28.0f;
        m_pcbMappedPass->m_Waves[2].m_fAmplitude  = 5.0f;
        m_pcbMappedPass->m_Waves[2].m_fSteepness  = 0.3f;
        m_pcbMappedPass->m_Waves[2].m_fSpeed      = 9.0f;
        m_pcbMappedPass->m_Waves[2].m_xmf2Direction = XMFLOAT2(0.6f, -0.8f);
        m_pcbMappedPass->m_Waves[2].m_fFadeSpeed  = 0.15f;

        m_pcbMappedPass->m_Waves[3].m_fWavelength = 30.0f;
        m_pcbMappedPass->m_Waves[3].m_fAmplitude  = 2.5f;
        m_pcbMappedPass->m_Waves[3].m_fSteepness  = 0.25f;
        m_pcbMappedPass->m_Waves[3].m_fSpeed      = 10.0f;
        m_pcbMappedPass->m_Waves[3].m_xmf2Direction = XMFLOAT2(0.5f, 0.9f);
        m_pcbMappedPass->m_Waves[3].m_fFadeSpeed  = 0.0f;

        m_pcbMappedPass->m_Waves[4].m_fWavelength = 22.0f;
        m_pcbMappedPass->m_Waves[4].m_fAmplitude  = 1.5f;
        m_pcbMappedPass->m_Waves[4].m_fSteepness  = 0.2f;
        m_pcbMappedPass->m_Waves[4].m_fSpeed      = 12.0f;
        m_pcbMappedPass->m_Waves[4].m_xmf2Direction = XMFLOAT2(-0.9f, 0.4f);
        m_pcbMappedPass->m_Waves[4].m_fFadeSpeed  = 0.0f;
    }

    m_bInBossRoom = true;
    OutputDebugString(L"[Scene] Water boss room ready - Kraken spawned!\n");
}

void Scene::StartNetworkKrakenCutscene(GameObject* pKrakenObj, uint64 monsterId)
{
    // 1. 서버가 스폰한 Kraken 오브젝트가 없으면 컷신 시작 불가
    if (!pKrakenObj)
        return;

    // 2. 네트워크 Kraken 컷신 대상 저장
    // 온라인 모드에서는 서버 몬스터가 EnemyComponent 없이 GameObject로만 존재하므로
    // 기존 m_pPreloadedKraken 대신 별도 GameObject 포인터를 사용한다.
    m_pNetworkKrakenCutsceneObject = pKrakenObj;
    m_nNetworkKrakenCutsceneMonsterId = monsterId;

    // 3. 컷신 기준 위치 저장
    // 이후 Rumble/Rise/Burst/Jump/Slam 단계에서 기준 좌표로 사용된다.
    m_xmf3PendingKrakenPos = pKrakenObj->GetTransform()->GetPosition();

    // 4. 컷신 상태 초기화
    // Rumble부터 시작하면 기존 Scene::Update()의 Kraken 컷신 상태머신이 이어서 처리한다.
    m_eKrakenStage = KrakenCutsceneStage::Rumble;
    m_fKrakenEmergeTimer = 0.0f;
    m_bSlamShakeTriggered = false;
    m_bKrakenRoarFadedToIdle = false;

    // 5. Kraken을 수면 아래 작은 크기로 배치
    // 컷신 진행 중 점점 커지고 위로 올라오는 연출을 위해 초기 상태를 숨긴다.
    XMFLOAT3 pos = m_xmf3PendingKrakenPos;
    pos.y = -5.0f;
    pKrakenObj->GetTransform()->SetPosition(pos);
    pKrakenObj->GetTransform()->SetScale(0.05f, 0.05f, 0.05f);

    // 온라인 Kraken은 EnemyComponent 없이 서버 몬스터 GameObject로 존재한다.
    // 오프라인 Kraken처럼 등장 순간부터 촉수 Idle 애니가 계속 움직이도록
    // 컷신 시작 시 Idle 계열 클립을 강제로 루프 재생한다.
    if (auto* pAnim = pKrakenObj->GetComponent<AnimationComponent>())
    {
        pAnim->CrossFade("Idle", 0.1f, true, true);
    }

    // 6. 물 Plane 범위 보정
    // 보스룸의 현재 방 바운딩 박스 기준으로 수면 크기와 중심을 맞춘다.
    if (m_pWaterPlane && m_pCurrentRoom)
    {
        const BoundingBox& rb = m_pCurrentRoom->GetBoundingBox();
        constexpr float kSafeMul = 1.6f;

        float scaleX = rb.Extents.x * kSafeMul * 2.0f;
        float scaleZ = rb.Extents.z * kSafeMul * 2.0f;

        m_pWaterPlane->GetTransform()->SetScale(scaleX, 1.0f, scaleZ);

        XMFLOAT3 wp = m_pWaterPlane->GetTransform()->GetPosition();
        wp.x = rb.Center.x;
        wp.z = rb.Center.z;
        m_pWaterPlane->GetTransform()->SetPosition(wp);
    }

    // 7. 카메라 컷신 시작
    // 기존 오프라인 Kraken 등장 컷신과 동일하게 Kraken 등장 위치를 바라보게 한다.
    XMFLOAT3 camFocus = m_xmf3PendingKrakenPos;
    camFocus.y = 0.0f;

    m_pCamera->StartCinematic(
        camFocus,
        45.0f,
        25.0f,
        m_pCamera->IsFreeCam() ? 45.0f : 200.0f
    );

    // 8. 로그 출력
    WriteNetworkLog("[Scene] Network Kraken cutscene: RUMBLE");
}

bool Scene::IsNetworkKrakenCutsceneTarget(uint64 monsterId) const
{
    // WaterRise 단계부터는 조작권이 돌아오고 Kraken 전투가 시작되므로
    // 서버 S_MONSTER_MOVE를 막으면 안 된다.
    // Rumble~Slam까지만 Scene 컷신 상태머신이 Kraken 위치를 직접 제어한다.
    return m_nNetworkKrakenCutsceneMonsterId == monsterId &&
        m_eKrakenStage != KrakenCutsceneStage::None &&
        m_eKrakenStage != KrakenCutsceneStage::WaterRise;
}

void Scene::TransitionToEarthStage(int roomIndex)
{
    OutputDebugString(L"[Scene] ========== EARTH STAGE ==========\n");
    if (!IsReadyForTransition()) return;
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드

    if (m_pPlayerGameObject)
    {
        XMFLOAT3 pp = m_pPlayerGameObject->GetTransform()->GetPosition();
        pp.y = 0.0f;
        m_pPlayerGameObject->GetTransform()->SetPosition(pp);
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>()) pPC->DisableFallZone();
    }

    m_eCurrentTheme = StageTheme::Earth;
    m_bInBossRoom = false;
    m_nRoomCount = 0;  // 새 스테이지 진입 → 방 카운트 리셋

    m_vShaders[0]->ClearRenderComponents();
    ProcessPendingDeletions();
    m_vRooms.clear();
    m_pCurrentRoom = nullptr;
    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    // 이전 스테이지의 CBV 리소스 재사용 캐시 클리어 — 스테이지별 슬롯 타입 패턴이
    // 달라 SRV가 CBV 슬롯을 덮어쓰는 충돌 방지. 뷰는 항상 새로 생성한다.
    m_vCBCache.clear();
    if (m_pTorchSystem) m_pTorchSystem->Clear();

    // 용암·물 바닥 숨기기
    if (m_pLavaPlane)  m_pLavaPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    if (m_pWaterPlane) m_pWaterPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    // 바위 바닥 표시 — Earth 전용
    if (m_pRockPlane)  m_pRockPlane->GetTransform()->SetPosition(0.0f, -3.5f, -200.0f);

    for (auto& pGO : m_vGameObjects)
    {
        if (pGO.get() != m_pLavaPlane && pGO.get() != m_pWaterPlane)
            ReAddRenderComponentsToShader(pGO.get());
    }

    ID3D12Device*              pDevice      = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = Dx12App::GetInstance()->GetCommandList();

    int safeRoomIndex = roomIndex;

    if (safeRoomIndex < 0)
        safeRoomIndex = 0;

    if (safeRoomIndex >= static_cast<int>(m_vMapPool.size()))
        safeRoomIndex = static_cast<int>(m_vMapPool.size()) - 1;

    m_strCurrentMap = m_vMapPool[safeRoomIndex];

    char mapLog[256];
    sprintf_s(mapLog, "[Scene] Earth stage load roomIndex=%d map=%s",
        safeRoomIndex, m_strCurrentMap.c_str());
    WriteNetworkLog(mapLog);

    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());
    if (!bLoaded) { OutputDebugString(L"[Scene] Earth stage map load failed!\n"); return; }

    // 사막 스테이지 전용 데코 프롭(.bin 메쉬)을 walkable floor tile 위에 자동 산포.
    // 방마다 floor tile 위치가 달라도 그 위에서만 샘플링하므로 항상 plyer 이동 가능 영역에 배치됨.
    // scatter config 는 공유 (desert_props.json) — 방별로 다른 layout 은 RNG seed 가 맵 경로 해시여서 자동으로.
    MapLoader::ScatterPropsOnFloorTiles(
        m_strCurrentMap.c_str(),
        "Assets/MapData/desert_props.json",
        this, pDevice, pCommandList, m_vShaders[0].get());

    if (m_pCurrentRoom) {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects()) pGO->Update(0.0f);
        // Earth 전용 기믹 활성화
        m_pCurrentRoom->InitRockfallManager(pDevice, pCommandList, m_vShaders[0].get());
        m_pCurrentRoom->SetState(RoomState::Active);
    }

    if (m_pInteractionCube) {
        auto* pI = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pI) pI->Hide();
        m_bInteractionCubeActive = false;
    }
    if (m_pPlayerGameObject) {
        auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
        if (pPC) pPC->ResetGroundY();
    }

    // Gerstner Wave를 꺼줌 (땅 맵엔 물 없음)
    if (m_pcbMappedPass)
        for (int i = 0; i < 5; i++) m_pcbMappedPass->m_Waves[i].m_fAmplitude = 0.0f;

    OutputDebugString(L"[Scene] Earth stage ready!\n");
}

void Scene::TransitionToGrassStage(int roomIndex)
{
    OutputDebugString(L"[Scene] ========== GRASS STAGE ==========\n");
    if (!IsReadyForTransition()) return;
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드

    if (m_pPlayerGameObject)
    {
        XMFLOAT3 pp = m_pPlayerGameObject->GetTransform()->GetPosition();
        pp.y = 0.0f;
        m_pPlayerGameObject->GetTransform()->SetPosition(pp);
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>()) pPC->DisableFallZone();
    }

    m_eCurrentTheme = StageTheme::Grass;
    m_bInBossRoom = false;
    m_nRoomCount = 0;  // 새 스테이지 진입 → 방 카운트 리셋

    // 테마 sky color + 이전 ambient 정리
    ApplyThemeSkyColor();
    CleanupWindAmbient();

    m_vShaders[0]->ClearRenderComponents();
    ProcessPendingDeletions();
    m_vRooms.clear();
    m_pCurrentRoom = nullptr;
    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    // 이전 스테이지의 CBV 리소스 재사용 캐시 클리어 — 스테이지별 슬롯 타입 패턴이
    // 달라 SRV가 CBV 슬롯을 덮어쓰는 충돌 방지. 뷰는 항상 새로 생성한다.
    m_vCBCache.clear();
    if (m_pTorchSystem) m_pTorchSystem->Clear();

    if (m_pLavaPlane)  m_pLavaPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    if (m_pWaterPlane) m_pWaterPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    if (m_pRockPlane)  m_pRockPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);

    for (auto& pGO : m_vGameObjects)
    {
        if (pGO.get() != m_pLavaPlane && pGO.get() != m_pWaterPlane)
            ReAddRenderComponentsToShader(pGO.get());
    }

    ID3D12Device*              pDevice      = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = Dx12App::GetInstance()->GetCommandList();

    int safeRoomIndex = roomIndex;

    if (safeRoomIndex < 0)
        safeRoomIndex = 0;

    if (safeRoomIndex >= static_cast<int>(m_vMapPool.size()))
        safeRoomIndex = static_cast<int>(m_vMapPool.size()) - 1;

    m_strCurrentMap = m_vMapPool[safeRoomIndex];

    char mapLog[256];
    sprintf_s(mapLog, "[Scene] Grass stage load roomIndex=%d map=%s",
        safeRoomIndex, m_strCurrentMap.c_str());
    WriteNetworkLog(mapLog);

    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());
    if (!bLoaded) { OutputDebugString(L"[Scene] Grass stage map load failed!\n"); return; }

    if (m_pCurrentRoom) {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects()) pGO->Update(0.0f);
        m_pCurrentRoom->SetState(RoomState::Active);
        // 바람 ambient — 모든 grass 방에 즉시 spawn
        SetupWindAmbient(m_pCurrentRoom->GetBoundingBox());
    }

    if (m_pInteractionCube) {
        auto* pI = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pI) pI->Hide();
        m_bInteractionCubeActive = false;
    }
    if (m_pPlayerGameObject) {
        auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
        if (pPC) pPC->ResetGroundY();
    }

    if (m_pcbMappedPass)
        for (int i = 0; i < 5; i++) m_pcbMappedPass->m_Waves[i].m_fAmplitude = 0.0f;

    OutputDebugString(L"[Scene] Grass stage ready!\n");
}

void Scene::TransitionToEarthBossRoom()
{
    OutputDebugString(L"[Scene] ========== EARTH BOSS ROOM (GOLEM) ==========\n");
    if (!IsReadyForTransition()) return;
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드

    if (m_pPlayerGameObject)
    {
        XMFLOAT3 pp = m_pPlayerGameObject->GetTransform()->GetPosition();
        pp.y = 0.0f;
        m_pPlayerGameObject->GetTransform()->SetPosition(pp);
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>()) pPC->DisableFallZone();
    }

    m_eCurrentTheme = StageTheme::Earth;

    m_vShaders[0]->ClearRenderComponents();
    ProcessPendingDeletions();
    m_vRooms.clear();
    m_pCurrentRoom = nullptr;
    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    // 이전 스테이지의 CBV 리소스 재사용 캐시 클리어 — 스테이지별 슬롯 타입 패턴이
    // 달라 SRV가 CBV 슬롯을 덮어쓰는 충돌 방지. 뷰는 항상 새로 생성한다.
    m_vCBCache.clear();
    if (m_pTorchSystem) m_pTorchSystem->Clear();

    if (m_pLavaPlane)  m_pLavaPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    if (m_pWaterPlane) m_pWaterPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    // 바위 바닥 표시 — Earth 보스전
    if (m_pRockPlane)  m_pRockPlane->GetTransform()->SetPosition(0.0f, -3.5f, -200.0f);

    for (auto& pGO : m_vGameObjects)
    {
        if (pGO.get() != m_pLavaPlane && pGO.get() != m_pWaterPlane)
            ReAddRenderComponentsToShader(pGO.get());
    }

    ID3D12Device*              pDevice      = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = Dx12App::GetInstance()->GetCommandList();

    // 보스 공용 맵 — Red Dragon 과 동일 (rooms.json 의 bossRoom)
    m_strCurrentMap = m_strBossMap;
    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());
    if (!bLoaded) { OutputDebugString(L"[Scene] Earth boss map load failed!\n"); return; }

    if (m_pCurrentRoom)
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects()) pGO->Update(0.0f);

    if (m_pCurrentRoom && m_pEnemySpawner)
    {
        RoomSpawnConfig emptyConfig;
        m_pCurrentRoom->SetSpawnConfig(emptyConfig);

        NetworkManager* pNet = NetworkManager::GetInstance();
        bool bOnline = (pNet && pNet->IsConnected());

        if (bOnline)
        {
            // 온라인 모드에서는 서버가 S_MONSTER_SPAWN으로 Golem 보스를 생성함
            OutputDebugString(L"[Scene] Online mode - skip local Earth boss spawn (Golem)\n");
            m_pCurrentRoom->SetState(RoomState::Active);
        }
        else
        {
            OutputDebugString(L"[Scene] Offline mode - Spawning Golem boss at room center\n");
            // 골렘은 고정형 → 방 중앙에 바로 배치 (인트로 비행 없음 — "순간이동" 처럼 보이던 이슈 제거)
            const BoundingBox& roomBB = m_pCurrentRoom->GetBoundingBox();
            XMFLOAT3 golemPos = { roomBB.Center.x, 0.0f, roomBB.Center.z };  // XZ 중앙, Y=0(지면)

            GameObject* pGolem = m_pEnemySpawner->SpawnEnemy(m_pCurrentRoom, "Golem", golemPos, m_pPlayerGameObject);
            if (pGolem)
            {
                if (auto* pA = pGolem->GetComponent<AnimationComponent>()) pA->SetCullEnabled(false);
            }
            // 인트로 호출 제거 — 골렘은 제단에 박혀있는 석상 컨셉이라 내려오는 연출 불필요

            m_pCurrentRoom->SetState(RoomState::Active);
        }
    }

    if (m_pInteractionCube) {
        auto* pI = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pI) pI->Hide();
        m_bInteractionCubeActive = false;
    }
    if (m_pPlayerGameObject) {
        auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
        if (pPC) pPC->ResetGroundY();
    }
    if (m_pcbMappedPass)
        for (int i = 0; i < 5; i++) m_pcbMappedPass->m_Waves[i].m_fAmplitude = 0.0f;

    m_bInBossRoom = true;
    OutputDebugString(L"[Scene] Earth boss room ready - Golem spawned!\n");
}

void Scene::TransitionToGrassBossRoom()
{
    OutputDebugString(L"[Scene] ========== GRASS BOSS ROOM (DEMON) ==========\n");
    if (!IsReadyForTransition()) return;
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드

    // 이전 비행 테스트 상태(F6 등) 초기화
    if (m_pPlayerGameObject)
    {
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>())
            if (pPC->IsFlightMode()) pPC->ExitFlightMode();
    }

    if (m_pCamera)
    {
        m_pCamera->SetFlightMode(false);
        m_pCamera->SetFovDegrees(m_pCamera->GetBaseFovDeg());
    }

    m_pFlightBossDummy = nullptr;
    m_fFlightFovOffsetCur = 0.0f;
    m_fFlightBossHitFlashTimer = 0.0f;
    m_fFlightBossSkillTimer = 0.0f;

    // 잔존 탄환 모두 제거
    if (m_pVFXManager)
    {
        for (auto& b : m_FlightBossBullets)
            if (b.fluidId >= 0) m_pVFXManager->Stop(b.fluidId);
    }
    m_FlightBossBullets.clear();

    if (m_pPlayerGameObject)
    {
        XMFLOAT3 pp = m_pPlayerGameObject->GetTransform()->GetPosition();
        pp.y = 0.0f;
        m_pPlayerGameObject->GetTransform()->SetPosition(pp);

        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>())
            pPC->DisableFallZone();
    }

    m_eKrakenStage = KrakenCutsceneStage::None;
    m_eCurrentTheme = StageTheme::Grass;

    m_vShaders[0]->ClearRenderComponents();
    ProcessPendingDeletions();
    m_vRooms.clear();
    m_pCurrentRoom = nullptr;
    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    m_vCBCache.clear();

    if (m_pTorchSystem) m_pTorchSystem->Clear();

    if (m_pLavaPlane)  m_pLavaPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    if (m_pWaterPlane) m_pWaterPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    if (m_pRockPlane)  m_pRockPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);

    for (auto& pGO : m_vGameObjects)
    {
        if (pGO.get() != m_pLavaPlane && pGO.get() != m_pWaterPlane)
            ReAddRenderComponentsToShader(pGO.get());
    }

    ID3D12Device* pDevice = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = Dx12App::GetInstance()->GetCommandList();

    // 보스 공용 맵 로드
    m_strCurrentMap = m_strBossMap;
    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());

    if (!bLoaded)
    {
        OutputDebugString(L"[Scene] Grass boss map load failed!\n");
        return;
    }

    if (m_pCurrentRoom)
    {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects())
            pGO->Update(0.0f);
    }

    if (m_pCurrentRoom && m_pEnemySpawner)
    {
        RoomSpawnConfig emptyConfig;
        m_pCurrentRoom->SetSpawnConfig(emptyConfig);

        NetworkManager* pNet = NetworkManager::GetInstance();
        bool bOnline = (pNet && pNet->IsConnected());

        // 맵 중앙 기준 BoundingBox
        const BoundingBox& demonBB = m_pCurrentRoom->GetBoundingBox();
        XMFLOAT3 demonPos = XMFLOAT3(demonBB.Center.x, 0.0f, demonBB.Center.z);

        // Grass Boss Room BoundingBox 디버그 로그
        {
            char logBuf[256];
            float minExt = (std::min)(demonBB.Extents.x, demonBB.Extents.z);

            sprintf_s(logBuf,
                "[CLIENT][GrassBossBB] centerX=%.2f centerZ=%.2f minExt=%.2f",
                demonBB.Center.x,
                demonBB.Center.z,
                minExt);

            WriteNetworkLog(logBuf);
        }

        GameObject* pDemon = nullptr;

        if (bOnline)
        {
            // 온라인 모드에서는 서버가 Demon 보스를 생성함
            OutputDebugString(L"[Scene] Online mode - skip local Grass boss spawn (Demon)\n");
        }
        else
        {
            // 오프라인 모드에서는 로컬 Demon 보스를 생성함
            OutputDebugString(L"[Scene] Offline mode - Spawning Demon boss (ground)\n");

            pDemon = m_pEnemySpawner->SpawnEnemy(
                m_pCurrentRoom,
                "Demon",
                demonPos,
                m_pPlayerGameObject
            );

            if (pDemon)
            {
                if (auto* pA = pDemon->GetComponent<AnimationComponent>())
                    pA->SetCullEnabled(false);

                pDemon->GetTransform()->SetRotation(0.0f, 180.0f, 0.0f);
            }
        }

        // ── 기둥 스폰: 온라인/오프라인 공통 ──
        std::vector<GameObject*> vPillars;
        {
            XMFLOAT3 center = { demonBB.Center.x, 0.0f, demonBB.Center.z };
            float minExt = (std::min)(demonBB.Extents.x, demonBB.Extents.z);

            float dDiag = minExt * 0.65f;
            float dCard = minExt * 0.55f;

            XMFLOAT3 positions[6] = {
                { center.x + dDiag * 0.7071f, 0.0f, center.z + dDiag * 0.7071f },
                { center.x - dDiag * 0.7071f, 0.0f, center.z + dDiag * 0.7071f },
                { center.x + dDiag * 0.7071f, 0.0f, center.z - dDiag * 0.7071f },
                { center.x - dDiag * 0.7071f, 0.0f, center.z - dDiag * 0.7071f },
                { center.x + dCard,           0.0f, center.z },
                { center.x - dCard,           0.0f, center.z },
            };

            Mesh* pColumnMesh = MapLoader::LoadMesh(
                "Assets/MapData/meshes/ColumnBig_001.obj",
                pDevice,
                pCommandList
            );

            if (pColumnMesh) pColumnMesh->AddRef();

            const char* pPillarTexPath = "Assets/MapData/meshes/textures/lm_cliff_01_dif.png";

            for (int i = 0; i < 6; ++i)
            {
                GameObject* pPillar = CreateGameObject(pDevice, pCommandList);
                if (!pPillar) continue;

                if (auto* pT = pPillar->GetTransform())
                {
                    pT->SetPosition(positions[i].x, positions[i].y, positions[i].z);
                    pT->SetScale(5.0f, 5.0f, 5.0f);
                }

                if (pColumnMesh) pPillar->SetMesh(pColumnMesh);

                pPillar->SetTextureName(pPillarTexPath);

                D3D12_CPU_DESCRIPTOR_HANDLE pillarCpuH;
                D3D12_GPU_DESCRIPTOR_HANDLE pillarGpuH;
                AllocateDescriptor(&pillarCpuH, &pillarGpuH);

                pPillar->LoadTexture(pDevice, pCommandList, pillarCpuH);
                pPillar->SetSrvGpuDescriptorHandle(pillarGpuH);

                MATERIAL stoneMat;
                stoneMat.m_cAmbient = XMFLOAT4(0.45f, 0.45f, 0.47f, 1.0f);
                stoneMat.m_cDiffuse = XMFLOAT4(1.00f, 1.00f, 1.00f, 1.0f);
                stoneMat.m_cSpecular = XMFLOAT4(0.30f, 0.30f, 0.30f, 16.0f);
                stoneMat.m_cEmissive = XMFLOAT4(0.05f, 0.05f, 0.06f, 1.0f);
                pPillar->SetMaterial(stoneMat);

                auto* pRC = pPillar->AddComponent<RenderComponent>();
                if (pColumnMesh) pRC->SetMesh(pColumnMesh);
                m_vShaders[0]->AddRenderComponent(pRC);

                auto* pCol = pPillar->AddComponent<ColliderComponent>();
                pCol->SetExtents(1.5f, 6.0f, 1.5f);
                pCol->SetCenter(0.0f, 3.0f, 0.0f);
                pCol->SetLayer(CollisionLayer::Wall);
                pCol->SetCollisionMask(
                    CollisionLayer::Player |
                    CollisionLayer::PlayerBullet |
                    CollisionLayer::EnemyBullet
                );

                vPillars.push_back(pPillar);
            }
        }

        // 오프라인 Demon에만 기둥 리스트 전달
        if (pDemon)
        {
            if (auto* pE = pDemon->GetComponent<EnemyComponent>())
                pE->SetEnvironmentObstacles(vPillars);
        }

        // ── 디버그용 영구 tornado VFX ──
        m_xmf3DebugWindPos = XMFLOAT3(demonPos.x + 22.0f, 0.5f, demonPos.z);
        m_fDebugWindVFXTimer = 0.0f;

        if (m_pVFXManager)
        {
            m_nDebugWindVFXId = m_pVFXManager->Spawn(
                "Demon_Tornado",
                m_xmf3DebugWindPos,
                XMFLOAT3(0.0f, 1.0f, 0.0f),
                0u,
                false
            );
        }

        // ── 4스테이지 바람 ambient: 배경 토네이도 + 잎 + 풀 ──
        CleanupWindAmbient();
        SetupWindAmbient(demonBB);

        m_pCurrentRoom->SetState(RoomState::Active);
    }

    if (m_pInteractionCube)
    {
        auto* pI = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pI) pI->Hide();
        m_bInteractionCubeActive = false;
    }

    if (m_pPlayerGameObject)
    {
        auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
        if (pPC) pPC->ResetGroundY();
    }

    if (m_pcbMappedPass)
    {
        for (int i = 0; i < 5; i++)
            m_pcbMappedPass->m_Waves[i].m_fAmplitude = 0.0f;
    }

    m_bInBossRoom = true;
    OutputDebugString(L"[Scene] Grass boss room ready - Demon spawned!\n");
}

// ────────────────────────────────────────────────────────────────────────────
// 최종 보스방 (DarkLord / DarkKnight) — 오프라인 전용, 단일 보스 직행
// 정리/맵 로드 흐름은 TransitionToGrassBossRoom 의 단순화 버전.
// ────────────────────────────────────────────────────────────────────────────
void Scene::TransitionToDarkLordRoom(bool bStartIntro)
{
    OutputDebugString(L"[Scene] ========== DARK LORD ROOM (FINAL BOSS) ==========\n");
    if (!IsReadyForTransition()) return;
    CancelTransientStateBeforeTransition();   // 컷씬/카메라/raw ptr 일괄 정리 — 크래시 가드

    CleanupWindAmbient();

    // 최종 보스방 — 이전 스테이지에서 살아있던 모든 VFX (Demon_Tornado / Wind_Petals / Wind_DustMotes
    //   / Wind_DriftLeaves / 보스 spell ambient 등) 강제 종료. CleanupWindAmbient 는 ambient ID 만
    //   stop 하지만, 보스 스킬 / 일회성 spawn 은 별도 ID로 살아남는다. 결과: Dark 아레나에 입장
    //   직후 회오리·꽃잎 등이 잔존하는 시각 오염 제거.
    if (m_pVFXManager) m_pVFXManager->ClearAll();

    if (m_pPlayerGameObject)
    {
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>())
            if (pPC->IsFlightMode()) pPC->ExitFlightMode();
    }
    if (m_pCamera)
    {
        m_pCamera->SetFlightMode(false);
        m_pCamera->SetFovDegrees(m_pCamera->GetBaseFovDeg());
    }
    m_pFlightBossDummy = nullptr;
    m_fFlightFovOffsetCur = 0.0f;

    if (m_pPlayerGameObject)
    {
        XMFLOAT3 pp = m_pPlayerGameObject->GetTransform()->GetPosition();
        pp.y = 0.0f;
        m_pPlayerGameObject->GetTransform()->SetPosition(pp);
        if (auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>())
            pPC->DisableFallZone();
    }

    m_eKrakenStage = KrakenCutsceneStage::None;
    m_eCurrentTheme = StageTheme::Dark;

    m_vShaders[0]->ClearRenderComponents();
    ProcessPendingDeletions();
    m_vRooms.clear();
    m_pCurrentRoom = nullptr;
    m_nNextDescriptorIndex = m_nPersistentDescriptorEnd;
    m_vCBCache.clear();

    if (m_pTorchSystem) m_pTorchSystem->Clear();

    if (m_pLavaPlane)  m_pLavaPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    if (m_pWaterPlane) m_pWaterPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);
    if (m_pRockPlane)  m_pRockPlane->GetTransform()->SetPosition(0.0f, -10000.0f, 0.0f);

    for (auto& pGO : m_vGameObjects)
    {
        if (pGO.get() != m_pLavaPlane && pGO.get() != m_pWaterPlane && pGO.get() != m_pRockPlane)
            ReAddRenderComponentsToShader(pGO.get());
    }

    ID3D12Device* pDevice = Dx12App::GetInstance()->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = Dx12App::GetInstance()->GetCommandList();

    // 일단 보스 공용 맵 재사용 (전용 맵은 추후 추가)
    m_strCurrentMap = m_strBossMap;
    bool bLoaded = MapLoader::LoadIntoScene(
        m_strCurrentMap.c_str(), this, pDevice, pCommandList, m_vShaders[0].get());

    if (!bLoaded)
    {
        OutputDebugString(L"[Scene] DarkLord map load failed!\n");
        return;
    }

    if (m_pCurrentRoom)
    {
        for (const auto& pGO : m_pCurrentRoom->GetGameObjects())
            pGO->Update(0.0f);
    }

    // ── 보스 spawn 은 컷씬 Phase 4 진입 시점으로 지연.
    //   룸 자체는 Active 로 둬야 CRoom::Update 가 돌며 모든 GameObject 의 transform CB 가 매 프레임
    //   갱신됨 — 안 그러면 Sanctum decals / 슬래시 / 보스 등 cutscene 동안 spawn 된 모든 오브젝트가
    //   stale/zero CB 로 렌더돼 안 보이거나 (0,0,0) 에 박힘.
    //   빈 spawnConfig + 보스 미스폰 상태라 m_bEnemiesSpawned 가 false → CheckClearCondition 트리거 X.
    if (m_pCurrentRoom)
    {
        RoomSpawnConfig emptyConfig;
        m_pCurrentRoom->SetSpawnConfig(emptyConfig);
        m_pCurrentRoom->SetState(RoomState::Active);
    }

    if (m_pInteractionCube)
    {
        auto* pI = m_pInteractionCube->GetComponent<InteractableComponent>();
        if (pI) pI->Hide();
        m_bInteractionCubeActive = false;
    }
    if (m_pPlayerGameObject)
    {
        auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
        if (pPC) pPC->ResetGroundY();
    }

    // 네트워크 모드에서는 방만 먼저 로드한다.
    // 실제 컷신 시작은 서버 S_BOSS_EVENT_INTRO 수신 후 StartNetworkDarkLordIntro()에서 한다.
    if (!bStartIntro)
    {
        m_bInBossRoom = true;
        OutputDebugString(L"[Scene] DarkLord room ready — waiting server BossEvent intro\n");
        return;
    }

    // ── 입장 컷씬 시작 ───────────────────────────────────────────────────────
    //   Sanctum 페이즈로 진입 → vivid 4원소 sigil + 빛 기둥 spawn + 카메라 cinematic orbit.
    //   m_fSanctumBlend = 0.0 (vivid 톤). Phase 3 의 Sever 에서 1.0 로 점프.
    if (m_pCurrentRoom)
    {
        const BoundingBox& bb = m_pCurrentRoom->GetBoundingBox();
        SetupElementalSanctum(bb);

        m_eDarkLordIntroStage = DarkLordIntroStage::Sanctum;
        m_fDarkLordIntroTimer = 0.0f;
        m_fSanctumBlend       = 0.0f;
        m_bIntroSeverShakeTriggered = false;
        m_bIntroBossSpawned         = false;
        m_nIntroAbsorbCount         = 0;
        m_bScreenSplitTriggered     = false;

        // ── 입장 컷씬 동안 플레이어/포탈 hide ────────────────────────────────
        //   플레이어가 hierarchy mesh (parent + children) 일 가능성 — RenderComponent
        //   flag 만으로는 자식 mesh 안 가려짐. Transform 자체를 화면 밖으로 이동해서
        //   확실히 hide. 컷씬 종료 시 원래 위치로 복귀.
        if (m_pPlayerGameObject)
        {
            // 1) RenderComponent flag (root mesh 라도 끔)
            if (auto* pRC = m_pPlayerGameObject->GetComponent<RenderComponent>())
                pRC->SetVisible(false);
            // 2) Hierarchy 자식들도 — m_pChild / m_pSibling 트리 traverse 해서 모두 hide.
            std::function<void(GameObject*)> hideTree = [&](GameObject* pGO) {
                if (!pGO) return;
                if (auto* pRC = pGO->GetComponent<RenderComponent>())
                    pRC->SetVisible(false);
                hideTree(pGO->m_pChild);
                hideTree(pGO->m_pSibling);
            };
            hideTree(m_pPlayerGameObject->m_pChild);
            // 3) Transform Y stash + 멀리 이동 (안전망 — flag 가 일부 경로 못 잡아도)
            XMFLOAT3 origPos = m_pPlayerGameObject->GetTransform()->GetPosition();
            m_xmf3PlayerIntroStashPos = origPos;
            m_bPlayerIntroStashed = true;
            m_pPlayerGameObject->GetTransform()->SetPosition(origPos.x, -3000.0f, origPos.z);
        }
        if (m_pInteractionCube)
        {
            if (auto* pRC = m_pInteractionCube->GetComponent<RenderComponent>())
                pRC->SetVisible(false);
            std::function<void(GameObject*)> hideTree = [&](GameObject* pGO) {
                if (!pGO) return;
                if (auto* pRC = pGO->GetComponent<RenderComponent>())
                    pRC->SetVisible(false);
                hideTree(pGO->m_pChild);
                hideTree(pGO->m_pSibling);
            };
            hideTree(m_pInteractionCube->m_pChild);
        }

        if (m_pCamera)
        {
            // Sanctum 카메라 — 방 중심 약간 위, 거리 70, pitch 28°, yaw 0° (정면).
            XMFLOAT3 camFocus = bb.Center; camFocus.y = 3.0f;
            m_pCamera->StartCinematic(camFocus, 70.0f, 28.0f, 0.0f);
        }
    }

    m_bInBossRoom = true;
    OutputDebugString(L"[Scene] DarkLord room ready — entering intro cutscene (Sanctum phase)\n");
}

// ── 4원소 Sanctum 자동 배치 ─────────────────────────────────────────────────
//   방 BB 의 cardinal 4방 (N/E/S/W) 에 원소 sigil 데칼 + 빛 기둥 VFX spawn.
//   결과: Phase 1 동안 vivid 4원소 성소 분위기. Phase 3~4 에서 흡수 + cleanup.
void Scene::SetupElementalSanctum(const BoundingBox& roomBB)
{
    if (!m_pDecalManager || !m_pVFXManager) return;

    CleanupElementalSanctum();   // 안전 — 직전 spawn 잔존 제거.

    XMFLOAT3 center = roomBB.Center;
    center.y = 0.05f;

    // BB 반경의 60% 위치에 4방향 배치.
    const float r = ((roomBB.Extents.x < roomBB.Extents.z)
                     ? roomBB.Extents.x : roomBB.Extents.z) * 0.60f;
    // ★ 4 원소별 vivid 파티클 컬럼 + 색조명. (이전 흰 톤 Wind_UpdraftSmall 폐기)
    struct Slot { XMFLOAT3 offset; XMFLOAT4 color; ElementType element; const char* pillarFx; };
    Slot slots[4] = {
        { {  0.0f, 0.0f, +r }, { 0.30f, 1.05f, 0.70f, 1.0f }, ElementType::Wind,  "Sanctum_Pillar_Wind"  },   // 북 — 바람
        { { +r,    0.0f, 0.0f}, { 1.20f, 0.45f, 0.15f, 1.0f }, ElementType::Fire,  "Sanctum_Pillar_Fire"  },   // 동 — 불
        { {  0.0f, 0.0f, -r }, { 1.10f, 0.65f, 0.18f, 1.0f }, ElementType::Earth, "Sanctum_Pillar_Earth" },   // 남 — 땅
        { { -r,    0.0f, 0.0f}, { 0.22f, 0.68f, 1.20f, 1.0f }, ElementType::Water, "Sanctum_Pillar_Water" },   // 서 — 물
    };

    const float kAuraSize    = 55.0f;     // zone 한가운데 큰 색 wash
    const float kAuraRotSpd  = 0.15f;
    const float kLifetime    = 1e9f;
    const float kRevealDur   = 1.2f;
    const int   kSigilsPerZone   = 6;     // 4 → 6 (마법진 밀도 ↑)
    const int   kPillarsPerZone  = 10;    // 6 → 10 (컬럼 풍성)
    const float kZoneScatterRadius = 14.0f;

    // 결정론적 PRNG — 매 실행 동일 패턴, 단 cardinal 직선 배치 아니라 organic.
    auto frand = [seed = (uint32_t)0u](float lo, float hi) mutable {
        seed = seed * 1664525u + 1013904223u;
        float u = (float)(seed >> 8) / (float)(1u << 24);
        return lo + (hi - lo) * u;
    };

    for (const Slot& s : slots)
    {
        XMFLOAT3 zoneCenter = { center.x + s.offset.x, center.y, center.z + s.offset.z };

        SanctumElement el{};

        // (1) Zone 큰 aura wash — 강한 색조명. 알파 1.0 + 색감 풀세기.
        XMFLOAT4 auraColor(s.color.x, s.color.y, s.color.z, 1.0f);
        el.auraDecalSlot = m_pDecalManager->Spawn(
            DecalTexture::MagicCircle, zoneCenter, kAuraSize,
            0.0f, kLifetime, auraColor, kAuraRotSpd, kRevealDur * 1.3f);
        // (1b) 코어 강조 decal — zone 정중앙에 작고 강한 색 hotspot (대비 ↑).
        XMFLOAT4 coreColor(s.color.x * 1.20f, s.color.y * 1.20f, s.color.z * 1.20f, 1.0f);
        int coreSlot = m_pDecalManager->Spawn(
            DecalTexture::MagicCircle, zoneCenter, kAuraSize * 0.35f,
            45.0f, kLifetime, coreColor, kAuraRotSpd * 2.5f, kRevealDur * 0.8f);
        if (coreSlot >= 0) el.sigilDecalSlots.push_back(coreSlot);

        // (2) 작은 sigil 들을 zone 안에 흩뿌리기 — 크기·각도·위치 무작위.
        for (int k = 0; k < kSigilsPerZone; ++k)
        {
            float ang = frand(0.0f, 6.2832f);
            float dist = frand(0.2f, 1.0f) * kZoneScatterRadius;
            XMFLOAT3 sp = {
                zoneCenter.x + cosf(ang) * dist,
                zoneCenter.y,
                zoneCenter.z + sinf(ang) * dist
            };
            float size = frand(8.0f, 16.0f);
            float rot  = frand(0.0f, 360.0f);
            float spd  = frand(0.3f, 0.7f);
            int slot = m_pDecalManager->Spawn(
                DecalTexture::MagicCircle, sp, size,
                rot, kLifetime, s.color, spd, kRevealDur);
            if (slot >= 0) el.sigilDecalSlots.push_back(slot);
        }

        // (3) 원소별 파티클 컬럼 — 균일 분포 (sqrt → 원판 균등, 중앙 집중 X).
        //     최소 거리 0.30 보장 → 코어 decal 위에 안 겹침. 외곽까지 풍성히 흩뿌림.
        for (int k = 0; k < kPillarsPerZone; ++k)
        {
            float ang  = frand(0.0f, 6.2832f);
            float u    = frand(0.0f, 1.0f);
            // sqrt 분포 — 면적 균등. 최소 0.30 (코어 비워둠).
            float dist = (0.30f + sqrtf(u) * 0.70f) * kZoneScatterRadius;
            XMFLOAT3 pp = {
                zoneCenter.x + cosf(ang) * dist,
                zoneCenter.y,
                zoneCenter.z + sinf(ang) * dist
            };
            int fxId = m_pVFXManager->Spawn(s.pillarFx, pp,
                                             XMFLOAT3(0.0f, 1.0f, 0.0f), 0u, false);
            if (fxId >= 0) el.pillarVFXIds.push_back(fxId);
        }

        // (4) 진입 임팩트 — zone 중앙에 SigilImpact 1발 (원소색 ring + spark burst).
        m_pVFXManager->Spawn(SlashCue::ImpactEffectName(s.element), zoneCenter,
                              XMFLOAT3(0.0f, 1.0f, 0.0f), 0u, false);

        // (5) Charge aura — zone 중앙에 떠다니는 원소 응축 오라 (추가 vivid).
        m_pVFXManager->Spawn(SlashCue::ChargeAuraEffectName(s.element), zoneCenter,
                              XMFLOAT3(0.0f, 1.0f, 0.0f), 0u, false);

        m_vSanctumElements.push_back(el);
    }
}

void Scene::CleanupElementalSanctum()
{
    for (auto& el : m_vSanctumElements)
    {
        if (m_pDecalManager)
        {
            if (el.auraDecalSlot >= 0) m_pDecalManager->Stop(el.auraDecalSlot);
            for (int slot : el.sigilDecalSlots)
                if (slot >= 0) m_pDecalManager->Stop(slot);
        }
        if (m_pVFXManager)
        {
            for (int id : el.pillarVFXIds)
                if (id >= 0) m_pVFXManager->Stop(id);
        }
    }
    m_vSanctumElements.clear();
}

// ── 스크린 베기 envelope 분해 — Dx12App::RenderText 가 다중 레이어 합성.
//   단일 alpha 대신 각 레이어 (predark / warning / flash / core / void / red glow / afterglow)
//   가 시간 축에서 독립 envelope 을 가짐. 카멘/히어로 타임라인 (~1.2s) 을 0~1 progress 로 정규화.
namespace
{
    static inline float SmoothStep01(float a, float b, float x)
    {
        float u = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return u * u * (3.0f - 2.0f * u);
    }
    // Gaussian 펄스 — center 부근에서 1, 양쪽으로 빠르게 감쇠. width 는 반치폭 근사.
    static inline float Pulse(float t, float center, float width)
    {
        float w = (width > 1e-4f) ? width : 1e-4f;
        float x = (t - center) / w;
        return expf(-x * x * 4.0f);
    }
}

float Scene::GetIntroLetterboxAmt() const
{
    if (m_eDarkLordIntroStage == DarkLordIntroStage::None) return 0.0f;
    // Sever 후반(0.50~1.0) slide in.
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Sever)
    {
        float t = (m_fDarkLordIntroTimer - DLI_T_DREAD) / (DLI_T_SEVER - DLI_T_DREAD);
        return SmoothStep01(0.50f, 1.0f, t);
    }
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Devour) return 1.0f;
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Dominion)
    {
        float t = (m_fDarkLordIntroTimer - DLI_T_DEVOUR) / (DLI_T_DOMINION - DLI_T_DEVOUR);
        return 1.0f - SmoothStep01(0.55f, 1.0f, t);
    }
    return 0.0f;
}

float Scene::GetIntroBossNameAlpha() const
{
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Devour)
    {
        float t = (m_fDarkLordIntroTimer - DLI_T_SEVER) / (DLI_T_DEVOUR - DLI_T_SEVER);
        // Devour 전반부 fade in (0.15→0.40), 후반 hold.
        return SmoothStep01(0.20f, 0.45f, t);
    }
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Dominion)
    {
        float t = (m_fDarkLordIntroTimer - DLI_T_DEVOUR) / (DLI_T_DOMINION - DLI_T_DEVOUR);
        // Dominion 후반 fade out.
        return 1.0f - SmoothStep01(0.50f, 0.95f, t);
    }
    return 0.0f;
}

float Scene::GetIntroBlackoutAlpha() const
{
    // Sever 후반 ~ Devour 중반의 풀스크린 암전 envelope.
    //   타임라인:
    //     Sever t = 0.86 → 1.0  : 0 → 1 (빠른 ramp up, flash 직후)
    //     Devour t = 0.00 → 0.18: 1.0 hold (완전 검정)
    //     Devour t = 0.18 → 0.60: 1.0 → 0.0 (보스 솟구침과 동기화 fade out)
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Sever)
    {
        float t = (m_fDarkLordIntroTimer - DLI_T_DREAD) / (DLI_T_SEVER - DLI_T_DREAD);
        return SmoothStep01(0.86f, 1.0f, t);
    }
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Devour)
    {
        float t = (m_fDarkLordIntroTimer - DLI_T_SEVER) / (DLI_T_DEVOUR - DLI_T_SEVER);
        if (t < 0.18f) return 1.0f;
        // ease-out — 처음엔 빠르게 풀리고 끝에서 부드럽게.
        float fadeT = SmoothStep01(0.18f, 0.60f, t);
        return 1.0f - fadeT;
    }
    return 0.0f;
}

float Scene::GetIntroEdgeVignetteAmt() const
{
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Sever)
    {
        float t = (m_fDarkLordIntroTimer - DLI_T_DREAD) / (DLI_T_SEVER - DLI_T_DREAD);
        return SmoothStep01(0.55f, 0.85f, t);
    }
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Devour) return 1.0f;
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Dominion)
    {
        float t = (m_fDarkLordIntroTimer - DLI_T_DEVOUR) / (DLI_T_DOMINION - DLI_T_DEVOUR);
        return 1.0f - SmoothStep01(0.55f, 1.0f, t);
    }
    return 0.0f;
}

Scene::IntroSeverState Scene::GetIntroSeverState() const
{
    IntroSeverState s{};
    if (m_eDarkLordIntroStage != DarkLordIntroStage::Sever) return s;
    float t = (m_fDarkLordIntroTimer - DLI_T_DREAD) / (DLI_T_SEVER - DLI_T_DREAD);
    if (t < 0.0f || t > 1.0f) return s;
    s.active   = true;
    s.progress = t;

    // pre-dark — 0.0 ~ 0.45 까지 점점 어두워졌다가 flash 직전에 max,
    //   flash 후엔 0.55~0.80 사이에서 부드럽게 사라짐.
    s.predarkAlpha = SmoothStep01(0.00f, 0.40f, t) * (1.0f - SmoothStep01(0.55f, 0.85f, t)) * 0.55f;

    // warning line — 0.27 ~ 0.40 사이에 얇게 떴다가 flash 직전 사라짐.
    s.warningLineAlpha = SmoothStep01(0.27f, 0.38f, t) * (1.0f - SmoothStep01(0.40f, 0.46f, t));

    // flash — 0.45 부근 매우 짧은 풀스크린 화이트 (~70ms 폭).
    s.flashAlpha = Pulse(t, 0.46f, 0.05f);

    // core (얇은 흰 절단선) — flash 와 함께 등장, recovery 까지 천천히 fade.
    s.coreAlpha = SmoothStep01(0.40f, 0.48f, t) * (1.0f - SmoothStep01(0.70f, 0.95f, t));

    // void gap (검은 균열) — flash 이후 0.50 ~ 0.85 사이 강하게.
    s.voidAlpha = SmoothStep01(0.48f, 0.58f, t) * (1.0f - SmoothStep01(0.80f, 0.95f, t)) * 0.85f;

    // red glow (가장자리 진홍) — flash 직후 가장 강, recovery 까지 잔광 유지.
    s.redGlowAlpha = SmoothStep01(0.45f, 0.55f, t) * (1.0f - SmoothStep01(0.85f, 1.00f, t));

    // afterglow (recovery 단계의 옅은 잔광) — 0.75 이후 부드럽게 fade out.
    s.afterGlowAlpha = SmoothStep01(0.55f, 0.75f, t) * (1.0f - SmoothStep01(0.92f, 1.00f, t)) * 0.50f;

    // strike head — 0.18 ~ 0.48 동안 좌하 → 우상 sweep. flash 전후로 가시.
    s.slashHeadT     = std::clamp((t - 0.18f) / 0.30f, 0.0f, 1.0f);
    s.slashHeadAlpha = SmoothStep01(0.18f, 0.24f, t) * (1.0f - SmoothStep01(0.46f, 0.55f, t));

    return s;
}

// ── 방/스테이지 전환 직전 일괄 정리 ─────────────────────────────────────────
//   모든 TransitionTo* 진입부에서 호출. 이전 방의 컷씬 driver 들이 raw pointer 로
//   곧 파기될 GameObject 를 들고 있는 패턴이 핵심 크래시 원인 — 여기서 한 번에 끊는다.
//   레퍼런스 케이스:
//     1) Red Dragon 입장 중 'B'/'N'/'0'/'9' → m_pDragonIntroEnemy dangling → Update 에서 UAF.
//     2) DarkLord Sanctum/Sever 중 강제 전환 → m_pDarkLordCutsceneObject + m_pCurrentRoom dangling.
//     3) 컷씬 카메라가 StartCinematic 상태로 남아 다음 방 진입 후 stale lookAt 으로 회전.
//     4) Boss grace timer 가 사라진 보스 기준으로 카운트만 흐름.
void Scene::CancelTransientStateBeforeTransition()
{
    // ── Red Dragon (Fire boss) intro ────────────────────────────────────────
    //   m_pDragonIntroEnemy 는 EnemyComponent* raw pointer. 보스가 다음 m_vRooms.clear() 에서
    //   파기되므로 여기서 null 로 끊지 않으면 다음 Scene::Update 의 (1700) IsInIntro() 가 UAF.
    m_pDragonIntroEnemy = nullptr;
    m_eLastDragonPhase  = BossIntroPhase::None;

    // ── DarkLord 입장 컷씬 ─────────────────────────────────────────────────
    //   state machine 을 None 으로 끊고 raw pointer 도 해제. 안 그러면 다음 프레임
    //   UpdateDarkLordIntro() 가 여전히 phase 를 살려둔 채 m_pDarkLordCutsceneObject 또는
    //   m_pCurrentRoom (이미 nullptr) 을 dereference.
    if (m_eDarkLordIntroStage != DarkLordIntroStage::None)
    {
        OutputDebugString(L"[Scene] CancelTransientStateBeforeTransition: aborting DarkLord intro\n");
    }
    m_eDarkLordIntroStage         = DarkLordIntroStage::None;
    m_fDarkLordIntroTimer         = 0.0f;
    m_fSanctumBlend               = 1.0f;   // dark 톤 기본값 복귀
    m_bIntroSeverShakeTriggered   = false;
    m_bIntroBossSpawned           = false;
    m_nIntroAbsorbCount           = 0;
    m_pDarkLordCutsceneObject     = nullptr;
    m_bDarkLordIntroNetworkMode   = false;
    m_nNetworkDarkLordIntroMonsterId = 0;
    m_pIntroSlashOverlay          = nullptr;
    m_pIntroSlashMesh             = nullptr;
    CleanupElementalSanctum();   // sigil / aura / pillar decal+VFX 정리

    // ── Kraken (Water boss) 컷씬 ───────────────────────────────────────────
    m_eKrakenStage              = KrakenCutsceneStage::None;
    m_fKrakenEmergeTimer        = 0.0f;
    m_bSlamShakeTriggered       = false;
    m_bKrakenRoarFadedToIdle    = false;

    // ── 보스 grace period — 보스가 사라지면 카운트도 의미 없음 ────────────
    m_fBossGracePeriodRemain    = 0.0f;

    // ── Drop 상호작용 — 다음 방으로 raw drop pointer 가 새지 않게 ────────
    m_pCurrentDropItem          = nullptr;
    m_eDropState                = DropInteractionState::None;
    m_sSelectedRuneId.clear();
    m_nSelectedRuneOptionIndex  = -1;

    // ── 카메라: cinematic / shake / extra orbit 부스트 즉시 종료 ─────────
    //   StopCinematic 미호출 시 다음 방의 player 추적이 stale lookAt 으로 흔들림.
    if (m_pCamera)
    {
        if (m_pCamera->IsCinematic()) m_pCamera->StopCinematic();
        if (m_pCamera->IsShaking())   m_pCamera->StopShake();
        m_pCamera->SetExtraOrbitDistanceTarget(0.0f);
    }

    // ── flight 모드 보스 추적 — 보스가 사라지면 raw pointer 끊기 ───────
    m_pFlightBossDummy          = nullptr;
}

void Scene::StartNetworkDarkLordIntro(GameObject* pDarkLordObj, uint64 monsterId)
{
    if (!m_pCurrentRoom || !pDarkLordObj)
    {
        OutputDebugString(L"[Scene] StartNetworkDarkLordIntro failed: room or boss is null\n");
        return;
    }

    const BoundingBox& bb = m_pCurrentRoom->GetBoundingBox();

    // 네트워크 컷신 상태 기록
    m_bDarkLordIntroNetworkMode = true;
    m_nNetworkDarkLordIntroMonsterId = monsterId;

    // 기존 DarkLord 인트로 상태머신을 그대로 사용한다.
    SetupElementalSanctum(bb);

    m_eDarkLordIntroStage = DarkLordIntroStage::Sanctum;
    m_fDarkLordIntroTimer = 0.0f;
    m_fSanctumBlend = 0.0f;
    m_bIntroSeverShakeTriggered = false;
    m_bIntroBossSpawned = true; // 서버가 이미 스폰한 보스를 사용하므로 로컬 SpawnEnemy 금지
    m_nIntroAbsorbCount = 0;
    m_bScreenSplitTriggered = false;

    m_pDarkLordCutsceneObject = pDarkLordObj;

    // 서버 스폰 보스를 컷신 시작 상태로 맞춘다.
    if (auto* pT = pDarkLordObj->GetTransform())
    {
        XMFLOAT3 spawnPos(bb.Center.x, -6.0f, bb.Center.z);
        pT->SetPosition(spawnPos);
        pT->SetRotation(0.0f, 180.0f, 0.0f);
        pT->SetScale(1.0f, 1.0f, 1.0f);
    }

    if (auto* pAnim = pDarkLordObj->GetComponent<AnimationComponent>())
    {
        pAnim->SetCullEnabled(false);
        pAnim->CrossFade("fightidle", 0.15f, true);
    }

    // 네트워크 보스는 서버가 AI/판정을 담당하므로 클라 로컬 AI는 계속 정지시킨다.
    if (auto* pEC = pDarkLordObj->GetComponent<EnemyComponent>())
    {
        pEC->SetAIPaused(true);
        pEC->SetInvincible(true);
    }

    // 컷신 동안 플레이어 숨김
    if (m_pPlayerGameObject)
    {
        if (auto* pRC = m_pPlayerGameObject->GetComponent<RenderComponent>())
            pRC->SetVisible(false);

        std::function<void(GameObject*)> hideTree = [&](GameObject* pGO)
            {
                if (!pGO) return;

                if (auto* pRC = pGO->GetComponent<RenderComponent>())
                    pRC->SetVisible(false);

                hideTree(pGO->m_pChild);
                hideTree(pGO->m_pSibling);
            };

        hideTree(m_pPlayerGameObject->m_pChild);

        XMFLOAT3 origPos = m_pPlayerGameObject->GetTransform()->GetPosition();
        m_xmf3PlayerIntroStashPos = origPos;
        m_bPlayerIntroStashed = true;
        m_pPlayerGameObject->GetTransform()->SetPosition(origPos.x, -3000.0f, origPos.z);
    }

    if (m_pInteractionCube)
    {
        if (auto* pRC = m_pInteractionCube->GetComponent<RenderComponent>())
            pRC->SetVisible(false);

        std::function<void(GameObject*)> hideTree = [&](GameObject* pGO)
            {
                if (!pGO) return;

                if (auto* pRC = pGO->GetComponent<RenderComponent>())
                    pRC->SetVisible(false);

                hideTree(pGO->m_pChild);
                hideTree(pGO->m_pSibling);
            };

        hideTree(m_pInteractionCube->m_pChild);
    }

    if (m_pCamera)
    {
        XMFLOAT3 camFocus = bb.Center;
        camFocus.y = 3.0f;
        m_pCamera->StartCinematic(camFocus, 70.0f, 28.0f, 0.0f);
    }

    m_bInBossRoom = true;
    OutputDebugString(L"[Scene] Network DarkLord intro started\n");
}

// ── DarkLord 입장 컷씬 driver — Kraken 패턴의 cumulative-timer state machine.
//   매 프레임 Scene::Update 에서 호출. m_eDarkLordIntroStage 가 None 이면 즉시 return.
void Scene::UpdateDarkLordIntro(float dt)
{
    if (m_eDarkLordIntroStage == DarkLordIntroStage::None) return;

    // 안전망 — Devour/Dominion 진입 후 cutscene 보스가 사라졌거나 방 자체가 날아간 상태면
    //   다음 dereference 가 UAF/NPE. CancelTransientStateBeforeTransition() 이 누락된 경로
    //   (예: 외부 코드가 m_vRooms 를 직접 건드린 경우) 의 안전망.
    bool bNeedBoss = (m_eDarkLordIntroStage == DarkLordIntroStage::Devour
                   || m_eDarkLordIntroStage == DarkLordIntroStage::Dominion);
    if ((bNeedBoss && !m_pDarkLordCutsceneObject) || !m_pCurrentRoom)
    {
        OutputDebugString(L"[Scene] UpdateDarkLordIntro: state stale (boss/room gone) — aborting\n");
        m_eDarkLordIntroStage     = DarkLordIntroStage::None;
        m_fDarkLordIntroTimer     = 0.0f;
        m_pDarkLordCutsceneObject = nullptr;
        if (m_pCamera && m_pCamera->IsCinematic()) m_pCamera->StopCinematic();
        return;
    }

    m_fDarkLordIntroTimer += dt;
    const float T = m_fDarkLordIntroTimer;

    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    auto easeOutCubic = [](float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); };
    auto easeInQuad   = [](float t) { return t * t; };

    GameObject* pBoss = m_pDarkLordCutsceneObject;

    // ── Phase 1: Sanctum (0 ~ DLI_T_SANCTUM) ────────────────────────────────
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Sanctum)
    {
        m_fSanctumBlend = 0.0f;   // 완전 vivid 톤 고정.
        // 카메라 — yaw 0° → +60° 로 슬로 팬, distance 70 → 60 으로 천천히 다가옴.
        float t = T / DLI_T_SANCTUM;
        float e = easeOutCubic(std::clamp(t, 0.0f, 1.0f));
        float dist = lerp(70.0f, 60.0f, e);
        float yaw  = lerp(0.0f, 60.0f, e);
        if (m_pCamera) m_pCamera->SetCinematicOrbit(dist, 28.0f, yaw);

        if (T >= DLI_T_SANCTUM)
        {
            m_eDarkLordIntroStage = DarkLordIntroStage::Dread;
            if (m_pCamera) m_pCamera->StartShake(0.35f, DLI_T_DREAD - DLI_T_SANCTUM);
            OutputDebugString(L"[Scene] DarkLord intro: DREAD\n");
        }
        return;
    }

    // ── Phase 2: Dread (DLI_T_SANCTUM ~ DLI_T_DREAD) ────────────────────────
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Dread)
    {
        float t = (T - DLI_T_SANCTUM) / (DLI_T_DREAD - DLI_T_SANCTUM);
        t = std::clamp(t, 0.0f, 1.0f);
        // sanctum → dim 으로 0.0 → 0.35 까지만 dim. 본격 dark 는 Sever 에서.
        m_fSanctumBlend = lerp(0.0f, 0.35f, easeInQuad(t));
        // 카메라 — 더 zoom in. yaw 60° → 90° 로 회전 (원소 4방 둘러보는 느낌).
        float dist = lerp(60.0f, 52.0f, t);
        float yaw  = lerp(60.0f, 90.0f, t);
        if (m_pCamera) m_pCamera->SetCinematicOrbit(dist, 26.0f, yaw);

        if (T >= DLI_T_DREAD)
        {
            m_eDarkLordIntroStage = DarkLordIntroStage::Sever;
            // ★ Sever 진입 — 강력한 카메라 셰이크 + 슬래시 빌보드 spawn.
            if (m_pCamera) m_pCamera->StartShake(3.8f, 0.45f);
            m_bIntroSeverShakeTriggered = true;

            // ★ 화면 베기는 월드 메쉬 X — Dx12App::RenderText 가 SpriteBatch 로 스크린 공간
            //   대각선 슬래시 오버레이를 그림 (GetIntroSlashScreenAlpha() 로 강도 조회).
            //   카메라 각도와 무관하게 화면 사각형 정확히 대각선 풀스케일 보장.
            //   여기선 카메라 셰이크 + 라이팅 spike 만 처리. m_pIntroSlashOverlay 는 nullptr 유지.

            // 테마 라이팅은 슬래시 flash 도중 빠르게 전환되지만 4원소 sigil/pillar 는
            //   유지 — Devour 페이즈에서 보스가 하나씩 "흡수"하는 연출로 사용됨.
            //   m_fSanctumBlend 는 Sever 동안 0.35→1.0 으로 lerp (아래 phase 처리).

            OutputDebugString(L"[Scene] DarkLord intro: SEVER\n");
        }
        return;
    }

    // ── Phase 3: Sever (DLI_T_DREAD ~ DLI_T_SEVER) ──────────────────────────
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Sever)
    {
        float severT = (T - DLI_T_DREAD) / (DLI_T_SEVER - DLI_T_DREAD);
        severT = std::clamp(severT, 0.0f, 1.0f);
        // 라이팅: 0.35 → 1.0 으로 빠르게 lerp (전반부 0.5 에서 거의 도달).
        m_fSanctumBlend = lerp(0.35f, 1.0f, easeOutCubic(std::clamp(severT * 2.0f, 0.0f, 1.0f)));

        // ★ 화면 분리 후처리 — Sever 후반 (severT >= 0.55) 에서 1회 trigger.
        //   베기 라인이 화면에 자리잡은 후 화면 두 조각이 -45° 수직 방향으로 슬라이드.
        //   duration 1.2s 동안 진행 → Devour 페이즈 일부까지 잔존하다 자연스럽게 종료.
        if (!m_bScreenSplitTriggered && severT >= 0.55f)
        {
            m_bScreenSplitTriggered = true;
            if (auto* pApp = Dx12App::GetInstance())
            {
                if (auto* pSplit = pApp->GetScreenSplit())
                {
                    constexpr float kSlashAngle = -0.7853982f;   // -45° (베기 라인과 동일)
                    pSplit->RequestSplit(1.20f, kSlashAngle, 0.22f);
                }
            }
            HitStopSystem::Get().Request(0.06f);   // 분리 시작 순간 짧은 정지
        }

        // 스크린 슬래시 오버레이는 GetIntroSlashScreenAlpha() 가 매 프레임 계산해서 SpriteBatch
        //   로 그림. 여기선 별도 처리 불필요 — 라이팅 lerp 만 위에서 진행.

        if (T >= DLI_T_SEVER)
        {
            m_eDarkLordIntroStage = DarkLordIntroStage::Devour;

            // ★ 4 원소 sigil + aura + pillar 즉시 일괄 정리 — 블랙아웃 hold 중이라 시각 무관.
            //   사용자 요구: "암전 풀리면 원소들 다 사라져 있어야 함".
            CleanupElementalSanctum();
            m_nIntroAbsorbCount = 4;   // Devour 안에서 absorb tick 추가 처리 차단.

            // ★ DarkLord spawn/reveal
// 오프라인은 여기서 SpawnEnemy("DarkLord")를 생성하고,
// 네트워크는 StartNetworkDarkLordIntro()에서 받은 서버 보스 오브젝트를 그대로 사용한다.
            if (m_pCurrentRoom)
            {
                const BoundingBox& bb = m_pCurrentRoom->GetBoundingBox();

                if (m_pEnemySpawner && !m_bIntroBossSpawned)
                {
                    XMFLOAT3 spawnPos(bb.Center.x, -6.0f, bb.Center.z);
                    GameObject* pDarkLord = m_pEnemySpawner->SpawnEnemy(
                        m_pCurrentRoom, "DarkLord", spawnPos, m_pPlayerGameObject);

                    if (pDarkLord)
                    {
                        m_pDarkLordCutsceneObject = pDarkLord;
                        m_bIntroBossSpawned = true;
                    }
                }

                if (m_pDarkLordCutsceneObject)
                {
                    if (auto* pA = m_pDarkLordCutsceneObject->GetComponent<AnimationComponent>())
                        pA->SetCullEnabled(false);

                    m_pDarkLordCutsceneObject->GetTransform()->SetRotation(0.0f, 180.0f, 0.0f);
                    m_pDarkLordCutsceneObject->GetTransform()->SetScale(1.0f, 1.0f, 1.0f);

                    // 네트워크 보스는 서버가 AI/판정을 담당하므로 로컬 AI는 계속 정지.
                    if (auto* pEC = m_pDarkLordCutsceneObject->GetComponent<EnemyComponent>())
                    {
                        pEC->SetAIPaused(true);
                        pEC->SetInvincible(true);
                    }
                }

                // 카메라 — 보스 등장 ¾ 각도 reveal.
                XMFLOAT3 camFocus = bb.Center;
                camFocus.y = 14.0f;

                if (m_pCamera)
                    m_pCamera->StartCinematic(camFocus, 140.0f, 18.0f, 180.0f);
            }

            OutputDebugString(L"[Scene] DarkLord intro: DEVOUR\n");
        }
        return;
    }

    // ── Phase 4: Devour (DLI_T_SEVER ~ DLI_T_DEVOUR) ────────────────────────
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Devour)
    {
        float t = (T - DLI_T_SEVER) / (DLI_T_DEVOUR - DLI_T_SEVER);
        t = std::clamp(t, 0.0f, 1.0f);
        float e = easeOutCubic(t);

        // 보스 — scale 1.0 → 10.0 (프리셋 기본값) / Y -6 → 0 으로 lerp. 솟구침 dramatic.
        if (pBoss)
        {
            float s = lerp(1.0f, 10.0f, e);
            float y = lerp(-6.0f, 0.0f, e);
            pBoss->GetTransform()->SetScale(s, s, s);
            const BoundingBox& bb = m_pCurrentRoom ? m_pCurrentRoom->GetBoundingBox() : BoundingBox{};
            XMFLOAT3 pos(bb.Center.x, y, bb.Center.z);
            pBoss->GetTransform()->SetPosition(pos);
        }

        // 카메라 — yaw 180 유지, distance 140 → 115 천천히 zoom in. pitch 18 유지.
        float dist = lerp(140.0f, 115.0f, t);
        if (m_pCamera) m_pCamera->SetCinematicOrbit(dist, 18.0f, 180.0f);

        // ── 4 박자 셰이크 — 원소들은 이미 Sever→Devour 전환에서 일괄 정리됨.
        //   여기선 블랙아웃 너머로 "쾅 쾅 쾅 쾅" 전달용 camera shake 만 발사.
        static const float kAbsorbTickFrac[4] = { 0.02f, 0.06f, 0.10f, 0.14f };
        while (m_nIntroAbsorbCount < 4 && t >= kAbsorbTickFrac[m_nIntroAbsorbCount])
        {
            if (m_pCamera) m_pCamera->StartShake(0.9f, 0.18f);
            m_nIntroAbsorbCount++;
        }
        if (T >= DLI_T_DEVOUR)
        {
            m_eDarkLordIntroStage = DarkLordIntroStage::Dominion;
            // ★ 보스 포효 — Unreal Take 또는 Roar 클립.
            if (pBoss)
            {
                if (auto* pAnim = pBoss->GetComponent<AnimationComponent>())
                    pAnim->CrossFade("Unreal Take", 0.15f, false);
            }
            if (m_pCamera) m_pCamera->StartShake(4.0f, 1.0f);   // 셰이크 ↑↑ (게임 최강)

            // ★ 보스 등장 시그니처 — 8방향 × 4원소 + 보스 발치 거대 폭발
            //   거리 ↓ (12→9), Bolt 도 같이 spawn (8방향 호 + 8 발사형), 보스 발치 거대 ring
            if (pBoss && m_pVFXManager)
            {
                TransformComponent* pBT = pBoss->GetTransform();
                XMFLOAT3 bossPos = pBT->GetPosition();
                float baseYaw = XMConvertToRadians(pBT->GetRotation().y);
                const float kDist = 9.0f;   // 12 → 9 (보스 더 가까이 호)
                struct { float angleDeg; ElementType elem; } k8[8] = {
                    {   0.0f, ElementType::Fire  }, {  45.0f, ElementType::Wind  },
                    {  90.0f, ElementType::Water }, { 135.0f, ElementType::Earth },
                    { 180.0f, ElementType::Fire  }, { 225.0f, ElementType::Wind  },
                    { 270.0f, ElementType::Water }, { 315.0f, ElementType::Earth },
                };
                for (auto& s : k8)
                {
                    float yaw = baseYaw + XMConvertToRadians(s.angleDeg);
                    XMFLOAT3 fwd = { sinf(yaw), 0.0f, cosf(yaw) };
                    XMFLOAT3 sp  = { bossPos.x + fwd.x * kDist,
                                     bossPos.y + 5.0f,
                                     bossPos.z + fwd.z * kDist };
                    std::string crescentName = "Boss_CrescentSigil_";
                    std::string boltName     = "Boss_CrescentBolt_";
                    switch (s.elem)
                    {
                    case ElementType::Fire:  crescentName += "Fire";  boltName += "Fire";  break;
                    case ElementType::Water: crescentName += "Water"; boltName += "Water"; break;
                    case ElementType::Wind:  crescentName += "Wind";  boltName += "Wind";  break;
                    case ElementType::Earth: crescentName += "Earth"; boltName += "Earth"; break;
                    default: crescentName += "Fire"; boltName += "Fire"; break;
                    }
                    crescentName += "_Heavy";
                    m_pVFXManager->Spawn(crescentName, sp, fwd, 0u, false);

                    // 발치 ring 충격파
                    XMFLOAT3 ringPos = { sp.x, bossPos.y + 0.2f, sp.z };
                    m_pVFXManager->Spawn(SlashCue::ImpactEffectName(s.elem),
                                          ringPos, fwd, 0u, false);

                    // ★ 추가: 발사형 Bolt — 8 방향 외곽으로 발사 (사방 leakage 시각)
                    XMFLOAT3 boltPos = { bossPos.x + fwd.x * (kDist + 4.0f),
                                         bossPos.y + 5.0f,
                                         bossPos.z + fwd.z * (kDist + 4.0f) };
                    m_pVFXManager->Spawn(boltName, boltPos, fwd, 0u, false);
                }

                // ★ 보스 발치 거대 폭발 — 4 원소 ring 모두 spawn (color stacked)
                XMFLOAT3 footPos = { bossPos.x, bossPos.y + 0.3f, bossPos.z };
                m_pVFXManager->Spawn(SlashCue::ImpactEffectName(ElementType::Fire),  footPos, XMFLOAT3(1,0,0), 0u, false);
                m_pVFXManager->Spawn(SlashCue::ImpactEffectName(ElementType::Water), footPos, XMFLOAT3(0,0,1), 0u, false);
                m_pVFXManager->Spawn(SlashCue::ImpactEffectName(ElementType::Wind),  footPos, XMFLOAT3(-1,0,0), 0u, false);
                m_pVFXManager->Spawn(SlashCue::ImpactEffectName(ElementType::Earth), footPos, XMFLOAT3(0,0,-1), 0u, false);

                // 화이트플래시 풀파워 + 긴 히트스톱
                if (auto* pApp = Dx12App::GetInstance())
                {
                    if (auto* pFlash = pApp->GetWhiteFlash())
                        pFlash->RequestFlash(0.95f, 0.40f);     // 0.80→0.95, 0.28→0.40
                }
                HitStopSystem::Get().Request(0.18f);            // 0.10→0.18 (찰나 길게)
            }

            OutputDebugString(L"[Scene] DarkLord intro: DOMINION (8방향 시그니처 발동)\n");
        }
        return;
    }

    // ── Phase 5: Dominion (DLI_T_DEVOUR ~ DLI_T_DOMINION) → 입력 복구 ─────────
    if (m_eDarkLordIntroStage == DarkLordIntroStage::Dominion)
    {
        // 카메라 — 시그니처 발동 순간 매우 가까이 (보스 + 9m 호 다 보임), 후반 zoom-out.
        //   0~0.6s : dist 55 → 75 (8방향 검기 dominant 하게 보임)
        //   0.6~1.2s : dist 75 → 105 (zoom out, 전투 자세 framing)
        float t = (T - DLI_T_DEVOUR) / (DLI_T_DOMINION - DLI_T_DEVOUR);
        t = std::clamp(t, 0.0f, 1.0f);
        float dist;
        float pitch;
        if (t < 0.5f)
        {
            // 시그니처 발동 직후 — 매우 가까이 framing (게임 최근접)
            float u = t / 0.5f;
            dist  = lerp(55.0f, 75.0f, u);
            pitch = lerp(12.0f, 16.0f, u);
        }
        else
        {
            // 후반 — 점차 zoom out 으로 전투 자세
            float u = (t - 0.5f) / 0.5f;
            dist  = lerp(75.0f, 105.0f, u);
            pitch = lerp(16.0f, 22.0f, u);
        }
        if (m_pCamera) m_pCamera->SetCinematicOrbit(dist, pitch, 180.0f);

        if (T >= DLI_T_DOMINION)
        {
            // 컷씬 종료 — 카메라 해제, 룸 Active, 보스 Idle 전환, 입력 복구.
            m_eDarkLordIntroStage = DarkLordIntroStage::None;
            m_fDarkLordIntroTimer = 0.0f;

            // 플레이어 mesh 복원 — RenderComponent flag + hierarchy 자식 + Transform 복귀.
            //   발판/포탈은 최종 보스에서 불필요 → hidden 유지.
            if (m_pPlayerGameObject)
            {
                if (auto* pRC = m_pPlayerGameObject->GetComponent<RenderComponent>())
                    pRC->SetVisible(true);
                std::function<void(GameObject*)> showTree = [&](GameObject* pGO) {
                    if (!pGO) return;
                    if (auto* pRC = pGO->GetComponent<RenderComponent>())
                        pRC->SetVisible(true);
                    showTree(pGO->m_pChild);
                    showTree(pGO->m_pSibling);
                };
                showTree(m_pPlayerGameObject->m_pChild);
                // Transform 복귀
                if (m_bPlayerIntroStashed)
                {
                    m_pPlayerGameObject->GetTransform()->SetPosition(
                        m_xmf3PlayerIntroStashPos.x, 0.0f, m_xmf3PlayerIntroStashPos.z);
                    m_bPlayerIntroStashed = false;
                }
            }

            if (m_pCamera) m_pCamera->StopCinematic();
            if (m_pCurrentRoom) m_pCurrentRoom->SetState(RoomState::Active);
            if (pBoss)
            {
                if (auto* pAnim = pBoss->GetComponent<AnimationComponent>())
                    pAnim->CrossFade("Idle", 0.25f, true);
                // ★ 보스 AI / 무적 즉시 풀지 않음 — 사용자 요구: "보스 공격 타이밍 늦춰서 억까 X".
                //   m_fBossGracePeriodRemain 으로 추가 3초 동안 정지·무적 유지. 입력은 복구된 상태라
                //   플레이어가 위치 잡고 전투 준비 가능. Scene::Update 가 매 프레임 카운트다운하다
                //   0 도달 시 SetAIPaused(false) + SetInvincible(false) 호출.
                if (m_bDarkLordIntroNetworkMode)
                {
                    // 네트워크 모드에서는 서버가 AI/패턴을 담당한다.
                    // 클라 로컬 EnemyComponent는 끝까지 정지 상태로 둔다.
                    if (auto* pEC = pBoss->GetComponent<EnemyComponent>())
                    {
                        pEC->SetAIPaused(true);
                        pEC->SetInvincible(true);
                    }

                    m_fBossGracePeriodRemain = 0.0f;
                }
                else
                {
                    // 오프라인 모드에서만 로컬 AI grace period 사용
                    m_fBossGracePeriodRemain = DLI_T_BOSS_GRACE_AFTER_CUTSCENE;
                }
            }
            // 흡수 못한 sigil/pillar 가 남아있을 경우 안전 정리 (정상 흐름이면 no-op).
            CleanupElementalSanctum();

            // 네트워크 DarkLord 컷신 종료를 서버에 알린다.
            // 서버는 이 패킷을 받으면 introLockTimer를 풀고 전투를 시작한다.
            if (m_bDarkLordIntroNetworkMode && m_nNetworkDarkLordIntroMonsterId != 0)
            {
                NetworkManager::GetInstance()->SendBossCutsceneEnd(
                    m_nNetworkDarkLordIntroMonsterId,
                    static_cast<uint32>(Protocol::BOSS_EVENT_INTRO),
                    0
                );

                NetworkManager::GetInstance()->SetCutscenePlaying(false);
                WriteNetworkLog("[Network] DarkLord intro cutscene end sent");

                m_bDarkLordIntroNetworkMode = false;
                m_nNetworkDarkLordIntroMonsterId = 0;
            }

            OutputDebugString(L"[Scene] DarkLord intro: COMPLETE — gameplay starts\n");
        }
        return;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// 4스테이지 바람 ambient: 모든 grass 방에 공통 spawn (배경 토네이도/업드래프트/잎)
// ────────────────────────────────────────────────────────────────────────────
void Scene::SetupWindAmbient(const BoundingBox& roomBB)
{
    if (!m_pVFXManager) return;

    XMFLOAT3 center(roomBB.Center.x, 0.0f, roomBB.Center.z);
    XMFLOAT3 up(0.0f, 1.0f, 0.0f);
    float ex = roomBB.Extents.x;
    float ez = roomBB.Extents.z;

    // 배경 거대 토네이도 3 — 방 외곽 너머에서도 보이게 멀리
    XMFLOAT3 bigT[3] = {
        { center.x - ex * 1.6f, 0.5f, center.z + ez * 1.4f },
        { center.x + ex * 1.7f, 0.5f, center.z - ez * 1.0f },
        { center.x + ex * 0.2f, 0.5f, center.z - ez * 1.9f },
    };
    for (auto& p : bigT)
    {
        int id = m_pVFXManager->Spawn("Demon_Tornado_Big", p, up, 0u, false);
        if (id >= 0) m_vAmbientWindIds.push_back(id);
    }

    // (작은 업드래프트 기둥 5종 제거 — 방 전환 시 정리되지 않고 누적되어 시각 노이즈 유발)

    // 잎 드리프트 6 인스턴스 — 맵 전역에 흩뿌려지도록 위치/방향 다양화
    //   각 인스턴스 width 18, length 120 → 좁은 스트림이 휙휙 지나가는 강풍 흐름.
    //   Y 위치 전반 상향 (이전 3~11 → 6~14) + dir.y 살짝 양수 → 바닥에 깔리지 않게.
    struct DriftCfg { XMFLOAT3 pos; XMFLOAT3 dir; };
    DriftCfg drifts[6] = {
        // 좌측에서 우로 (다른 Y 높이)
        { { center.x - ex * 1.3f, 7.0f,  center.z + ez * 0.4f }, { 1.0f, 0.12f, 0.0f } },
        { { center.x - ex * 1.2f, 12.0f, center.z - ez * 0.5f }, { 0.92f, 0.08f, 0.40f } },
        // 후방에서 전방
        { { center.x + ex * 0.5f, 9.0f,  center.z - ez * 1.3f }, { -0.30f, 0.12f, 0.95f } },
        { { center.x - ex * 0.6f, 14.0f, center.z - ez * 1.2f }, { 0.20f, 0.05f, 0.98f } },
        // 대각선 — 우상 → 좌하
        { { center.x + ex * 1.2f, 10.0f, center.z + ez * 1.2f }, { -0.7071f, 0.10f, -0.7071f } },
        // 우하 → 좌상
        { { center.x + ex * 1.1f, 8.0f,  center.z - ez * 0.8f }, { -0.85f, 0.12f, 0.53f } },
    };
    for (auto& d : drifts)
    {
        int id = m_pVFXManager->Spawn("Wind_DriftLeaves", d.pos, d.dir, 0u, false);
        if (id >= 0) m_vAmbientWindIds.push_back(id);
    }

    // ── 공중 dust motes (10 인스턴스): 다양한 높이, 약한 swirl, 느린 드리프트 ──
    //    "공기 자체가 흐른다" 느낌 — 잎보다 가볍고 작음.
    struct DustCfg { XMFLOAT3 pos; XMFLOAT3 dir; };
    DustCfg dusts[10] = {
        { { center.x - ex * 1.2f, 1.5f,  center.z + ez * 0.3f }, { 1.0f, 0.02f, 0.10f } },
        { { center.x - ex * 1.0f, 4.0f,  center.z - ez * 0.6f }, { 0.95f, 0.05f, 0.30f } },
        { { center.x - ex * 1.1f, 7.5f,  center.z + ez * 0.7f }, { 1.0f, 0.0f, -0.20f } },
        { { center.x - ex * 0.9f, 12.0f, center.z + ez * 0.0f }, { 0.92f, 0.05f, 0.40f } },
        { { center.x + ex * 0.3f, 2.5f,  center.z - ez * 1.2f }, { -0.10f, 0.05f, 0.99f } },
        { { center.x - ex * 0.4f, 6.0f,  center.z - ez * 1.3f }, { 0.30f, 0.0f, 0.95f } },
        { { center.x + ex * 1.1f, 5.0f,  center.z + ez * 1.0f }, { -0.7071f, 0.05f, -0.7071f } },
        { { center.x + ex * 1.0f, 9.0f,  center.z - ez * 0.5f }, { -0.85f, 0.0f, 0.53f } },
        { { center.x + ex * 0.6f, 3.5f,  center.z + ez * 1.1f }, { -0.50f, 0.05f, -0.87f } },
        { { center.x - ex * 0.2f, 10.0f, center.z + ez * 0.8f }, { 0.60f, 0.0f, -0.80f } },
    };
    for (auto& d : dusts)
    {
        int id = m_pVFXManager->Spawn("Wind_DustMotes", d.pos, d.dir, 0u, false);
        if (id >= 0) m_vAmbientWindIds.push_back(id);
    }

    // ── 꽃잎 흩날림 (4 인스턴스): 살짝 큰 옅은 핑크 파티클 — 시즈널 풀밭 분위기 ──
    struct PetalCfg { XMFLOAT3 pos; XMFLOAT3 dir; };
    PetalCfg petals[4] = {
        { { center.x - ex * 1.1f, 4.5f,  center.z - ez * 0.2f }, { 1.0f,  0.03f,  0.20f } },
        { { center.x + ex * 0.4f, 6.0f,  center.z - ez * 1.2f }, { 0.0f,  0.03f,  0.99f } },
        { { center.x + ex * 1.1f, 7.5f,  center.z + ez * 0.6f }, { -0.7f, 0.03f, -0.71f } },
        { { center.x - ex * 0.6f, 5.5f,  center.z + ez * 1.0f }, { 0.5f,  0.03f, -0.87f } },
    };
    for (auto& d : petals)
    {
        int id = m_pVFXManager->Spawn("Wind_Petals", d.pos, d.dir, 0u, false);
        if (id >= 0) m_vAmbientWindIds.push_back(id);
    }

    m_fPeriodicTornadoTimer = 0.0f;
    m_nPeriodicTornadoId = -1;
    m_fGustBurstTimer = 0.0f;

    // ── 절차적 풀(grass) 군집: 6~8개, BoundingBox 안에 분산 배치 ──
    // 텍스처 없음(셰이더에서 vertex 그라데이션). 알파 컷아웃 X (taper 형상으로 충분).
    {
        ID3D12Device* pDev = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetDevice() : nullptr;
        ID3D12GraphicsCommandList* pCmd = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetCommandList() : nullptr;
        if (pDev && pCmd && !m_vShaders.empty())
        {
            // 갈대밭 컨셉: floor tile 위에만 군집 배치 — 맵 바깥 ring fill 은 제거.
            // ── 풀 클럼프 배치 전략 ────────────────────────────────────────────
            //   tile 그룹: floor tile 위 (walkable, 플레이어가 지나갈 수 있는 영역)
            const int kTileClumpCount = 14;
            const int kRingClumpCount = 0;   // 맵 바깥 fill 비활성 — 사용자 요청
            const int kClumpCount = kTileClumpCount + kRingClumpCount;
            struct ClumpCfg { int nBlades; float fRadius; };
            ClumpCfg cfgs[kClumpCount] = {
                // === Tile 그룹 — walkable 영역. 반경 ↓ (타일 안에 풀잎 유지)
                { 55, 4.0f }, { 60, 4.2f }, { 50, 3.8f }, { 55, 4.0f },
                { 48, 3.5f }, { 52, 3.8f }, { 45, 3.2f }, { 50, 3.5f },
                { 30, 2.5f }, { 32, 2.5f }, { 28, 2.2f }, { 30, 2.2f },
                { 26, 2.0f }, { 28, 2.0f },
            };

            // ── (1) Tile 그룹 좌표 수집: 맵 JSON 의 floor tile 만, 스폰 근처 제외 ──
            std::vector<XMFLOAT3> tiles = MapLoader::GetFloorTilePositions(m_strCurrentMap.c_str());

            XMFLOAT3 spawnPos(0.0f, 0.0f, 0.0f);
            if (m_pPlayerGameObject)
                spawnPos = m_pPlayerGameObject->GetTransform()->GetPosition();
            std::vector<XMFLOAT3> tileAvail;
            tileAvail.reserve(tiles.size());
            const float kSpawnExc = 8.0f;
            for (const auto& t : tiles) {
                float dx = t.x - spawnPos.x;
                float dz = t.z - spawnPos.z;
                if (dx*dx + dz*dz >= kSpawnExc * kSpawnExc) tileAvail.push_back(t);
            }
            if (tileAvail.empty()) tileAvail = tiles;

            // 외곽 타일 제거 — 풀 군집이 맵 경계 밖으로 뻗어나가지 않게.
            {
                float minX = +1e9f, maxX = -1e9f, minZ = +1e9f, maxZ = -1e9f;
                for (const auto& t : tiles) {
                    if (t.x < minX) minX = t.x;
                    if (t.x > maxX) maxX = t.x;
                    if (t.z < minZ) minZ = t.z;
                    if (t.z > maxZ) maxZ = t.z;
                }
                constexpr float kEdgeMargin = 5.0f;
                std::vector<XMFLOAT3> inner;
                inner.reserve(tileAvail.size());
                for (const auto& t : tileAvail) {
                    if (t.x > minX + kEdgeMargin && t.x < maxX - kEdgeMargin &&
                        t.z > minZ + kEdgeMargin && t.z < maxZ - kEdgeMargin)
                        inner.push_back(t);
                }
                if (!inner.empty()) tileAvail = std::move(inner);
            }

            unsigned int placeSeed = 2166136261u;
            for (char c : m_strCurrentMap) {
                placeSeed ^= (unsigned int)(unsigned char)c;
                placeSeed *= 16777619u;
            }
            std::mt19937 placeRng(placeSeed);
            std::shuffle(tileAvail.begin(), tileAvail.end(), placeRng);

            // ── (2) Ring 좌표 생성: tile bbox 바깥 + roomBB 바깥 ring 에 균등 분포 ──
            //     tile 들의 XZ bbox 계산 → ring 반지름 = max half-extent × 1.45
            float tileMinX = +1e9f, tileMaxX = -1e9f;
            float tileMinZ = +1e9f, tileMaxZ = -1e9f;
            for (const auto& t : tiles) {
                if (t.x < tileMinX) tileMinX = t.x;
                if (t.x > tileMaxX) tileMaxX = t.x;
                if (t.z < tileMinZ) tileMinZ = t.z;
                if (t.z > tileMaxZ) tileMaxZ = t.z;
            }
            float tileCx = (tileMinX + tileMaxX) * 0.5f;
            float tileCz = (tileMinZ + tileMaxZ) * 0.5f;
            float tileHalfX = (tileMaxX - tileMinX) * 0.5f;
            float tileHalfZ = (tileMaxZ - tileMinZ) * 0.5f;
            float ringHalfX = tileHalfX * 1.45f;
            float ringHalfZ = tileHalfZ * 1.45f;
            std::uniform_real_distribution<float> distAngleJit(-0.20f, 0.20f);   // ring 각도 jitter (rad)
            std::uniform_real_distribution<float> distRJit(0.90f, 1.30f);        // ring 반지름 jitter

            // ── (3) 배치 ── tile 그룹 + ring 그룹 합쳐서 진행
            int tileTotal = (tileAvail.size() < (size_t)kTileClumpCount) ? (int)tileAvail.size() : kTileClumpCount;

            for (int i = 0; i < kClumpCount; ++i)
            {
                XMFLOAT3 clumpPos(0.0f, 0.0f, 0.0f);
                bool useTile = (i < kTileClumpCount) && (i < tileTotal);
                if (useTile) {
                    clumpPos = tileAvail[i];
                } else {
                    // Ring 분포: 균등 각도 + jitter, 타원형 경로 따라.
                    int ringIdx = i - kTileClumpCount;
                    float baseAngle = (float)ringIdx / (float)kRingClumpCount * 6.2831853f;
                    float ang = baseAngle + distAngleJit(placeRng);
                    float rJit = distRJit(placeRng);
                    clumpPos.x = tileCx + std::cos(ang) * ringHalfX * rJit;
                    clumpPos.z = tileCz + std::sin(ang) * ringHalfZ * rJit;
                }
                clumpPos.y = 0.0f;

                // 군집별 결정적 seed — 매번 같은 모양 (방 재진입해도 일관성)
                unsigned int seed = 0xA5F00Du + static_cast<unsigned int>(i) * 0x9E3779B9u;

                // Stylized 풀잎: 베이스 넓고 끝 뾰족. 이번엔 전체적으로 키워서 존재감 ↑.
                //   높이 4.5 → 6.5, 너비 0.75 → 1.05.
                GrassClumpMesh* pMesh = new GrassClumpMesh(
                    pDev, pCmd,
                    cfgs[i].nBlades,
                    cfgs[i].fRadius,
                    /*fBladeHeight*/ 6.5f,    // 키 더 크게
                    /*fBladeWidth*/  1.05f,   // 잎 더 넓게
                    seed,
                    /*bTaper*/ true);

                GameObject* pClump = CreateGameObject(pDev, pCmd);
                if (!pClump)
                {
                    delete pMesh;  // refcount 0 — 그냥 delete
                    continue;
                }

                if (auto* pT = pClump->GetTransform())
                    pT->SetPosition(clumpPos.x, clumpPos.y, clumpPos.z);

                // SetMesh가 AddRef → refcount 1. CleanupWindAmbient에서 SetMesh(nullptr)로 0 → delete
                pClump->SetMesh(pMesh);

                // 풀 셰이딩 플래그 ON — VS sway + PS vertex 그라데이션 (텍스처 X)
                pClump->SetGrass(true);

                // 머티리얼: 흰색 diffuse — 셰이더가 grass 그라데이션으로 덮음. ambient는 0.5로 그라데이션 잘 살게.
                MATERIAL grassMat;
                grassMat.m_cAmbient  = XMFLOAT4(0.50f, 0.50f, 0.50f, 1.0f);
                grassMat.m_cDiffuse  = XMFLOAT4(1.00f, 1.00f, 1.00f, 1.0f);
                grassMat.m_cSpecular = XMFLOAT4(0.05f, 0.05f, 0.05f, 16.0f);
                grassMat.m_cEmissive = XMFLOAT4(0.00f, 0.00f, 0.00f, 1.0f);
                pClump->SetMaterial(grassMat);

                auto* pRC = pClump->AddComponent<RenderComponent>();
                pRC->SetMesh(pMesh);
                m_vShaders[0]->AddRenderComponent(pRC);

                m_vGrassClumpObjects.push_back(pClump);
            }
        }
    }
}

void Scene::CleanupWindAmbient()
{
    // 절차적 풀 군집 정리:
    //   1) shader 렌더 리스트에서 RenderComponent 제거 (이번 프레임 이후 렌더 X)
    //   2) GameObject SetMesh(nullptr) → mesh refcount 1→0 → delete (Mesh 메모리 회수)
    //   3) MarkForDeletion → 다음 ProcessPendingDeletions에서 GameObject 정리
    //
    // Dangling 방어: TransitionToGrassBossRoom 등 일부 경로에서 m_vRooms.clear() 가
    //   먼저 grass clump GameObject 를 파괴하고 m_vGrassClumpObjects 의 raw pointer 들은
    //   stale 한 상태로 남는다. 그 상태에서 본 함수가 호출되면 pClump->GetComponent()
    //   가 freed 메모리를 dereference → 크래시. 처리 전 alive 여부를 직접 확인.
    auto IsAlive = [this](GameObject* pObj) -> bool {
        if (!pObj) return false;
        for (auto& up : m_vGameObjects)
            if (up.get() == pObj) return true;
        for (auto& pRoom : m_vRooms)
        {
            if (!pRoom) continue;
            for (auto& up : pRoom->GetGameObjects())
                if (up.get() == pObj) return true;
        }
        return false;
    };

    for (GameObject* pClump : m_vGrassClumpObjects)
    {
        if (!IsAlive(pClump)) continue;
        if (auto* pRC = pClump->GetComponent<RenderComponent>())
        {
            if (!m_vShaders.empty()) m_vShaders[0]->RemoveRenderComponent(pRC);
        }
        pClump->SetMesh(nullptr);  // refcount 1 → 0 → delete GrassClumpMesh
        MarkForDeletion(pClump);
    }
    m_vGrassClumpObjects.clear();

    if (!m_pVFXManager)
    {
        m_vAmbientWindIds.clear();
        m_nPeriodicTornadoId = -1;
        m_nTornadoWarningVFXId = -1;
        m_eTornadoPhase = TornadoEventPhase::Idle;
        return;
    }
    for (int id : m_vAmbientWindIds) if (id >= 0) m_pVFXManager->Stop(id);
    m_vAmbientWindIds.clear();
    if (m_nPeriodicTornadoId >= 0) m_pVFXManager->Stop(m_nPeriodicTornadoId);
    m_nPeriodicTornadoId = -1;
    if (m_nTornadoWarningVFXId >= 0) m_pVFXManager->Stop(m_nTornadoWarningVFXId);
    m_nTornadoWarningVFXId = -1;
    m_fPeriodicTornadoTimer = 0.0f;
    m_fTornadoDamageTickTimer = 0.0f;
    m_eTornadoPhase = TornadoEventPhase::Idle;

    // 트랩된 플레이어 해제
    for (GameObject* pP : GetAllPlayers())
    {
        if (pP)
            if (auto* pPC = pP->GetComponent<PlayerComponent>())
                pPC->ExitTornadoTrap();
    }
}

// 네트워크 맵 토네이도 이벤트 시작
void Scene::StartNetworkMapTornadoEvent(const DirectX::XMFLOAT3& pos, float warningSec, float activeSec)
{
    // 서버 좌표 저장
    m_xmf3TornadoEventPos = pos;
    m_fNetworkTornadoWarningSec = warningSec;
    m_fNetworkTornadoActiveSec = activeSec;
    m_bUseNetworkTornadoEvent = true;

    // 기존 진행 중인 토네이도 정리
    if (m_nPeriodicTornadoId >= 0 && m_pVFXManager)
    {
        m_pVFXManager->Stop(m_nPeriodicTornadoId);
        m_nPeriodicTornadoId = -1;
    }

    if (m_nTornadoWarningVFXId >= 0 && m_pVFXManager)
    {
        m_pVFXManager->Stop(m_nTornadoWarningVFXId);
        m_nTornadoWarningVFXId = -1;
    }

    // Warning 페이즈부터 시작
    m_eTornadoPhase = TornadoEventPhase::Warning;
    m_fPeriodicTornadoTimer = 0.0f;
    m_fTornadoDamageTickTimer = 0.0f;

    if (m_pVFXManager)
    {
        XMFLOAT3 warnPos = m_xmf3TornadoEventPos;
        warnPos.y = 0.1f;

        m_nTornadoWarningVFXId = m_pVFXManager->Spawn(
            "Wind_TornadoWarning",
            warnPos,
            XMFLOAT3(0.0f, 1.0f, 0.0f),
            0u,
            false
        );
    }
}

// ────────────────────────────────────────────────────────────────────────────
// 스테이지 테마별 sky/clear color
//   Fire  — 따뜻한 적-주황 (0.18, 0.06, 0.04)
//   Water — 깊은 청 (0.05, 0.10, 0.18)
//   Earth — 갈-회색 (0.10, 0.08, 0.06)
//   Grass — 청록 새벽빛 (0.45, 0.62, 0.55) 바람 컨셉
// ────────────────────────────────────────────────────────────────────────────
// ── 전환 prerequisite 가드 ────────────────────────────────────────────────────
//   다른 PC 의 오프라인 모드 + 디버그 키(B/N)/네트워크 패킷 으로 맵 전환 시,
//   Scene::Init 의 셰이더/메쉬 로드가 완료되기 전 TransitionTo* 가 호출되면 크래시.
//   PC 환경(HDD 속도, GPU mem, 빌드 시점 timing) 차이로 개발 PC 는 통과, 다른 PC 는 실패.
bool Scene::IsReadyForTransition() const
{
    if (m_vShaders.empty())
    {
        OutputDebugString(L"[Scene] Transition skipped — shaders not initialized yet\n");
        return false;
    }
    return true;
}

// ── Sandstorm 진입점 ──────────────────────────────────────────────────────────
//   서버 권위화 후: 모래폭풍 broadcast 패킷 수신 시 이 함수만 호출하면 envelope 자동 진행.
//   현재는 Scene::Update 의 cycleTimer 가 자동 트리거.
void Scene::TriggerSandstorm(float duration)
{
    if (duration <= 0.0f) duration = kSandstormDefaultSec;
    m_bSandstormActive     = true;
    m_fSandstormDuration   = duration;
    m_fSandstormPhaseTimer = 0.0f;
    m_fSandstormStrength   = 0.0f;
    m_fSandstormCycleTimer = 0.0f;
}

// ── Wind gust 진입점 (Grass) — Sandstorm 과 동일 패턴 ──────────────────────────
void Scene::TriggerWindGust(float duration)
{
    if (duration <= 0.0f) duration = kWindGustDefaultSec;
    m_bWindGustActive     = true;
    m_fWindGustDuration   = duration;
    m_fWindGustPhaseTimer = 0.0f;
    m_fWindGustStrength   = 0.0f;
    m_fWindGustCycleTimer = 0.0f;
}

void Scene::ApplyThemeSkyColor()
{
    auto* pApp = Dx12App::GetInstance();
    if (!pApp) return;
    switch (m_eCurrentTheme)
    {
    case StageTheme::Fire:  pApp->SetClearColor(0.18f, 0.06f, 0.04f); break;
    case StageTheme::Water: pApp->SetClearColor(0.05f, 0.10f, 0.18f); break;
    case StageTheme::Earth: pApp->SetClearColor(0.10f, 0.08f, 0.06f); break;
    case StageTheme::Grass: pApp->SetClearColor(0.45f, 0.62f, 0.55f); break;
    case StageTheme::Dark:  pApp->SetClearColor(0.03f, 0.02f, 0.05f); break;  // 거의 검은 보라
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Flight Mode 프로토타입 (4스테이지 바람 보스 비행 슈팅 - Step 1)
// F6 토글: 더미 보스 박스 스폰 + Player/Camera FlightMode 진입/이탈
// ────────────────────────────────────────────────────────────────────────────
void Scene::SpawnFlightBossDummy(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList)
{
    if (m_pFlightBossDummy) return;

    // global object 로 생성 (room 외부)
    CRoom* pTempRoom = m_pCurrentRoom;
    m_pCurrentRoom = nullptr;
    m_pFlightBossDummy = CreateGameObject(pDevice, pCommandList);
    m_pCurrentRoom = pTempRoom;

    // 플레이어 앞 높이 ~25, 거리 ~30 위치
    XMFLOAT3 spawnPos = { 0.0f, 25.0f, 30.0f };
    if (m_pPlayerGameObject && m_pPlayerGameObject->GetTransform())
    {
        XMFLOAT3 pp = m_pPlayerGameObject->GetTransform()->GetPosition();
        spawnPos = { pp.x, pp.y + 22.0f, pp.z + 30.0f };
    }
    m_pFlightBossDummy->GetTransform()->SetPosition(spawnPos);
    m_pFlightBossDummy->GetTransform()->SetScale(6.0f, 6.0f, 6.0f);  // 보스급 크기

    CubeMesh* pCubeMesh = new CubeMesh(pDevice, pCommandList, 1.0f, 1.0f, 1.0f);
    m_pFlightBossDummy->SetMesh(pCubeMesh);

    MATERIAL windMaterial;
    windMaterial.m_cAmbient  = XMFLOAT4(0.1f, 0.15f, 0.1f, 1.0f);
    windMaterial.m_cDiffuse  = XMFLOAT4(0.6f, 0.85f, 0.7f, 1.0f);   // 청록빛 (바람 컨셉)
    windMaterial.m_cSpecular = XMFLOAT4(0.4f, 0.4f, 0.4f, 16.0f);
    windMaterial.m_cEmissive = XMFLOAT4(0.05f, 0.15f, 0.1f, 1.0f);
    m_pFlightBossDummy->SetMaterial(windMaterial);

    m_pFlightBossDummy->AddComponent<RenderComponent>()->SetMesh(pCubeMesh);
    if (!m_vShaders.empty())
        m_vShaders[0]->AddRenderComponent(m_pFlightBossDummy->GetComponent<RenderComponent>());

    // 초기 진행 방향 = +Z (yaw 0)
    m_fFlightBossYawDeg = 0.0f;
    m_fFlightCurveTime  = 0.0f;
    m_pFlightBossDummy->GetTransform()->SetRotation(0.0f, m_fFlightBossYawDeg, 0.0f);

    OutputDebugString(L"[Scene] Flight boss dummy spawned (rail)\n");
}

void Scene::ToggleFlightMode(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList)
{
    if (!m_pPlayerGameObject || !m_pCamera) return;
    auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
    if (!pPC) return;

    if (pPC->IsFlightMode())
    {
        pPC->ExitFlightMode();
        m_pCamera->SetFlightMode(false);
        // 윈드 이미터: LightEmitterSystem 기반은 자동 만료(짧은 lifetime), 별도 종료 불필요
        m_fFlightWindAccum = 0.f;
        // 잔존 탄환 정리 (유체 트레일 stop)
        if (m_pVFXManager)
            for (auto& b : m_FlightBossBullets)
                if (b.fluidId >= 0) m_pVFXManager->Stop(b.fluidId);
        m_FlightBossBullets.clear();
        // FOV 복원
        m_pCamera->SetFovDegrees(m_pCamera->GetBaseFovDeg());
        m_fFlightFovOffsetCur = 0.0f;
        OutputDebugString(L"[Scene] Flight Mode: OFF\n");
    }
    else
    {
        if (!m_pFlightBossDummy)
            SpawnFlightBossDummy(pDevice, pCommandList);
        else
        {
            // 재진입 시 보스 위치를 플레이어 앞쪽으로 리셋 (장시간 비활성화 후 멀리 가버린 경우 대비)
            XMFLOAT3 pp = m_pPlayerGameObject->GetTransform()->GetPosition();
            m_pFlightBossDummy->GetTransform()->SetPosition(XMFLOAT3(pp.x, pp.y + 22.0f, pp.z + 30.0f));
            m_fFlightBossYawDeg = 0.0f;
            m_fFlightCurveTime  = 0.0f;
            m_pFlightBossDummy->GetTransform()->SetRotation(0.0f, m_fFlightBossYawDeg, 0.0f);
        }

        pPC->EnterFlightMode(m_pFlightBossDummy);
        m_pCamera->SetFlightMode(true, m_pFlightBossDummy);
        OutputDebugString(L"[Scene] Flight Mode: ON (F6 to exit)\n");
    }
}

bool Scene::IsFlightHUDActive() const
{
    if (!m_pPlayerGameObject) return false;
    auto* pPC = m_pPlayerGameObject->GetComponent<PlayerComponent>();
    return pPC && pPC->IsFlightMode();
}

void Scene::FlightShoot(const XMFLOAT3& muzzlePos, const XMFLOAT3& dirNormalized)
{
    if (!m_pFlightBossDummy || !m_pVFXManager) return;

    XMVECTOR ro = XMLoadFloat3(&muzzlePos);
    XMVECTOR rd = XMLoadFloat3(&dirNormalized);

    // 보스 sphere 충돌 (스케일 6.0 → 반경 ~ 4.0 + 약간의 여유)
    XMFLOAT3 bp = m_pFlightBossDummy->GetTransform()->GetPosition();
    XMFLOAT3 bs = m_pFlightBossDummy->GetTransform()->GetScale();
    float bossRadius = bs.x * 0.65f + 1.0f;
    XMVECTOR center = XMLoadFloat3(&bp);
    XMVECTOR oc = center - ro;
    float t = XMVectorGetX(XMVector3Dot(oc, rd));

    bool bHit = false;
    XMFLOAT3 hitPoint = muzzlePos;
    if (t > 0.0f && t < 250.0f)
    {
        XMVECTOR closest = ro + rd * t;
        float d = XMVectorGetX(XMVector3Length(center - closest));
        if (d < bossRadius)
        {
            bHit = true;
            DirectX::XMStoreFloat3(&hitPoint, closest);
        }
    }

    // 머즐 플래시 — 작은 Sphere Burst
    {
        EffectLayer layer;
        layer.type          = EmitterType::Sphere;
        layer.particleCount = 14;
        layer.coreColor     = { 0.85f, 0.95f, 1.0f, 1.0f };
        layer.edgeColor     = { 0.2f,  0.4f,  0.9f, 0.0f };
        layer.speedMin      = 2.0f;
        layer.speedMax      = 5.0f;
        layer.lifetimeMin   = 0.05f;
        layer.lifetimeMax   = 0.18f;
        layer.sizeScale     = 0.6f;
        layer.sphere.radius = 0.4f;
        m_pVFXManager->SpawnLightLayer(muzzlePos, dirNormalized, layer, /*isPlayer*/true);
    }

    if (bHit)
    {
        // 피격 폭발 — Burst (중력 영향)
        EffectLayer layer;
        layer.type          = EmitterType::Burst;
        layer.particleCount = 28;
        layer.coreColor     = { 1.0f, 0.95f, 0.7f, 1.0f };
        layer.edgeColor     = { 0.6f, 0.3f,  0.1f, 0.0f };
        layer.speedMin      = 5.0f;
        layer.speedMax      = 10.0f;
        layer.lifetimeMin   = 0.20f;
        layer.lifetimeMax   = 0.45f;
        layer.sizeScale     = 0.8f;
        layer.burst.bounceCoeff = 0.f;
        layer.burst.fadeOut     = true;
        layer.burst.fadeSize    = true;
        m_pVFXManager->SpawnLightLayer(hitPoint, dirNormalized, layer, /*isPlayer*/true);

        m_fFlightBossHitFlashTimer = kFlightHitFlashDuration;
        m_nFlightHitCount++;
    }
    else
    {
        // 미스 트레이서: 라인 따라 일정 간격 작은 Sphere Burst (시각용)
        XMVECTOR end = ro + rd * 70.0f;
        for (int i = 1; i <= 4; ++i)
        {
            float u = (float)i / 5.0f;
            XMVECTOR p = XMVectorLerp(ro, end, u);
            XMFLOAT3 pp; DirectX::XMStoreFloat3(&pp, p);

            EffectLayer layer;
            layer.type          = EmitterType::Sphere;
            layer.particleCount = 4;
            layer.coreColor     = { 0.7f, 0.85f, 1.0f, 0.9f };
            layer.edgeColor     = { 0.2f, 0.3f,  0.6f, 0.0f };
            layer.speedMin      = 0.5f;
            layer.speedMax      = 1.5f;
            layer.lifetimeMin   = 0.05f;
            layer.lifetimeMax   = 0.15f;
            layer.sizeScale     = 0.45f;
            layer.sphere.radius = 0.2f;
            m_pVFXManager->SpawnLightLayer(pp, dirNormalized, layer, /*isPlayer*/true);
        }
    }
}

void Scene::UpdateFlightFX(float deltaTime, InputSystem* pInputSystem)
{
    if (!m_pPlayerGameObject || !m_pVFXManager) return;
    TransformComponent* pPT = m_pPlayerGameObject->GetTransform();
    if (!pPT) return;

    // 부스트 입력
    bool bBoost = pInputSystem && pInputSystem->IsKeyDown(VK_SHIFT);

    // ── FOV 펀치: 비행 항시 +6deg, 부스트 시 +22deg (광각감 살림)
    if (m_pCamera)
    {
        float target = bBoost ? kFlightBoostFovOffset : kFlightBaseFovOffset;
        float k = 1.0f - expf(-(bBoost ? 9.0f : 5.0f) * deltaTime);
        m_fFlightFovOffsetCur += (target - m_fFlightFovOffsetCur) * k;
        m_pCamera->SetFovDegrees(m_pCamera->GetBaseFovDeg() + m_fFlightFovOffsetCur);
    }

    // ── 윈드 라인 — LightEmitterSystem(Sphere) 짧은 Burst 주기 재스폰.
    // 모션 forward = m_fFlightBossYawDeg 직접 계산 (보스 visual rotation 과 분리)
    float yawRad = XMConvertToRadians(m_fFlightBossYawDeg);
    XMVECTOR fwd = XMVectorSet(sinf(yawRad), 0.0f, cosf(yawRad), 0.0f);

    XMFLOAT3 fwdF; DirectX::XMStoreFloat3(&fwdF, fwd);
    float windSpeed = bBoost ? 160.0f : 110.0f;

    // ~30Hz 재스폰 (0.033s). 부스트 시 ~50Hz (0.020s).
    const float windInterval = bBoost ? 0.020f : 0.033f;
    m_fFlightWindAccum += deltaTime;
    int spawnSteps = 0;
    while (m_fFlightWindAccum >= windInterval && spawnSteps < 4)
    {
        m_fFlightWindAccum -= windInterval;
        ++spawnSteps;

        // 플레이어 앞쪽 8단위에서 스폰 → 뒤로 빠르게 흘러내려옴
        XMVECTOR ppV = XMLoadFloat3(&pPT->GetPosition());
        XMVECTOR spawn = ppV + fwd * 8.0f;
        XMFLOAT3 spawnPos; DirectX::XMStoreFloat3(&spawnPos, spawn);

        // direction = -fwd (뒤로 흐름)
        XMFLOAT3 backDir = { -fwdF.x, 0.0f, -fwdF.z };

        EffectLayer layer;
        layer.type          = EmitterType::Cone;
        layer.particleCount = bBoost ? 12 : 8;   // 1회 스폰량
        layer.coreColor     = { 0.95f, 1.0f, 1.0f, 0.7f };
        layer.edgeColor     = { 0.55f, 0.85f, 1.0f, 0.0f };
        layer.speedMin      = windSpeed * 0.85f;
        layer.speedMax      = windSpeed * 1.05f;
        layer.lifetimeMin   = 0.40f;
        layer.lifetimeMax   = 0.85f;
        layer.sizeScale     = 0.85f;
        layer.cone.halfAngle     = 35.0f;        // 시야 양옆까지 spread
        layer.cone.gravityScale  = 0.f;
        layer.cone.startSizeMult = 1.0f;
        layer.cone.endSizeMult   = 0.05f;
        layer.cone.fadeAlpha     = true;
        layer.cone.fadeSize      = false;
        m_pVFXManager->SpawnLightLayer(spawnPos, backDir, layer, /*isPlayer*/true);
    }
}

void Scene::UpdateFlightBoss(float deltaTime)
{
    if (!m_pFlightBossDummy) return;
    TransformComponent* pBT = m_pFlightBossDummy->GetTransform();
    if (!pBT) return;

    // 코스 곡선: 사인파 yaw 변동(-15° ~ +15°) — 단순 길찾기
    m_fFlightCurveTime += deltaTime;
    float targetYaw = sinf(m_fFlightCurveTime * 0.35f) * 15.0f;
    float yawDelta = targetYaw - m_fFlightBossYawDeg;
    m_fFlightBossYawDeg += yawDelta * fminf(1.0f, deltaTime * 1.5f);

    // 보스 visual rotation: motion yaw + 180 — Demon mesh natural facing이 -Z이므로
    // 모션 방향(+Z 계열)을 보려면 메시를 180 돌려야 함
    pBT->SetRotation(0.0f, m_fFlightBossYawDeg + 180.0f, 0.0f);

    // 모션 forward는 yaw로 직접 계산 (mesh rotation 보정과 무관하게 일관)
    float yawRad = XMConvertToRadians(m_fFlightBossYawDeg);
    XMVECTOR motionFwd = XMVectorSet(sinf(yawRad), 0.0f, cosf(yawRad), 0.0f);

    XMFLOAT3 bp = pBT->GetPosition();
    XMVECTOR bpv = XMLoadFloat3(&bp);
    bpv = bpv + motionFwd * (m_fFlightBossSpeed * deltaTime);
    DirectX::XMStoreFloat3(&bp, bpv);
    pBT->SetPosition(bp);

    // ── 보스 기본 공격: 부채꼴 탄막 (속성별 색만 다름) ─────────
    m_fFlightBossSkillTimer += deltaTime;
    if (m_fFlightBossSkillTimer >= kFlightBossSkillCooldown && m_pPlayerGameObject)
    {
        m_fFlightBossSkillTimer = 0.0f;
        FireFlightBossBarrage();
    }

    // 활성 탄환 갱신 (이동/충돌/수명)
    UpdateFlightBossBullets(deltaTime);
}

void Scene::GetFlightBulletColors(ElementType e, XMFLOAT4& outStart, XMFLOAT4& outEnd) const
{
    // 속성별 탄막 색상 — 메커닉은 동일, 비주얼만 차별화
    switch (e)
    {
    case ElementType::Fire:
        outStart = { 1.0f, 0.7f, 0.2f, 1.0f };
        outEnd   = { 0.8f, 0.2f, 0.05f, 0.0f };
        break;
    case ElementType::Water:
        outStart = { 0.4f, 0.8f, 1.0f, 1.0f };
        outEnd   = { 0.1f, 0.3f, 0.7f, 0.0f };
        break;
    case ElementType::Earth:
        outStart = { 0.85f, 0.7f, 0.4f, 1.0f };
        outEnd   = { 0.4f,  0.3f, 0.15f, 0.0f };
        break;
    case ElementType::Wind:
    default:
        outStart = { 0.85f, 1.0f, 0.95f, 1.0f };
        outEnd   = { 0.45f, 0.85f, 0.7f, 0.0f };
        break;
    }
}

void Scene::FireFlightBossBarrage()
{
    if (!m_pFlightBossDummy || !m_pPlayerGameObject) return;
    TransformComponent* pBT = m_pFlightBossDummy->GetTransform();
    if (!pBT) return;

    XMFLOAT3 bossPos = pBT->GetPosition();
    bossPos.y -= 2.0f;  // 가슴 높이

    XMFLOAT3 playerPos = m_pPlayerGameObject->GetTransform()->GetPosition();
    XMVECTOR toPlayer = XMVectorSubtract(XMLoadFloat3(&playerPos), XMLoadFloat3(&bossPos));
    if (XMVectorGetX(XMVector3LengthSq(toPlayer)) < 0.01f)
    {
        // fallback: motion forward 반대(보스 뒤쪽) 방향으로
        float yawRad = XMConvertToRadians(m_fFlightBossYawDeg);
        toPlayer = XMVectorSet(-sinf(yawRad), 0.0f, -cosf(yawRad), 0.0f);
    }
    XMVECTOR baseDir = XMVector3Normalize(toPlayer);

    XMFLOAT4 colStart, colEnd;
    GetFlightBulletColors(m_eFlightBossElement, colStart, colEnd);

    // 유체 VFX 정의 — 속성에 따라 색/궤도 자동 선택
    FluidSkillVFXDef fluidDef = FluidSkillVFXManager::GetVFXDef(m_eFlightBossElement);
    // 보스 탄막은 작고 빠르게 — 입자 수/반경 살짝 줄여서 5발 동시에도 부담 적게
    fluidDef.particleCount = 80;
    fluidDef.spawnRadius   = 0.5f;

    const int N = kFlightBulletsPerVolley;
    for (int i = 0; i < N; ++i)
    {
        // 부채꼴 각도: -fan ~ +fan 등분
        float t = (N == 1) ? 0.0f : ((float)i / (float)(N - 1)) * 2.0f - 1.0f; // -1..+1
        float angRad = XMConvertToRadians(kFlightBulletFanDeg * t);

        // baseDir 을 worldUp 축 기준 angRad 회전
        XMMATRIX yawRot = XMMatrixRotationY(angRad);
        XMVECTOR dir = XMVector3TransformNormal(baseDir, yawRot);
        dir = XMVector3Normalize(dir);

        FlightBossBullet b;
        b.pos = bossPos;
        XMFLOAT3 d; DirectX::XMStoreFloat3(&d, dir);
        b.vel = { d.x * kFlightBulletSpeed, d.y * kFlightBulletSpeed, d.z * kFlightBulletSpeed };
        b.lifeRemain = kFlightBulletLifetime;

        // 유체 트레일 (적 전용 VFX 매니저 — 빌보드 렌더, SSF 제외)
        b.fluidId = -1;
        if (m_pVFXManager)
            b.fluidId = m_pVFXManager->SpawnEffect(b.pos, d, fluidDef);

        // 출발 순간 LightEmitter burst (속성색 임팩트감 추가)
        if (m_pVFXManager)
        {
            EffectLayer layer;
            layer.type          = EmitterType::Sphere;
            layer.particleCount = 10;
            layer.coreColor     = colStart;
            layer.edgeColor     = colEnd;
            layer.speedMin      = 1.5f;
            layer.speedMax      = 4.0f;
            layer.lifetimeMin   = 0.10f;
            layer.lifetimeMax   = 0.20f;
            layer.sizeScale     = 0.7f;
            layer.sphere.radius = 0.3f;
            // 보스 발사 = 적 슬롯
            m_pVFXManager->SpawnLightLayer(b.pos, d, layer, /*isPlayer*/false);
        }

        m_FlightBossBullets.push_back(b);
    }

    OutputDebugString(L"[FlightBoss] Fluid barrage volley fired\n");
}

void Scene::UpdateFlightBossBullets(float deltaTime)
{
    if (m_FlightBossBullets.empty()) return;

    XMFLOAT3 playerPos = m_pPlayerGameObject
        ? m_pPlayerGameObject->GetTransform()->GetPosition()
        : XMFLOAT3{ 0, 0, 0 };
    XMVECTOR pp = XMLoadFloat3(&playerPos);

    for (auto it = m_FlightBossBullets.begin(); it != m_FlightBossBullets.end(); )
    {
        // 이동
        it->pos.x += it->vel.x * deltaTime;
        it->pos.y += it->vel.y * deltaTime;
        it->pos.z += it->vel.z * deltaTime;
        it->lifeRemain -= deltaTime;

        // 유체 트레일 위치/방향 갱신
        if (m_pVFXManager && it->fluidId >= 0)
        {
            float invSpeed = 1.0f / (kFlightBulletSpeed > 0.0f ? kFlightBulletSpeed : 1.0f);
            XMFLOAT3 dir = { it->vel.x * invSpeed, it->vel.y * invSpeed, it->vel.z * invSpeed };
            m_pVFXManager->Track(it->fluidId, it->pos, dir);
        }

        // 플레이어 충돌 (sphere)
        XMVECTOR bp = XMLoadFloat3(&it->pos);
        XMVECTOR diff = pp - bp;
        float distSq = XMVectorGetX(XMVector3LengthSq(diff));
        bool bHitPlayer = (distSq < kFlightBulletHitRadius * kFlightBulletHitRadius);

        bool bExpired = (it->lifeRemain <= 0.0f);

        if (bHitPlayer || bExpired)
        {
            // 유체 트레일 종료 — 피격은 수렴(impact), 자연 소멸은 stop
            if (m_pVFXManager && it->fluidId >= 0)
            {
                if (bHitPlayer)
                    m_pVFXManager->Impact(it->fluidId, it->pos);
                else
                    m_pVFXManager->Stop(it->fluidId);
            }

            // 임팩트 burst — 유체 수렴 + 입자 분산으로 펀치감 강화 (피격 시만)
            if (bHitPlayer && m_pVFXManager)
            {
                XMFLOAT4 colStart, colEnd;
                GetFlightBulletColors(m_eFlightBossElement, colStart, colEnd);

                EffectLayer layer;
                layer.type          = EmitterType::Sphere;
                layer.particleCount = 22;
                layer.coreColor     = colStart;
                layer.edgeColor     = colEnd;
                layer.speedMin      = 3.0f;
                layer.speedMax      = 8.0f;
                layer.lifetimeMin   = 0.12f;
                layer.lifetimeMax   = 0.30f;
                layer.sizeScale     = 0.85f;
                layer.sphere.radius = 0.5f;
                m_pVFXManager->SpawnLightLayer(it->pos, XMFLOAT3(0, 1, 0),
                                               layer, /*isPlayer*/false);
            }

            it = m_FlightBossBullets.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

EnemyComponent* Scene::FindNearestEnemy(const DirectX::XMFLOAT3& pos) const
{
    EnemyComponent* nearest = nullptr;
    float bestDistSq = FLT_MAX;

    for (const auto& pRoom : m_vRooms)
    {
        if (!pRoom) continue;
        for (EnemyComponent* pEnemy : pRoom->GetEnemies())
        {
            if (!pEnemy || pEnemy->IsDead()) continue;
            GameObject* pObj = pEnemy->GetOwner();
            if (!pObj || !pObj->GetTransform()) continue;
            const XMFLOAT3& ePos = pObj->GetTransform()->GetPosition();
            float dx = ePos.x - pos.x;
            float dz = ePos.z - pos.z;
            float distSq = dx * dx + dz * dz;
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                nearest = pEnemy;
            }
        }
    }
    return nearest;
}