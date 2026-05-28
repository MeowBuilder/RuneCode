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
#include "EffectRegistry.h"
#include "SkillTypes.h"
#include "ProjectileManager.h"
#include "PlayerComponent.h"
#include "Camera.h"
#include "DamageNumberManager.h"
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
#include "RushFrontAttackBehavior.h"
#include "FixatedChargeAttackBehavior.h"
#include "Dx12App.h"
#include "MapLoader.h"
#include "CharacterData.h"
#include <cmath>
#include <random>
#include <cstring>

// ServerPacketHandler.cpp에 정의된 파일 로그 함수 — network_log.txt에 append
extern void WriteNetworkLog(const std::string& msg);

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

void NetworkManager::Update(Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList)
{
    if (!pScene || !pDevice || !pCommandList)
        return;

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
            ProcessMovePlayer(cmd.playerId, cmd.x, cmd.y, cmd.z, cmd.dirX, cmd.dirY, cmd.dirZ);
            break;

        case NetworkCommand::Skill:
            ProcessSkill(pScene, cmd.playerId, cmd.skillType, cmd.x, cmd.y, cmd.z, cmd.dirX, cmd.dirY, cmd.dirZ);
            break;

        case NetworkCommand::SetLocalPlayerId:
            // 이미 1차에서 처리됨
            break;

        case NetworkCommand::RoomTransition:
            ProcessRoomTransition(pScene, cmd.stageIndex, cmd.roomIndex, cmd.isBossRoom, cmd.mapId);
            break;

        case NetworkCommand::MonsterSpawn:
            ProcessMonsterSpawn(pScene, pDevice, pCommandList,
                                cmd.monsterId, cmd.monsterType,
                                cmd.x, cmd.y, cmd.z, cmd.monsterYaw,
                                cmd.monsterHp, cmd.monsterIsBoss);
            break;

        case NetworkCommand::MonsterMove:
            ProcessMonsterMove(cmd.monsterId, cmd.x, cmd.y, cmd.z, cmd.monsterYaw);
            break;

        case NetworkCommand::MonsterDespawn:
            ProcessMonsterDespawn(pScene, cmd.monsterId);
            break;

        case NetworkCommand::MonsterAttack:
            ProcessMonsterAttack(pScene, cmd.monsterId, cmd.attackType, cmd.windupSec,cmd.targetPlayerId, cmd.x, cmd.y, cmd.z, cmd.effectPositions, cmd.effectOption);
            break;

        case NetworkCommand::PlayerDamage:
            ProcessPlayerDamage(pScene, cmd.playerId, cmd.damage, cmd.currentHp, cmd.isDead, cmd.attackerMonsterId);
            break;

        case NetworkCommand::MonsterDamage:
            ProcessMonsterDamage(pScene, cmd.monsterId, cmd.damage, cmd.currentHp, cmd.isDead,cmd.attackerPlayerId, cmd.skillType);
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

        case NetworkCommand::BossEvent:
            ProcessBossEvent(pScene, cmd.monsterId, cmd.bossEventType, cmd.phaseIndex);
            break;
        }
    }

    // 방 전환이 처리된 바로 그 프레임에는 TorchInteract 타이머를 줄이지 않음
    // 즉, 최소 다음 프레임부터 카운트다운 시작
    if (roomTransitionProcessedThisFrame)
        return;

    // 방 전환 후 지연 TorchInteract 처리
    // ProcessRoomTransition 직후 바로 보내면 맵 로딩/오브젝트 정리와
    // 몬스터 스폰 패킷이 겹쳐 클라가 끊길 수 있으므로 몇 프레임 뒤에 전송한다.
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
}

void NetworkManager::SendPortalInteract()
{
    if (!m_bConnected || !m_pSession)
    {
        WriteNetworkLog("[Network] SendPortalInteract BLOCKED (not connected or no session)");
        return;
    }

    Protocol::C_PORTAL_INTERACT pkt;
    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    OutputDebugString(L"[Network] C_PORTAL_INTERACT sent\n");
    WriteNetworkLog("[Network] C_PORTAL_INTERACT sent");
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
                                      float targetX, float targetY, float targetZ)
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

    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(pkt);
    m_pSession->Send(sendBuffer);

    char buf[256];
    sprintf_s(buf, "[Network] C_PLAYER_ATTACK sent: skillType=%d pos=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f)",
        skillType, x, y, z, targetX, targetY, targetZ);
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
                                       float x, float y, float z, float yaw,
                                       float hp, bool isBoss)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    NetworkCommandData cmd{};
    cmd.type = NetworkCommand::MonsterSpawn;
    cmd.monsterId = monsterId;
    cmd.monsterType = monsterType;
    cmd.x = x; cmd.y = y; cmd.z = z;
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

    // 방 전환 전에 네트워크 Golem 연출 정리
    // 이전 방의 EnemyComponent / Rock / Indicator를 물고 있으면 방 정리 후 크래시 가능
    for (auto& entry : m_vNetworkGolemBehaviors)
    {
        if (entry.behavior)
            entry.behavior->Reset();
    }
    m_vNetworkGolemBehaviors.clear();

    // 중복 전환 방어: 서버가 S_ROOM_TRANSITION 을 동일 프레임에 두 번 보내거나
    // 클라 큐에 중복 push 된 경우, 방 정리 중 다시 정리·재생성 호출로 dangling pointer 크래시 발생.
    if (m_bInRoomTransition)
    {
        WriteNetworkLog("[Network] ProcessRoomTransition skipped: already transitioning");
        return;
    }
    m_bInRoomTransition = true;

    wchar_t buf[512];
    swprintf_s(buf, L"[Network] ProcessRoomTransition stage=%u room=%u boss=%d mapId=%S\n",
        stageIndex, roomIndex, isBossRoom ? 1 : 0, mapId.c_str());
    OutputDebugString(buf);

    // 이전 방 서버 몬스터 전부 정리 — GameObject 는 Scene 에 MarkForDeletion 으로 삭제 예약,
    // 보조 맵들은 즉시 clear. 새 방에서 같은 monsterId 가 재전송돼도 깨끗한 상태에서 재스폰됨.
    for (auto& kv : m_mapServerMonsters)
    {
        if (kv.second) pScene->MarkForDeletion(kv.second);
    }
    m_mapServerMonsters.clear();
    m_mapServerMonsterClips.clear();
    m_mapServerMonsterTarget.clear();
    m_mapServerMonsterMoveTime.clear();
    m_mapServerMonsterAttackTimer.clear();
    m_mapServerMonsterHitFlashTimer.clear();
    m_setDeadServerMonsters.clear();

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

    // 서버가 내려준 mapId 우선 분기. mapId 가 비었거나 미매칭이면 stageIndex/roomIndex 기반 fallback.
    bool bHandled = false;
    if (!mapId.empty())
    {
        if (mapId == "fire_boss")           { pScene->TransitionToBossRoom();        bHandled = true; }
        else if (mapId == "water_boss")     { pScene->TransitionToWaterBossRoom();   bHandled = true; }
        else if (mapId == "earth_boss")     { pScene->TransitionToEarthBossRoom();   bHandled = true; }
        else if (mapId == "grass_boss")     { pScene->TransitionToGrassBossRoom();   bHandled = true; }
        else if (mapId.rfind("fire_room_", 0) == 0)
        {
            pScene->TransitionToRoomByIndex(static_cast<int>(roomIndex));
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
            default: pScene->TransitionToBossRoom();       break;
            }
        }
        else
        {
            pScene->TransitionToRoomByIndex(static_cast<int>(roomIndex));
        }
    }

    // 원격 플레이어 좌표 리셋 — 서버 HandlePortalInteract 는 좌표를 건드리지 않으므로
    // 이전 방 좌표가 그대로 남아 새 맵에서 맵 밖/이상한 위치로 보일 수 있음 ("안 보이는 현상").
    // 다음 S_MOVE 패킷 오면 실제 위치로 갱신되므로 임시로 로컬 플레이어 근처에 모아둠.
    if (GameObject* pLocal = pScene->GetPlayer())
    {
        if (auto* pLocalT = pLocal->GetTransform())
        {
            XMFLOAT3 localPos = pLocalT->GetPosition();
            for (auto& kv : m_mapRemotePlayers)
            {
                if (GameObject* pRemote = kv.second)
                {
                    if (auto* pT = pRemote->GetTransform())
                    {
                        pT->SetPosition(localPos.x, localPos.y, localPos.z);
                    }
                }
            }
        }
    }

    // 방 전환이 끝나면 서버 몬스터 스폰을 트리거해야 함.
    // 서버 Room 은 HandleTorchInteract 를 받아야만 몬스터를 스폰하도록 돼 있음 (최초 방 진입과 동일 플로우).
    // 오프라인에서는 방 Active 시 자동 스폰이지만, 네트워크 모드에서는 C_TORCH_INTERACT 가 스폰 트리거.
    // 첫 방에선 맵에 배치된 횃불/큐브를 F 로 눌러 시작했지만, 다음 방 이후에는 자동으로 요청해준다.
    m_bPendingTorchInteract = true;
    m_nPendingTorchInteractFrame = 30; // 약 0.5초 정도

    WriteNetworkLog("[Network] Pending C_TORCH_INTERACT scheduled");

    m_bInRoomTransition = false;
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

