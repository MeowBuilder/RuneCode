#pragma once
#include "Component.h"
#include <DirectXMath.h>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include "ThreatSystem.h"
#include "StatusEffect.h"
#include "SkillTypes.h"

using namespace DirectX;

class IAttackBehavior;
class CRoom;
class AnimationComponent;
class BossPhaseController;
class BossPhaseConfig;
class FluidSkillVFXManager;

struct EnemyAnimationConfig
{
    std::string m_strIdleClip = "idle";
    std::string m_strChaseClip = "Run_Forward";
    std::string m_strAttackClip = "Combat_Unarmed_Attack";
    std::string m_strStaggerClip = "Combat_Stun";
    std::string m_strDeathClip = "Death";

    bool m_bLoopIdle = true;
    bool m_bLoopChase = true;
    bool m_bLoopAttack = false;
    bool m_bLoopStagger = false;
    bool m_bLoopDeath = false;
};

enum class IndicatorType
{
    None,
    Circle,       // Melee: circle around enemy
    RushCircle,   // Rush + 360 AoE: line + circle at destination
    RushCone,     // Rush + cone: line + fan at destination
    ForwardBox    // 보스 앞으로 뻗어나가는 직사각형 영역 (촉수 휩쓸기 등)
};

struct AttackIndicatorConfig
{
    IndicatorType m_eType = IndicatorType::None;
    float m_fRushDistance = 0.0f;    // Length of rush path (speed * duration)
    float m_fHitRadius = 0.0f;      // Radius for circle AoE, or half-width for ForwardBox
    float m_fConeAngle = 0.0f;      // Cone angle in degrees (for RushCone)
    float m_fHitLength = 0.0f;      // ForwardBox 전방 길이 (boss → 전방 N 유닛)
};

enum class EnemyState
{
    Idle,
    Chase,
    Attack,
    Stagger,
    Dead
};

// Boss intro cutscene phases
enum class BossIntroPhase
{
    None,           // No intro (regular enemy)
    FlyingIn,       // Flying down from sky
    Landing,        // Landing animation
    Roaring,        // Scream/roar animation
    Done            // Intro complete, start combat
};

struct EnemyStats
{
    float m_fMaxHP = 100.0f;
    float m_fCurrentHP = 100.0f;
    float m_fMoveSpeed = 5.0f;
    float m_fAttackRange = 2.0f;
    float m_fAttackCooldown = 1.0f;
    float m_fDetectionRange = 50.0f;

    // Boss distance-based attack ranges
    float m_fLongRangeThreshold = 30.0f;   // 이 거리 이상: 비행/브레스 공격
    float m_fMidRangeThreshold = 15.0f;    // 이 거리 이상: 특수 공격
    // 이 거리 미만: 근접 공격
};

class EnemyComponent : public Component
{
public:
    EnemyComponent(GameObject* pOwner);
    virtual ~EnemyComponent();

    virtual void Update(float deltaTime) override;

    // State management
    void ChangeState(EnemyState newState);
    EnemyState GetState() const { return m_eCurrentState; }

    // Target management
    void SetTarget(GameObject* pTarget) { m_pTarget = pTarget; }
    GameObject* GetTarget() const { return m_pTarget; }

    // AI 일시 정지 (4스테이지 비행 보스 — Scene 이 직접 transform 제어)
    void SetAIPaused(bool bPaused) { m_bAIPaused = bPaused; }
    bool IsAIPaused() const { return m_bAIPaused; }

    // Threat (Aggro) System
    void RegisterAllPlayers(const std::vector<GameObject*>& players);
    void AddThreat(GameObject* pPlayer, float fAmount);
    void ReduceThreat(GameObject* pPlayer, float fAmount);
    ThreatTable& GetThreatTable() { return m_ThreatTable; }

    // Stats
    void SetStats(const EnemyStats& stats) { m_Stats = stats; }
    EnemyStats& GetStats() { return m_Stats; }
    const EnemyStats& GetStats() const { return m_Stats; }
    float GetHpRatio() const { return (m_Stats.m_fMaxHP > 0.f) ? m_Stats.m_fCurrentHP / m_Stats.m_fMaxHP : 0.f; }

