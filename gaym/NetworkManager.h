#pragma once

// ServerCore 헤더들
#include "ServerCore/CorePch.h"
#include "ServerCore/Service.h"
#include "ServerCore/ThreadManager.h"
#include "Protocol/ServerPacketHandler.h"

#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <array>
#include <DirectXMath.h>
#include "SkillTypes.h"   // ElementType (PendingMonsterVFX)
#include "IAttackBehavior.h" // 네트워크 Golem AttackBehavior 보관용

// 전방 선언
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
class GameObject;
class Scene;
class EnemyComponent;
class FluidSkillVFXManager;

// 네트워크 플레이어 정보 (큐에 저장용)
struct NetworkPlayerInfo
{
    uint64 playerId;
    std::string name;
    int playerType;
    float x, y, z;
};

// 방 클리어 보상 룬 오브젝트 정보
struct NetworkRewardRuneObjectInfo
{
    int64 ownerPlayerId = 0;
    DirectX::XMFLOAT3 pos = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);

    // 서버가 결정한 룬 선택지 3개
    std::array<std::string, 3> runeIds = { "", "", "" };
};

// 플레이어 연출 액션 타입
// 10~ 활성화 룬 시각 동기화: C_PLAYER_ACTION 을 재활용해 charge/enhance/place 의 begin/end 상태를
//   다른 클라에 알린다. dirX 슬롯에 skillSlot 정수를 실어 보낸다 (Q=0,E=1,R=2,RC=3).
//   x/y/z 는 액션 위치 (CHARGE/ENHANCE: 시전자 위치, PLACE: 트랩 좌표).
enum : uint32
{
    PLAYER_ACTION_DASH_CAPE_FLUTTER  = 1,
    PLAYER_ACTION_PORTAL_INTRO_FLY   = 2,

    PLAYER_ACTION_CHARGE_BEGIN       = 10, // dirX = skillSlot
    PLAYER_ACTION_CHARGE_END         = 11, // dirX = skillSlot (cancel or release)

    PLAYER_ACTION_ENHANCE_BEGIN      = 20, // dirX = skillSlot, dirY = durationSec
    PLAYER_ACTION_ENHANCE_END        = 21, // dirX = skillSlot (expire or consume)

    PLAYER_ACTION_PLACE_SPAWN        = 30, // x,y,z = trapPos, dirX = skillSlot
    PLAYER_ACTION_PLACE_FIRE         = 31, // x,y,z = trapPos, dirX = skillSlot

    PLAYER_ACTION_CHANNEL_BEGIN      = 40, // dirX = skillSlot
    PLAYER_ACTION_CHANNEL_END        = 41, // dirX = skillSlot

	PLAYER_ACTION_R_MAGIC_CIRCLE     = 50, // R 스킬 시전 시 마법진 소환 연출  
};

// 패킷 처리를 위한 명령 타입
enum class NetworkCommand
{
    Spawn,
    Despawn,
    Move,
    Skill,
    PlayerAction,
    SetLocalPlayerId,
    RoomTransition,
    MonsterSpawn,
    MonsterMove,
    MonsterDespawn,
    MonsterAttack,
    PlayerDamage,
    MonsterDamage,
    RoomCleared,
    BossEvent,
    MonsterStagger,
	MapTornadoEvent,
    RoomStart,
    RoomRewardSpawn,
    RuneRewardPicked,
    RuneEquip,
    RuneHomingTarget,
    RuneTrigger
};

// 네트워크 명령 구조체
struct NetworkCommandData
{
    NetworkCommand type;
    uint64 playerId;
    std::string name;
    int playerType;
    float x, y, z;
    float dirX, dirY, dirZ;  // 방향 정보
    int skillType;           // 스킬 타입 (Protocol::SkillType)
    uint32 playerActionType = 0; // 플레이어 연출 액션 타입

    // Room transition fields
    uint32 stageIndex;
    uint32 roomIndex;
    bool isBossRoom;
    std::string mapId;

    // Monster fields
    uint64 monsterId;
    uint32 monsterType;
    uint32 monsterAttackType = 0; // 서버 공격 패턴 타입
    uint32 monsterVisualType = 0; // 서버 외형 프리셋 타입
    float monsterYaw;
    float monsterHp;
    bool monsterIsBoss;

    // Combat fields
    uint32 attackType;
    float windupSec;
    float duration;          // 기절/그로기 지속 시간
    uint64 targetPlayerId;
    float damage;
    float currentHp;
    bool  isDead;
    uint64 attackerMonsterId;
    uint64 attackerPlayerId;

    // 공격 이펙트 위치 목록
    std::vector<DirectX::XMFLOAT3> effectPositions;

    // 공격 이펙트 옵션값 
    uint32 effectOption = 0;

    // Boss event fields (S_BOSS_EVENT)
    uint32 bossEventType;   // Protocol::BossEventType (1=Intro, 2=PhaseChange, 3=Death)
    uint32 phaseIndex;

    // 맵 토네이도 이벤트 필드
    float mapTornadoX = 0.0f;
    float mapTornadoY = 0.0f;
    float mapTornadoZ = 0.0f;
    float mapTornadoWarningSec = 0.0f;
    float mapTornadoActiveSec = 0.0f;

    // 방 클리어 보상 오브젝트 필드
    float rewardPortalX = 0.0f;
    float rewardPortalY = 0.0f;
    float rewardPortalZ = 0.0f;

    bool rewardHasSecondPortal = false;
    float rewardSecondPortalX = 0.0f;
    float rewardSecondPortalY = 0.0f;
    float rewardSecondPortalZ = 0.0f;

    std::vector<NetworkRewardRuneObjectInfo> rewardRuneObjects;

    // 룬 장착 결과 필드
    uint32 runeSkillSlot = 0;
    uint32 runeSlotIndex = 0;
    uint32 runeStackCount = 1;
    std::string runeId;

