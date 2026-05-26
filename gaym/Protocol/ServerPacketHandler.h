#pragma once
#include "Protocol.pb.h"

using PacketHandlerFunc = std::function<bool(PacketSessionRef&, BYTE*, int32)>;
extern PacketHandlerFunc GPacketHandler[UINT16_MAX];

enum : uint16
{
	PKT_C_LOGIN = 1000,
	PKT_S_LOGIN = 1001,
	PKT_C_ENTER_GAME = 1002,
	PKT_S_ENTER_GAME = 1003,
	PKT_C_CHAT = 1004,
	PKT_S_CHAT = 1005,
	PKT_S_SPAWN = 1006,
	PKT_S_DESPAWN = 1007,
	PKT_C_MOVE = 1008,
	PKT_S_MOVE = 1009,
	PKT_C_SKILL = 1010,
	PKT_S_SKILL = 1011,
	PKT_C_PORTAL_INTERACT = 1012,
	PKT_C_TORCH_INTERACT = 1013,
	PKT_S_ROOM_TRANSITION = 1014,
	PKT_S_MONSTER_SPAWN = 1015,
	PKT_S_MONSTER_MOVE = 1016,
	PKT_S_MONSTER_DESPAWN = 1017,
	PKT_S_MONSTER_ATTACK = 1018,
	PKT_S_PLAYER_DAMAGE = 1019,
	PKT_C_PLAYER_ATTACK = 1020,
	PKT_S_MONSTER_DAMAGE = 1021,
	PKT_S_ROOM_CLEARED = 1022,
	// [DEBUG] 현재 방 안 살아있는 몬스터 전체 즉사. 본문 비어있음 — 빈 메시지 형식 C_PORTAL_INTERACT 재활용.
	// 정식 패치 시 별도 message 로 분리할 것.
	PKT_C_DEBUG_KILL_ALL = 1023,
	PKT_S_BOSS_EVENT = 1024,
	PKT_C_BOSS_CUTSCENE_END = 1025,
	PKT_S_MONSTER_STAGGER = 1026,
};

// Custom Handlers
bool Handle_INVALID(PacketSessionRef& session, BYTE* buffer, int32 len);
bool Handle_S_LOGIN(PacketSessionRef& session, Protocol::S_LOGIN& pkt);
bool Handle_S_ENTER_GAME(PacketSessionRef& session, Protocol::S_ENTER_GAME& pkt);
bool Handle_S_CHAT(PacketSessionRef& session, Protocol::S_CHAT& pkt);
bool Handle_S_SPAWN(PacketSessionRef& session, Protocol::S_SPAWN& pkt);
bool Handle_S_DESPAWN(PacketSessionRef& session, Protocol::S_DESPAWN& pkt);
bool Handle_S_MOVE(PacketSessionRef& session, Protocol::S_MOVE& pkt);
bool Handle_S_SKILL(PacketSessionRef& session, Protocol::S_SKILL& pkt);
bool Handle_S_ROOM_TRANSITION(PacketSessionRef& session, Protocol::S_ROOM_TRANSITION& pkt);
bool Handle_S_MONSTER_SPAWN(PacketSessionRef& session, Protocol::S_MONSTER_SPAWN& pkt);
bool Handle_S_MONSTER_MOVE(PacketSessionRef& session, Protocol::S_MONSTER_MOVE& pkt);
bool Handle_S_MONSTER_DESPAWN(PacketSessionRef& session, Protocol::S_MONSTER_DESPAWN& pkt);
bool Handle_S_MONSTER_ATTACK(PacketSessionRef& session, Protocol::S_MONSTER_ATTACK& pkt);
bool Handle_S_PLAYER_DAMAGE(PacketSessionRef& session, Protocol::S_PLAYER_DAMAGE& pkt);
bool Handle_S_MONSTER_DAMAGE(PacketSessionRef& session, Protocol::S_MONSTER_DAMAGE& pkt);
bool Handle_S_ROOM_CLEARED(PacketSessionRef& session, Protocol::S_ROOM_CLEARED& pkt);
bool Handle_S_BOSS_EVENT(PacketSessionRef& session, Protocol::S_BOSS_EVENT& pkt);
bool Handle_S_MONSTER_STAGGER(PacketSessionRef& session, Protocol::S_MONSTER_STAGGER& pkt);

