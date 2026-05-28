#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

class GameObject;
class Scene;
class CRoom;

// 부유 바위 발사: 보스 주변에 작은 바위들을 공중에 띄워놓고 → 플레이어 방향으로 순차 발사
//   - Summon: 바위들 공중 부유 (궤도 회전)
//   - Charge: 타겟 선정 + 짧은 흔들림 예고
//   - Fire: 순차 발사 (직선 비행)
//   - Recovery: 정리
// 멀티 플레이어 지원 — 각 바위가 랜덤 플레이어를 타겟
class RockBarrageAttackBehavior : public IAttackBehavior
{
public:
    RockBarrageAttackBehavior(
        int   nRockCount          = 5,
        float fDamagePerRock      = 70.0f,
        float fProjectileRadius   = 3.0f,   // 히트박스 반경
        float fProjectileSpeed    = 28.0f,  // 발사 속도
        float fOrbitRadius        = 12.0f,  // 보스 주변 궤도 반경
        float fOrbitHeight        = 13.0f,  // 보스 위 높이
        float fSummonTime         = 2.0f,
        float fChargeTime         = 0.7f,
        float fFireInterval       = 0.3f,   // 바위 간 발사 간격
        float fFlightTimeout      = 3.0f,   // 바위 최대 비행 시간 (아무것도 안 맞으면 fade)
        float fRecoveryTime       = 1.5f,
        float fCameraShakeIntensity = 1.5f,
        float fCameraShakeDuration  = 0.3f,
        // 유도성 — 0 이면 직선, 높을수록 플레이어를 따라감. 0.5~1.5 권장
        float fHomingStrength     = 0.8f
    );
    virtual ~RockBarrageAttackBehavior() = default;

    // 네트워크 타겟/seed 설정
    // 서버가 정한 targetPlayerId 대상에게 모든 클라가 같은 바위를 발사하도록 한다.
    void SetNetworkTarget(GameObject* pTargetPlayer);
    void SetNetworkEffectSeed(uint32 seed);


    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

    // 휘두르기 모션 — "팔을 휘둘러 바위들을 발사하는" 연출에 어울림 (RockFall 과 통일)
    virtual const char* GetAnimClipName() const override { return "Golem_battle_attack02_ge"; }
    virtual bool  ShouldShowHitZone() const override { return false; }
    // 애니 1회 재생 후 idle 전환 — 돌이 계속 날아가는 동안 보스는 대기
    virtual bool  ShouldLoopAnim() const override { return false; }

private:
    enum class RockState { Floating, Flying, Done };

    struct RockProjectile
    {
        GameObject* pRock = nullptr;
        GameObject* pTargetPlayer = nullptr;     // 유도 대상 (발사 시 선정)
        XMFLOAT3    velocity   = { 0, 0, 0 };    // Fly 중 속도
        float       orbitAngle = 0.0f;           // Summon/Charge 궤도 각도 (rad)
        float       fireDelay  = 0.0f;           // 언제 발사될지 (Fire phase 타이머 기준)
        float       flightTimer = 0.0f;          // 발사 후 경과 시간
        RockState   state = RockState::Floating;
        bool        bHitDealt = false;

        // 개별 랜덤 특성
        XMFLOAT3    rotationSpeed = { 0, 0, 0 }; // 회전 속도 (deg/s)
        float       scaleMultiplier = 1.0f;
    };

    void SpawnRocks(EnemyComponent* pEnemy);
    void UpdateFloating(float dt, EnemyComponent* pEnemy);
    void AssignTargetsAndFire(EnemyComponent* pEnemy);
    void UpdateFlying(float dt, EnemyComponent* pEnemy);
    void CleanupAll();

    // Parameters
    int   m_nRockCount;
    float m_fDamagePerRock;
    float m_fProjectileRadius;
    float m_fProjectileSpeed;
    float m_fOrbitRadius;
    float m_fOrbitHeight;
    float m_fSummonTime;
    float m_fChargeTime;
    float m_fFireInterval;
    float m_fFlightTimeout;
    float m_fRecoveryTime;
    float m_fCameraShakeIntensity;
    float m_fCameraShakeDuration;
    float m_fHomingStrength;

    // Runtime
    enum class Phase { Summon, Charge, Fire, Recovery };
    Phase m_ePhase = Phase::Summon;
    float m_fTimer = 0.0f;
    bool  m_bFinished = false;
    bool  m_bAnimReturnedToIdle = false;

    XMFLOAT3 m_xmf3BossCenter = { 0, 0, 0 };  // Summon 시점 보스 위치 (rocks 기준점)

    std::vector<RockProjectile> m_vRocks;

    Scene*  m_pScene = nullptr;
    CRoom*  m_pRoom  = nullptr;

    // 네트워크 동기화용 타겟
    bool m_bUseNetworkTarget = false;
    GameObject* m_pNetworkTargetPlayer = nullptr;

    // 네트워크 동기화용 seed
    bool m_bUseNetworkSeed = false;
    uint32 m_uNetworkSeed = 0;
};