    // Damage handling
    // bTriggerStagger=false: 데미지만 입히고 경직 없음 (다단히트 스킬용)
    // bExecRune=true: 이 타격이 처형자 룬 스킬에서 온 경우 — 킬 시 skull 표시
    void TakeDamage(float fDamage, bool bTriggerStagger = true, bool bExecRune = false);
    bool IsDead() const { return m_eCurrentState == EnemyState::Dead; }

    // 방어 분쇄 디버프: defenseMult (0~1, 예: 0.75 = 방어력 25% 감소), duration 초
    void ApplyDefenseDebuff(float defenseMult, float duration)
    {
        if (defenseMult < m_fDefenseMult) { // 더 강한 디버프만 덮어씀
            m_fDefenseMult      = defenseMult;
            m_fDefenseDebuffTimer = duration;
        }
    }
    float GetDefenseMult() const { return m_fDefenseMult; }

    // ── 상태이상 시스템 ──────────────────────────────────────────────────────
    // 화상: 매 1초 tickDmgBase * stacks 피해. Normal=최대3중첩, Epic(작열)=최대5중첩
    void ApplyBurn(int stacks, float duration, float tickDmgBase, int maxStacks = 3);
    // 빙결: 이동속도 -15% per 중첩. 3중첩 달성 시 완전 빙결 1.5초
    void ApplyChill(int stacks, float duration, int maxStacks = 3);
    // 균열: 방어력 -8% per 중첩. 3중첩 달성 시 경직(Stagger)
    void ApplyFracture(int stacks, float duration, int maxStacks = 3);

    bool  IsBurning()    const { return m_Burn.IsActive(); }
    bool  IsChilled()    const { return m_Chill.IsActive(); }
    bool  IsFractured()  const { return m_Fracture.IsActive(); }
    bool  IsFrozen()     const { return m_bFrozen; }
    int   GetBurnStacks()     const { return m_Burn.stacks; }
    int   GetChillStacks()    const { return m_Chill.stacks; }
    int   GetFractureStacks() const { return m_Fracture.stacks; }
    // 이동속도 배율: 빙결=0, 냉기 중첩당 -15%
    float GetChillSlowMult()  const;

    // 상태이상 적용/틱 시 VFX 콜백 (Scene 등 외부에서 설정)
    using StatusVFXCallback = std::function<void(ElementType, int stacks, const XMFLOAT3& pos)>;
    void SetStatusVFXCallback(StatusVFXCallback cb) { m_onStatusChanged = std::move(cb); }

    void SetVFXManager(FluidSkillVFXManager* mgr) { m_pStatusVFXMgr = mgr; }

    // Attack behavior
    void SetAttackBehavior(std::unique_ptr<IAttackBehavior> pBehavior);
    IAttackBehavior* GetAttackBehavior() const { return m_pAttackBehavior.get(); }

    // Special attack behavior (for bosses)
    void SetSpecialAttackBehavior(std::unique_ptr<IAttackBehavior> pBehavior);
    IAttackBehavior* GetSpecialAttackBehavior() const { return m_pSpecialAttackBehavior.get(); }
    bool IsUsingSpecialAttack() const { return m_bUsingSpecialAttack; }

    // Factories: recreate behaviors each use so random selection actually varies
    void SetAttackFactory(std::function<std::unique_ptr<IAttackBehavior>()> fn) { m_fnAttackFactory = std::move(fn); }
    void SetSpecialAttackFactory(std::function<std::unique_ptr<IAttackBehavior>()> fn) { m_fnSpecialAttackFactory = std::move(fn); }

