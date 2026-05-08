#pragma once

// ServerCore 헤더들
#include "ServerCore/CorePch.h"
#include "ServerCore/Service.h"
#include "ServerCore/ThreadManager.h"
#include "Protocol/ServerPacketHandler.h"

#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <DirectXMath.h>
#include "SkillTypes.h"   // ElementType (PendingMonsterVFX)

// 전방 선언
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
class GameObject;
class Scene;

// 네트워크 플레이어 정보 (큐에 저장용)
struct NetworkPlayerInfo
{
    uint64 playerId;
    std::string name;
    int playerType;
    float x, y, z;
};

// 패킷 처리를 위한 명령 타입
enum class NetworkCommand
{
    Spawn,
    Despawn,
    Move,
    Skill,
    SetLocalPlayerId,
    RoomTransition,
    MonsterSpawn,
    MonsterMove,
    MonsterDespawn,
    MonsterAttack,
    PlayerDamage,
    MonsterDamage,
    RoomCleared,
    BossEvent
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

    // Room transition fields
    uint32 stageIndex;
    uint32 roomIndex;
    bool isBossRoom;
    std::string mapId;

    // Monster fields
    uint64 monsterId;
    uint32 monsterType;
    float monsterYaw;
    float monsterHp;
    bool monsterIsBoss;

    // Combat fields
    uint32 attackType;
    float windupSec;
    uint64 targetPlayerId;
    float damage;
    float currentHp;
    bool  isDead;
    uint64 attackerMonsterId;
    uint64 attackerPlayerId;

    // Boss event fields (S_BOSS_EVENT)
    uint32 bossEventType;   // Protocol::BossEventType (1=Intro, 2=PhaseChange, 3=Death)
    uint32 phaseIndex;
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
    void Update(Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList);

    // 로컬 플레이어 이동 전송 (위치 + 방향)
    void SendMove(float x, float y, float z, float dirX, float dirY, float dirZ);

    // 로컬 플레이어 스킬 전송
    void SendSkill(int skillType, float x, float y, float z, float dirX, float dirY, float dirZ);

    // 포탈 상호작용 전송 (F키)
    void SendPortalInteract();

    // 방 전투 시작 요청 전송 (상호작용 큐브 F키) — 서버가 몬스터 스폰 트리거
    void SendTorchInteract();

    // 플레이어 공격(히트 판정 요청) 전송 — 서버가 히트 판정 후 S_MONSTER_DAMAGE 브로드캐스트
    void SendPlayerAttack(int skillType,
                          float x, float y, float z,
                          float dirX, float dirY, float dirZ,
                          float targetX, float targetY, float targetZ);

    // [DEBUG] 현재 방 전체 즉사 (서버 권위) — F8 키 (F11 은 전체화면과 겹침). 서버가 모든 살아있는 몬스터에
    // 9999 데미지 적용 후 정상적인 S_MONSTER_DAMAGE(isDead=true) → 사망 애니 → S_ROOM_CLEARED 자연 진행
    void SendDebugKillAll();

    // 로컬 플레이어 ID 설정/조회 (atomic으로 스레드 안전)
    void SetLocalPlayerId(uint64 playerId) { m_nLocalPlayerId.store(playerId); }
    uint64 GetLocalPlayerId() const { return m_nLocalPlayerId.load(); }

    // 세션 참조 설정 (GameSession::OnConnected에서 호출)
    void SetSession(std::shared_ptr<GameSession> session) { m_pSession = session; }
    std::shared_ptr<GameSession> GetSession() const { return m_pSession; }

    // 네트워크 스레드에서 호출 - 명령을 큐에 추가
    void QueueSpawnPlayer(uint64 playerId, const std::string& name, int playerType, float x, float y, float z);
    void QueueDespawnPlayer(uint64 playerId);
    void QueueMovePlayer(uint64 playerId, float x, float y, float z, float dirX, float dirY, float dirZ);
    void QueueSkill(uint64 playerId, int skillType, float x, float y, float z, float dirX, float dirY, float dirZ);
    void QueueSetLocalPlayerId(uint64 playerId);
    void QueueRoomTransition(uint32 stageIndex, uint32 roomIndex, bool isBossRoom, const std::string& mapId);