    // 룬 유도 타겟 연출 필드
    int32 runeHomingSkillSlot = 0;
    int32 runeHomingSkillType = 0;
    uint64 runeHomingTargetMonsterId = 0;

    float runeHomingTargetX = 0.0f;
    float runeHomingTargetY = 0.0f;
    float runeHomingTargetZ = 0.0f;

    float runeHomingOriginX = 0.0f;
    float runeHomingOriginY = 0.0f;
    float runeHomingOriginZ = 0.0f;

    // S_RUNE_TRIGGER 필드 (runeId 는 위 공용 필드 재사용, x/y/z 도 재사용)
    int32  runeTriggerSkillSlot       = -1;
    int32  runeTriggerSkillType       = 0;
    int32  runeTriggerType            = 0;
    uint64 runeTriggerTargetMonsterId = 0;
    uint64 runeTriggerTargetPlayerId  = 0;
    float  runeTriggerValue1          = 0.0f;
    float  runeTriggerValue2          = 0.0f;

    // S_SKILL 룬 보정 필드 — 서버 권위 radius/damage 배율을 원격 클라가 그대로 사용
    int32  skillSlot                  = -1;
    float  skillRadiusMult            = 1.0f;
    float  skillDamageMult            = 1.0f;
};

// =============================================================================
// GameSession: 서버와의 세션을 관리하는 클래스
// =============================================================================
class GameSession : public PacketSession
{
public:
    virtual ~GameSession() override;

protected:
    // 서버 연결 성공 시 호출
    virtual void OnConnected() override;

    // 서버로부터 패킷 수신 시 호출
    virtual void OnRecvPacket(BYTE* buffer, int32 len) override;

    // 연결 해제 시 호출
    virtual void OnDisconnected() override;
};

// =============================================================================
// NetworkManager: 네트워크 연결 및 원격 플레이어 관리
// =============================================================================
class NetworkManager
{
public:
    // 싱글톤 접근자
    static NetworkManager* GetInstance();

    NetworkManager();
    ~NetworkManager();

    // 초기화 및 정리
    bool Initialize();
    void Shutdown();

    // 서버 연결
    bool Connect(const std::wstring& ip, uint16 port);
    void Disconnect();
    // 실제로 "합류 완료" 상태: TCP 핸드셰이크 + ENTER_GAME 응답(LocalPlayerId 발급)까지.
    // 서버가 꺼졌거나 핸드셰이크만 된 상태는 false → 호출자는 오프라인 폴백 경로로 빠짐.
    bool IsConnected() const { return m_bConnected && m_pSession != nullptr && m_nLocalPlayerId.load() != 0; }

    // 프레임마다 호출 (큐에 쌓인 명령 처리)
    void Update(Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, float deltaTime);

    // 캐릭터 선택 확정 시 호출 — playerIndex 는 ElementType(0=Fire,1=Water,2=Wind,3=Earth)
    void SendEnterGame(int playerIndex);

    // 로컬 플레이어 이동 전송 (위치 + 방향)
    void SendMove(float x, float y, float z, float dirX, float dirY, float dirZ);

    // 로컬 플레이어 스킬 전송
    void SendSkill(int skillType, float x, float y, float z, float dirX, float dirY, float dirZ);

    // 로컬 플레이어 연출 액션 전송
    void SendPlayerAction(uint32 actionType, float x, float y, float z, float dirX, float dirY, float dirZ);

    // 포탈 상호작용 전송 (F키)
    void SendPortalInteract(uint32 portalType = 0);

    // 방 전투 시작 요청 전송 (상호작용 큐브 F키) — 서버가 몬스터 스폰 트리거
    void SendTorchInteract();

    // 룬 보상 획득 전송
    void SendRuneRewardPick();

    // 룬 장착 요청 전송
    void SendRuneEquip(uint32 rewardOptionIndex, uint32 skillSlot, uint32 runeSlotIndex);

    // 플레이어 공격(히트 판정 요청) 전송 — 서버가 히트 판정 후 S_MONSTER_DAMAGE 브로드캐스트
    void SendPlayerAttack(int skillType,
                          float x, float y, float z,
                          float dirX, float dirY, float dirZ,
                          float targetX, float targetY, float targetZ,
                          float chargeRatio = 0.0f);

    // 보스 컷신 종료 알림 전송
    void SendBossCutsceneEnd(uint64 monsterId, uint32 eventType, uint32 phaseIndex);

    // [DEBUG] 현재 방 전체 즉사 (서버 권위) — F8 키 (F11 은 전체화면과 겹침). 서버가 모든 살아있는 몬스터에
    // 9999 데미지 적용 후 정상적인 S_MONSTER_DAMAGE(isDead=true) → 사망 애니 → S_ROOM_CLEARED 자연 진행
    void SendDebugKillAll();

    // 로컬 플레이어 ID 설정/조회 (atomic으로 스레드 안전)
    void SetLocalPlayerId(uint64 playerId) { m_nLocalPlayerId.store(playerId); }
    uint64 GetLocalPlayerId() const { return m_nLocalPlayerId.load(); }

    // 컷신 중 입력/패킷 차단용
    void SetCutscenePlaying(bool playing) { m_bCutscenePlaying = playing; }
    bool IsCutscenePlaying() const { return m_bCutscenePlaying; }
    bool IsBlockingServerBossIntroActive() const;

    // 세션 참조 설정 (GameSession::OnConnected에서 호출)
    void SetSession(std::shared_ptr<GameSession> session) { m_pSession = session; }
    std::shared_ptr<GameSession> GetSession() const { return m_pSession; }