    // Flying attack behavior (for bosses with flight capability)
    void SetFlyingAttackBehavior(std::unique_ptr<IAttackBehavior> pBehavior);
    IAttackBehavior* GetFlyingAttackBehavior() const { return m_pFlyingAttackBehavior.get(); }
    bool IsUsingFlyingAttack() const { return m_bUsingFlyingAttack; }
    void SetFlyingAttackCooldown(float fCooldown) { m_fFlyingAttackCooldown = fCooldown; }
    void SetFlyingAttackChance(int nChance) { m_nFlyingAttackChance = nChance; }

    // Boss settings
    void SetBoss(bool bIsBoss) { m_bIsBoss = bIsBoss; }
    bool IsBoss() const { return m_bIsBoss; }

    // Boss Phase System
    void SetBossPhaseController(std::unique_ptr<BossPhaseController> pController);
    BossPhaseController* GetPhaseController() const { return m_pPhaseController.get(); }
    void SetSpeedMultiplier(float fMultiplier) { m_fSpeedMultiplier = fMultiplier; }
    float GetSpeedMultiplier() const { return m_fSpeedMultiplier; }
    bool CanUseFlyingAttack() const;  // 현재 페이즈에서 비행 공격 가능 여부

    // Invincibility (used during special attacks)
    void SetInvincible(bool bInvincible) { m_bInvincible = bInvincible; }
    bool IsInvincible() const { return m_bInvincible; }

    // Special attack cooldown
    void SetSpecialAttackCooldown(float fCooldown) { m_fSpecialAttackCooldown = fCooldown; }
    void SetSpecialAttackChance(int nChance) { m_nSpecialAttackChance = nChance; }  // 0-100

    // Room reference for death callback
    void SetRoom(CRoom* pRoom) { m_pRoom = pRoom; }
    CRoom* GetRoom() const { return m_pRoom; }

    // Death callback
    using DeathCallback = std::function<void(EnemyComponent*)>;
    void SetOnDeathCallback(DeathCallback callback) { m_OnDeathCallback = callback; }

    // AI utility
    float GetDistanceToTarget() const;
    void FaceTarget(float dt = 0.0f, bool bInstant = false);  // Smooth rotation towards target (dt=0 means instant)
    void MoveTowardsTarget(float dt);

    // Separation (avoid stacking with other enemies)
    void SetSeparationRadius(float fRadius) { m_fSeparationRadius = fRadius; }
    void SetSeparationStrength(float fStrength) { m_fSeparationStrength = fStrength; }

    // Animation
    void SetAnimationComponent(AnimationComponent* pAnimComp) { m_pAnimationComp = pAnimComp; }
    AnimationComponent* GetAnimationComponent() const { return m_pAnimationComp; }
    void SetAnimationConfig(const EnemyAnimationConfig& config) { m_AnimConfig = config; }

    // Flying mode
    void SetFlying(bool bFlying, float fHeight = 15.0f) { m_bIsFlying = bFlying; m_fFlyHeight = fHeight; }
    bool IsFlying() const { return m_bIsFlying; }

    // Stationary mode — 고정형 보스 (이동 없음, 회전만 느리게, 거리 무관 공격)
    void SetStationary(bool bStationary) { m_bStationary = bStationary; }
    bool IsStationary() const { return m_bStationary; }
    void SetRotationSpeed(float fDegPerSec) { m_fRotationSpeed = fDegPerSec; }

    // 기본 애니 재생속도 (프리셋에서 설정). 공격 behavior 가 일시적으로 override 할 때 복원용
    void  SetBaseAnimPlaybackSpeed(float fSpeed) { m_fBaseAnimPlaybackSpeed = fSpeed; }
    float GetBaseAnimPlaybackSpeed() const       { return m_fBaseAnimPlaybackSpeed; }

    // Boss intro cutscene
    void StartBossIntro(float fStartHeight = 30.0f);
    bool IsInIntro() const { return m_eIntroPhase != BossIntroPhase::None && m_eIntroPhase != BossIntroPhase::Done; }
    BossIntroPhase GetIntroPhase() const { return m_eIntroPhase; }