    // 몬스터 큐잉 (네트워크 스레드에서 호출 → 메인 스레드에서 처리)
    void QueueMonsterSpawn(uint64 monsterId, uint32 monsterType,
                           float x, float y, float z, float yaw,
                           float hp, bool isBoss);
    void QueueMonsterMove(uint64 monsterId, float x, float y, float z, float yaw);
    void QueueMonsterDespawn(uint64 monsterId);

    // 전투 큐잉 (S_MONSTER_ATTACK / S_PLAYER_DAMAGE)
    void QueueMonsterAttack(uint64 monsterId, uint64 targetPlayerId, uint32 attackType,
                            float x, float y, float z, float yaw, float windupSec);
    void QueuePlayerDamage(uint64 playerId, float damage, float currentHp, bool isDead, uint64 attackerMonsterId);

    // 몬스터 피격 / 방 클리어 큐잉 (네트워크 스레드 → 메인 스레드)
    void QueueMonsterDamage(uint64 monsterId, float damage, float currentHp, bool isDead,
                            uint64 attackerPlayerId, int skillType);
    void QueueRoomCleared(uint32 stageIndex, uint32 roomIndex);

    // 보스 이벤트 (인트로/페이즈 전환/사망 컷씬)
    void QueueBossEvent(uint64 monsterId, uint32 eventType, uint32 phaseIndex);

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

    // 네트워크 스레드에서 메인 스레드로 전달할 명령 큐
    std::mutex m_queueMutex;
    std::vector<NetworkCommandData> m_vCommandQueue;

    // LocalPlayerId가 설정되기 전에 도착한 Spawn 명령을 보류
    std::vector<NetworkCommandData> m_vPendingSpawns;

    // 메인 스레드에서 실행할 명령 처리
    void ProcessSpawnPlayer(Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                           uint64 playerId, const std::string& name, int playerType, float x, float y, float z);
    void ProcessDespawnPlayer(Scene* pScene, uint64 playerId);
    void ProcessMovePlayer(uint64 playerId, float x, float y, float z, float dirX, float dirY, float dirZ);
    void ProcessSkill(Scene* pScene, uint64 playerId, int skillType, float x, float y, float z, float dirX, float dirY, float dirZ);
    void ProcessRoomTransition(Scene* pScene, uint32 stageIndex, uint32 roomIndex, bool isBossRoom, const std::string& mapId);

    // 몬스터 처리 (메인 스레드)
    void ProcessMonsterSpawn(Scene* pScene, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                             uint64 monsterId, uint32 monsterType,
                             float x, float y, float z, float yaw,
                             float hp, bool isBoss);
    void ProcessMonsterMove(uint64 monsterId, float x, float y, float z, float yaw);
    void ProcessMonsterDespawn(Scene* pScene, uint64 monsterId);

    // 전투 처리 (메인 스레드)
    void ProcessMonsterAttack(Scene* pScene, uint64 monsterId, uint32 attackType, float windupSec,
                              uint64 targetPlayerId, float atkX, float atkY, float atkZ);
    void ProcessPlayerDamage(Scene* pScene, uint64 playerId, float damage, float currentHp, bool isDead, uint64 attackerMonsterId);
    void ProcessMonsterDamage(Scene* pScene, uint64 monsterId, float damage, float currentHp, bool isDead,
                              uint64 attackerPlayerId, int skillType);
    void ProcessRoomCleared(Scene* pScene, uint32 stageIndex, uint32 roomIndex);
    void ProcessBossEvent(Scene* pScene, uint64 monsterId, uint32 eventType, uint32 phaseIndex);

    // 서버 몬스터 관리 (메인 스레드에서만 접근)
    std::unordered_map<uint64, GameObject*> m_mapServerMonsters;

    // 몬스터별 preset 클립 이름 (Idle/Walk/Attack/Death 각 모델별로 다름)
    // monsterType 도 함께 보관 — ProcessMonsterAttack 에서 (monsterType, attackType) 별 클립 분기에 사용
    struct ServerMonsterClips
    {
        std::string idle, walk, attack, death;
        uint32 monsterType = 0;
        bool   isBoss = false;
    };
    std::unordered_map<uint64, ServerMonsterClips> m_mapServerMonsterClips;

