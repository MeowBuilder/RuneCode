#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

class GameObject;
class Mesh;

// 대형 바위 낙하 공격: 보스 주변 N 개 지점에 바위 낙하
//   Windup 동안 바닥 인디케이터로 예고 → Drop 단계에서 바위 하늘→지면 포물선 낙하 → 착지 시 AOE 데미지
//   여러 개 인디케이터 / 바위를 behavior 내에서 자체 관리 (EnemyComponent 단일 인디케이터 시스템과 별개)
class RockFallAttackBehavior : public IAttackBehavior
{
public:
    RockFallAttackBehavior(
        int   nRockCount          = 10,     // 낙하 바위 개수 — 넓은 전장 커버
        float fDamagePerRock      = 80.0f,
        float fRockAoeRadius      = 10.0f,  // 각 바위 당 AOE 반경
        float fSpawnMinRadius     = 18.0f,  // 최소 스폰 반경 (보스 주변)
        float fSpawnMaxRadius     = 65.0f,  // 최대 스폰 반경 (플레이어 거리)
        float fWindupTime         = 2.0f,
        float fDropDuration       = 0.8f,   // 하늘 → 지면 낙하 시간
        float fRecoveryTime       = 2.0f,
        float fCameraShakeIntensity = 2.8f,
        float fCameraShakeDuration  = 0.5f,
        const char* pClipOverride = nullptr  // 보스마다 다른 휘두름 클립 지정 가능
    );
    virtual ~RockFallAttackBehavior() = default;
    
    // 네트워크 낙석 위치 동기화 데이터 설정
    void SetNetworkEffectData(const std::vector<DirectX::XMFLOAT3>& positions, uint32 seed);

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

    // 바위 흩뿌리기 — 팔 휘두르는 모션 (attack02) 이 바위를 사방으로 뿌리는 연출에 어울림.
    // override 있으면 보스 전용 클립 사용.
    virtual const char* GetAnimClipName() const override {
        return m_strClipOverride ? m_strClipOverride : "Golem_battle_attack02_ge";
    }
    virtual float GetTimeToHit() const override { return m_fWindupTime + m_fDropDuration; }
    // 각 바위가 자체 인디케이터 가짐 → 전역 인디케이터는 끔 (None 반환)
    virtual bool  ShouldShowHitZone() const override { return false; }
    // 애니 1회 재생 — 휘두르기 후 자동 idle 전환 (loop 끝에서 움찔 방지)
    virtual bool  ShouldLoopAnim() const override { return false; }

private:
    struct RockInstance
    {
        XMFLOAT3  landingPos;       // 착지 위치 (지상)
        XMFLOAT3  skyStartPos;      // 하늘 스폰 위치
        GameObject* pRock = nullptr;       // 실제 바위 GameObject
        GameObject* pIndicator = nullptr;  // 바닥 예고 인디케이터 (Ring)
        GameObject* pIndicatorFill = nullptr;  // 차오름 Fill (Disc)
        bool      bImpacted = false;

        // 개별 랜덤 특성 (바위마다 다른 모양/움직임)
        XMFLOAT3 initialRotation = { 0, 0, 0 };  // 초기 자세
        XMFLOAT3 rotationSpeed   = { 0, 0, 0 };  // 낙하 중 회전 속도 (deg/s)
        float    scaleMultiplier = 1.0f;         // 스케일 가변 ±%
        float    archHeight      = 8.0f;         // 포물선 아치 높이 가변
    };

    void SpawnIndicators(EnemyComponent* pEnemy);
    void SpawnRocks(EnemyComponent* pEnemy);
    void UpdateRockFall(float dt);
    void DealImpactDamage(EnemyComponent* pEnemy);
    void CleanupAll();

    // Parameters
    int   m_nRockCount;
    float m_fDamagePerRock;
    float m_fRockAoeRadius;
    float m_fSpawnMinRadius;
    float m_fSpawnMaxRadius;
    float m_fWindupTime;
    float m_fDropDuration;
    float m_fRecoveryTime;
    float m_fCameraShakeIntensity;
    float m_fCameraShakeDuration;

    // Runtime
    enum class Phase { Windup, Drop, Recovery };
    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;
    bool  m_bFinished = false;
    bool  m_bAnimReturnedToIdle = false;   // 공격 애니 1회 재생 후 idle 전환 완료

    std::vector<RockInstance> m_vRocks;

    // 참조 (Execute 시 저장)
    class Scene*  m_pScene = nullptr;
    class CRoom*  m_pRoom  = nullptr;

    // 네트워크 동기화용 낙석 데이터
    bool m_bUseNetworkLandingPositions = false;

    // 서버가 보내준 낙석 착지 위치 목록
    std::vector<DirectX::XMFLOAT3> m_vNetworkLandingPositions;

    // 서버가 보내준 동기화 seed
    uint32 m_uNetworkSeed = 0;

    const char* m_strClipOverride = nullptr;
};