void NetworkManager::QueueSkill(uint64 playerId, int skillType, float x, float y, float z, float dirX, float dirY, float dirZ)
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
        ProcessMovePlayer(playerId, x, y, z, 0.0f, 0.0f, 1.0f);
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

    // 이름 설정
    sprintf_s(pRemotePlayer->m_pstrFrameName, "RemotePlayer_%llu", playerId);

    // 위치 및 스케일 설정
    TransformComponent* pTransform = pRemotePlayer->GetTransform();
    if (pTransform)
    {
        pTransform->SetPosition(x, y, z);
        pTransform->SetScale(5.0f, 5.0f, 5.0f);
    }

    // 애니메이션 추가 — 원소별 anim 파일.
    auto* pAnim = pRemotePlayer->AddComponent<AnimationComponent>();
    if (pAnim)
    {
        pAnim->LoadAnimation(cdata.animPath);
        pAnim->Play("Idle", true);
    }

    // 셰이더 등록
    Shader* pDefaultShader = pScene->GetDefaultShader();
    if (pDefaultShader)
    {
        pScene->AddRenderComponentsToHierarchy(pDevice, pCommandList, pRemotePlayer, pDefaultShader, true);
    }

    // 컴포넌트 초기화 (AnimationComponent::BuildBoneCache 포함)
    pRemotePlayer->Init(pDevice, pCommandList);

    // 맵에 등록
    m_mapRemotePlayers[playerId] = pRemotePlayer;
    m_mapRemotePlayerElement[playerId] = remoteElement;

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

    // 맵에서 제거
    m_mapRemotePlayers.erase(it);
    m_mapRemotePlayerElement.erase(playerId);
    m_mapRemotePlayerMoveTime.erase(playerId);
    m_setDeadRemotePlayers.erase(playerId);
    m_mapRemotePlayerHitFlashTimer.erase(playerId);

    wchar_t buf[128];
    swprintf_s(buf, L"[Network] Despawned remote player %llu\n", playerId);
    OutputDebugString(buf);
}

void NetworkManager::ProcessMovePlayer(uint64 playerId, float x, float y, float z, float dirX, float dirY, float dirZ)
{
    // 로컬 플레이어라면 무시 (로컬은 자체 업데이트)
    if (playerId == m_nLocalPlayerId.load())
        return;

    auto it = m_mapRemotePlayers.find(playerId);
    if (it == m_mapRemotePlayers.end())
        return;

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
            // atan2로 Y축 회전각 계산 (라디안)
            float yaw = atan2f(dirX, dirZ);
            // 라디안을 도(degree)로 변환
            float yawDegrees = XMConvertToDegrees(yaw);

            // Y축 회전만 적용 (기존 X, Z 회전은 유지)
            XMFLOAT3 currentRot = pTransform->GetRotation();
            pTransform->SetRotation(currentRot.x, yawDegrees, currentRot.z);
        }
    }

    // 죽은 원격 플레이어는 데스 애니 유지 — walk/idle 로 덮지 않음
    bool bDead = (m_setDeadRemotePlayers.find(playerId) != m_setDeadRemotePlayers.end());

    // 걷기 애니메이션 활성화
    AnimationComponent* pAnim = pRemotePlayer->GetComponent<AnimationComponent>();
    if (pAnim && !bDead)
    {
        // CrossFade로 부드럽게 전환 (이미 걷기 중이면 무시)
        pAnim->CrossFade("Walk", 0.1f, true);
    }

    // 마지막 이동 시간 기록 (idle 전환용)
    if (!bDead)
        m_mapRemotePlayerMoveTime[playerId] = 0.0f;
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
            if (!bDead && playerIt != m_mapRemotePlayers.end())
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

