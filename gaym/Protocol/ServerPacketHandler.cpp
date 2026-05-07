#include "../ServerCore/pch.h"
#include "ServerPacketHandler.h"
#include "../NetworkManager.h"
#include <fstream>
#include <string>

// 파일 로그 함수
void WriteNetworkLog(const std::string& msg)
{
    std::ofstream ofs("network_log.txt", std::ios::app);
    ofs << msg << std::endl;
}

PacketHandlerFunc GPacketHandler[UINT16_MAX];

// 잘못된 패킷 처리
bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len)
{
    PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);

    OutputDebugStringA("[Network] Invalid packet received\n");
    WriteNetworkLog("[Network] Invalid packet received");

    return false;
}

// 로그인 응답 처리
bool Handle_S_LOGIN(PacketSessionRef& session, Protocol::S_LOGIN& pkt)
{
    if (pkt.success() == false)
    {
        OutputDebugStringA("[Network] Login failed\n");
        WriteNetworkLog("[Network] Login failed");
        return true;
    }

    OutputDebugStringA("[Network] Login success\n");
    WriteNetworkLog("[Network] Login success");

    if (pkt.players().size() == 0)
    {
        OutputDebugStringA("[Network] No characters, need to create one\n");
        WriteNetworkLog("[Network] No characters, need to create one");
    }

    // 자동으로 게임 입장 요청
    Protocol::C_ENTER_GAME enterGamePkt;
    enterGamePkt.set_playerindex(0);  // 첫번째 캐릭터로 입장
    auto sendBuffer = ServerPacketHandler::MakeSendBuffer(enterGamePkt);
    session->Send(sendBuffer);

    OutputDebugStringA("[Network] C_ENTER_GAME sent\n");
    WriteNetworkLog("[Network] C_ENTER_GAME sent");

    return true;
}

// 게임 입장 응답 처리
bool Handle_S_ENTER_GAME(PacketSessionRef& session, Protocol::S_ENTER_GAME& pkt)
{
    if (pkt.success())
    {
        uint64 playerId = pkt.playerid();

        // 로컬 플레이어 ID 설정 - 큐를 통해 메인 스레드에서 처리
        // 이렇게 해야 Spawn 명령보다 먼저 처리됨을 보장할 수 있음
        NetworkManager* pNetMgr = NetworkManager::GetInstance();
        if (pNetMgr)
        {
            pNetMgr->QueueSetLocalPlayerId(playerId);

            wchar_t wbuf[128];
            swprintf_s(wbuf, L"[Network] ENTER_GAME Success! My LocalPlayerId = %llu\n", playerId);
            OutputDebugString(wbuf);

            char buf[128];
            sprintf_s(buf, "[Network] ENTER_GAME Success! My LocalPlayerId = %llu", playerId);
            WriteNetworkLog(buf);
        }
    }
    else
    {
        OutputDebugStringA("[Network] ENTER_GAME Failed by server\n");
        WriteNetworkLog("[Network] ENTER_GAME Failed by server");
    }

    return true;
}

// 채팅 메시지 수신
bool Handle_S_CHAT(PacketSessionRef& session, Protocol::S_CHAT& pkt)
{
    OutputDebugStringA("[Network] Chat: ");
    OutputDebugStringA(pkt.msg().c_str());
    OutputDebugStringA("\n");

    std::string log = "[Network] Chat: " + pkt.msg();
    WriteNetworkLog(log);

    return true;
}
// 플레이어 스폰 처리
bool Handle_S_SPAWN(PacketSessionRef& session, Protocol::S_SPAWN& pkt)
{
    const Protocol::Player& player = pkt.player();

    uint64 playerId = player.playerid();
    std::string name = player.name();
    int playerType = static_cast<int>(player.playertype());

    // 현재 설정된 내 로컬 ID 확인
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    uint64 myLocalId = pNetMgr ? pNetMgr->GetLocalPlayerId() : 0;

    wchar_t wbuf[256];
    swprintf_s(wbuf, L"[Network] S_SPAWN received: PlayerId=%llu (MyLocalId=%llu, IsSelf=%s) Name=%hs\n",
        playerId, myLocalId, (playerId == myLocalId ? L"TRUE" : L"FALSE"), name.c_str());
    OutputDebugString(wbuf);

    char buf[256];
    sprintf_s(buf, "[Network] Handle Spawn: PktId=%llu, MyLocalId=%llu, IsSelf=%s, Name=%s",
        playerId, myLocalId, (playerId == myLocalId ? "TRUE" : "FALSE"), name.c_str());
    WriteNetworkLog(buf);

    // 기본 위치 (서버 데이터가 있으면 사용)
    float x = 0.0f, y = 0.0f, z = 0.0f;

    // NetworkManager를 통해 메인 스레드에서 처리하도록 큐에 추가
    if (pNetMgr)
    {
        pNetMgr->QueueSpawnPlayer(playerId, name, playerType, x, y, z);

        char buf2[256];
        sprintf_s(buf2, "[Network] QueueSpawnPlayer called: PlayerId=%llu, IsRemote=%s, Pos=(%.1f, %.1f, %.1f)",
            playerId, (playerId != myLocalId ? "TRUE" : "FALSE"), x, y, z);
        WriteNetworkLog(buf2);
    }

    return true;
}