    // 네트워크 스레드에서 호출 - 명령을 큐에 추가
    void QueueSpawnPlayer(uint64 playerId, const std::string& name, int playerType, float x, float y, float z);
    void QueueDespawnPlayer(uint64 playerId);
    void QueueMovePlayer(uint64 playerId, float x, float y, float z, float dirX, float dirY, float dirZ);
    void QueueSkill(uint64 playerId, int skillType, float x, float y, float z, float dirX, float dirY, float dirZ,
                    int32 skillSlot = -1, float radiusMult = 1.0f, float damageMult = 1.0f);
    void QueuePlayerAction(uint64 playerId, uint32 actionType, float x, float y, float z, float dirX, float dirY, float dirZ);
    void QueueSetLocalPlayerId(uint64 playerId);
    void QueueRoomTransition(uint32 stageIndex, uint32 roomIndex, bool isBossRoom, const std::string& mapId);

    // 몬스터 큐잉 (네트워크 스레드에서 호출 → 메인 스레드에서 처리)
    void QueueMonsterSpawn(uint64 monsterId, uint32 monsterType, uint32 attackType, uint32 visualType, float x, float y, float z, float yaw, float hp, bool isBoss);
    void QueueMonsterMove(uint64 monsterId, float x, float y, float z, float yaw);
    void QueueMonsterDespawn(uint64 monsterId);

    // 전투 큐잉 (S_MONSTER_ATTACK / S_PLAYER_DAMAGE)
    void QueueMonsterAttack(uint64 monsterId, uint64 targetPlayerId, uint32 attackType, float x, float y, float z, float yaw, float windupSec, const std::vector<DirectX::XMFLOAT3>& effectPositions = {}, uint32 effectOption = 0);
    void QueuePlayerDamage(uint64 playerId, float damage, float currentHp, bool isDead, uint64 attackerMonsterId);

    // 방 시작 큐잉
    void QueueRoomStart(uint64 starterPlayerId);

    // 몬스터 피격 / 방 클리어 큐잉 (네트워크 스레드 → 메인 스레드)
    void QueueMonsterDamage(uint64 monsterId, float damage, float currentHp, bool isDead, uint64 attackerPlayerId, int skillType);
    void QueueRoomCleared(uint32 stageIndex, uint32 roomIndex);

	// 방 보상 스폰 큐잉
    void QueueRoomRewardSpawn(uint32 stageIndex, uint32 roomIndex, const DirectX::XMFLOAT3& portalPos, bool hasSecondPortal, const DirectX::XMFLOAT3& secondPortalPos, const std::vector<NetworkRewardRuneObjectInfo>& runeObjects);

	// 룬 보상 획득 큐잉
    void QueueRuneRewardPicked(uint64 ownerPlayerId);

	// 룬 장착 큐잉
    void QueueRuneEquip(uint64 playerId, uint32 skillSlot, uint32 runeSlotIndex, const std::string& runeId, uint32 stackCount);

    // 룬 유도 타겟 큐잉
    void QueueRuneHomingTarget(uint64 playerId, int32 skillSlot, int32 skillType, uint64 targetMonsterId, const DirectX::XMFLOAT3& targetPos, const DirectX::XMFLOAT3& originPos);

    // 룬 발동(S_RUNE_TRIGGER) 큐잉 — 서버 권위 룬 결과를 클라 시각화에 전달
    void QueueRuneTrigger(uint64 playerId, int32 skillSlot, int32 skillType,
                          const std::string& runeId, int32 triggerType,
                          uint64 targetMonsterId, uint64 targetPlayerId,
                          const DirectX::XMFLOAT3& pos, float value1, float value2);

    // 보스 이벤트 (인트로/페이즈 전환/사망 컷씬)
    void QueueBossEvent(uint64 monsterId, uint32 eventType, uint32 phaseIndex);

	// 몬스터 스태거 큐잉 — 피격 시 잠시 멈추는 효과
    void QueueMonsterStagger(uint64 monsterId, float duration);
    
    // 맵 토네이도 이벤트 큐잉
    void QueueMapTornadoEvent(float x, float y, float z, float warningSec, float activeSec);

    // 서버 몬스터 조회
    GameObject* GetServerMonster(uint64 monsterId);
    bool HasServerMonsters() const { return !m_mapServerMonsters.empty(); }
    const std::unordered_map<uint64, GameObject*>& GetServerMonsters() const { return m_mapServerMonsters; }

    // 원격 플레이어 조회
    GameObject* GetRemotePlayer(uint64 playerId);

private:
    static NetworkManager* s_pInstance;

    // 서버 연결 상태
    std::atomic<bool> m_bConnected = false;
    std::atomic<bool> m_bShutdownRequested = false;

    // 로컬 플레이어 ID (atomic으로 스레드 안전하게 관리)
    std::atomic<uint64> m_nLocalPlayerId = 0;

    // 네트워크 서비스 및 세션
    std::shared_ptr<ClientService> m_pService;
    std::shared_ptr<GameSession> m_pSession;

    // 원격 플레이어 관리 (메인 스레드에서만 접근)
    std::unordered_map<uint64, GameObject*> m_mapRemotePlayers;
    // 원격 플레이어의 캐릭터 원소 — ProcessSkill 에서 (element, slot) 매핑에 사용.
    std::unordered_map<uint64, ElementType> m_mapRemotePlayerElement;

    // 원격 플레이어 연출 액션 잠금
    std::unordered_map<uint64, float> m_mapRemotePlayerActionLockTimer;

    // 원격 플레이어 포탈 Intro Fly 연출 상태
    struct RemotePlayerPortalIntroFlyEffect
    {
        float introTimer = 3.0f;        // StartIntroFly(duration) 기존 클라 수치
        float landingHoldTimer = 0.0f; // 착지 후 Landing 유지 시간
        float velocityY = 0.0f;        // PlayerComponent와 같은 자유낙하 방식
        float groundY = 0.0f;          // 포탈 베이스 표면 Y

        float groundX = 0.0f;
        float groundZ = 0.0f;