void NetworkManager::TickPendingMeteorShowers(FluidSkillVFXManager* pVFXManager, float deltaTime)
{
    if (!pVFXManager) return;

    // MeteorBehavior 상수와 동일 (시각 일치).
    constexpr int   SHOWER_COUNT       = 6;
    constexpr float SHOWER_INTERVAL    = 0.5f;
    constexpr float SMALL_SPAWN_HEIGHT = 40.0f;
    constexpr float SMALL_FALL_SPEED   = 28.0f;
    constexpr float FINAL_SPAWN_HEIGHT = 60.0f;
    constexpr float FINAL_FALL_SPEED   = 22.0f;
    constexpr float FINAL_DELAY        = 0.5f;
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
                sm.spawned      = true;
                sm.fallElapsed  = 0.0f;
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
                sh.finalFallElapsed  = 0.0f;
                sh.finalFallDuration = FINAL_SPAWN_HEIGHT / FINAL_FALL_SPEED;
                sh.finalSpawned      = true;
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
                EffectDef impDef  = EffectRegistry::Get().GetEffect("R_MeteorImpact",     RUNE_NONE);
                EffectDef fireDef = EffectRegistry::Get().GetEffect("R_MeteorGroundFire", RUNE_NONE);
                pVFXManager->SpawnEffectDef(sh.targetPos, upDir, impDef,  true);
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
        state.totalElapsed   += deltaTime;

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

void NetworkManager::ProcessSkill(Scene* pScene, uint64 playerId, int skillType, float x, float y, float z, float dirX, float dirY, float dirZ)
{
    // 로컬 플레이어라면 무시 (로컬은 자체 처리)
    if (playerId == m_nLocalPlayerId.load())
        return;

    auto it = m_mapRemotePlayers.find(playerId);
    if (it == m_mapRemotePlayers.end())
        return;

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
    if (pAnim)
    {
        // 스킬 애니메이션은 한 번만 재생 (루프 X), forceRestart=true로 연속 공격 시에도 재시작
        pAnim->CrossFade("Attack1", 0.1f, false, true);
    }

    // caster 위치(서버가 전달) — y 는 발 위치. 오프라인의 "캐릭터 머리 높이" 는 +5.0f.
    const XMFLOAT3 casterPos    = XMFLOAT3(x, y, z);
    const XMFLOAT3 casterHead   = XMFLOAT3(x, y + 5.0f, z);

    // wire dir 슬롯: Q/E/RC 는 lookDir(정규화), R 은 절대 target 좌표 (SkillComponent.cpp).
    // 일단 lookDir 해석부터 (R 처리에서는 별도로 target 사용).
    XMFLOAT3 lookDir = XMFLOAT3(dirX, dirY, dirZ);
    {
        float L = sqrtf(lookDir.x*lookDir.x + lookDir.y*lookDir.y + lookDir.z*lookDir.z);
        if (L > 0.001f) { lookDir.x/=L; lookDir.y/=L; lookDir.z/=L; }
        else            { lookDir = XMFLOAT3(0.0f, 0.0f, 1.0f); }
    }
    XMFLOAT3 horizontalDir = XMFLOAT3(lookDir.x, 0.0f, lookDir.z);
    {
        float L = sqrtf(horizontalDir.x*horizontalDir.x + horizontalDir.z*horizontalDir.z);
        if (L > 0.001f) { horizontalDir.x/=L; horizontalDir.z/=L; }
        else            { horizontalDir = XMFLOAT3(0.0f, 0.0f, 1.0f); }
    }

    FluidSkillVFXManager* pVFXManager = pScene ? pScene->GetFluidVFXManager() : nullptr;
    ProjectileManager*    pProjManager = pScene ? pScene->GetProjectileManager() : nullptr;

    // 원격 플레이어 element
    ElementType remoteElement = ElementType::Water;
    {
        auto eIt = m_mapRemotePlayerElement.find(playerId);
        if (eIt != m_mapRemotePlayerElement.end())
            remoteElement = eIt->second;
    }

    auto spawnOneShot = [&](const char* effectName, const XMFLOAT3& origin, const XMFLOAT3& dir) -> int
    {
        if (!pVFXManager) return -1;
        EffectDef def = EffectRegistry::Get().GetEffect(effectName, RUNE_NONE);
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
            XMFLOAT3 fallPos   = XMFLOAT3(tgt.x, tgt.y + 5.5f, tgt.z);
            XMFLOAT3 puddlePos = XMFLOAT3(tgt.x, tgt.y + 2.5f, tgt.z);
            // 두 effect 모두 EffectDef.duration = -1 (offline 에선 behavior 가 수동 종료). 원격은 동일 시각에 자동 종료 필요.
            // FALL_DURATION=1.5s, DURATION=6.0s (WaterPuddleBehavior.h).
            int fallId = spawnOneShot("Q_WaterFall",   fallPos,   XMFLOAT3(0.0f, -1.0f, 0.0f));
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
            const int   SPIKE_COUNT   = 4;
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
                chanDir    = lookDir;
                effectName = "E_FireBeam_Core";
            }
            else // Water
            {
                // 송신 측이 dir 슬롯에 target 실어 보냄.
                XMFLOAT3 tgt = wireTarget;
                chanOrigin = XMFLOAT3(tgt.x, tgt.y + 3.0f, tgt.z);
                chanDir    = XMFLOAT3(0.0f, 1.0f, 0.0f);
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
            XMFLOAT3 ringPos  = XMFLOAT3(casterPos.x, casterPos.y,        casterPos.z);
            XMFLOAT3 backDir  = XMFLOAT3(-horizontalDir.x, 0.0f, -horizontalDir.z);
            spawnOneShot("E_GaleRush_Burst", burstPos, horizontalDir);
            spawnOneShot("E_GaleRush_Ring",  ringPos,  horizontalDir);
            spawnOneShot("E_GaleRush_Trail", burstPos, backDir);
        }
        else // Earth EarthArmor
        {
            // caster 위치(지면), up
            spawnOneShot("E_EarthArmor_Burst", casterPos, XMFLOAT3(0.0f, 1.0f, 0.0f));
            spawnOneShot("E_EarthArmor_Aura",  casterPos, XMFLOAT3(0.0f, 1.0f, 0.0f));
        }
        break;
    }

    case 3:  // R — wire dir 슬롯 = 절대 target 좌표 (정규화 아님)
    {
        XMFLOAT3 targetPos = XMFLOAT3(dirX, dirY, dirZ);

        // caster → target 평면 방향
        XMFLOAT3 toTarget = XMFLOAT3(targetPos.x - casterPos.x, 0.0f, targetPos.z - casterPos.z);
        {
            float L = sqrtf(toTarget.x*toTarget.x + toTarget.z*toTarget.z);
            if (L > 0.001f) { toTarget.x/=L; toTarget.z/=L; }
            else            { toTarget = XMFLOAT3(0.0f, 0.0f, 1.0f); }
        }

        switch (remoteElement)
        {
        case ElementType::Fire:
        {
            // Meteor 샤워 — 즉시 spawn 이 아니라 시간차 시뮬레이션 큐에 등록.
            // (MeteorBehavior::Update 와 동일 흐름: 6개 소형 0.5s 간격 → 마지막 후 0.5s → 최종 대형)
            constexpr int   SHOWER_COUNT       = 6;
            constexpr float SHOWER_INTERVAL    = 0.5f;
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
                float angle  = angleDist(rng);
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
            spawnOneShot("R_TidalWave",      casterHead, toTarget);
            spawnOneShot("R_TidalWave_Foam", casterHead, toTarget);
            break;
        }
        case ElementType::Wind:
        {
            // Tornado 는 Instant 활성화 (TornadoBehavior::DURATION=6.0). 송신 측이 후속 패킷 안 보냄.
            // 첫 패킷에 spawn 하고 6초 lifetime 부여. 이후 같은 player 가 R 보내면 TrackEffect 로 위치 갱신.
            XMFLOAT3 tornadoPos = XMFLOAT3(targetPos.x, 0.0f, targetPos.z);
            XMFLOAT3 upDir      = XMFLOAT3(0.0f, 1.0f, 0.0f);

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
            spawnOneShot("R_Earthquake_Ring",  casterPos, XMFLOAT3(0.0f, 1.0f, 0.0f));
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

        // Wind RC (WindShot) 만 관통 — WindShotBehavior.cpp:53 과 일치.
        // 나머지 (Fire/Water/Earth RC) 는 첫 충돌에 폭발 (오프라인과 동일).
        bool isPiercing = (remoteElement == ElementType::Wind);

        pProjManager->SpawnProjectile(
            projOrigin, projTarget,
            0.0f,                       // damage=0 — 서버 권위 (실제 데미지는 S_MONSTER_DAMAGE 로 적용)
            speed, radius, explosionRadius,
            remoteElement, pRemotePlayer,
            /*isPlayerProjectile*/true, scale,
            RuneCombo{}, 0.0f,
            /*maxDistance*/100.0f,
            /*isPiercing*/isPiercing
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
    case 2: // Melee → FireGolem_Rd
        return { "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd.bin",
                 "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/FireGolem_Rd/Textures/T_FireGolem_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death" };
    case 3: // Ranged → MagmaElemental_Rd
        return { "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd.bin",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/MagmaElemental_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/MagmaElemental_Rd/Textures/T_MagmaElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death" };
    case 4: // RushAoE → MoltenElemental_Rd
        return { "Assets/Enemies/Elementals/MoltenElemental_Rd/MoltenElemental_Rd.bin",
                 "Assets/Enemies/Elementals/MoltenElemental_Rd/MoltenElemental_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/MoltenElemental_Rd/Textures/T_MoltenElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death" };
    case 5: // RushFront → ChaosElemental_Rd
        return { "Assets/Enemies/Elementals/ChaosElemental_Rd/ChaosElemental_Rd.bin",
                 "Assets/Enemies/Elementals/ChaosElemental_Rd/ChaosElemental_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/ChaosElemental_Rd/Textures/T_ChaosElemental_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death" };
    case 6: // Dragon (Red)
        return { "Assets/Enemies/Dragon/Red.bin",
                 "Assets/Enemies/Dragon/Red_Anim.bin",
                 3.0f, "Idle01", "Walk", "",
                 "Flame Attack", "Die" };
    case 7: // Kraken
        return { "Assets/Enemies/Kraken/KRAKEN.bin",
                 "Assets/Enemies/Kraken/KRAKEN_Anim.bin",
                 3.0f, "Idle", "Walk", "",
                 "Attack_Forward_RM", "Death" };
    case 8: // Golem
        return { "Assets/Enemies/Golem/Golem01_Generic_prefab.bin",
                 "Assets/Enemies/Golem/Golem01_Generic_prefab_Anim.bin",
                 14.0f, "Golem_stand_ge", "Golem_battle_stand_ge",
                 "Assets/Enemies/Golem/Textures/chr_04_Golem_alb.png",
                 "Golem_battle_attack01_ge", "Golem_battle_die_ge" };;
    case 9: // Demon
        return { "Assets/Enemies/demon/Demon.bin",
                 "Assets/Enemies/demon/Demon_Anim.bin",
                 8.0f, "Idle1", "Run", "",
                 "attack1", "Death1" };
    case 10: // BlueDragon (EnemySpawner: idle="Idle", chase="Walk")
        return { "Assets/Enemies/Dragon_blue/Blue.bin",
                 "Assets/Enemies/Dragon_blue/Blue_Anim.bin",
                 3.0f, "Idle", "Walk", "",
                 "Fireball Shoot", "Die" };
    case 1: // TestEnemy — FireGolem_Rd 로 fallback (Melee 타입과 동일)
    default:
        return { "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd.bin",
                 "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd_Anim.bin",
                 5.5f, "idle", "Run_Forward",
                 "Assets/Enemies/Elementals/FireGolem_Rd/Textures/T_FireGolem_Rd_D.png",
                 "Combat_Unarmed_Attack", "Death" };
    }
}

// EnemySpawner::LoadTextureToHierarchy 미러 — 하이러키 순회하며 텍스처+흰 머티리얼 적용.
// 목적: MATERIAL이 garbage로 초기화되어 diffuse=0 → 메쉬가 까맣게 렌더되어 보이지 않는 문제 해결.
static void ApplyWhiteMaterialAndTextureToHierarchy(
    Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
    GameObject* pGO, const char* texturePath)
{
    if (!pGO || !pScene) return;

    if (pGO->GetMesh())
    {
        MATERIAL mat;
        mat.m_cAmbient  = XMFLOAT4(0.3f, 0.3f, 0.3f, 1.0f);
        mat.m_cDiffuse  = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
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

    if (pGO->m_pChild)   ApplyWhiteMaterialAndTextureToHierarchy(pScene, pDevice, pCommandList, pGO->m_pChild, texturePath);
    if (pGO->m_pSibling) ApplyWhiteMaterialAndTextureToHierarchy(pScene, pDevice, pCommandList, pGO->m_pSibling, texturePath);
}

void NetworkManager::ProcessMonsterSpawn(Scene* pScene, ID3D12Device* pDevice,
                                         ID3D12GraphicsCommandList* pCommandList,
                                         uint64 monsterId, uint32 monsterType,
                                         float x, float y, float z, float yaw,
                                         float hp, bool isBoss)
{
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

    MonsterPreset preset = GetMonsterPresetByType(monsterType);

    // 서버 몬스터는 로컬 Room에 속하지 않는 전역 오브젝트로 생성
    CRoom* pPrevRoom = pScene->GetCurrentRoom();
    pScene->SetCurrentRoom(nullptr);

    GameObject* pMonster = MeshLoader::LoadGeometryFromFile(
        pScene, pDevice, pCommandList, nullptr, preset.meshPath);

    pScene->SetCurrentRoom(pPrevRoom);

    if (!pMonster)
    {
        wchar_t buf[256];
        swprintf_s(buf, L"[Network] ProcessMonsterSpawn: mesh load FAILED type=%u path=%hs\n",
            monsterType, preset.meshPath);
        OutputDebugString(buf);
        return;
    }

    sprintf_s(pMonster->m_pstrFrameName, "NetMonster_%llu", monsterId);

    // 위치/회전/스케일
    TransformComponent* pT = pMonster->GetTransform();
    if (pT)
    {
        pT->SetPosition(x, y, z);
        pT->SetScale(preset.scale, preset.scale, preset.scale);
        // 서버가 yaw를 도(degree)로 보냄 → 그대로 사용 (이중 변환 버그 제거)
        pT->SetRotation(0.0f, yaw, 0.0f);
    }

    // 애니메이션
    auto* pAnim = pMonster->AddComponent<AnimationComponent>();
    if (pAnim)
    {
        pAnim->LoadAnimation(preset.animPath);
        pAnim->Play(preset.idleClip, true);
    }

    // 흰 머티리얼 + 텍스처 강제 적용 (MeshLoader가 .bin에서 세팅 안 했을 경우 대비)
    // → 서버 권위 스폰에서 유일하게 빠져 있던 스텝. EnemySpawner::LoadTextureToHierarchy 미러.
    ApplyWhiteMaterialAndTextureToHierarchy(pScene, pDevice, pCommandList, pMonster, preset.texturePath);

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
        clips.idle        = preset.idleClip;
        clips.walk        = preset.walkClip;
        clips.attack      = preset.attackClip;
        clips.death       = preset.deathClip;
        clips.monsterType = monsterType;
        clips.isBoss      = isBoss;
        m_mapServerMonsterClips[monsterId] = clips;
    }

    // 보스 인디케이터 4개 사전 할당 (Circle border/fill, Box border/fill) — 모두 hidden.
    //   ProcessMonsterAttack 에서 (monsterType, attackType) 로 타입/크기 결정해서 위치/스케일 갱신.
    if (isBoss)
    {
        if (EnemySpawner* pSpawner = pScene ? pScene->GetEnemySpawner() : nullptr)
        {
            auto set = pSpawner->CreateNetBossIndicators();
            ServerMonsterIndicators ind;
            ind.circleBorder = set.circleBorder;
            ind.circleFill   = set.circleFill;
            ind.boxBorder    = set.boxBorder;
            ind.boxFill      = set.boxFill;
            HideMonsterIndicators(ind);
            m_mapServerMonsterIndicators[monsterId] = ind;
        }
    }

    // 보간 타겟 초기값 = 스폰 위치 (첫 MOVE 전까진 제자리)
    ServerMonsterTarget initTgt;
    initTgt.px = x; initTgt.py = y; initTgt.pz = z;
    initTgt.yaw = yaw;
    initTgt.hasTarget = true;
    m_mapServerMonsterTarget[monsterId] = initTgt;

    // 보스면 spawn 위치 영구 보존 — MegaBreath cover 좌표 기준점
    if (isBoss)
    {
        ServerBossSpawnPos sp{ x, y, z };
        m_mapServerBossSpawnPos[monsterId] = sp;
    }

    // 디버그: 실제 배치된 transform과 preset 클립 확인 (VS Output + file 둘 다)
    XMFLOAT3 finalPos = pT ? pT->GetPosition() : XMFLOAT3{0,0,0};
    XMFLOAT3 finalRot = pT ? pT->GetRotation() : XMFLOAT3{0,0,0};
    XMFLOAT3 finalSca = pT ? pT->GetScale()    : XMFLOAT3{1,1,1};
    wchar_t wbuf[512];
    swprintf_s(wbuf, L"[Network] Spawned NetMonster_%llu type=%u boss=%d hp=%.1f\n"
                     L"  pos=(%.2f,%.2f,%.2f) rot=(%.1f,%.1f,%.1f) scale=(%.2f,%.2f,%.2f)\n"
                     L"  idleClip=%hs walkClip=%hs mesh=%hs\n",
        monsterId, monsterType, isBoss ? 1 : 0, hp,
        finalPos.x, finalPos.y, finalPos.z,
        finalRot.x, finalRot.y, finalRot.z,
        finalSca.x, finalSca.y, finalSca.z,
        preset.idleClip, preset.walkClip, preset.meshPath);
    OutputDebugString(wbuf);

    char abuf[512];
    sprintf_s(abuf, "[Network] Spawned NetMonster_%llu type=%u boss=%d hp=%.1f | pos=(%.2f,%.2f,%.2f) rot=(%.1f,%.1f,%.1f) scale=(%.2f,%.2f,%.2f) | idleClip=%s walkClip=%s mesh=%s",
        monsterId, monsterType, isBoss ? 1 : 0, hp,
        finalPos.x, finalPos.y, finalPos.z,
        finalRot.x, finalRot.y, finalRot.z,
        finalSca.x, finalSca.y, finalSca.z,
        preset.idleClip, preset.walkClip, preset.meshPath);
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
    if (pScene && pScene->IsNetworkKrakenCutsceneTarget(monsterId))
    {
        // Kraken 2페이즈 컷신 중에는 Scene 컷신 상태머신이 위치를 직접 제어한다.
        // 서버 S_MONSTER_MOVE가 컷신 위치를 덮어쓰면 Kraken이 안 보이거나
        // 컷신 종료 후 텔레포트처럼 보이므로 MOVE는 무시한다.
        return;
    }

    auto it = m_mapServerMonsters.find(monsterId);
    if (it == m_mapServerMonsters.end())
        return;

    GameObject* pMonster = it->second;

    // 직접 SetPosition하지 않고 타겟만 갱신. InterpolateServerMonsters에서 부드럽게 접근.
    ServerMonsterTarget& tgt = m_mapServerMonsterTarget[monsterId];
    tgt.px = x; tgt.py = y; tgt.pz = z;
    tgt.yaw = yaw;
    if (!tgt.hasTarget)
    {
        // 첫 패킷은 즉시 스냅 (스폰 직후 0,0,0에서 시작하지 않게)
        TransformComponent* pT = pMonster->GetTransform();
        if (pT)
        {
            pT->SetPosition(x, y, z);
            XMFLOAT3 rot = pT->GetRotation();
            pT->SetRotation(rot.x, yaw, rot.z);
        }
        tgt.hasTarget = true;
    }

    // 공격 애니 재생 중이면 walk 로 덮어쓰지 않음 — 자연스러운 전환
    bool bAttackLocked = false;
    {
        auto atkIt = m_mapServerMonsterAttackTimer.find(monsterId);
        if (atkIt != m_mapServerMonsterAttackTimer.end() && atkIt->second > 0.0f)
            bAttackLocked = true;
    }

    // 죽은 몬스터는 death 애니 유지 — walk 로 덮지 않음 (despawn 대기 중 2s 동안 이동 패킷 올 수 있음)
    bool bDead = (m_setDeadServerMonsters.find(monsterId) != m_setDeadServerMonsters.end());

    // 걷기 애니메이션 부드럽게 전환 — preset별 walk 클립 이름 사용
    auto* pAnim = pMonster->GetComponent<AnimationComponent>();
    if (pAnim && !bAttackLocked && !bDead)
    {
        auto clipIt = m_mapServerMonsterClips.find(monsterId);
        const char* walkClip = (clipIt != m_mapServerMonsterClips.end())
            ? clipIt->second.walk.c_str() : "Walk";
        pAnim->CrossFade(walkClip, 0.1f, true);
    }

    m_mapServerMonsterMoveTime[monsterId] = 0.0f;
}

void NetworkManager::CheckServerMonsterIdle(float deltaTime)
{
    // (0) Hit flash 페이드아웃 — SetHitFlashAll 이 자동 감쇠 안 하므로 수동 tick (원격 플레이어와 동일 패턴)
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
        if (it->second <= 0.0f) it = m_mapServerMonsterHitFlashTimer.erase(it);
        else ++it;
    }

    // (1) 공격 애니 타이머 감소 — 0 되면 idle 로 자동 복귀 (Move 안 오고 공격만 끝난 경우)
    for (auto it = m_mapServerMonsterAttackTimer.begin(); it != m_mapServerMonsterAttackTimer.end(); )
    {
        it->second -= deltaTime;
        if (it->second <= 0.0f)
        {
            auto mIt = m_mapServerMonsters.find(it->first);
            // 죽은 몬스터는 death 애니 유지 — idle 로 덮지 않음
            bool bDead = (m_setDeadServerMonsters.find(it->first) != m_setDeadServerMonsters.end());
            if (mIt != m_mapServerMonsters.end() && !bDead)
            {
                auto* pAnim = mIt->second->GetComponent<AnimationComponent>();
                if (pAnim)
                {
                    auto clipIt = m_mapServerMonsterClips.find(it->first);
                    const char* idleClip = (clipIt != m_mapServerMonsterClips.end())
                        ? clipIt->second.idle.c_str() : "Idle";
                    pAnim->CrossFade(idleClip, 0.15f, true);
                }
            }
            it = m_mapServerMonsterAttackTimer.erase(it);
        }
        else ++it;
    }

    // (2) 기존: Move 후 일정 시간 idle 전환
    for (auto it = m_mapServerMonsterMoveTime.begin(); it != m_mapServerMonsterMoveTime.end(); )
    {
        it->second += deltaTime;
        if (it->second >= IDLE_TRANSITION_TIME)
        {
            auto mIt = m_mapServerMonsters.find(it->first);
            bool bDead = (m_setDeadServerMonsters.find(it->first) != m_setDeadServerMonsters.end());
            if (mIt != m_mapServerMonsters.end() && !bDead)
            {
                // 공격 애니 재생 중이면 건드리지 않음 (공격 타이머 쪽이 마무리함)
                auto atkIt = m_mapServerMonsterAttackTimer.find(it->first);
                bool bAttackLocked = (atkIt != m_mapServerMonsterAttackTimer.end() && atkIt->second > 0.0f);

                auto* pAnim = mIt->second->GetComponent<AnimationComponent>();
                if (pAnim && !bAttackLocked)
                {
                    auto clipIt = m_mapServerMonsterClips.find(it->first);
                    const char* idleClip = (clipIt != m_mapServerMonsterClips.end())
                        ? clipIt->second.idle.c_str() : "Idle";
                    pAnim->CrossFade(idleClip, 0.2f, true);
                }
            }
            it = m_mapServerMonsterMoveTime.erase(it);
        }
        else ++it;
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
        return 1.0f;
    case 8: // TailSweep — 빠른 휩쓸기 (서버 windup 0)
        return 0.3f;
    case 9: // GroundRupture — 콤보 첫 hit
        return 0.5f;
    case 4: // RushFront — 즉시 (이동기)
        return 0.0f;
    case 10: // FlyingBarrage — 약간 지연
        return 0.5f;
    case 2: // Ranged
        return 0.3f;

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

    case 7: // Kraken — preset ForwardBox 14×30
        switch (attackType)
        {
        case 5:  // Breath (잉크) — 좁은 spread 면 표시, 넓으면 억제. 서버는 spread 안 보내므로 표시.
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 14.0f; p.length = 30.0f; break;
        case 8:  // TailSweep
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 14.0f; p.length = 30.0f; break;
        case 9:  // GroundRupture (3연타 콤보)
            p.type = NetworkManager::NetIndicatorType::ForwardBox;
            p.radius = 14.0f; p.length = 30.0f; break;
        default: p.type = NetworkManager::NetIndicatorType::None; break;
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
            // GolemJumpShock
            // JumpSlamAttackBehavior 기반이라 원형 영역이 필요함
            p.type = NetworkManager::NetIndicatorType::Circle;
            p.radius = 55.0f;
            break;

        case 22:
            // GolemWideSlam
            // JumpSlamAttackBehavior 기반이라 원형 영역이 필요함
            p.type = NetworkManager::NetIndicatorType::Circle;
            p.radius = 75.0f;
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

    case 10: // BlueDragon — preset Circle r=14
        switch (attackType)
        {
        case 5:  p.type = NetworkManager::NetIndicatorType::Circle; p.radius = 14.0f; break;
        case 7:  p.type = NetworkManager::NetIndicatorType::Circle; p.radius = 9.0f;  break;
        case 8:  p.type = NetworkManager::NetIndicatorType::Circle; p.radius = 14.0f; break;
        case 4:  p.type = NetworkManager::NetIndicatorType::None;   break;
        default: p.type = NetworkManager::NetIndicatorType::Circle; p.radius = 14.0f; break;
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
                                float len = sqrtf(dx*dx + dz*dz);
                                if (len < 0.001f) { dx = 0.f; dz = 1.f; }
                                else              { dx /= len; dz /= len; }
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
                    float len = sqrtf(dx*dx + dz*dz);
                    if (len < 0.001f) { dx = 0.f; dz = 1.f; }
                    else              { dx /= len; dz /= len; }
                    float ang = it->fanAngleDeg * (3.14159265f / 180.0f);
                    float c = cosf(ang), s = sinf(ang);
                    DirectX::XMFLOAT3 dir{ dx * c - dz * s, 0.f, dx * s + dz * c };

                    // EffectDef — 오프라인 MegaBreathAttackBehavior::SpawnFireWave 와 동일
                    EffectDef def;
                    def.name    = "Net_MegaBreath";
                    def.element = ElementType::Fire;

                    EffectLayer layer;
                    layer.type      = EmitterType::SPH_Beam;
                    layer.element   = ElementType::Fire;
                    layer.coreColor = { 1.0f, 0.45f, 0.10f, 1.0f };
                    layer.edgeColor = { 0.95f, 0.35f, 0.08f, 0.95f };
                    layer.useSSF    = true;

                    SPHEmitterParams& sph = layer.sph;
                    sph.particleCount = it->beamParticleCount;
                    sph.spawnRadius   = (it->fanAngleDeg == 0.0f) ? 3.0f : 2.5f;
                    sph.particleSize  = 1.8f;

                    VFXPhase phase;
                    phase.startTime = 0.f;
                    phase.duration  = it->beamDuration + 0.5f;
                    phase.motionMode = ParticleMotionMode::Beam;
                    phase.beamDesc.beamLength    = it->beamLength;
                    phase.beamDesc.spreadRadius  = 6.0f * it->beamSpreadMult;
                    phase.beamDesc.speedMin      = it->beamLength / (it->beamDuration * 0.7f);
                    phase.beamDesc.speedMax      = phase.beamDesc.speedMin * 1.4f;
                    phase.beamDesc.swirlExpand   = true;
                    phase.beamDesc.swirlSpeed    = 0.6f;
                    phase.beamDesc.swirlFadeEnd  = 0.f;
                    phase.beamDesc.enableFlow    = true;
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
                if (ind.activeType == NetIndicatorType::ForwardBox)
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
                    mat.m_cAmbient  = XMFLOAT4(0.6f, 0.02f, 0.02f, 1.0f);
                    mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.15f, 0.1f,  1.0f);
                    mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f,  0.0f,  1.0f);
                    mat.m_cEmissive = XMFLOAT4(3.5f, 0.4f, 0.15f, 1.0f);   // 2.0→3.5 더 밝게
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
                    mat.m_cAmbient  = XMFLOAT4(0.3f, 0.02f, 0.0f, 1.0f);
                    mat.m_cDiffuse  = XMFLOAT4(1.0f, 0.2f + 0.6f * fillProgress, 0.05f, 1.0f);
                    mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                    // 처음 0.5 → 끝 2.5 (붉음 → 노란 가열)
                    mat.m_cEmissive = XMFLOAT4(
                        0.5f + 2.0f * fillProgress,
                        0.1f + 1.4f * fillProgress,
                        0.05f, 1.0f);
                    ind.circleFill->SetMaterial(mat);
                }
            }
        }
        else if (ind.activeType == NetIndicatorType::ForwardBox)
        {
            // 작은 박스도 잘 보이게 최소 반폭/길이 보장
            float fHalfW = (ind.hitRadius < 5.0f) ? 5.0f : ind.hitRadius;
            float fLen   = (ind.hitLength < 10.0f) ? 10.0f : ind.hitLength;
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
    // monsterType: 6 Dragon, 7 Kraken, 8 Golem, 9 Demon, 10 BlueDragon
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
    default:
        return nullptr;  // 일반 몹 → preset 기본
    }
}

void NetworkManager::ProcessMonsterAttack(Scene* pScene, uint64 monsterId, uint32 attackType, float windupSec, uint64 targetPlayerId, float atkX, float atkY, float atkZ, const std::vector<DirectX::XMFLOAT3>& effectPositions, uint32 effectOption)
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

    GameObject* pMonster = it->second;
    auto* pAnim = pMonster->GetComponent<AnimationComponent>();
    if (!pAnim) return;

    // (monsterType, attackType) 별 클립 우선 사용. 매핑 없으면 preset 의 기본 attack 클립으로 폴백.
    auto clipIt = m_mapServerMonsterClips.find(monsterId);
    uint32 mt = (clipIt != m_mapServerMonsterClips.end()) ? clipIt->second.monsterType : 0;
    const char* perTypeClip = GetMonsterAttackClipForType(mt, attackType);
    const char* attackClip =
        (perTypeClip && perTypeClip[0] != '\0') ? perTypeClip
        : ((clipIt != m_mapServerMonsterClips.end() && !clipIt->second.attack.empty())
            ? clipIt->second.attack.c_str() : "Attack");

    pAnim->CrossFade(attackClip, 0.1f, false, true);  // forceRestart — 연속 공격도 처음부터

    // 공격 애니 지속 시간 등록 — 이 기간 Move 왔을 때 walk 로 덮지 않음
    //  서버 windupSec(예고) + 추정 재생시간. 짧은 windup 공격도 최소 ATTACK_ANIM_LOCK 은 유지
    float lockDur = fmaxf(windupSec + 0.4f, ATTACK_ANIM_LOCK);

    // Golem은 공격 모션이 길어서 서버 windupSec 기준으로 idle 복귀하면 모션이 중간에 끊김
    if (mt == 8)
    {
        switch (attackType)
        {
        case 21: // GolemJumpShock
            lockDur = 2.8f;
            break;

        case 22: // GolemWideSlam
            lockDur = 4.8f;
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
            ind.activeType  = params.type;
            ind.windupTotal = windupSec;
            ind.windupTimer = 0.0f;
            ind.hitRadius   = params.radius;
            ind.hitLength   = params.length;
            ind.tint        = params.tint;
            ind.attackType  = attackType;
            ind.anchorX = atkX; ind.anchorY = atkY; ind.anchorZ = atkZ;
            // ForwardBox: 보스의 현재 yaw 사용 (transform 에서 직접 읽기)
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
                { targetPos = pT->GetPosition(); targetPos.y += 1.5f; bHasTarget = true; }
        }
        else
        {
            auto rIt = m_mapRemotePlayers.find(targetPlayerId);
            if (rIt != m_mapRemotePlayers.end() && rIt->second)
                if (auto* pT = rIt->second->GetTransform())
                { targetPos = pT->GetPosition(); targetPos.y += 1.5f; bHasTarget = true; }
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
            float halfSpread   = (count > 1) ? (spreadDeg * 0.5f) : 0.0f;
            float fireInterval = (count > 0) ? (dur / (float)count) : 0.0f;

            for (int i = 0; i < count; ++i)
            {
                float t = (count > 1) ? ((float)i / (count - 1)) : 0.5f;
                float angDeg = -halfSpread + spreadDeg * t;

                PendingMonsterVFX p;
                p.kind        = PendingVFXKind::Projectile;
                p.delay       = startDelay + i * fireInterval;
                p.monsterId   = monsterId;          // 실시간 위치 추적
                p.startPos    = startPos;           // fallback (보스 사라지면 사용)
                p.targetPos   = targetPos;          // 패킷 시점 타겟 위치
                p.yOffset     = 2.0f;
                p.fanAngleDeg = angDeg;
                p.fireRange   = 60.0f;
                p.speed     = speed;
                p.radius    = radius;
                p.scale     = scale;
                p.maxDist   = maxDist;
                p.element   = elem;
                m_vPendingMonsterVFX.push_back(p);
            }
        };

        auto QueueExplosion = [&](const XMFLOAT3& pos, float delaySec)
        {
            PendingMonsterVFX p;
            p.kind     = PendingVFXKind::Explosion;
            p.delay    = delaySec;
            p.startPos = pos;
            p.element  = elem;
            m_vPendingMonsterVFX.push_back(p);
        };

        auto QueueShake = [&](float delaySec, float intensity, float duration)
        {
            PendingMonsterVFX p;
            p.kind     = PendingVFXKind::CameraShake;
            p.delay    = delaySec;
            p.shakeIntensity = intensity;
            p.shakeDuration  = duration;
            m_vPendingMonsterVFX.push_back(p);
        };

        // VFX 가 실제 스폰될 delay — 서버 windupSec 가 아닌 애니 release frame 기준 (sync 문제 해결).
        //   서버 windupSec 는 데미지 타이밍이고 클라 애니 peak 와 다름. 위 GetVfxStartDelay 참고.
        const float startDelay = GetVfxStartDelay(mt, attackType, windupSec);

        switch (attackType)
        {
        case 2:   // Ranged — 단발 직선
            {
                PendingMonsterVFX p;
                p.kind = PendingVFXKind::Projectile;
                p.delay = startDelay;
                p.startPos = startPos; p.targetPos = targetPos;
                p.speed = 18.0f; p.radius = 0.5f; p.scale = 1.0f; p.maxDist = 80.0f;
                p.element = elem;
                m_vPendingMonsterVFX.push_back(p);
            }
            break;

        case 5:   // Breath
            // Red Dragon (6): 5발 50° 큰 화염 — 전투 중 잘 보이게 scale 3.0
            // Kraken  (7):   10발 55° 잉크 다발 — 다발이라 개별 scale 1.5
            // BlueDragon(10): 5발 50° scale 2.5
            if (mt == 6)      QueueFan(5,  50.0f, 35.0f, 1.2f, 3.0f, 70.0f, startDelay, 0.8f);
            else if (mt == 7) QueueFan(10, 55.0f, 32.0f, 0.8f, 1.5f, 60.0f, startDelay, 1.1f);
            else              QueueFan(5,  50.0f, 35.0f, 1.0f, 2.5f, 70.0f, startDelay, 0.8f);
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
                case 0: wallPos.x =  69.95f; wallPos.z =   6.71f; break; // +X (84.95-15)
                case 1: wallPos.x = -129.95f; wallPos.z =   6.71f; break; // -X (-144.95+15)
                case 2: wallPos.x = -30.0f;  wallPos.z = 113.40f; break; // +Z (128.40-15)
                default: wallPos.x = -30.0f; wallPos.z = -99.95f; break; // -Z (-114.95+15)
                }
                cs.wallPos = wallPos;
                cs.bossSpawnPos = XMFLOAT3{ -30.0f, 0.0f, 6.71f }; // 룸 중심 game world = cover/카메라 기준
                cs.active = true;
                m_mapServerMegaBreathCutscenes[monsterId] = std::move(cs);

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
            QueueExplosion(XMFLOAT3{ atkX, atkY + 0.2f, atkZ }, startDelay);
            QueueShake(startDelay, 2.5f, 0.5f);
            // 보스 점프 액션 — 포물선 yOffset (서버는 XZ 이미 텔레포트됨)
            {
                ServerBossAction act;
                act.kind = BossActionKind::Jump;
                act.timer = 0.0f;
                act.duration = startDelay > 0.f ? startDelay : 1.0f; // windup 만큼 점프
                act.peakHeight = 8.0f; // 점프 정점 높이
                m_mapServerBossActions[monsterId] = act;
            }
            break;

        case 8:   // TailSweep
            if (mt == 6)
            {
                // Red Dragon — windupSec=0 (서버 default) 라 즉시 임팩트.
                //   보스 발 밑 폭발 + 좌우로 호 그리며 여러 폭발 (꼬리가 휩쓰는 호)
                QueueExplosion(XMFLOAT3{ atkX, atkY + 0.2f, atkZ }, startDelay);
                // 보스 yaw 기준 정면 ±90° 호로 4개 폭발 (꼬리 sweep 시뮬레이션)
                if (auto* pT = pMonster->GetTransform())
                {
                    float yawRad = pT->GetRotation().y * (3.14159265f / 180.0f);
                    for (int i = 0; i < 4; ++i)
                    {
                        float angle = yawRad + (-1.5708f + (i / 3.0f) * 3.1416f);
                        float r = 8.0f;
                        DirectX::XMFLOAT3 ep{ atkX + sinf(angle) * r, atkY + 0.2f, atkZ + cosf(angle) * r };
                        QueueExplosion(ep, startDelay + 0.05f * i);
                    }
                }
                QueueShake(startDelay, 2.0f, 0.4f);
            }
            // 다른 보스는 애니메이션만 (인디케이터로 대체)
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

void NetworkManager::QueueBossEvent(uint64 monsterId, uint32 eventType, uint32 phaseIndex)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    NetworkCommandData cmd{};
    cmd.type           = NetworkCommand::BossEvent;
    cmd.monsterId      = monsterId;
    cmd.bossEventType  = eventType;
    cmd.phaseIndex     = phaseIndex;

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
    case 9:  return "Idle1";         // Demon — 별도 포효 클립 없음
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
            st.phase       = BossIntroPhase::FlyingIn;
            st.phaseTimer  = 0.0f;
            st.startHeight = 25.0f;  // 오프라인 Scene.cpp 와 동일 (Red Dragon 25u)

            if (auto* pT = pBoss->GetTransform())
            {
                XMFLOAT3 p = pT->GetPosition();
                st.bossX   = p.x;
                st.bossZ   = p.z;
                st.groundY = p.y;            // 스폰 좌표 = 지면 y
                st.curY    = p.y + st.startHeight; // 시작은 25u 위
                pT->SetPosition(p.x, st.curY, p.z);
            }
            st.active = true;
            m_mapServerBossIntros[monsterId] = st;

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
    case 21:
        // GolemJumpShock - 작은 원형 충격파
        behavior = std::make_unique<JumpSlamAttackBehavior>(
            140.0f,
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
            150.0f,
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
            90.0f,
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
            90.0f,
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
            100.0f,
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
            55.0f,
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

void NetworkManager::ProcessMonsterDamage(Scene* pScene, uint64 monsterId, float damage,
                                          float currentHp, bool isDead,
                                          uint64 attackerPlayerId, int skillType)
{
    if (!pScene) return;

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

    if (isDead)
    {
        // 사망 애니 재생 (preset deathClip) — 이후 MonsterMove/Attack 전환 skip
        auto* pAnim = pMonster->GetComponent<AnimationComponent>();
        auto clipIt = m_mapServerMonsterClips.find(monsterId);
        if (pAnim && clipIt != m_mapServerMonsterClips.end() && !clipIt->second.death.empty())
        {
            pAnim->CrossFade(clipIt->second.death.c_str(), 0.15f, false, true);
        }

        m_setDeadServerMonsters.insert(monsterId);
        m_mapServerMonsterMoveTime.erase(monsterId);
        m_mapServerMonsterAttackTimer.erase(monsterId);
    }

    char buf[192];
    sprintf_s(buf, "[Network] MonsterDamage applied: id=%llu dmg=%.1f hp=%.1f dead=%d attacker=%llu skill=%d",
              monsterId, damage, currentHp, isDead ? 1 : 0, attackerPlayerId, skillType);
    WriteNetworkLog(buf);
}

void NetworkManager::ProcessRoomCleared(Scene* pScene, uint32 stageIndex, uint32 roomIndex)
{
    if (!pScene) return;

    // 현재 로컬 방을 Cleared 상태로 마크하고 포탈 큐브 스폰 (오프라인 경로와 동일 연출)
    CRoom* pRoom = pScene->GetCurrentRoom();
    if (pRoom)
    {
        if (pRoom->GetState() != RoomState::Cleared)
            pRoom->SetState(RoomState::Cleared);

        if (!pRoom->HasPortalCube())
            pRoom->SpawnPortalCube();
    }

    char buf[128];
    sprintf_s(buf, "[Network] RoomCleared applied: stage=%u room=%u portalSpawned=%d",
              stageIndex, roomIndex, (pRoom && pRoom->HasPortalCube()) ? 1 : 0);
    WriteNetworkLog(buf);
}

void NetworkManager::ProcessMonsterDespawn(Scene* pScene, uint64 monsterId)
{
    auto it = m_mapServerMonsters.find(monsterId);
    if (it == m_mapServerMonsters.end())
        return;

    GameObject* pMonster = it->second;
    pScene->MarkForDeletion(pMonster);
    m_mapServerMonsters.erase(it);
    m_mapServerMonsterMoveTime.erase(monsterId);
    m_mapServerMonsterClips.erase(monsterId);
    m_mapServerMonsterTarget.erase(monsterId);
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
    constexpr float TAKEOFF_TIME    = 0.9f;
    constexpr float MOVE_TIME       = 3.0f;
    constexpr float LANDING_TIME    = 0.7f;
    constexpr float COVER_TIME      = 1.2f;
    constexpr float WINDUP_TIME     = 5.5f;
    constexpr float BREATH_TIME     = 6.5f;
    constexpr float RECOVERY_TIME   = 1.2f;
    constexpr float RETTAKEOFF_TIME = 0.9f;
    constexpr float RETFLY_TIME     = 3.0f;
    constexpr float RETLAND_TIME    = 0.7f;
    constexpr float FLY_HEIGHT      = 18.0f;
    constexpr float COVER_DIST      = 57.5f;  // game world: 11.5(JSON) * MAP_SCALE(5) = 57.5
    constexpr float COVER_SCALE     = 5.0f;
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
            cs.camDist  += (tgtDist  - cs.camDist) * rate;
            cs.camPitch += (tgtPitch - cs.camPitch) * rate;
            float yawDelta = tgtYaw - cs.camYaw;
            while (yawDelta >  180.f) yawDelta -= 360.f;
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
            float radius = sqrtf(dxc*dxc + dzc*dzc);
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
            // 이동 방향 바라봄
            float dx = cs.wallPos.x - p.x, dz = cs.wallPos.z - p.z;
            if (fabsf(dx) + fabsf(dz) > 0.01f)
                pT->SetRotation(0.f, atan2f(dx, dz) * (180.f / 3.14159265f), 0.f);

            float dxc = p.x - roomCenter.x, dzc = p.z - roomCenter.z;
            float radius = sqrtf(dxc*dxc + dzc*dzc);
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
            while (diff >  180.f) diff -= 360.f;
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
                        { pCT->SetPosition(coverPos[i].x, coverPos[i].y, coverPos[i].z);
                          pCT->SetScale(COVER_SCALE, COVER_SCALE, COVER_SCALE);
                          pCT->SetRotation(0.0f, 0.0f, 0.0f); } // 명시적 회전 0 (역방향 mesh 방지)
                        Mesh* pMesh = MapLoader::LoadMesh("Assets/MapData/meshes/ColumnBig_001.obj", pDev, pCmd);
                        if (pMesh) { pMesh->AddRef(); pCover->SetMesh(pMesh); }
                        MATERIAL mat;
                        mat.m_cAmbient  = XMFLOAT4(0.3f, 0.3f, 0.3f, 1.0f);
                        mat.m_cDiffuse  = XMFLOAT4(0.7f, 0.7f, 0.7f, 1.0f);
                        mat.m_cSpecular = XMFLOAT4(0.2f, 0.2f, 0.2f, 8.0f);
                        mat.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                        pCover->SetMaterial(mat);
                        auto* pRC = pCover->AddComponent<RenderComponent>();
                        if (pMesh) pRC->SetMesh(pMesh);
                        pShader->AddRenderComponent(pRC);
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
                // 보스 회전 — 방 중심 향함 (cover/wall 기준)
                float dxc = roomCenter.x - dragonPos.x, dzc = roomCenter.z - dragonPos.z;
                if (fabsf(dxc) + fabsf(dzc) > 0.01f)
                    pT->SetRotation(0.f, atan2f(dxc, dzc) * (180.f / 3.14159265f), 0.f);
            }
            break;
        }
        case MegaBreathPhase::Windup:
        {
            // over-shoulder 카메라 — lookAt = 플레이어, yaw = (player - dragon)
            float ddx = playerPos.x - dragonPos.x, ddz = playerPos.z - dragonPos.z;
            float yaw = atan2f(ddx, ddz) * (180.f / 3.14159265f);
            BlendCamera(XMFLOAT3{ playerPos.x, playerPos.y + 1.0f, playerPos.z }, 62.f, 55.f, yaw, 2.0f);

            if (cs.phaseTimer >= WINDUP_TIME)
            {
                cs.phase = MegaBreathPhase::Breath;
                cs.phaseTimer = 0.f;
                if (pAnim) pAnim->CrossFade("Flame Attack", 0.2f, true);
                if (pCam) pCam->StartShake(2.5f, BREATH_TIME);
                WriteNetworkLog("[Network] MegaBreath phase -> Breath (5-fan SPH spawn)");

                // 5-fan SPH beam spawn (오프라인 SpawnFireWave 1:1)
                if (auto* pFluidVFX = pScene ? pScene->GetFluidVFXManager() : nullptr)
                {
                    // 오프라인 SpawnFireWave: 보스 yaw 기준 입 위치 (전방 17u, 머리 7u 위)
                    XMFLOAT3 bossRot = pT->GetRotation();
                    float yawRad = bossRot.y * (3.14159265f / 180.f);
                    XMFLOAT3 forward{ sinf(yawRad), 0.f, cosf(yawRad) };
                    XMFLOAT3 mouth{
                        dragonPos.x + forward.x * 17.0f,
                        dragonPos.y + 7.0f,
                        dragonPos.z + forward.z * 17.0f
                    };

                    const float beamAngles[5]      = { -12.f, -6.f, 0.f, 6.f, 12.f };
                    const int   beamParticles[5]   = { 2800, 3600, 4400, 3600, 2800 };
                    const float beamSpreadMults[5] = { 0.85f, 0.95f, 1.0f, 0.95f, 0.85f };
                    for (int i = 0; i < 5; ++i)
                    {
                        float a = beamAngles[i] * (3.14159265f / 180.f);
                        float c = cosf(a), s = sinf(a);
                        XMFLOAT3 dir{ forward.x * c - forward.z * s, 0.f, forward.x * s + forward.z * c };

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
                        sph.particleCount = beamParticles[i];
                        sph.spawnRadius   = (i == 2) ? 3.0f : 2.5f;
                        sph.particleSize  = 1.8f;
                        VFXPhase ph;
                        ph.startTime = 0.f;
                        ph.duration = BREATH_TIME + 0.5f;
                        ph.motionMode = ParticleMotionMode::Beam;
                        // 오프라인 game world: sqrt((2*114.95)² + (2*121.675)²) * 0.9 ≈ 301u
                        ph.beamDesc.beamLength    = 301.0f;
                        // 오프라인 perpExtent * 1.4 = max(114.95, 121.675) * 1.4 ≈ 170u
                        ph.beamDesc.spreadRadius  = 170.0f * beamSpreadMults[i];
                        ph.beamDesc.speedMin      = 301.0f / (BREATH_TIME * 0.7f);
                        ph.beamDesc.speedMax      = ph.beamDesc.speedMin * 1.4f;
                        ph.beamDesc.swirlExpand   = true;
                        ph.beamDesc.swirlSpeed    = 0.6f;
                        ph.beamDesc.swirlFadeEnd  = 0.f;
                        ph.beamDesc.enableFlow    = true;
                        ph.beamDesc.verticalScale = 0.18f;
                        sph.phases.push_back(ph);
                        sph.maxParticleSpeed = ph.beamDesc.speedMax * 1.2f;
                        def.layers.push_back(std::move(layer));

                        // isPlayerEffect=true 필수 — SSF 파이프라인 (RenderDepth→Blur→Composite) 거쳐야 매끈한 빔
                        // false 면 RenderEnemyEffects 빌보드만 호출돼 안 보이거나 구슬처럼 됨 (오프라인 동일)
                        cs.beamVFXIds[i] = pFluidVFX->SpawnEffectDef(mouth, dir, def, true);
                    }
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
                pT->SetRotation(0.f, atan2f(dx, dz) * (180.f / 3.14159265f), 0.f);
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
            { tIt->second.px = cs.originalPos.x; tIt->second.py = cs.originalPos.y; tIt->second.pz = cs.originalPos.z; }
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
        auto* pCam  = pScene->GetCamera();
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

    //case 28: // DemonShortRush
    //    behavior = std::make_unique<RushFrontAttackBehavior>(
    //        55.0f,
    //        28.0f, 0.85f,
    //        0.25f, 0.15f, 1.0f,
    //        8.5f, 75.0f
    //    );
    //    break;

    //case 29: // DemonLongRush
    //    behavior = std::make_unique<RushFrontAttackBehavior>(
    //        70.0f,
    //        34.0f, 1.2f,
    //        0.30f, 0.20f, 1.2f,
    //        10.5f, 95.0f
    //    );
    //    break;

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
    // 각 몬스터의 현재 transform을 타겟을 향해 exponential smoothing.
    // 서버 MOVE 패킷이 띄엄띄엄 와도 움직임은 부드럽게 이어짐.
    constexpr float POS_SMOOTH_RATE = 12.0f;  // 높을수록 빨리 따라감 (클 수록 덜 부드러움)
    constexpr float YAW_SMOOTH_RATE = 10.0f;

    const float posAlpha = 1.0f - expf(-POS_SMOOTH_RATE * deltaTime);
    const float yawAlpha = 1.0f - expf(-YAW_SMOOTH_RATE * deltaTime);

    for (auto& kv : m_mapServerMonsterTarget)
    {
        uint64 monsterId = kv.first;
        const ServerMonsterTarget& tgt = kv.second;
        if (!tgt.hasTarget) continue;

        Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
        if (pScene && pScene->IsNetworkKrakenCutsceneTarget(monsterId))
        {
            continue;
        }

        // 인트로/메가브레스 컷신 진행 중인 보스는 보간 스킵 — Update*가 위치 직접 제어
        if (m_mapServerBossIntros.find(monsterId) != m_mapServerBossIntros.end())
            continue;
        if (m_mapServerMegaBreathCutscenes.find(monsterId) != m_mapServerMegaBreathCutscenes.end())
            continue;

        auto mIt = m_mapServerMonsters.find(monsterId);
        if (mIt == m_mapServerMonsters.end()) continue;

        TransformComponent* pT = mIt->second->GetTransform();
        if (!pT) continue;

        // 위치 보간
        XMFLOAT3 cur = pT->GetPosition();

        float dx = tgt.px - cur.x;
        float dy = tgt.py - cur.y;
        float dz = tgt.pz - cur.z;
        float distSq = dx * dx + dy * dy + dz * dz;

        // 서버 목표 위치와 거의 가까우면 보간하지 말고 바로 스냅한다.
        if (distSq <= 0.25f) // 0.5m 이내
        {
            pT->SetPosition(tgt.px, tgt.py, tgt.pz);
        }
        else
        {
            XMFLOAT3 next;
            next.x = cur.x + dx * posAlpha;
            next.y = cur.y + dy * posAlpha;
            next.z = cur.z + dz * posAlpha;
            pT->SetPosition(next);
        }

        // yaw 보간 — 360 경계 넘어갈 때 최단 경로 선택
        XMFLOAT3 rot = pT->GetRotation();
        float delta = tgt.yaw - rot.y;
        while (delta >  180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        float nextYaw = rot.y + delta * yawAlpha;
        pT->SetRotation(rot.x, nextYaw, rot.z);
    }
}