    // 공격 애니 재생 중인 몬스터 — 이 시간 동안은 Move 와서도 Walk 로 덮어쓰지 않음
    std::unordered_map<uint64, float> m_mapServerMonsterAttackTimer;
    static constexpr float ATTACK_ANIM_LOCK = 0.6f;  // 공격 애니 지속 (대략)

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

    // 서버 MOVE 패킷 간격이 띄엄띄엄해서 직접 SetPosition하면 순간이동처럼 보임.
    // 타겟 pos/yaw 저장 → 매 프레임 exponential smoothing으로 접근.
    struct ServerMonsterTarget
    {
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        float yaw = 0.0f;
        bool  hasTarget = false;
    };
    std::unordered_map<uint64, ServerMonsterTarget> m_mapServerMonsterTarget;

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
        float windupTimer = 0.0f;     // 0..windupTotal, 만료 시 hide
        float hitRadius = 0.0f;        // Circle: r, ForwardBox: half-width
        float hitLength = 0.0f;        // ForwardBox: 전방 길이
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
    enum class PendingVFXKind { Projectile, Explosion, CameraShake };
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
        float    fanAngleDeg = 0.0f;     // 정면 forward 기준 ± offset (Projectile 만)
        float    fireRange = 60.0f;      // 타겟까지 투영 거리 (Projectile 만)
        float    speed     = 18.0f;
        float    radius    = 0.5f;
        float    scale     = 1.0f;
        float    maxDist   = 60.0f;
        ElementType element = ElementType::Fire;
        // CameraShake 전용
        float    shakeIntensity = 0.0f;
        float    shakeDuration  = 0.0f;
    };
    std::vector<PendingMonsterVFX> m_vPendingMonsterVFX;

public:
    // 매 프레임 타겟을 향해 몬스터 transform 보간 (Dx12App 메인 루프에서 호출)
    void InterpolateServerMonsters(float deltaTime);

    // 서버 보스 인디케이터(Circle/ForwardBox) fill 진행도 갱신 (Dx12App 메인 루프에서 호출)
    //   ProcessMonsterAttack 에서 windup 시작, 매 프레임 0→1 차오르고 종료 시 hide.
    void UpdateServerMonsterIndicators(float deltaTime);

    // 지연 VFX 큐 tick — windup 후/동안 스폰. 매 프레임 호출.
    void UpdatePendingMonsterVFX(Scene* pScene, float deltaTime);

private:
    std::unordered_map<uint64, float> m_mapServerMonsterMoveTime;  // idle 전환용

    // 원격 플레이어 마지막 이동 시간 (idle 전환용)
    std::unordered_map<uint64, float> m_mapRemotePlayerMoveTime;

    // 사망한 원격 플레이어 ID — 데스 애니 유지용 (move/idle 전환 skip)
    std::unordered_set<uint64> m_setDeadRemotePlayers;

    // 원격 플레이어 hit flash 타이머 — 피격 시 glow 가 남지 않도록 페이드아웃
    std::unordered_map<uint64, float> m_mapRemotePlayerHitFlashTimer;
    static constexpr float REMOTE_HIT_FLASH_DURATION = 0.15f;

    // idle 전환까지 대기 시간 (초)
    static constexpr float IDLE_TRANSITION_TIME = 0.15f;

    // 원격 플레이어 VFX 상태 (채널링 스킬 방향 추적용)
    struct RemoteVFXState
    {
        int vfxId = -1;
        int skillType = 0;
        float lastUpdateTime = 0.0f;
    };
    std::unordered_map<uint64, RemoteVFXState> m_mapRemotePlayerVFX;

    // VFX 타임아웃 (초) - 이 시간 동안 스킬 패킷이 없으면 VFX 종료
    static constexpr float VFX_TIMEOUT = 0.2f;

public:
    // 원격 플레이어 idle 전환 체크 (Update에서 호출)
    void CheckRemotePlayerIdle(float deltaTime);

    // 원격 플레이어 VFX 타임아웃 체크 (Update에서 호출)
    void CheckRemotePlayerVFXTimeout(Scene* pScene, float deltaTime);

    // 서버 몬스터 idle 전환 체크 (Update에서 호출)
    void CheckServerMonsterIdle(float deltaTime);
};