// 플레이어 디스폰 처리
bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt)
{
    uint64 playerId = pkt.playerid();

    wchar_t wbuf[128];
    swprintf_s(wbuf, L"[Network] S_DESPAWN received: PlayerId=%llu\n", playerId);
    OutputDebugString(wbuf);

    char buf[128];
    sprintf_s(buf, "[Network] S_DESPAWN received: PlayerId=%llu", playerId);
    WriteNetworkLog(buf);

    // NetworkManager를 통해 메인 스레드에서 처리
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueDespawnPlayer(playerId);
        WriteNetworkLog("[Network] QueueDespawnPlayer called");
    }

    return true;
}

// 플레이어 이동 처리
bool Handle_S_MOVE(PacketSessionRef& session, Protocol::S_MOVE& pkt)
{
    uint64 playerId = pkt.playerid();
    float x = pkt.x();
    float y = pkt.y();
    float z = pkt.z();
    float dirX = pkt.dirx();
    float dirY = pkt.diry();
    float dirZ = pkt.dirz();

    char buf[256];
    sprintf_s(buf, "[Network] S_MOVE received: PlayerId=%llu Pos=(%.1f, %.1f, %.1f) Dir=(%.2f, %.2f, %.2f)",
        playerId, x, y, z, dirX, dirY, dirZ);
    WriteNetworkLog(buf);

    // NetworkManager를 통해 메인 스레드에서 처리
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueMovePlayer(playerId, x, y, z, dirX, dirY, dirZ);
        WriteNetworkLog("[Network] QueueMovePlayer called");
    }

    return true;
}

// 몬스터 스폰 처리
bool Handle_S_MONSTER_SPAWN(PacketSessionRef& session, Protocol::S_MONSTER_SPAWN& pkt)
{
    const Protocol::MonsterInfo& m = pkt.monster();

    char buf[256];
    sprintf_s(buf, "[Network] S_MONSTER_SPAWN received: id=%llu type=%u pos=(%.1f,%.1f,%.1f) yaw=%.2f hp=%.1f boss=%d",
        m.monsterid(), m.monstertype(),
        m.x(), m.y(), m.z(),
        m.yaw(), m.hp(), m.isboss() ? 1 : 0);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueMonsterSpawn(m.monsterid(), m.monstertype(),
                                   m.x(), m.y(), m.z(), m.yaw(),
                                   m.hp(), m.isboss());
    }
    return true;
}

// 몬스터 이동 처리
bool Handle_S_MONSTER_MOVE(PacketSessionRef& session, Protocol::S_MONSTER_MOVE& pkt)
{
    uint64 id = pkt.monsterid();
    float x = pkt.x(), y = pkt.y(), z = pkt.z();
    float yaw = pkt.yaw();

    char buf[256];
    sprintf_s(buf, "[Network] S_MONSTER_MOVE: id=%llu pos=(%.2f,%.2f,%.2f) yaw=%.2f",
        id, x, y, z, yaw);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
        pNetMgr->QueueMonsterMove(id, x, y, z, yaw);
    return true;
}

// 몬스터 디스폰 처리
bool Handle_S_MONSTER_DESPAWN(PacketSessionRef& session, Protocol::S_MONSTER_DESPAWN& pkt)
{
    uint64 id = pkt.monsterid();

    char buf[128];
    sprintf_s(buf, "[Network] S_MONSTER_DESPAWN: id=%llu", id);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
        pNetMgr->QueueMonsterDespawn(id);
    return true;
}

// 방 전환 처리
bool Handle_S_ROOM_TRANSITION(PacketSessionRef& session, Protocol::S_ROOM_TRANSITION& pkt)
{
    uint32 stageIndex = pkt.stageindex();
    uint32 roomIndex = pkt.roomindex();
    bool isBossRoom = pkt.isbossroom();
    std::string mapId = pkt.mapid();

    char buf[512];
    sprintf_s(buf, "[Network] S_ROOM_TRANSITION received: stage=%u room=%u boss=%d mapId=%s",
        stageIndex, roomIndex, isBossRoom ? 1 : 0, mapId.c_str());
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueRoomTransition(stageIndex, roomIndex, isBossRoom, mapId);
    }

    return true;
}