        bool onGround = false;
    };

    std::unordered_map<uint64, RemotePlayerPortalIntroFlyEffect> m_mapRemotePlayerPortalIntroFlyEffects;

    void UpdateRemotePlayerActionLocks(float deltaTime);
    void UpdateRemotePlayerPortalIntroFlyEffects(float deltaTime);

    // 네트워크 스레드에서 메인 스레드로 전달할 명령 큐
    std::mutex m_queueMutex;
    std::vector<NetworkCommandData> m_vCommandQueue;

    // LocalPlayerId가 설정되기 전에 도착한 Spawn 명령을 보류
    std::vector<NetworkCommandData> m_vPendingSpawns;

    // 메인 스레드에서 실행할 명령 처리
    void ProcessSpawnPlayer(Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, uint64 playerId, const std::string& name, int playerType, float x, float y, float z);
    void ProcessDespawnPlayer(Scene* pScene, uint64 playerId);
    void ProcessMovePlayer(uint64 playerId, float x, float y, float z, float dirX, float dirY, float dirZ);
    void ProcessSkill(Scene* pScene, uint64 playerId, int skillType, float x, float y, float z, float dirX, float dirY, float dirZ,
                      int32 skillSlot = -1, float radiusMult = 1.0f, float damageMult = 1.0f);
    void ProcessPlayerAction(Scene* pScene, uint64 playerId, uint32 actionType, float x, float y, float z, float dirX, float dirY, float dirZ);
    void ProcessRoomTransition(Scene* pScene, uint32 stageIndex, uint32 roomIndex, bool isBossRoom, const std::string& mapId);

    // 몬스터 처리 (메인 스레드)
    void ProcessMonsterSpawn(Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, uint64 monsterId, uint32 monsterType, uint32 attackType, uint32 visualType, float x, float y, float z, float yaw, float hp, bool isBoss);
    void ProcessMonsterMove(uint64 monsterId, float x, float y, float z, float yaw);
    void ProcessMonsterDespawn(Scene* pScene, uint64 monsterId);

    // 전투 처리 (메인 스레드)
    void ProcessMonsterAttack(Scene* pScene, uint64 monsterId, uint32 attackType, float windupSec, uint64 targetPlayerId, float atkX, float atkY, float atkZ, const std::vector<DirectX::XMFLOAT3>& effectPositions, uint32 effectOption);
    void ProcessPlayerDamage(Scene* pScene, uint64 playerId, float damage, float currentHp, bool isDead, uint64 attackerMonsterId);
    void ProcessMonsterDamage(Scene* pScene, uint64 monsterId, float damage, float currentHp, bool isDead, uint64 attackerPlayerId, int skillType);
    void ProcessRoomCleared(Scene* pScene, uint32 stageIndex, uint32 roomIndex);
    void ProcessRoomStart(Scene* pScene, uint64 starterPlayerId);
    void ProcessRoomRewardSpawn(Scene* pScene, uint32 stageIndex, uint32 roomIndex, const DirectX::XMFLOAT3& portalPos, bool hasSecondPortal, const DirectX::XMFLOAT3& secondPortalPos, const std::vector<NetworkRewardRuneObjectInfo>& runeObjects);
    void ProcessRuneRewardPicked(Scene* pScene, uint64 ownerPlayerId);
    void ProcessRuneEquip(Scene* pScene, uint64 playerId, uint32 skillSlot, uint32 runeSlotIndex, const std::string& runeId, uint32 stackCount);
    void ProcessRuneHomingTarget(Scene* pScene, uint64 playerId, int32 skillSlot, int32 skillType, uint64 targetMonsterId, const DirectX::XMFLOAT3& targetPos, const DirectX::XMFLOAT3& originPos);
    void ProcessRuneTrigger(Scene* pScene,
                            uint64 playerId, int32 skillSlot, int32 skillType,
                            const std::string& runeId, int32 triggerType,
                            uint64 targetMonsterId, uint64 targetPlayerId,
                            const DirectX::XMFLOAT3& pos, float value1, float value2);

    // 메아리(ABY_ECO) ECHO_FIRE 시 원본 스킬 시각을 echo 위치에 50% 스케일로 재생
    void SpawnEchoSkillVFX(Scene* pScene, int skillType, ElementType element,
                           const DirectX::XMFLOAT3& pos, uint32_t runeFlags = 0);

    // playerId 기준 원소 조회 — local 은 PlayerComponent, remote 는 m_mapRemotePlayerElement
    ElementType GetPlayerElement(uint64 playerId) const;
    void ProcessBossEvent(Scene* pScene, uint64 monsterId, uint32 eventType, uint32 phaseIndex);
    void ProcessMonsterStagger(uint64 monsterId, float duration);

	// 몬스터 공격 애니메이션 재생 (ProcessMonsterAttack에서 호출)
    void PlayNetworkGolemAttackBehavior(Scene* pScene, GameObject* pMonster, uint64 monsterId, uint32 attackType, uint64 targetPlayerId, const std::vector<DirectX::XMFLOAT3>& effectPositions, uint32 effectOption);

    // 서버 권위 일반 몬스터 공격 연출 실행
    void PlayNetworkNormalMonsterAttackBehavior(Scene* pScene, GameObject* pMonster, uint64 monsterId, uint32 monsterType, uint32 attackType, uint64 targetPlayerId);

    // 서버 몬스터 관리 (메인 스레드에서만 접근)
    std::unordered_map<uint64, GameObject*> m_mapServerMonsters;

    // 몬스터별 preset 클립 이름 (Idle/Walk/Attack/Death 각 모델별로 다름)
    // monsterType 도 함께 보관 — ProcessMonsterAttack 에서 (monsterType, attackType) 별 클립 분기에 사용
    struct ServerMonsterClips
    {
        std::string idle, walk, attack, death;
        uint32 monsterType = 0;
        uint32 attackType = 0;
        uint32 visualType = 0;
        bool   isBoss = false;
        bool   isMiniBoss = false;
    };
    std::unordered_map<uint64, ServerMonsterClips> m_mapServerMonsterClips;

