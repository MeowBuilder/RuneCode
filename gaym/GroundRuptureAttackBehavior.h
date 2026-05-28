#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

class GameObject;
class Scene;
class CRoom;

// 바닥 균열 공격: 보스 중심에서 4 또는 8 방향으로 선형 균열이 뻗어남
//   - Windup: 각 방향 인디케이터(긴 직사각형) 가 보스에서 바깥쪽으로 차오름
//   - Impact: 균열 선을 따라 바위가 솟아오르며 데미지
//   - Recovery: 정리
// 원형 회피가 아닌 "균열 사이 안전지대" 로 회피 유도
class GroundRuptureAttackBehavior : public IAttackBehavior
{
public:
    enum class RuptureShape { Cross, XDiag };  // 십자 or X자

    GroundRuptureAttackBehavior(
        RuptureShape eShape       = RuptureShape::Cross,
        float fDamage             = 100.0f,
        float fLineLength         = 70.0f,   // 각 균열 길이 (보스 중심 → 끝)
        float fLineHalfWidth      = 4.0f,    // 균열 반폭 (데미지 박스 반폭)
        float fWindupTime         = 2.2f,
        float fImpactTime         = 0.6f,    // 터지는 지속 시간
        float fRecoveryTime       = 1.8f,
        float fCameraShakeIntensity = 2.8f,
        float fCameraShakeDuration  = 0.5f
    );
    virtual ~GroundRuptureAttackBehavior() = default;
    
    // 네트워크 균열 연출 seed 설정
    void SetNetworkEffectSeed(uint32 seed);

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

    // 팔 휘두르기 (attack02) 재사용 — 땅을 쓸어 가르는 느낌
    virtual const char* GetAnimClipName() const override { return "Golem_battle_attack02_ge"; }
    virtual float GetTimeToHit() const override { return m_fWindupTime; }
    virtual bool  ShouldShowHitZone() const override { return false; }
    // 애니 1회 재생 — 휘두른 후 idle 전환, 균열/바위는 계속 활성
    virtual bool  ShouldLoopAnim() const override { return false; }

private:
    struct RuptureLine
    {
        XMFLOAT3    direction = { 0, 0, 1 };     // 균열 진행 방향 (normalized)
        GameObject* pBorder = nullptr;           // 테두리 indicator (고정 크기)
        GameObject* pFill   = nullptr;           // 차오름 fill (0 → 길이)
        std::vector<GameObject*> vBurstRocks;    // Impact 시 솟는 바위들
    };

    void SpawnIndicators(EnemyComponent* pEnemy);
    void SpawnBurstRocks(EnemyComponent* pEnemy);
    void UpdateIndicatorsFill(float progress);
    void DealLineDamage(EnemyComponent* pEnemy);
    void CleanupAll();

    RuptureShape m_eShape;
    float m_fDamage;
    float m_fLineLength;
    float m_fLineHalfWidth;
    float m_fWindupTime;
    float m_fImpactTime;
    float m_fRecoveryTime;
    float m_fCameraShakeIntensity;
    float m_fCameraShakeDuration;

    enum class Phase { Windup, Impact, Recovery };
    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;
    bool  m_bFinished = false;
    bool  m_bDamageDealt = false;
    bool  m_bAnimReturnedToIdle = false;

    XMFLOAT3 m_xmf3BossCenter = { 0, 0, 0 };   // 균열 원점 (보스 발밑)
    std::vector<RuptureLine> m_vLines;

    Scene* m_pScene = nullptr;
    CRoom* m_pRoom  = nullptr;

    bool m_bUseNetworkSeed = false; // 네트워크 동기화용 seed
    uint32 m_uNetworkSeed = 0;      // 서버가 보내준 seed
};
