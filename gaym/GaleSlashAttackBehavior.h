#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

class GameObject;
class Scene;
class CRoom;

// 돌풍 슬래시: 보스 중심에서 4방향 (십자/X자) 진공파가 뻗어나감
//   Windup: 각 방향 인디케이터(긴 직사각형) 가 보스에서 바깥으로 차오름
//   Impact: 각 라인 끝에 E_GaleRush_Cone VFX 발사 + 라인 박스 데미지
//   Recovery: 정리
class GaleSlashAttackBehavior : public IAttackBehavior
{
public:
    enum class SlashShape { Cross, XDiag };

    GaleSlashAttackBehavior(
        SlashShape eShape         = SlashShape::Cross,
        float fDamage             = 80.0f,
        float fLineLength         = 30.0f,
        float fLineHalfWidth      = 3.5f,
        float fWindupTime         = 1.4f,
        float fImpactTime         = 0.4f,
        float fRecoveryTime       = 1.2f,
        float fCameraShakeIntensity = 1.5f,
        float fCameraShakeDuration  = 0.35f
    );
    virtual ~GaleSlashAttackBehavior() = default;

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

    virtual const char* GetAnimClipName() const override { return "attack4"; }
    virtual float GetTimeToHit() const override { return m_fWindupTime; }
    virtual bool  ShouldShowHitZone() const override { return false; }
    virtual bool  ShouldLoopAnim() const override { return false; }

private:
    struct SlashLine
    {
        XMFLOAT3    direction = { 0, 0, 1 };
        GameObject* pBorder = nullptr;
        GameObject* pFill   = nullptr;
        int         nConeVFX = -1;
    };

    void SpawnIndicators(EnemyComponent* pEnemy);
    void FireConeVFX(EnemyComponent* pEnemy);
    void UpdateIndicatorsFill(float progress);
    void DealLineDamage(EnemyComponent* pEnemy);
    void CleanupAll();

    SlashShape m_eShape;
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

    XMFLOAT3 m_xmf3BossCenter = { 0, 0, 0 };
    std::vector<SlashLine> m_vLines;

    Scene* m_pScene = nullptr;
    CRoom* m_pRoom  = nullptr;
};