    // 서버 몬스터 현재 애니메이션 클립 캐시
    std::unordered_map<uint64, std::string> m_mapServerMonsterCurrentAnimClip;

    // 공격 애니 재생 중인 몬스터 — 이 시간 동안은 Move 와서도 Walk 로 덮어쓰지 않음
    std::unordered_map<uint64, float> m_mapServerMonsterAttackTimer;
    static constexpr float ATTACK_ANIM_LOCK = 1.4f;  // 공격 애니 지속 (대략)

    // 서버 몬스터 hit flash 타이머 — 피격 시 glow 페이드아웃 (원격 플레이어와 동일 패턴)
    std::unordered_map<uint64, float> m_mapServerMonsterHitFlashTimer;
    static constexpr float SERVER_MONSTER_HIT_FLASH_DURATION = 0.15f;

    // 사망 애니 재생된 서버 몬스터 ID — 이후 Move/Idle/Attack 전환 skip
    std::unordered_set<uint64> m_setDeadServerMonsters;

    // 방 전환 중복 방지 — S_ROOM_TRANSITION 이 서버에서 중복 전송되거나, 같은 프레임에
    // 두 번 큐잉되는 경우 ProcessRoomTransition 을 여러 번 실행해 자원 이중 정리/생성으로
    // 크래시 나는 케이스 차단. 전환 처리 진입 시 true, 완료 시 false.
    bool m_bPendingTorchInteract = false;
    int m_nPendingTorchInteractFrame = 0;
    bool m_bInRoomTransition = false;

    // 보스 이벤트 중복 방지
    bool m_bCutscenePlaying = false;
    float m_fMegaBreathInputLockTimer = 0.0f;

    // 서버 MOVE 패킷 간격이 띄엄띄엄해서 직접 SetPosition하면 순간이동처럼 보임.
    // 타겟 pos/yaw 저장 → 매 프레임 exponential smoothing으로 접근.
    struct ServerMonsterTarget
    {
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        float yaw = 0.0f;
        bool  hasTarget = false;
    };
    std::unordered_map<uint64, ServerMonsterTarget> m_mapServerMonsterTarget;

    // 서버 몬스터 스폰 등장 연출 상태
    // 서버 몬스터는 이미 생성해두되, 일정 시간 동안 공중 포탈 위치에 머물렀다가 바닥으로 낙하시킨다.
    struct ServerMonsterSpawnEffect
    {
        float elapsed = 0.0f;

        float portalDelay = 0.7f;
        float fallTime = 0.4f;
        float portalHeight = 6.0f;

        float groundX = 0.0f;
        float groundY = 0.0f;
        float groundZ = 0.0f;

        float yaw = 0.0f;
    };
    std::unordered_map<uint64, ServerMonsterSpawnEffect> m_mapServerMonsterSpawnEffects;

    // 보스 spawn 위치 — MegaBreath cover 4개 spawn 좌표 (방 중심).
    // ProcessMonsterSpawn 시점에 boss 면 기록.
    struct ServerBossSpawnPos { float x = 0, y = 0, z = 0; };
    std::unordered_map<uint64, ServerBossSpawnPos> m_mapServerBossSpawnPos;

    // ── 보스 인트로 컷신 (BOSS_EVENT_INTRO 수신 시 활성) ─────────────────────
    //   클라 오프라인 EnemyComponent::StartBossIntro 4단계(FlyingIn → Landing → Roaring)를
    //   네트워크 보스에도 적용. InterpolateServerMonsters 직후 yOffset 을 적용해 시각적으로 강하`.
    //   진입 단계에서는 서버 위치 보간을 그대로 사용하되 Y 만 인트로 곡선으로 강제 오버라이드.
    // 오프라인 EnemyComponent::StartBossIntro 와 동일한 시퀀스 (FlyingIn 등속 강하 → Landing → Roaring)
    // 오프라인 EnemyComponent::StartBossIntro 와 동일한 시퀀스 + Outro 블렌드
    enum class BossIntroPhase { FlyingIn = 0, Landing = 1, Roaring = 2, Outro = 3, Done = 4 };
    struct ServerBossIntroState
    {
        BossIntroPhase phase = BossIntroPhase::FlyingIn;
        float phaseTimer  = 0.0f;
        float startHeight = 25.0f; // 오프라인 동일 (Red Dragon 25u)
        float groundY     = 0.0f;  // 착지 목표 y
        float curY        = 25.0f; // 매 프레임 갱신 y (8u/s 등속 강하)
        float bossX       = 0.0f;
        float bossZ       = 0.0f;
        bool  flyAnimFired  = false;
        bool  landAnimFired = false;
        bool  roarAnimFired = false;
        bool  active        = true;
    };
    std::unordered_map<uint64, ServerBossIntroState> m_mapServerBossIntros;