    // Attack indicators
    void SetIndicatorConfig(const AttackIndicatorConfig& config) { m_IndicatorConfig = config; }
    void SetRushLineIndicator(GameObject* pIndicator) { m_pRushLineIndicator = pIndicator; }
    void SetHitZoneIndicator(GameObject* pIndicator) { m_pHitZoneIndicator = pIndicator; }
    // fill 오브젝트 (Circle 텔레그래프용 — 테두리는 m_pHitZoneIndicator, 내부 차오름은 이쪽)
    void SetHitZoneFillIndicator(GameObject* pIndicator) { m_pHitZoneFillIndicator = pIndicator; }

    // 네트워크 일반 몬스터 연출용 인디케이터 갱신
    void ResetNetworkAttackIndicator()
    {
        m_fIndicatorTimer = 0.0f;
        HideIndicators();
    }

    void UpdateNetworkAttackIndicator(float dt)
    {
        m_fIndicatorTimer += dt;
        ShowIndicators();
    }

    void HideNetworkAttackIndicator()
    {
        HideIndicators();
    }


    // 공격 원점 forward offset (크라켄처럼 몸 앞쪽 촉수에서 공격이 나가는 경우 사용)
    // 보스 위치에서 forward 방향으로 N 유닛 앞을 "공격 중심"으로 취급
    void  SetAttackOriginForwardOffset(float f) { m_fAttackOriginForwardOffset = f; }
    float GetAttackOriginForwardOffset() const  { return m_fAttackOriginForwardOffset; }
    XMFLOAT3 GetAttackOrigin() const;   // 보스 yaw 기준 forward offset 적용된 XZ 위치 (Y는 보스 Y 그대로)
    // 보스 앞쪽 직사각형 영역 안에 타겟이 있는지 (로컬 X: ±widthHalf, 로컬 Z: 0~length)
    bool IsTargetInForwardRect(float fWidthHalf, float fLength) const;

    // 환경 장애물 (4스테이지 데몬 fixated charge — 기둥 충돌 감지용)
    void SetEnvironmentObstacles(std::vector<GameObject*> obstacles) { m_vEnvironmentObstacles = std::move(obstacles); }
    const std::vector<GameObject*>& GetEnvironmentObstacles() const  { return m_vEnvironmentObstacles; }

    // FixatedCharge 실패(허공 돌진) 카운터 — 다음 charge windup 단축에 사용
    int  GetMissedFixatedCharges() const { return m_nMissedFixatedCharges; }
    void IncrementMissedFixatedCharges() { ++m_nMissedFixatedCharges; }
    void ResetMissedFixatedCharges()     { m_nMissedFixatedCharges = 0; }

private:
    // State update functions
    void UpdateIdle(float dt);
    void UpdateChase(float dt);
    void UpdateAttack(float dt);
    void UpdateStagger(float dt);
    void UpdateDead(float dt);

    void Die();
    void ShowIndicators();
    void HideIndicators();
    void UpdateStatusEffects(float dt);
    void RefreshStatusOutline();

private:
    // Threat System
    ThreatTable m_ThreatTable;
    float m_fTargetReevaluationTimer = 0.0f;
    void ReevaluateTarget();

    EnemyState m_eCurrentState = EnemyState::Idle;
    EnemyStats m_Stats;
    std::unique_ptr<IAttackBehavior> m_pAttackBehavior;
    std::unique_ptr<IAttackBehavior> m_pSpecialAttackBehavior;  // Special pattern for bosses
    std::unique_ptr<IAttackBehavior> m_pFlyingAttackBehavior;   // Flying pattern for bosses
    std::function<std::unique_ptr<IAttackBehavior>()> m_fnAttackFactory;
    std::function<std::unique_ptr<IAttackBehavior>()> m_fnSpecialAttackFactory;
    GameObject* m_pTarget = nullptr;
    CRoom* m_pRoom = nullptr;
    bool m_bAIPaused = false;  // 4스테이지 비행 보스: Scene 이 transform 직접 제어