class ServerPacketHandler
{
public:
	static void Init()
	{
		for (int32 i = 0; i < UINT16_MAX; i++)
		GPacketHandler[i] = Handle_INVALID;
		GPacketHandler[PKT_S_LOGIN] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_LOGIN>(Handle_S_LOGIN, session, buffer, len); };
		GPacketHandler[PKT_S_ENTER_GAME] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_ENTER_GAME>(Handle_S_ENTER_GAME, session, buffer, len); };
		GPacketHandler[PKT_S_CHAT] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_CHAT>(Handle_S_CHAT, session, buffer, len); };
		GPacketHandler[PKT_S_SPAWN] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_SPAWN>(Handle_S_SPAWN, session, buffer, len); };
		GPacketHandler[PKT_S_DESPAWN] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_DESPAWN>(Handle_S_DESPAWN, session, buffer, len); };
		GPacketHandler[PKT_S_MOVE] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_MOVE>(Handle_S_MOVE, session, buffer, len); };
		GPacketHandler[PKT_S_SKILL] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_SKILL>(Handle_S_SKILL, session, buffer, len); };
		GPacketHandler[PKT_S_ROOM_TRANSITION] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_ROOM_TRANSITION>(Handle_S_ROOM_TRANSITION, session, buffer, len); };
		GPacketHandler[PKT_S_MONSTER_SPAWN] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_MONSTER_SPAWN>(Handle_S_MONSTER_SPAWN, session, buffer, len); };
		GPacketHandler[PKT_S_MONSTER_MOVE] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_MONSTER_MOVE>(Handle_S_MONSTER_MOVE, session, buffer, len); };
		GPacketHandler[PKT_S_MONSTER_DESPAWN] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_MONSTER_DESPAWN>(Handle_S_MONSTER_DESPAWN, session, buffer, len); };
		GPacketHandler[PKT_S_MONSTER_ATTACK] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_MONSTER_ATTACK>(Handle_S_MONSTER_ATTACK, session, buffer, len); };
		GPacketHandler[PKT_S_PLAYER_DAMAGE] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_PLAYER_DAMAGE>(Handle_S_PLAYER_DAMAGE, session, buffer, len); };
		GPacketHandler[PKT_S_MONSTER_DAMAGE] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_MONSTER_DAMAGE>(Handle_S_MONSTER_DAMAGE, session, buffer, len); };
		GPacketHandler[PKT_S_ROOM_CLEARED] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_ROOM_CLEARED>(Handle_S_ROOM_CLEARED, session, buffer, len); };
		GPacketHandler[PKT_S_BOSS_EVENT] = [](PacketSessionRef& session, BYTE* buffer, int32 len) {return HandlePacket<Protocol::S_BOSS_EVENT>(Handle_S_BOSS_EVENT, session, buffer, len); };
		GPacketHandler[PKT_S_MONSTER_STAGGER] = [](PacketSessionRef& session, BYTE* buffer, int32 len) { return HandlePacket<Protocol::S_MONSTER_STAGGER>(Handle_S_MONSTER_STAGGER, session, buffer, len); };
	}

	static bool HandlePacket(PacketSessionRef& session, BYTE* buffer, int32 len)
	{
		PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);
		return GPacketHandler[header->id](session, buffer, len);
	}
	static SendBufferRef MakeSendBuffer(Protocol::C_LOGIN& pkt) { return MakeSendBuffer(pkt, PKT_C_LOGIN); }
	static SendBufferRef MakeSendBuffer(Protocol::C_ENTER_GAME& pkt) { return MakeSendBuffer(pkt, PKT_C_ENTER_GAME); }
	static SendBufferRef MakeSendBuffer(Protocol::C_CHAT& pkt) { return MakeSendBuffer(pkt, PKT_C_CHAT); }
	static SendBufferRef MakeSendBuffer(Protocol::C_MOVE& pkt) { return MakeSendBuffer(pkt, PKT_C_MOVE); }
	static SendBufferRef MakeSendBuffer(Protocol::C_SKILL& pkt) { return MakeSendBuffer(pkt, PKT_C_SKILL); }
	static SendBufferRef MakeSendBuffer(Protocol::C_PORTAL_INTERACT& pkt) { return MakeSendBuffer(pkt, PKT_C_PORTAL_INTERACT); }
	static SendBufferRef MakeSendBuffer(Protocol::C_TORCH_INTERACT& pkt) { return MakeSendBuffer(pkt, PKT_C_TORCH_INTERACT); }
	static SendBufferRef MakeSendBuffer(Protocol::C_PLAYER_ATTACK& pkt) { return MakeSendBuffer(pkt, PKT_C_PLAYER_ATTACK); }
	static SendBufferRef MakeSendBuffer(Protocol::C_BOSS_CUTSCENE_END& pkt) { return MakeSendBuffer(pkt, PKT_C_BOSS_CUTSCENE_END); }
	// [DEBUG] 빈 본문 — C_PORTAL_INTERACT 메시지 형식만 빌려서 ID 만 다르게 보냄
	static SendBufferRef MakeDebugKillAllSendBuffer() { Protocol::C_PORTAL_INTERACT pkt; return MakeSendBuffer(pkt, PKT_C_DEBUG_KILL_ALL); }
	

private:
	template<typename PacketType, typename ProcessFunc>
	static bool HandlePacket(ProcessFunc func, PacketSessionRef& session, BYTE* buffer, int32 len)
	{
		PacketType pkt;
		if (pkt.ParseFromArray(buffer + sizeof(PacketHeader), len - sizeof(PacketHeader)) == false)
			return false;

		return func(session, pkt);
	}

	template<typename T>
	static SendBufferRef MakeSendBuffer(T& pkt, uint16 pktId)
	{
		const uint16 dataSize = static_cast<uint16>(pkt.ByteSizeLong());
		const uint16 packetSize = dataSize + sizeof(PacketHeader);

		SendBufferRef sendBuffer = GSendBufferManager->Open(packetSize);
		PacketHeader* header = reinterpret_cast<PacketHeader*>(sendBuffer->Buffer());
		header->size = packetSize;
		header->id = pktId;
		ASSERT_CRASH(pkt.SerializeToArray(&header[1], dataSize));
		sendBuffer->Close(packetSize);

		return sendBuffer;
	}
};