    // ── MegaBreath 컷신 (옵션A — 클라 단독 처리, 오프라인 MegaBreathAttackBehavior 1:1 이식) ─
    //   서버는 위치 broadcast 안 하고 데미지만 (Breathing 윈도우). 클라가 보스 시각 위치 + 카메라 +
    //   기둥 + SPH 5빔 모두 직접 제어.
    enum class MegaBreathPhase
    {
        None = 0,
        TakeOff,        // 0.9s  보스 y +0→18 easeIn
        MoveToWall,     // 3.0s  x,z origin→wall easeOut
        Landing,        // 0.7s  y +18→0 easeOut
        SpawnCover,     // 1.2s  4 기둥 spawn + establishing camera
        Windup,         // 5.5s  방 중심 향해 회전 + charge VFX
        Breath,         // 6.5s  5-fan SPH beam + 0.4s tick 데미지 (서버 처리)
        Recovery,       // 1.2s  beam stop + cover despawn + 카메라 블렌드
        ReturnTakeOff,  // 0.9s  y 0→18
        ReturnFly,      // 3.0s  x,z wall→origin
        ReturnLand,     // 0.7s  y 18→0
        Done
    };
    struct ServerMegaBreathCutscene
    {
        MegaBreathPhase phase = MegaBreathPhase::None;
        float phaseTimer = 0.0f;
        // 위치
        DirectX::XMFLOAT3 originalPos{0,0,0};   // 보스 진입 직전 위치 (복귀 목표)
        DirectX::XMFLOAT3 wallPos{0,0,0};       // 보스 비행 목표 (cardinal +25 from spawn)
        DirectX::XMFLOAT3 phaseStartPos{0,0,0}; // 각 phase 시작 시 보스 위치 (lerp 시작점)
        DirectX::XMFLOAT3 bossSpawnPos{0,0,0};  // 보스 spawn 좌표 (cover/카메라 reference)
        // VFX
        Vector<GameObject*> covers;             // 4 기둥
        int chargeVFXId = -1;
        int beamVFXIds[5] = { -1, -1, -1, -1, -1 };
        // 카메라 블렌드 상태
        DirectX::XMFLOAT3 camLookAt{0,0,0};
        float camDist = 50.f;
        float camPitch = 60.f;
        float camYaw = 45.f;
        bool  camInit = false;
        bool  active = false;
    };
    std::unordered_map<uint64, ServerMegaBreathCutscene> m_mapServerMegaBreathCutscenes;

    // ── 보스 액션 yOffset (점프/비행 시각화) ────────────────────────────────
    //   서버는 보스 위치를 XZ 만 다루므로, 클라가 attackType 별로 적절한 yOffset 곡선을 적용해
    //   "점프", "비행" 같은 시각 효과를 만들어준다 (실제 충돌은 서버 권위 그대로).
    enum class BossActionKind { None = 0, Jump = 1, Flying = 2 };
    struct ServerBossAction
    {
        BossActionKind kind = BossActionKind::None;
        float timer    = 0.0f;
        float duration = 0.0f;
        float peakHeight = 0.0f;  // Jump: 포물선 정점 높이, Flying: 비행 고도
    };
    std::unordered_map<uint64, ServerBossAction> m_mapServerBossActions;
public:
    // 매 프레임 인트로 상태 갱신 (Dx12App 에서 호출). 활성 보스의 yOffset 을 갱신하고 종료 처리.
    void UpdateServerBossIntros(Scene* pScene, float deltaTime);
    // 매 프레임 MegaBreath 컷신 잔여 시간 감소 + 종료 시 기둥 정리.
    void UpdateServerMegaBreathCutscenes(Scene* pScene, float deltaTime);
    // 매 프레임 보스 액션(Jump/Flying) yOffset 적용 + 종료 처리.
    void UpdateServerBossActions(Scene* pScene, float deltaTime);

    void StartMegaBreathInputLock(float duration)
    {
        m_fMegaBreathInputLockTimer = duration;
    }

    bool IsMegaBreathInputLockActive() const
    {
        return m_fMegaBreathInputLockTimer > 0.0f;
    }
private:

public:
    // ── 서버 보스 인디케이터 (Circle / ForwardBox) ──────────────────────────
    //   isBoss=true 로 스폰된 NetMonster 마다 4개의 인디케이터 GameObject(원/박스 × border/fill) 사전 할당.
    //   ProcessMonsterAttack 에서 (monsterType, attackType) 로 타입/크기 결정 → activeType 세팅 + windup 시작.
    //   CheckServerMonsterIdle 매 프레임 fill 스케일 갱신, windup 종료 시 hide.
    //   public — GetIndicatorParamsForAttack 자유 함수에서 사용
    enum class NetIndicatorType { None = 0, Circle = 1, ForwardBox = 2 };
private:
    struct ServerMonsterIndicators
    {
        GameObject* circleBorder = nullptr;
        GameObject* circleFill   = nullptr;
        GameObject* boxBorder    = nullptr;
        GameObject* boxFill      = nullptr;

        NetIndicatorType activeType = NetIndicatorType::None;
        float windupTotal = 0.0f;
        float windupTimer = 0.0f;      // 0..windupTotal, 만료 시 hide
        float hitRadius = 0.0f;        // Circle: r, ForwardBox: half-width
        float hitLength = 0.0f;        // ForwardBox: 전방 길이
        DirectX::XMFLOAT3 tint = DirectX::XMFLOAT3(1.0f, 0.1f, 0.1f); // 인디케이터 색상
        uint32 attackType = 0;         // 현재 인디케이터 공격 타입
        float anchorX = 0.0f;
        float anchorY = 0.0f;
        float anchorZ = 0.0f;
        float yawDeg  = 0.0f;          // ForwardBox 회전
    };
    std::unordered_map<uint64, ServerMonsterIndicators> m_mapServerMonsterIndicators;

    // 인디케이터 모두 hide (지면 아래 + scale 0)
    void HideMonsterIndicators(ServerMonsterIndicators& ind);

