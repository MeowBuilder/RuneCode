#include "../ServerCore/pch.h"
#include "ServerPacketHandler.h"
#include "../NetworkManager.h"
#include <fstream>
#include <string>
#include <vector>
#include <DirectXMath.h>

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

    // C_ENTER_GAME 은 캐릭터 선택 화면에서 사용자가 원소를 확정한 시점에
    // NetworkManager::SendEnterGame() 으로 보낸다 (playerindex = ElementType 0~3).
    OutputDebugStringA("[Network] S_LOGIN received, awaiting character pick\n");
    WriteNetworkLog("[Network] S_LOGIN received, awaiting character pick");

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
    const std::string& msg = pkt.msg();

    // 서버 RoomManager가 별도 proto 추가 없이 S_CHAT을 이용해 보내는 Room HUD 정보.
    // 예: [ROOM]Room #1 | Players 2/4 | Started
    static const std::string kRoomPrefix = "[ROOM]";

    if (msg.rfind(kRoomPrefix, 0) == 0)
    {
        std::string roomInfo = msg.substr(kRoomPrefix.size());

        if (NetworkManager* pNetMgr = NetworkManager::GetInstance())
        {
            pNetMgr->SetRoomInfoText(roomInfo);
        }

        WriteNetworkLog("[Network] RoomInfo: " + roomInfo);
        return true;
    }

    OutputDebugStringA("[Network] Chat: ");
    OutputDebugStringA(msg.c_str());
    OutputDebugStringA("\n");

    std::string log = "[Network] Chat: " + msg;
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

    //char buf[256];
    //sprintf_s(buf, "[Network] S_MOVE received: PlayerId=%llu Pos=(%.1f, %.1f, %.1f) Dir=(%.2f, %.2f, %.2f)",
    //    playerId, x, y, z, dirX, dirY, dirZ);
    //WriteNetworkLog(buf);

    // NetworkManager를 통해 메인 스레드에서 처리
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueMovePlayer(playerId, x, y, z, dirX, dirY, dirZ);
        // WriteNetworkLog("[Network] QueueMovePlayer called");
    }

    return true;
}

// 몬스터 스폰 처리
bool Handle_S_MONSTER_SPAWN(PacketSessionRef& session, Protocol::S_MONSTER_SPAWN& pkt)
{
    const Protocol::MonsterInfo& m = pkt.monster();

    char buf[320];
    sprintf_s(buf,
        "[Network] S_MONSTER_SPAWN received: id=%llu type=%u attack=%u visual=%u pos=(%.1f,%.1f,%.1f) yaw=%.2f hp=%.1f boss=%d",
        m.monsterid(),
        m.monstertype(),
        m.attacktype(),
        m.visualtype(),
        m.x(), m.y(), m.z(),
        m.yaw(),
        m.hp(),
        m.isboss() ? 1 : 0);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueMonsterSpawn(
            m.monsterid(),
            m.monstertype(),
            m.attacktype(),
            m.visualtype(),
            m.x(), m.y(), m.z(),
            m.yaw(),
            m.hp(),
            m.isboss()
        );
    }

    return true;
}

// 몬스터 이동 처리
bool Handle_S_MONSTER_MOVE(PacketSessionRef& session, Protocol::S_MONSTER_MOVE& pkt)
{
    uint64 id = pkt.monsterid();
    float x = pkt.x(), y = pkt.y(), z = pkt.z();
    float yaw = pkt.yaw();

    // S_MONSTER_MOVE는 매 프레임 대량 발생하므로 로그 비활성화
    // WriteNetworkLog(buf);

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

    // 서버 룬 보정 필드 — 서버 권위 radius/damage 배율 (AMP_RAD3 등). 0 이면 기본값(1.0) 처리.
    int32 skillSlot   = pkt.skillslot();
    float radiusMult  = pkt.radiusmult();
    float damageMult  = pkt.damagemult();

    char buf[320];
    sprintf_s(buf, "[Network] S_SKILL received: PlayerId=%llu SkillType=%d Pos=(%.1f, %.1f, %.1f) Dir=(%.2f, %.2f, %.2f) Slot=%d RadiusMult=%.2f DamageMult=%.2f",
        playerId, skillType, x, y, z, dirX, dirY, dirZ, skillSlot, radiusMult, damageMult);
    WriteNetworkLog(buf);

    // NetworkManager를 통해 메인 스레드에서 처리
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueSkill(playerId, skillType, x, y, z, dirX, dirY, dirZ,
                            skillSlot, radiusMult, damageMult);
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
    uint32 effectOption = pkt.effectoption();

    // 공격 이펙트 위치 목록
    std::vector<DirectX::XMFLOAT3> effectPositions;

    for (int i = 0; i < pkt.effectpositions_size(); ++i)
    {
        const Protocol::AttackEffectPosition& p = pkt.effectpositions(i);
        effectPositions.push_back(DirectX::XMFLOAT3(p.x(), p.y(), p.z()));
    }

    char buf[256];
    sprintf_s(buf, "[Network] S_MONSTER_ATTACK received: monsterId=%llu targetPlayerId=%llu attackType=%u pos=(%.2f, %.2f, %.2f) yaw=%.2f windupSec=%.2f effectPosCount=%d effectOption=%u",
        monsterId, targetPlayerId, attackType, x, y, z, yaw, windupSec, (int)effectPositions.size(), effectOption);
    WriteNetworkLog(buf);

    // 메인 스레드에서 공격 애니 / 장판 / VFX 재생
    // effectPositions가 있으면 ProcessMonsterAttack까지 전달해서 모든 클라가 같은 위치에 이펙트를 생성한다.
    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueMonsterAttack(monsterId, targetPlayerId, attackType, x, y, z, yaw, windupSec, effectPositions, effectOption);
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

// 몬스터 기절/그로기 처리
bool Handle_S_MONSTER_STAGGER(PacketSessionRef& session, Protocol::S_MONSTER_STAGGER& pkt)
{
    uint64 monsterId = pkt.monsterid();
    float duration = pkt.duration();

    char buf[160];
    sprintf_s(buf,
        "[Network] S_MONSTER_STAGGER received: monsterId=%llu duration=%.2f",
        monsterId,
        duration);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueMonsterStagger(monsterId, duration);
    }

    return true;
}

