#pragma once
#include "IAttackBehavior.h"
#include "SkillTypes.h"   // ElementType
#include <DirectXMath.h>
#include <vector>

// ─── DarkLordSwordRain ───────────────────────────────────────────────────────
//
// 다크로드 P3 (Fire) 시그니처 — 광장 N 곳에 검 낙하 AoE.
// 구조: SigilField 의 다중 인장 변형 — 인장 표시 → fDelay → 동시 폭발.
// SigilField 와 분리 운영하는 이유:
//   * 갯수와 spread 가 훨씬 커서 (default 7, spread 40~) "비" 톤을 강조.
//   * 각 hit 가 보스가 아니라 *플레이어 주변*에 분산되도록 분포 정책이 다름.
class DarkLordSwordRain : public IAttackBehavior
{
public:
    DarkLordSwordRain(ElementType element,
                      int   nSwordCount    = 7,
                      float fDamage        = 55.0f,
                      float fAoeRadius     = 6.0f,
                      float fSpawnMinRadius = 8.0f,
                      float fSpawnMaxRadius = 32.0f,
                      float fWindup        = 1.6f,
                      float fRecovery      = 1.5f);
    virtual ~DarkLordSwordRain() = default;

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override { return m_bFinished; }
    virtual void Reset() override;

    virtual const char* GetAnimClipName() const override { return "attack9"; }
    virtual float GetTimeToHit() const override { return m_fWindup; }
    virtual bool  ShouldShowHitZone() const override { return false; }
    virtual bool  ShouldLoopAnim() const override { return false; }
    virtual int   GetIndicatorTypeOverride() const override { return 0; /* None */ }

private:
    struct Drop
    {
        DirectX::XMFLOAT3 center = { 0,0,0 };
        class GameObject* pRing = nullptr;
        class GameObject* pFill = nullptr;
        bool bHit = false;
    };

    void SpawnIndicators();
    void DetonateAll();
    void CleanupAll();

    ElementType m_eElement;
    int   m_nSwordCount;
    float m_fDamage;
    float m_fAoeRadius;
    float m_fSpawnMinRadius;
    float m_fSpawnMaxRadius;
    float m_fWindup;
    float m_fRecovery;

    enum class Phase { Windup, Recovery };
    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;
    bool  m_bFinished = false;

    std::vector<Drop> m_vDrops;
    class Scene* m_pScene = nullptr;
    class CRoom* m_pRoom  = nullptr;
};