    // ── 보스 VFX 지연 스폰 큐 ─────────────────────────────────────────────
    //   오프라인 IAttackBehavior 처럼 windup 후 + breathDuration 동안 staggered 발사 재현.
    //   ProcessMonsterAttack 에서 attackType 별로 delay 가 다른 여러 항목을 push.
    //   매 프레임 delay 감소 → 0 도달 시 ProjectileManager / SpawnExplosionParticles 호출.
    enum class PendingVFXKind { Projectile, Explosion, CameraShake, SPHBeam };
    struct PendingMonsterVFX
    {
        PendingVFXKind kind = PendingVFXKind::Projectile;
        float    delay     = 0.0f;
        // 보스 실시간 위치 추적 (0 이면 추적 안 함 — 캐시된 startPos 사용).
        //   Projectile 의 경우: 보스가 windup/breath 동안 움직여도 입에서 발사되도록 매 스폰 시점에 위치 재조회.
        uint64   monsterId = 0;
        DirectX::XMFLOAT3 startPos  = { 0.f, 0.f, 0.f };  // 캐시된 fallback 위치
        DirectX::XMFLOAT3 targetPos = { 0.f, 0.f, 0.f };  // 캐시된 타겟 (player at packet time)
        float    yOffset   = 2.0f;       // 보스 base 에서 입 높이 (Projectile 만)
        float    fanAngleDeg = 0.0f;     // 정면 forward 기준 ± offset (Projectile/SPHBeam)
        float    fireRange = 60.0f;      // 타겟까지 투영 거리 (Projectile 만)
        float    speed     = 18.0f;
        float    radius    = 0.5f;
        float    scale     = 1.0f;
        float    maxDist   = 60.0f;
        ElementType element = ElementType::Fire;
        // CameraShake 전용
        float    shakeIntensity = 0.0f;
        float    shakeDuration  = 0.0f;
        // SPHBeam 전용 — 오프라인 MegaBreath 5-fan beam 재현
        int      beamParticleCount = 4400;
        float    beamSpreadMult    = 1.0f;
        float    beamLength        = 70.0f;
        float    beamDuration      = 4.0f;
    };
    std::vector<PendingMonsterVFX> m_vPendingMonsterVFX;

    // 네트워크 일반 몬스터 공격 연출 Entry
    struct NetworkNormalMonsterBehaviorEntry { uint64 monsterId = 0; };
    std::vector<NetworkNormalMonsterBehaviorEntry> m_vNetworkNormalMonsterBehaviors;

    // 네트워크 Golem 전용 AttackBehavior 보관
    struct NetworkGolemBehaviorEntry
    {
        std::unique_ptr<IAttackBehavior> behavior;
        EnemyComponent* owner = nullptr;
        float timer = 0.0f;
    };
    std::vector<NetworkGolemBehaviorEntry> m_vNetworkGolemBehaviors;

	// 네트워크 Demon 전용 AttackBehavior 보관 
    struct NetworkDemonBehaviorEntry
    {
        std::unique_ptr<IAttackBehavior> behavior;
        EnemyComponent* owner = nullptr;
        float timer = 0.0f;
    };
    std::vector<NetworkDemonBehaviorEntry> m_vNetworkDemonBehaviors;

    void PlayNetworkDemonAttackBehavior(Scene* pScene, GameObject* pMonster, uint64 monsterId, uint32 attackType, const std::vector<DirectX::XMFLOAT3>& effectPositions, uint32 effectOption);

public:
    // 매 프레임 타겟을 향해 몬스터 transform 보간 (Dx12App 메인 루프에서 호출)
    void InterpolateServerMonsters(float deltaTime);

    // 매 프레임 서버 몬스터 스폰 연출 갱신
    bool IsServerMonsterSpawnEffectActive(uint64 monsterId) const;
    void UpdateServerMonsterSpawnEffects(float deltaTime);
    
    // 서버 보스 인디케이터(Circle/ForwardBox) fill 진행도 갱신 (Dx12App 메인 루프에서 호출)
    //   ProcessMonsterAttack 에서 windup 시작, 매 프레임 0→1 차오르고 종료 시 hide.
    void UpdateServerMonsterIndicators(float deltaTime);

    // 지연 VFX 큐 tick — windup 후/동안 스폰. 매 프레임 호출.
    void UpdatePendingMonsterVFX(Scene* pScene, float deltaTime);

    // 네트워크 일반 몬스터 공격 연출 업데이트
    void UpdateNetworkNormalMonsterBehaviors(float deltaTime);

    // 네트워크 Golem 전용 AttackBehavior 갱신
    void UpdateNetworkGolemBehaviors(float deltaTime);

	// 네트워크 Demon 전용 AttackBehavior 갱신
    void UpdateNetworkDemonBehaviors(float deltaTime);

private:
    std::unordered_map<uint64, float> m_mapServerMonsterMoveTime;  // idle 전환용

    // 로컬 플레이어 초기 위치 동기화 예약
    int m_nInitialLocalPositionSyncFrames = 0;

    // 로컬 플레이어의 실제 시작 위치를 서버에 알려준다.
    void SyncLocalPlayerPositionToServer(Scene* pScene);
     
    // 원격 플레이어 마지막 이동 시간 (idle 전환용)
    std::unordered_map<uint64, float> m_mapRemotePlayerMoveTime;

    // 사망한 원격 플레이어 ID — 데스 애니 유지용 (move/idle 전환 skip)
    std::unordered_set<uint64> m_setDeadRemotePlayers;

    // 원격 플레이어 hit flash 타이머 — 피격 시 glow 가 남지 않도록 페이드아웃
    std::unordered_map<uint64, float> m_mapRemotePlayerHitFlashTimer;
    static constexpr float REMOTE_HIT_FLASH_DURATION = 0.15f;

    // idle 전환까지 대기 시간 (초)
    static constexpr float IDLE_TRANSITION_TIME = 0.15f;

    // 원격 플레이어 활성화 룬(차지/증강) VFX id — 시작 시 spawn, 종료 시 stop.
    //   매 프레임 원격 플레이어 위치로 TrackEffect 호출해서 발 아래에 붙어있도록 한다.
    std::unordered_map<uint64, int> m_mapRemoteChargeVFXId;
    std::unordered_map<uint64, int> m_mapRemoteEnhanceVFXId;

