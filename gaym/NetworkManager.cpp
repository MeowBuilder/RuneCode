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
            ProcessMonsterAttack(pScene, cmd.monsterId, cmd.attackType, cmd.windupSec,
                                 cmd.targetPlayerId, cmd.x, cmd.y, cmd.z);
            break;

        case NetworkCommand::PlayerDamage:
            ProcessPlayerDamage(pScene, cmd.playerId, cmd.damage, cmd.currentHp, cmd.isDead, cmd.attackerMonsterId);
            break;

        case NetworkCommand::MonsterDamage:
            ProcessMonsterDamage(pScene, cmd.monsterId, cmd.damage, cmd.currentHp, cmd.isDead,
                                 cmd.attackerPlayerId, cmd.skillType);
            break;

        case NetworkCommand::RoomCleared:
            ProcessRoomCleared(pScene, cmd.stageIndex, cmd.roomIndex);
            break;
        }
    }
}

void NetworkManager::SendMove(float x, float y, float z, float dirX, float dirY, float dirZ)
{
    if (!m_bConnected || !m_pSession)
        return;

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

void NetworkManager::QueueMonsterAttack(uint64 monsterId, uint64 targetPlayerId, uint32 attackType,
                                        float x, float y, float z, float yaw, float windupSec)
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

GameObject* NetworkManager::GetServerMonster(uint64 monsterId)
{
    auto it = m_mapServerMonsters.find(monsterId);
    return (it != m_mapServerMonsters.end()) ? it->second : nullptr;
}

void NetworkManager::ProcessRoomTransition(Scene* pScene, uint32 stageIndex, uint32 roomIndex, bool isBossRoom, const std::string& mapId)
{
    if (!pScene)
        return;

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

    // 서버가 내려준 mapId 우선 분기. mapId 가 비었거나 미매칭이면 stageIndex/roomIndex 기반 fallback.
    bool bHandled = false;
    if (!mapId.empty())
    {
        if (mapId == "fire_boss")           { pScene->TransitionToBossRoom();        bHandled = true; }
        else if (mapId == "water_boss")     { pScene->TransitionToWaterBossRoom();   bHandled = true; }
        else if (mapId == "earth_boss")     { pScene->TransitionToEarthBossRoom();   bHandled = true; }
        else if (mapId == "grass_boss")     { pScene->TransitionToGrassBossRoom();   bHandled = true; }
        else if (mapId == "water_room_01")  { pScene->TransitionToWaterStage();      bHandled = true; }
        else if (mapId == "earth_room_01")  { pScene->TransitionToEarthStage();      bHandled = true; }
        else if (mapId == "grass_room_01")  { pScene->TransitionToGrassStage();      bHandled = true; }
        else if (mapId == "fire_room_01")   { pScene->TransitionToRoomByIndex(static_cast<int>(roomIndex)); bHandled = true; }
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
    SendTorchInteract();

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

    // 새 원격 플레이어 모델 로드
    GameObject* pRemotePlayer = MeshLoader::LoadGeometryFromFile(pScene, pDevice, pCommandList, NULL, "Assets/Player/MageBlue.bin");
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

    // 애니메이션 추가
    auto* pAnim = pRemotePlayer->AddComponent<AnimationComponent>();
    if (pAnim)
    {
        pAnim->LoadAnimation("Assets/Player/MageBlue_Anim.bin");
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

void NetworkManager::CheckRemotePlayerVFXTimeout(Scene* pScene, float deltaTime)
{
    FluidSkillVFXManager* pVFXManager = pScene ? pScene->GetFluidVFXManager() : nullptr;
    if (!pVFXManager)
        return;

    for (auto it = m_mapRemotePlayerVFX.begin(); it != m_mapRemotePlayerVFX.end(); )
    {
        RemoteVFXState& state = it->second;
        state.lastUpdateTime += deltaTime;

        // 타임아웃 시 VFX 종료
        if (state.lastUpdateTime >= VFX_TIMEOUT)
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

    // 스킬 원점 (캐릭터 위치 + 높이 오프셋)
    XMFLOAT3 skillOrigin = XMFLOAT3(x, y + 1.5f, z);
    XMFLOAT3 skillDirection = XMFLOAT3(dirX, dirY, dirZ);

    // 방향 벡터 정규화
    float dirLen = sqrtf(dirX * dirX + dirY * dirY + dirZ * dirZ);
    if (dirLen > 0.001f)
    {
        skillDirection.x /= dirLen;
        skillDirection.y /= dirLen;
        skillDirection.z /= dirLen;
    }
    else
    {
        skillDirection = XMFLOAT3(0.0f, 0.0f, 1.0f);  // 기본 방향
    }

    // 스킬 타입에 따라 분기 처리
    // 1=Q (WaveSlash - VFX), 2=E (FireBeam - 채널링 VFX), 3=R (Meteor - 낙하 VFX), 4=RightClick (Fireball - 투사체)

    FluidSkillVFXManager* pVFXManager = pScene ? pScene->GetFluidVFXManager() : nullptr;
    ProjectileManager* pProjManager = pScene ? pScene->GetProjectileManager() : nullptr;

    switch (skillType)
    {
    case 1:  // Q 스킬 (WaveSlash) - VFX 단발
        if (pVFXManager)
        {
            // 수평 방향 VFX
            XMFLOAT3 horizontalDir = skillDirection;
            horizontalDir.y = 0.0f;
            float hLen = sqrtf(horizontalDir.x * horizontalDir.x + horizontalDir.z * horizontalDir.z);
            if (hLen > 0.001f)
            {
                horizontalDir.x /= hLen;
                horizontalDir.z /= hLen;
            }

            EffectDef def = EffectRegistry::Get().GetEffect("Q_WaveSlash", RUNE_NONE);
            int vfxId = pVFXManager->SpawnEffectDef(skillOrigin, horizontalDir, def, true);

            wchar_t vfxBuf[128];
            swprintf_s(vfxBuf, L"[Network] Spawned Q (WaveSlash) VFX: vfxId=%d\n", vfxId);
            OutputDebugString(vfxBuf);
        }
        break;

    case 2:  // E 스킬 (FireBeam) - 채널링 VFX (TrackEffect 필요)
        if (pVFXManager)
        {
            // 기존 VFX 상태 확인
            auto vfxIt = m_mapRemotePlayerVFX.find(playerId);
            bool hasExistingVFX = (vfxIt != m_mapRemotePlayerVFX.end() && vfxIt->second.vfxId >= 0);

            if (hasExistingVFX && vfxIt->second.skillType == skillType)
            {
                // 같은 스킬 계속 사용 중 → TrackEffect로 방향 업데이트
                pVFXManager->TrackEffect(vfxIt->second.vfxId, skillOrigin, skillDirection);
                vfxIt->second.lastUpdateTime = 0.0f;
            }
            else
            {
                // 새 스킬 → 기존 VFX 종료 후 새로 생성
                if (hasExistingVFX)
                {
                    pVFXManager->StopEffect(vfxIt->second.vfxId);
                }

                EffectDef def = EffectRegistry::Get().GetEffect("E_FireBeam_Core", RUNE_NONE);
                int vfxId = pVFXManager->SpawnEffectDef(skillOrigin, skillDirection, def, true);

                RemoteVFXState state;
                state.vfxId = vfxId;
                state.skillType = skillType;
                state.lastUpdateTime = 0.0f;
                m_mapRemotePlayerVFX[playerId] = state;

                wchar_t vfxBuf[128];
                swprintf_s(vfxBuf, L"[Network] Spawned E (FireBeam) VFX: vfxId=%d\n", vfxId);
                OutputDebugString(vfxBuf);
            }
        }
        break;

    case 3:  // R 스킬 (Meteor) - 상공에서 낙하 VFX
        if (pVFXManager)
        {
            // R 스킬은 송신 측이 dirX/Y/Z 슬롯에 **절대 타겟 좌표**를 실어 보냄
            // (SkillComponent 송신 로직 참조). 정규화된 방향 벡터가 아님에 주의.
            XMFLOAT3 targetPos = XMFLOAT3(dirX, dirY, dirZ);

            // 송신 측과 동일하게 MeteorBehavior::METEOR_SPAWN_HEIGHT = 50 사용
            const float meteorSpawnHeight = 50.0f;
            XMFLOAT3 spawnPos = XMFLOAT3(targetPos.x, targetPos.y + meteorSpawnHeight, targetPos.z);

            // 낙하 방향 = 아래
            XMFLOAT3 downDir = XMFLOAT3(0.0f, -1.0f, 0.0f);

            EffectDef def = EffectRegistry::Get().GetEffect("R_Meteor", RUNE_NONE);
            int vfxId = pVFXManager->SpawnEffectDef(spawnPos, downDir, def, true);

            wchar_t vfxBuf[128];
            swprintf_s(vfxBuf, L"[Network] Spawned R (Meteor) VFX: vfxId=%d, target=(%.1f,%.1f,%.1f) spawn=(%.1f,%.1f,%.1f)\n",
                vfxId, targetPos.x, targetPos.y, targetPos.z, spawnPos.x, spawnPos.y, spawnPos.z);
            OutputDebugString(vfxBuf);
        }
        break;

    case 4:  // 우클릭 (Fireball) - 실제 투사체
        if (pProjManager)
        {
            // Fireball 파라미터 (FireballBehavior 기준)
            float speed = 30.0f;
            float radius = 0.5f;
            float explosionRadius = 3.0f;
            float scale = 1.0f;

            // 타겟 위치 (수평 방향으로 50m)
            XMFLOAT3 targetPos = XMFLOAT3(
                skillOrigin.x + skillDirection.x * 50.0f,
                skillOrigin.y,  // 수평 유지
                skillOrigin.z + skillDirection.z * 50.0f
            );

            pProjManager->SpawnProjectile(
                skillOrigin,
                targetPos,
                0.0f,               // damage=0 (원격은 데미지 없음)
                speed,
                radius,
                explosionRadius,
                ElementType::Fire,
                pRemotePlayer,
                true,
                scale,
                RuneCombo{},
                0.0f
            );

            wchar_t projBuf[128];
            swprintf_s(projBuf, L"[Network] Spawned RightClick (Fireball) projectile\n");
            OutputDebugString(projBuf);
        }
        break;

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
        return { "Assets/Enemies/golem/Golem01_Generic_prefab.bin",
                 "Assets/Enemies/golem/Golem01_Generic_prefab_Anim.bin",
                 8.0f, "Golem_battle_stand_ge", "Golem_battle_walk_ge", "",
                 "Golem_battle_attack01_ge", "Golem_battle_die_ge" };
    case 9: // Demon
        return { "Assets/Enemies/demon/Demon.bin",
                 "Assets/Enemies/demon/Demon_Anim.bin",
                 3.5f, "Idle1", "Run", "",
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
    m_mapServerMonsterClips[monsterId] = {
        preset.idleClip, preset.walkClip, preset.attackClip, preset.deathClip
    };

    // 보간 타겟 초기값 = 스폰 위치 (첫 MOVE 전까진 제자리)
    ServerMonsterTarget initTgt;
    initTgt.px = x; initTgt.py = y; initTgt.pz = z;
    initTgt.yaw = yaw;
    initTgt.hasTarget = true;
    m_mapServerMonsterTarget[monsterId] = initTgt;

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

void NetworkManager::ProcessMonsterAttack(Scene* pScene, uint64 monsterId, uint32 attackType, float windupSec,
                                          uint64 targetPlayerId, float atkX, float atkY, float atkZ)
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

    // preset 기본 attack 클립 (attackType 별 세분화는 추후 확장 포인트)
    auto clipIt = m_mapServerMonsterClips.find(monsterId);
    const char* attackClip = (clipIt != m_mapServerMonsterClips.end() && !clipIt->second.attack.empty())
        ? clipIt->second.attack.c_str() : "Attack";

    pAnim->CrossFade(attackClip, 0.1f, false, true);  // forceRestart — 연속 공격도 처음부터

    // 공격 애니 지속 시간 등록 — 이 기간 Move 왔을 때 walk 로 덮지 않음
    //  서버 windupSec(예고) + 추정 재생시간. 짧은 windup 공격도 최소 ATTACK_ANIM_LOCK 은 유지
    float lockDur = fmaxf(windupSec + 0.4f, ATTACK_ANIM_LOCK);
    m_mapServerMonsterAttackTimer[monsterId] = lockDur;
    // 공격 중엔 idle 전환 억제
    m_mapServerMonsterMoveTime.erase(monsterId);

    // 원거리 공격 (MonsterAttackType::Ranged = 2) 이면 비쥬얼 투사체 스폰.
    //   - 데미지는 서버가 S_PLAYER_DAMAGE 로 별도 처리 → 여기선 damage=0 으로 시각만 재현.
    //   - CheckProjectileCollisions 의 enemy projectile 경로가 로컬 플레이어 충돌 시 projectile 제거해줌.
    if (attackType == 2)
    {
        ProjectileManager* pProj = pScene ? pScene->GetProjectileManager() : nullptr;
        if (pProj)
        {
            XMFLOAT3 startPos{ atkX, atkY + 1.5f, atkZ };

            // 타겟 플레이어 위치 (로컬 or 원격)
            XMFLOAT3 targetPos = startPos;
            uint64 localId = GetLocalPlayerId();
            if (targetPlayerId == localId)
            {
                if (GameObject* pLocal = pScene->GetPlayer())
                {
                    if (auto* pT = pLocal->GetTransform())
                    {
                        targetPos = pT->GetPosition();
                        targetPos.y += 1.5f;
                    }
                }
            }
            else
            {
                auto rIt = m_mapRemotePlayers.find(targetPlayerId);
                if (rIt != m_mapRemotePlayers.end() && rIt->second)
                {
                    if (auto* pT = rIt->second->GetTransform())
                    {
                        targetPos = pT->GetPosition();
                        targetPos.y += 1.5f;
                    }
                }
            }

            constexpr float RANGED_SPEED = 18.f;
            pProj->SpawnProjectile(
                startPos, targetPos,
                0.0f,        // damage (서버 권위)
                RANGED_SPEED,
                0.5f,        // collisionRadius
                0.0f,        // explosionRadius (없음)
                ElementType::Fire,
                nullptr,     // owner
                false,       // isPlayerProjectile → 로컬 플레이어와 충돌 체크 경로
                1.0f,        // scale
                RuneCombo{}, // 룬 없음
                0.0f,        // chargeRatio
                80.f,        // maxDistance
                false, false, 0.f, 0.f, 0.f,
                SkillSlot::Count);
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

    // 우클릭 (Fireball) 피격 시 폭발 VFX — 네트워크 몬스터는 EnemyComponent 가 없어서 로컬 투사체가 충돌
    // 감지를 못 하고 그냥 통과, 폭발 이펙트가 안 뜨기 때문에 서버 피격 통지 시점에 몬스터 위치에서 수동 생성.
    if (skillType == 4 /* SKILL_TYPE_MOUSE_RIGHT */)
    {
        ProjectileManager* pProj = pScene->GetProjectileManager();
        if (pProj && pMonster->GetTransform())
        {
            pProj->SpawnExplosionParticles(pMonster->GetTransform()->GetPosition(), ElementType::Fire);
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

    wchar_t buf[128];
    swprintf_s(buf, L"[Network] Despawned NetMonster_%llu\n", monsterId);
    OutputDebugString(buf);
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

        auto mIt = m_mapServerMonsters.find(monsterId);
        if (mIt == m_mapServerMonsters.end()) continue;

        TransformComponent* pT = mIt->second->GetTransform();
        if (!pT) continue;

        // 위치 보간
        XMFLOAT3 cur = pT->GetPosition();
        XMFLOAT3 next;
        next.x = cur.x + (tgt.px - cur.x) * posAlpha;
        next.y = cur.y + (tgt.py - cur.y) * posAlpha;
        next.z = cur.z + (tgt.pz - cur.z) * posAlpha;
        pT->SetPosition(next);

        // yaw 보간 — 360 경계 넘어갈 때 최단 경로 선택
        XMFLOAT3 rot = pT->GetRotation();
        float delta = tgt.yaw - rot.y;
        while (delta >  180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        float nextYaw = rot.y + delta * yawAlpha;
        pT->SetRotation(rot.x, nextYaw, rot.z);
    }
}