// Grass Boss Room 맵 토네이도 이벤트 처리
// 서버가 정한 좌표를 받아서 NetworkManager 큐로 넘긴다.
bool Handle_S_MAP_TORNADO_EVENT(PacketSessionRef& session, Protocol::S_MAP_TORNADO_EVENT& pkt)
{
    uint32 eventType = pkt.eventtype();
    float x = pkt.x();
    float y = pkt.y();
    float z = pkt.z();
    float warningSec = pkt.warningsec();
    float activeSec = pkt.activesec();

    char buf[256];
    sprintf_s(buf,
        "[Network] S_MAP_TORNADO_EVENT received: eventType=%u pos=(%.2f, %.2f, %.2f) warningSec=%.2f activeSec=%.2f",
        eventType, x, y, z, warningSec, activeSec);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueMapTornadoEvent(x, y, z, warningSec, activeSec);
    }

    return true;
}

// 서버 -> 클라 플레이어 연출 액션 처리
// 서버가 "누가 어떤 연출을 시작했는지" 알려주면 NetworkManager 큐로 넘긴다.
bool Handle_S_PLAYER_ACTION(PacketSessionRef& session, Protocol::S_PLAYER_ACTION& pkt)
{
    uint64 playerId = pkt.playerid();
    uint32 actionType = pkt.actiontype();

    char buf[256];
    sprintf_s(buf,
        "[Network] S_PLAYER_ACTION received: playerId=%llu actionType=%u pos=(%.2f, %.2f, %.2f)",
        playerId, actionType, pkt.x(), pkt.y(), pkt.z());
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueuePlayerAction(
            playerId,
            actionType,
            pkt.x(), pkt.y(), pkt.z(),
            pkt.dirx(), pkt.diry(), pkt.dirz());
    }

    return true;
}

bool Handle_S_ROOM_START(PacketSessionRef& session, Protocol::S_ROOM_START& pkt)
{
    uint64 starterPlayerId = pkt.starterplayerid();

    char buf[160];
    sprintf_s(buf,
        "[Network] S_ROOM_START received: starterPlayerId=%llu",
        starterPlayerId);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueRoomStart(starterPlayerId);
    }

    return true;
}

bool Handle_S_ROOM_REWARD_SPAWN(PacketSessionRef& session, Protocol::S_ROOM_REWARD_SPAWN& pkt)
{
    uint32 stageIndex = pkt.stageindex();
    uint32 roomIndex = pkt.roomindex();

    DirectX::XMFLOAT3 portalPos(
        pkt.portalx(),
        pkt.portaly(),
        pkt.portalz()
    );

    bool hasSecondPortal = pkt.hassecondportal();

    DirectX::XMFLOAT3 secondPortalPos(
        pkt.secondportalx(),
        pkt.secondportaly(),
        pkt.secondportalz()
    );

    std::vector<NetworkRewardRuneObjectInfo> runeObjects;
    runeObjects.reserve(pkt.runeobjects_size());

    for (int i = 0; i < pkt.runeobjects_size(); ++i)
    {
        const Protocol::RewardRuneObjectInfo& src = pkt.runeobjects(i);

        NetworkRewardRuneObjectInfo info;
        info.ownerPlayerId = src.ownerplayerid();
        info.pos = DirectX::XMFLOAT3(src.x(), src.y(), src.z());

        // 서버가 내려준 룬 3개를 큐 데이터에 복사한다.
        // 3개보다 적게 오면 빈 문자열로 남기고, 클라 생성부에서 fallback 처리한다.
        for (int r = 0; r < src.runeids_size() && r < 3; ++r)
        {
            info.runeIds[r] = src.runeids(r);
        }

        runeObjects.push_back(info);
    }

    char buf[320];
    sprintf_s(buf,
        "[Network] S_ROOM_REWARD_SPAWN received: stage=%u room=%u portal=(%.2f, %.2f, %.2f) hasSecondPortal=%d second=(%.2f, %.2f, %.2f) runeCount=%d",
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

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueRoomRewardSpawn(stageIndex, roomIndex, portalPos, hasSecondPortal, secondPortalPos, runeObjects);
    }

    return true;
}

