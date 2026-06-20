#pragma once
#include "IAttackBehavior.h"
#include "SkillTypes.h"   // ElementType
#include <DirectXMath.h>
#include <vector>

// ─── DarkLordSwordSeal ───────────────────────────────────────────────────────
//
// 다크로드 시그니처 기믹 — "검의 봉인 (Sword Seal)".
// 보스가 본체 검 외에도 4 자루의 영검을 공중에 띄우고 자신을 무적화한다.
// 봉인 검들은 보스를 중심으로 orbit (XZ 평면 회전) 하며 플레이어가 닿으면 데미지.
// 봉인 지속 시간 동안 보스는 정지 + 무적 상태 → 플레이어는 회피 + 시간 견디기.
// 봉인 종료 시 보스 무적 해제 + 검 사라짐 + (옵션) 본체 stagger 모먼트.
//
// 검 visual 은 `Assets/Enemies/DeathKnight/SM_DarkKnight2_Sword.bin` mesh 사용
// (보스가 들고 있는 검과 같은 모델). 추가로 원소색 emissive + Heavy crescent VFX
// 를 검 위치에 부착해 "원소 검광이 깃든 봉인" 톤.
class DarkLordSwordSeal : public IAttackBehavior
{
public:
    DarkLordSwordSeal(ElementType element       = ElementType::Fire,
                      float fDamage             = 45.0f,
                      float fDuration           = 7.0f,
                      float fOrbitRadius        = 20.0f,
                      float fOrbitSpeedDegPerSec = 65.0f,
                      float fSwordHitRadius     = 3.0f,
                      float fSwordVisualScale   = 15.0f,
                      int   nSwordCount         = 4);
    virtual ~DarkLordSwordSeal() = default;

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override { return m_bFinished; }
    virtual void SetNetworkVisualOnly(bool bEnable) { m_bNetworkVisualOnly = bEnable; }
    virtual void Reset() override;

    virtual const char* GetAnimClipName() const override { return "attack9"; }
    virtual float GetTimeToHit() const override { return 0.6f; }   // 봉인 spawn 까지의 windup
    virtual bool  ShouldShowHitZone() const override { return false; }
    virtual bool  ShouldLoopAnim() const override { return false; }
    virtual int   GetIndicatorTypeOverride() const override { return 0; /* None */ }

private:
    struct SealSword
    {
        class GameObject* pObj = nullptr;
        int               vfxSlot = -1;
        float             baseAngleDeg = 0.0f;   // 보스 기준 시작 각도 (0/90/180/270 + ε)
    };

    void SpawnSwords(EnemyComponent* pEnemy);
    void UpdateOrbit(float dt, EnemyComponent* pEnemy);
    void DealCollisionDamage(EnemyComponent* pEnemy);
    void Cleanup();

    ElementType m_eElement;
    float m_fDamage;
    float m_fDuration;
    float m_fOrbitRadius;
    float m_fOrbitSpeedDegPerSec;
    float m_fSwordHitRadius;
    float m_fSwordVisualScale;
    int   m_nSwordCount;

    enum class Phase { Windup, Active, End };
    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;        // phase 안 elapsed 시간

    float m_fSpawnDelay = 0.4f;   // windup → spawn 시점 (보스 모션과 동기화)
    float m_fEndDuration = 0.4f;  // 봉인 풀린 후 cleanup recovery

    float m_fAngleDeg     = 0.0f; // 누적 회전 각도
    float m_fDmgCooldown  = 0.0f;
    static constexpr float kDmgInterval = 0.55f;   // 검 충돌 데미지 간격

    bool m_bFinished       = false;
    bool m_bInvincibleSet  = false;   // SetInvincible(true) 호출 여부 추적
    bool m_bNetworkVisualOnly = false;

    std::vector<SealSword> m_vSwords;

    class Scene* m_pScene = nullptr;
    class CRoom* m_pRoom  = nullptr;
};