    // Boss flags
    bool m_bIsBoss = false;
    bool m_bInvincible = false;
    bool m_bStationary = false;   // 고정형 보스 — Chase 단계에서 이동 안 함
    bool m_bUsingSpecialAttack = false;
    bool m_bUsingFlyingAttack = false;

    // Boss Phase System
    std::unique_ptr<BossPhaseController> m_pPhaseController;
    float m_fSpeedMultiplier = 1.0f;

    // Special attack parameters
    float m_fSpecialAttackCooldown = 10.0f;
    float m_fSpecialCooldownTimer = 0.0f;
    int m_nSpecialAttackChance = 30;  // 30% chance when cooldown ready

    // Flying attack parameters
    float m_fFlyingAttackCooldown = 15.0f;
    float m_fFlyingCooldownTimer = 0.0f;
    int m_nFlyingAttackChance = 30;

    // Animation
    AnimationComponent* m_pAnimationComp = nullptr;
    EnemyAnimationConfig m_AnimConfig;

    // Attack indicators
    AttackIndicatorConfig m_IndicatorConfig;
    GameObject* m_pRushLineIndicator     = nullptr;
    GameObject* m_pHitZoneIndicator      = nullptr;  // 테두리 (공격 내내 고정 크기)
    GameObject* m_pHitZoneFillIndicator  = nullptr;  // 내부 fill (windup 동안 0 → 1 차오름)
    float       m_fIndicatorTimer    = 0.0f;  // Attack 진입 후 경과 시간 (그로우인/펄스/fill 진행도)
    float       m_fAttackOriginForwardOffset = 0.0f;  // 보스 앞쪽 공격 원점 오프셋 (크라켄=촉수 앞 8u)

    // Timers
    float m_fAttackCooldownTimer = 0.0f;
    float m_fStaggerTimer = 0.0f;
    float m_fDeadTimer = 0.0f;
    float m_fHitFlashTimer = 0.0f;

    // 방어 분쇄 디버프
    float m_fDefenseMult       = 1.0f;  // 1.0 = 정상, 0.75 = 방어력 25% 감소
    float m_fDefenseDebuffTimer = 0.0f;

    // ── 상태이상 ─────────────────────────────────────────────────────────────
    ElementStatusState m_Burn;
    ElementStatusState m_Chill;
    ElementStatusState m_Fracture;
    bool  m_bFrozen      = false;
    float m_fFrozenTimer = 0.f;
    StatusVFXCallback      m_onStatusChanged;

    // 상태이상 파티클 VFX
    FluidSkillVFXManager* m_pStatusVFXMgr = nullptr;
    int m_vfxBurnId     = -1;
    int m_vfxChillId    = -1;
    int m_vfxFreezeId   = -1;
    int m_vfxFractureId = -1;

    void SpawnStatusVFX(const char* effectId, int& outId);
    void StopStatusVFX(int& id);
    void TrackStatusVFX();

    // 타입 식별 마커 VFX (헤드/발밑 상시 표시) — 더 이상 사용 안 함 (메쉬 마커로 대체)
    int         m_vfxHeadMarkerId = -1;
    int         m_vfxFootMarkerId = -1;
    std::string m_strHeadMarkerEffect;
    std::string m_strFootMarkerEffect;
    static constexpr float HEAD_MARKER_Y_OFFSET = 4.5f;
    static constexpr float FOOT_MARKER_Y_OFFSET = 0.1f;

    // 메쉬 기반 타입 마커 (다층 마법진)
    //   - Head: 외곽 링 + 내부 디스크 (작은 광원)
    //   - Foot: 외곽 큰 링 + 내부 작은 링 (이중 동심원)
    //   회전 + 펄스로 살아있는 마법진 느낌
    class GameObject* m_pHeadMarker      = nullptr;
    class GameObject* m_pHeadMarkerInner = nullptr;
    class GameObject* m_pFootMarker      = nullptr;
    class GameObject* m_pFootMarkerInner = nullptr;
    float m_fMarkerRotation = 0.f;
    float m_fMarkerPulse    = 0.f;
    float m_fFootMarkerScale      = 2.0f;   // 외곽 발 마커 기준 스케일
    float m_fFootMarkerInnerScale = 1.0f;
    float m_fHeadMarkerScale      = 1.4f;
    float m_fHeadMarkerInnerScale = 0.6f;

public:
    void SetHeadMarkerEffect(const std::string& id) { m_strHeadMarkerEffect = id; }
    void SetFootMarkerEffect(const std::string& id) { m_strFootMarkerEffect = id; }
    void SpawnTypeMarkers();           // (legacy) 파티클 마커 — 호출 안 함
    void StopTypeMarkers();            // (legacy)