bool Handle_S_RUNE_REWARD_PICKED(PacketSessionRef& session, Protocol::S_RUNE_REWARD_PICKED& pkt)
{
    uint64 ownerPlayerId = pkt.ownerplayerid();

    char buf[160];
    sprintf_s(buf,
        "[Network] S_RUNE_REWARD_PICKED received: ownerPlayerId=%llu",
        ownerPlayerId);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueRuneRewardPicked(ownerPlayerId);
    }

    return true;
}

bool Handle_S_RUNE_EQUIP(PacketSessionRef& session, Protocol::S_RUNE_EQUIP& pkt)
{
    uint64 playerId = pkt.playerid();
    uint32 skillSlot = pkt.skillslot();
    uint32 runeSlotIndex = pkt.runeslotindex();
    std::string runeId = pkt.runeid();
    uint32 stackCount = pkt.stackcount();

    char buf[256];
    sprintf_s(buf,
        "[Network] S_RUNE_EQUIP received: playerId=%llu skillSlot=%u runeSlotIndex=%u runeId=%s stack=%u",
        playerId,
        skillSlot,
        runeSlotIndex,
        runeId.c_str(),
        stackCount);
    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueRuneEquip(
            playerId,
            skillSlot,
            runeSlotIndex,
            runeId,
            stackCount);
    }

    return true;
}

bool Handle_S_RUNE_HOMING_TARGET(PacketSessionRef& session, Protocol::S_RUNE_HOMING_TARGET& pkt)
{
    uint64 playerId = pkt.playerid();
    int32 skillSlot = pkt.skillslot();
    int32 skillType = static_cast<int32>(pkt.skilltype());
    uint64 targetMonsterId = pkt.targetmonsterid();

    DirectX::XMFLOAT3 targetPos(
        pkt.targetx(),
        pkt.targety(),
        pkt.targetz()
    );

    DirectX::XMFLOAT3 originPos(
        pkt.originx(),
        pkt.originy(),
        pkt.originz()
    );

    char buf[256];
    sprintf_s(buf,
        "[Network] S_RUNE_HOMING_TARGET received: playerId=%llu skillSlot=%d skillType=%d targetMonsterId=%llu target=(%.2f, %.2f, %.2f)",
        playerId,
        skillSlot,
        skillType,
        targetMonsterId,
        targetPos.x,
        targetPos.y,
        targetPos.z);

    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();

    if (pNetMgr)
    {
        pNetMgr->QueueRuneHomingTarget(
            playerId,
            skillSlot,
            skillType,
            targetMonsterId,
            targetPos,
            originPos
        );
    }

    return true;
}

bool Handle_S_RUNE_TRIGGER(PacketSessionRef& session, Protocol::S_RUNE_TRIGGER& pkt)
{
    uint64 playerId = pkt.playerid();
    int32 skillSlot = pkt.skillslot();
    int32 skillType = static_cast<int32>(pkt.skilltype());

    std::string runeId = pkt.runeid();
    int32 triggerType = static_cast<int32>(pkt.triggertype());

    uint64 targetMonsterId = pkt.targetmonsterid();
    uint64 targetPlayerId = pkt.targetplayerid();

    // 서버가 보낸 objectId를 살려서 NetworkManager까지 넘긴다.
    // TRF_DEP: trapId
    // TRF_CHA / TRF_ECH: sourceMonsterId
    // TRF_ORB: orbitalId
    // TRF_EMP: 0 = READY, 1 = CONSUME
    uint64 objectId = pkt.objectid();

    float x = pkt.x();
    float y = pkt.y();
    float z = pkt.z();

    // TRF_DEP 설치 룬은 다음처럼 사용한다.
    //
    // dirX = 대표 원소
    // dirY = RuneFlag
    // dirZ = 원소 마스크
    DirectX::XMFLOAT3 visualData(
        pkt.dirx(),
        pkt.diry(),
        pkt.dirz());

    float value1 = pkt.value1();
    float value2 = pkt.value2();

    char buf[512];
    sprintf_s(buf,
        "[Network] S_RUNE_TRIGGER received: playerId=%llu skillSlot=%d skillType=%d runeId=%s triggerType=%d targetMonsterId=%llu targetPlayerId=%llu objectId=%llu pos=(%.2f, %.2f, %.2f) value1=%.2f value2=%.2f",
        playerId,
        skillSlot,
        skillType,
        runeId.c_str(),
        triggerType,
        targetMonsterId,
        targetPlayerId,
        objectId,
        x, y, z,
        value1,
        value2
    );

    WriteNetworkLog(buf);

    NetworkManager* pNetMgr = NetworkManager::GetInstance();
    if (pNetMgr)
    {
        pNetMgr->QueueRuneTrigger(
            playerId,
            skillSlot,
            skillType,
            runeId,
            triggerType,
            targetMonsterId,
            targetPlayerId,
            objectId,
            DirectX::XMFLOAT3(x, y, z),
            visualData,
            value1,
            value2);
    }

    return true;
}