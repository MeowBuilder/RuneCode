#pragma once
#include "IAttackBehavior.h"
#include "SkillTypes.h"   // ElementType
#include <DirectXMath.h>
#include <vector>

// ─── DarkLordSigilField ──────────────────────────────────────────────────────
//
// 다크로드 P0 (Earth) 시그니처 — 지면 인장 지연 폭발.
// Windup 동안 지면에 원소별 색 인장(Ring + 차오름 Disc)을 그리고, fDelay 후 폭발 →
// 반경 내 플레이어에게 AoE 데미지. RockFall 의 다중 인디케이터 패턴을 단일/소수 인장
// 으로 단순화한 형태.
//
// 위치 정책:
//   - count == 1 : 플레이어 발 밑(Execute 시점 위치).
//   - count >= 2 : 보스 주변 spreadRadius 반경 원형으로 분산 + 플레이어 발치 1개 보장.
//
// VFX :
//   Windup    — 자체 RingMesh + DiscMesh (RockFall 패턴 그대로) + 원소색 emissive.
//   Detonate  — SlashCue::ImpactEffectName(element) Spawn + 카메라 셰이크.
class DarkLordSigilField : public IAttackBehavior
{
public:
    DarkLordSigilField(ElementType element,
                       float fDamage     = 70.0f,
                       float fRadius     = 6.5f,
                       float fDelay      = 1.25f,
                       int   nSigilCount = 1,
                       float fSpreadRadius = 14.0f,
                       float fRecoveryTime = 0.8f);
    virtual ~DarkLordSigilField() = default;

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override { return m_bFinished; }
    virtual void Reset() override;

    virtual const char* GetAnimClipName() const override { return "attack9"; }
    virtual float GetTimeToHit() const override { return m_fDelay; }
    // 자체 인장이 인디케이터 역할 → preset 인디케이터 끔.
    virtual bool  ShouldShowHitZone() const override { return false; }
    virtual bool  ShouldLoopAnim() const override { return false; }
    virtual int   GetIndicatorTypeOverride() const override { return 0; /* None */ }

    // 네트워크 연출 전용 모드:
    // 서버가 데미지를 처리하므로 클라 로컬 TakeDamage를 막는다.
    void SetNetworkVisualOnly(bool bEnable) { m_bNetworkVisualOnly = bEnable; }

    // 서버가 정한 인장 위치를 그대로 사용한다.
    void SetNetworkEffectPositions(const std::vector<DirectX::XMFLOAT3>& positions)
    {
        m_vNetworkEffectPositions = positions;
    }

private:
    struct SigilInstance
    {
        DirectX::XMFLOAT3 center = { 0,0,0 };
        class GameObject* pRing = nullptr;
        class GameObject* pFill = nullptr;
        bool              bDetonated = false;
    };

    void SpawnSigilVisuals(EnemyComponent* pEnemy);
    void DetonateAll(EnemyComponent* pEnemy);
    void CleanupAll();

    ElementType m_eElement;
    float m_fDamage;
    float m_fRadius;
    float m_fDelay;
    int   m_nSigilCount;
    float m_fSpreadRadius;
    float m_fRecoveryTime;

    enum class Phase { Windup, Recovery };
    Phase m_ePhase = Phase::Windup;
    float m_fTimer = 0.0f;
    bool  m_bFinished = false;

    std::vector<SigilInstance> m_vSigils;
    class Scene* m_pScene = nullptr;
    class CRoom* m_pRoom  = nullptr;

    bool m_bNetworkVisualOnly = false;
    std::vector<DirectX::XMFLOAT3> m_vNetworkEffectPositions;
};