    void SetHeadMarker(class GameObject* pMarker)      { m_pHeadMarker      = pMarker; }
    void SetHeadMarkerInner(class GameObject* pMarker) { m_pHeadMarkerInner = pMarker; }
    void SetFootMarker(class GameObject* pMarker)      { m_pFootMarker      = pMarker; }
    void SetFootMarkerInner(class GameObject* pMarker) { m_pFootMarkerInner = pMarker; }
    void SetMarkerScales(float footOuter, float footInner, float headOuter, float headInner)
    {
        m_fFootMarkerScale      = footOuter;
        m_fFootMarkerInnerScale = footInner;
        m_fHeadMarkerScale      = headOuter;
        m_fHeadMarkerInnerScale = headInner;
    }
    GameObject* GetHeadMarker() const { return m_pHeadMarker; }
    GameObject* GetFootMarker() const { return m_pFootMarker; }
private:
    void SpawnMarkerVFX(const char* effectId, int& outId, float yOffset);
    void TrackMarkerVFX();
    void TrackTypeMarkers(float dt);   // 메쉬 마커 위치/회전 갱신
    void HideTypeMarkers();            // 사망 시 Y=-1000 으로 숨김

    static constexpr float BURN_TICK_INTERVAL    = 1.0f;
    static constexpr float CHILL_SLOW_PER_STACK  = 0.15f;  // 중첩당 이동속도 -15%
    static constexpr float FRACTURE_DEF_PER_STACK = 0.08f; // 중첩당 방어력 -8%
    static constexpr float FREEZE_DURATION       = 3.5f;

    // Constants
    static constexpr float STAGGER_DURATION = 0.5f;
    static constexpr float FLASH_DURATION   = 0.15f;
    static constexpr float DEAD_LINGER_TIME = 2.0f;

    // Separation (avoid stacking) — 몬스터끼리 좀 더 퍼지는 느낌. 서버 Monster.cpp SEP_* 와 동일 값 유지 필수.
    float m_fSeparationRadius = 12.0f;   // 밀어내는 범위 (이전 8.0)
    float m_fSeparationStrength = 18.0f; // 밀어내는 힘 (이전 14.0)

    // Smooth rotation
    float m_fRotationSpeed = 180.0f;     // Degrees per second

    // 애니 재생속도 (프리셋 기본값). 공격 override 후 복원에 사용
    float m_fBaseAnimPlaybackSpeed = 1.0f;

    // Callbacks
    DeathCallback m_OnDeathCallback;

    // Gravity system
    float m_fVelocityY = 0.0f;
    bool m_bOnGround = false;
    static constexpr float GRAVITY = 50.0f;
    static constexpr float GROUND_Y = 0.0f;  // Tile surface height

    // Flying mode
    bool m_bIsFlying = false;
    float m_fFlyHeight = 15.0f;

    // 4스테이지 데몬 fixated charge 시스템
    std::vector<GameObject*> m_vEnvironmentObstacles;   // 기둥 등 장애물 — 돌진 충돌 감지
    int m_nMissedFixatedCharges = 0;                    // 허공 돌진 횟수 — escalation

    // Boss intro cutscene
    BossIntroPhase m_eIntroPhase = BossIntroPhase::None;
    float m_fIntroTimer = 0.0f;
    float m_fIntroStartHeight = 30.0f;
    float m_fIntroTargetHeight = 0.0f;
    void UpdateBossIntro(float dt);
};
