#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

class GameObject;
class Scene;
class CRoom;

// 순차 십자 폭발 패턴:
//   보스 중심에 회전 각도가 다른 "꽉 찬 십자"(세로+가로 막대) 3개를 예약하고
//   order 0 → 1 → 2 순서로 짧은 간격을 두고 폭발시킨다.
//   fill 시작 시점은 각 십자마다 달라서, 먼저 차오르는 쪽이 먼저 터진다 (순서 시각 큐).
//   emissive 밝기도 order 별로 달라서 (빨강 > 주황 > 노랑), 어떤 십자가 먼저 터질지 한눈에 읽힌다.
class SequentialCrossAttackBehavior : public IAttackBehavior
{
public:
    SequentialCrossAttackBehavior(
        float fDamagePerCross    = 70.0f,
        float fBarHalfLength     = 35.0f,   // 보스 중심 → 막대 끝
        float fBarHalfWidth      = 3.5f,    // 막대 반폭
        float fWindupTime        = 2.5f,    // 첫 폭발 직전까지 (order 0 fill 시간)
        float fExplosionInterval = 0.55f,   // 순차 폭발 간격
        float fExplosionFlash    = 0.35f,   // 폭발 순간 지속 (바위 솟음)
        float fRecoveryTime      = 1.4f,
        float fCameraShakeIntensity = 2.4f,
        float fCameraShakeDuration  = 0.45f
    );
    virtual ~SequentialCrossAttackBehavior() = default;

    // 네트워크 십자 폭발 연출 seed 설정
    void SetNetworkEffectSeed(uint32 seed);

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

    // 팔 휘두르기 재사용
    virtual const char* GetAnimClipName() const override { return "Golem_battle_attack02_ge"; }
    virtual float GetTimeToHit() const override { return m_fWindupTime; }
    virtual bool  ShouldShowHitZone() const override { return false; }
    virtual bool  ShouldLoopAnim() const override { return false; }

private:
    struct Cross
    {
        float yawDeg = 0.0f;          // 십자 회전 (세로 막대 기준)
        int   order  = 0;             // 0/1/2 폭발 순서
        float fillStartTime = 0.0f;   // windup 시작 기준, fill 차오르기 시작 시각
        float explosionTime = 0.0f;   // windup 시작 기준, 실제 폭발 시각
        bool  bExploded = false;

        GameObject* pBorderV = nullptr;
        GameObject* pBorderH = nullptr;
        GameObject* pFillV   = nullptr;
        GameObject* pFillH   = nullptr;
        std::vector<GameObject*> vBurstRocks;
    };

    void SpawnIndicators(EnemyComponent* pEnemy);
    void UpdateCrossFill(Cross& c, float windupElapsed);
    void ExplodeCross(EnemyComponent* pEnemy, Cross& c);
    void UpdateBurstRocks(float dt);
    void DealCrossDamage(EnemyComponent* pEnemy, const Cross& c);
    void CleanupCrossMeshes(Cross& c);
    void CleanupAll();

    void MakeBar(EnemyComponent* pEnemy, Cross& c, float yawDegBar,
                 bool bIsFill, const XMFLOAT4& emissive,
                 GameObject** outObj);

    float m_fDamagePerCross;
    float m_fBarHalfLength;
    float m_fBarHalfWidth;
    float m_fWindupTime;
    float m_fExplosionInterval;
    float m_fExplosionFlash;
    float m_fRecoveryTime;
    float m_fCameraShakeIntensity;
    float m_fCameraShakeDuration;

    enum class Phase { Windup, Explosions, Recovery };
    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;         // 각 phase 내부 타이머
    float m_fWindupElapsed = 0.0f; // windup 시작부터의 총 경과 (Explosions phase 동안도 계속 증가)
    bool  m_bFinished = false;
    bool  m_bAnimReturnedToIdle = false;

    XMFLOAT3 m_xmf3BossCenter = { 0, 0, 0 };
    std::vector<Cross> m_vCrosses;  // size 3

    Scene* m_pScene = nullptr;
    CRoom* m_pRoom  = nullptr;

    bool m_bUseNetworkSeed = false; // 네트워크 동기화용 seed
    uint32 m_uNetworkSeed = 0;      // 서버가 보내준 seed
};