    // 채널 룬 활성 원격 플레이어 — ProcessSkill 가 매 tick 마다 Attack1 CrossFade 를 새로 호출해서
    // 애니메이션이 0.2초마다 끊겨 보이고 렉처럼 느껴지는 문제를 막기 위한 마커.
    // CHANNEL_BEGIN 도착 시 추가, CHANNEL_END 도착 시 제거.
    std::unordered_set<uint64> m_setRemoteChannelingPlayers;

    // 채널 중 ProcessSkill VFX 스폰 throttle — 0.33초 (3 Hz) 이내 동일 플레이어 spawn 요청 skip.
    //   서버는 5 Hz (0.2초) 로 tick 보내지만 원격에서 spawn 비용이 누적되어 렉 유발.
    std::unordered_map<uint64, float> m_mapRemoteChannelLastSpawnTime;
    float m_fNetworkAccumulatedTime = 0.f; // throttle 비교용 시간 누적

    // 원격 플레이어 설치 룬(TRF_DEP) 데칼 ID — (playerId, skillSlot) 단위로 추적해서
    //   같은 슬롯 재설치 시 이전 데칼을 Stop. lifetime 30s 가 남아 화면에 잔류하는 문제 방지.
    std::unordered_map<uint64, std::array<int, 4>> m_mapRemotePlaceDecalIds;

    // 궤도 룬(TRF_ORB) — 원격 플레이어 RC 발사 시 0.5초 공전 visual 후 실제 투사체 spawn.
    //   서버는 즉시 데미지 처리하므로 visual 만 살짝 늦지만 수용 가능.
    struct PendingOrbitalProjectile
    {
        DirectX::XMFLOAT3 origin{};
        DirectX::XMFLOAT3 target{};
        float speed = 30.f;
        float radius = 0.5f;
        float explosionRadius = 3.f;
        float scale = 1.f;
        ElementType element = ElementType::Fire;
        GameObject* owner = nullptr;
        bool isPiercing = false;
        int orbVfxId = -1;
        float delay = 0.5f;
    };
    std::vector<PendingOrbitalProjectile> m_vPendingOrbitals;

public:
    // 매 프레임 호출 — 궤도 deferred 큐 tick
    void UpdatePendingOrbitals(Scene* pScene, float deltaTime);
private:

    // 원격 플레이어 VFX 상태 (채널링 스킬 방향 추적용)
    struct RemoteVFXState
    {
        int   vfxId          = -1;
        int   skillType      = 0;
        float lastUpdateTime = 0.0f;     // 마지막 패킷 이후 경과 시간
        float maxIdleTime    = 0.2f;     // 이 시간 동안 패킷 없으면 종료 (채널 종료 감지용)
        float totalElapsed   = 0.0f;     // spawn 이후 누적 시간
        float maxLifetime    = 0.0f;     // 0=무제한(idle 기반). >0 이면 강제 종료 시각 (스킬 로컬 DURATION 과 일치)
    };
    std::unordered_map<uint64, RemoteVFXState> m_mapRemotePlayerVFX;

    // 단발 multi-layer 스킬용 — 고정 lifetime 후 자동 StopEffect. player 와 무관.
    // (예: Water Q 의 fall + puddle 같이 한 스킬이 여러 vfxId 를 spawn 하고
    //   EffectDef.duration=-1 이라 자동 종료가 없는 경우.)
    struct TimedVFXKill
    {
        int   vfxId     = -1;
        float remaining = 0.0f;
    };
    std::vector<TimedVFXKill> m_vTimedVFXKills;

    // Fire R Meteor 샤워 시뮬레이션 — 원격 측 (MeteorBehavior::Update 와 같은 흐름의 경량 재현).
    struct PendingSmallMeteor
    {
        DirectX::XMFLOAT3 scatterPos;       // 지면 착지점
        DirectX::XMFLOAT3 spawnPos;         // scatterPos + (0, height, 0)
        float delayUntilSpawn = 0.0f;       // shower 시작 후 trail spawn 까지의 대기
        bool  spawned         = false;
        int   trailVfxId      = -1;
        float fallElapsed     = 0.0f;
        float fallDuration    = 0.0f;
        bool  impacted        = false;
    };
    struct PendingMeteorShower
    {
        DirectX::XMFLOAT3 targetPos;
        std::vector<PendingSmallMeteor> smallMeteors;
        float elapsed         = 0.0f;       // shower 시작 후 누적 시간
        bool  finalSpawned    = false;
        int   finalTrailId    = -1;
        int   finalOuterId    = -1;
        DirectX::XMFLOAT3 finalSpawnPos;
        float finalFallElapsed   = 0.0f;
        float finalFallDuration  = 0.0f;
        bool  finalImpacted   = false;
        float postImpactKeepalive = 0.0f;   // impact 후 잠시 더 살려두기 (impact VFX 자체 lifetime 위해)
    };
    std::vector<PendingMeteorShower> m_vPendingMeteorShowers;
    void TickPendingMeteorShowers(FluidSkillVFXManager* pVFXManager, float deltaTime);

    // VFX 기본 타임아웃 (초) - 이 시간 동안 스킬 패킷이 없으면 VFX 종료 (true channel 용)
    static constexpr float VFX_TIMEOUT = 0.2f;

public:
    // 원격 플레이어 idle 전환 체크 (Update에서 호출)
    void CheckRemotePlayerIdle(float deltaTime);

    // 원격 플레이어 VFX 타임아웃 체크 (Update에서 호출)
    void CheckRemotePlayerVFXTimeout(Scene* pScene, float deltaTime);

    // 활성화 룬 VFX 위치 추적 — charge_gather/enhance aura 가 원격 플레이어 발 아래 따라가도록 갱신
    void UpdateRemoteActivationRuneVFX(Scene* pScene);

    // 서버 몬스터 idle 전환 체크 (Update에서 호출)
    void CheckServerMonsterIdle(float deltaTime);
};