// 플레이어 스킬 처리
bool Handle_S_SKILL(PacketSessionRef& session, Protocol::S_SKILL& pkt)
{
    uint64 playerId = pkt.playerid();
    int skillType = static_cast<int>(pkt.skilltype());
    float x = pkt.x();
    float y = pkt.y();
    float z = pkt.z();
    float dirX = pkt.dirx();
    float dirY = pkt.diry();
    float dirZ = pkt.dirz();

    char buf[256];
    sprintf_s(buf, "[Network] S_SKILL received: PlayerId=%llu SkillType=%d Pos=(%.1f, %.1f, %.1f) Dir=(%.2f, %.2f, %.2f)",
        playerId, skillType, x, y, z, dirX, dirY, dirZ);
    WriteNetworkLog(buf);

    // NetworkManager를 통해 메인 스레드에서 처리
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueSkill(playerId, skillType, x, y, z, dirX, dirY, dirZ);
        WriteNetworkLog("[Network] QueueSkill called");
    }

    return true;
}

// 몬스터 공격 처리
bool Handle_S_MONSTER_ATTACK(PacketSessionRef& session, Protocol::S_MONSTER_ATTACK& pkt)
{
    uint64 monsterId = pkt.monsterid();
    uint64 targetPlayerId = pkt.targetplayerid();
    uint32 attackType = pkt.attacktype();
    float x = pkt.x();
    float y = pkt.y();
    float z = pkt.z();
    float yaw = pkt.yaw();
    float windupSec = pkt.windupsec();

    char buf[256];
    sprintf_s(buf, "[Network] S_MONSTER_ATTACK received: monsterId=%llu targetPlayerId=%llu attackType=%u pos=(%.2f, %.2f, %.2f) yaw=%.2f windupSec=%.2f",
        monsterId, targetPlayerId, attackType, x, y, z, yaw, windupSec);
    WriteNetworkLog(buf);

    // 메인 스레드에서 공격 애니 재생 (Process 경로)
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueMonsterAttack(monsterId, targetPlayerId, attackType, x, y, z, yaw, windupSec);
    }
    return true;
}

// 플레이어 피격 처리
bool Handle_S_PLAYER_DAMAGE(PacketSessionRef& session, Protocol::S_PLAYER_DAMAGE& pkt)
{
    uint64 playerId = pkt.playerid();
    float damage = pkt.damage();
    float currentHp = pkt.currenthp();
    bool isDead = pkt.isdead();
    uint64 attackerMonsterId = pkt.attackermonsterid();

    char buf[256];
    sprintf_s(buf, "[Network] S_PLAYER_DAMAGE received: playerId=%llu damage=%.2f currentHp=%.2f isDead=%d attackerMonsterId=%llu",
        playerId, damage, currentHp, isDead ? 1 : 0, attackerMonsterId);
    WriteNetworkLog(buf);

    // 메인 스레드에서 HP 반영 + 피격 연출 (Process 경로)
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueuePlayerDamage(playerId, damage, currentHp, isDead, attackerMonsterId);
    }
    return true;
}

// 몬스터 피격 처리 (플레이어 -> 몬스터 공격 결과)
bool Handle_S_MONSTER_DAMAGE(PacketSessionRef& session, Protocol::S_MONSTER_DAMAGE& pkt)
{
    uint64 monsterId = pkt.monsterid();
    float damage = pkt.damage();
    float currentHp = pkt.currenthp();
    bool isDead = pkt.isdead();
    uint64 attackerPlayerId = pkt.attackerplayerid();
    int skillType = static_cast<int>(pkt.skilltype());

    char buf[256];
    sprintf_s(buf, "[Network] S_MONSTER_DAMAGE received: monsterId=%llu damage=%.2f currentHp=%.2f isDead=%d attackerPlayerId=%llu skillType=%d",
        monsterId, damage, currentHp, isDead ? 1 : 0, attackerPlayerId, skillType);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueMonsterDamage(monsterId, damage, currentHp, isDead, attackerPlayerId, skillType);
    }
    return true;
}

// 방 클리어 처리
bool Handle_S_ROOM_CLEARED(PacketSessionRef& session, Protocol::S_ROOM_CLEARED& pkt)
{
    uint32 stageIndex = pkt.stageindex();
    uint32 roomIndex = pkt.roomindex();

    char buf[256];
    sprintf_s(buf, "[Network] S_ROOM_CLEARED received: stage=%u room=%u", stageIndex, roomIndex);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueRoomCleared(stageIndex, roomIndex);
    }
    return true;
}

// 보스 이벤트/컷씬 처리
bool Handle_S_BOSS_EVENT(PacketSessionRef& session, Protocol::S_BOSS_EVENT& pkt)
{
    uint64 monsterId = pkt.monsterid();
    Protocol::BossEventType eventType = pkt.eventtype();
    uint32 phaseIndex = pkt.phaseindex();

    char buf[256];
    sprintf_s(buf,
        "[Network] S_BOSS_EVENT received: monsterId=%llu eventType=%d phaseIndex=%u",
        monsterId,
        static_cast<int>(eventType),
        phaseIndex);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueBossEvent(monsterId, static_cast<uint32>(eventType), phaseIndex);
    }

    return true;
}