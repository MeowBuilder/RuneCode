#pragma once
#include "Component.h"

class InputSystem; // Forward declaration for InputSystem
class CCamera;     // Forward declaration for CCamera
class SkillComponent; // Forward declaration for SkillComponent

enum class PlayerAnimState { Idle, Walk, Attack, Dash };

class PlayerComponent : public Component
{
public:
    PlayerComponent(GameObject* pOwner);

    void PlayerUpdate(float deltaTime, InputSystem* pInputSystem, CCamera* pCamera);

    // Dash state query (for VFX / i-frame 판정)
    bool IsDashing() const { return m_fDashTimer > 0.0f; }
    float GetDashCooldownRatio() const { return (kDashCooldown > 0.0f) ? (m_fDashCooldownRemain / kDashCooldown) : 0.0f; }

    // HP System
    void TakeDamage(float fDamage);
    void Heal(float fAmount);
    float GetHPRatio() const { return m_fCurrentHP / m_fMaxHP; }
    float GetCurrentHP() const { return m_fCurrentHP; }
    float GetMaxHP() const { return m_fMaxHP; }
    bool IsDead() const { return m_fCurrentHP <= 0.0f || m_bNetworkDead; }

    // 보호막 시스템 (보호막 룬 L07)
    void  AddShield(float amount);
    float GetShield() const { return m_fShield; }
    float GetShieldRatio() const { return m_fShield / MAX_SHIELD; }

    // 네트워크 권위 HP 세팅 — 서버에서 S_PLAYER_DAMAGE 받아 호출. 로컬 TakeDamage 우회.
    void SetCurrentHP(float fHP);
    // 서버 데미지 알림 — HP 갱신은 SetCurrentHP 로 별도. 이건 피격 연출만 트리거.
    void TriggerHitFlash();
    // 서버 사망 알림 — 데스 애니 + 입력 차단
    void OnServerDeath();

    // Reset velocity when teleported — 중력이 플레이어를 바닥까지 끌어내리도록 onGround=false
    // (텔레포트 Y가 바닥보다 높으면 gravity가 자연스럽게 스냅; Y=0이면 즉시 ground 판정)
    void ResetGroundY() { m_fVelocityY = 0.0f; m_bOnGround = false; }

    // Flight Mode (4스테이지 바람 보스 비행 슈팅): 보스 중심 구면 좌표 비행
    void EnterFlightMode(GameObject* pFlightCenter);
    void ExitFlightMode();
    bool IsFlightMode() const { return m_bFlightMode; }
    GameObject* GetFlightCenter() const { return m_pFlightCenter; }

    // Fall zone: safe AABB(center±extents) 안 = 수면에 뜸(차오르는 물 따라 상승),
    // 밖 = 중력 낙하 → y<FALL_DEATH_Y 도달 시 즉사. 크라켄 WaterRise/전투에서만 활성화.
    void EnableFallZone(const XMFLOAT3& safeCenter, const XMFLOAT3& safeExtents);
    void DisableFallZone() { m_bFallZoneActive = false; m_fFallZoneWaterY = -1e9f; }
    void SetFallZoneWaterY(float waterY) { m_fFallZoneWaterY = waterY; }
    bool IsFallZoneActive() const { return m_bFallZoneActive; }

private:
    float m_fMaxHP = 100.0f;
    float m_fCurrentHP = 100.0f;
    float m_fShield    = 0.0f;
    static constexpr float MAX_SHIELD = 500.f;

    // Gravity system
    float m_fVelocityY = 0.0f;
    bool m_bOnGround = false;
    static constexpr float GRAVITY = 50.0f;
    static constexpr float GROUND_Y = 0.0f;  // Tile surface height

    // Fall zone (water-boss)
    bool m_bFallZoneActive = false;
    XMFLOAT3 m_xmf3SafeCenter  = {};
    XMFLOAT3 m_xmf3SafeExtents = {};
    float m_fFallZoneWaterY = -1e9f;  // 현재 수면 Y (Scene이 매 프레임 갱신)
    static constexpr float FALL_DEATH_Y = -10.0f;  // 맵 바닥(-4) 훨씬 아래로 떨어지면 즉사

    // Hit flash (서버 피격 이벤트 시 시각 피드백) — EnemyComponent 패턴 참고
    float m_fHitFlashTimer = 0.0f;
    static constexpr float kHitFlashDuration = 0.15f;

    // 서버가 전달한 사망 상태 (로컬 HP 가 아직 남아있어도 서버 권위 따름)
    bool m_bNetworkDead = false;

    // Animation state machine
    PlayerAnimState m_eAnimState = PlayerAnimState::Idle;
    float m_fAttackTimer = 0.0f;
    static constexpr float kAttackAnimDuration = 0.92f;  // Attack1 clip duration

    // Dash (Space key) — 짧은 무적 이동기. LevitateStart 애니 재사용 + 이미시브 플래시
    float m_fDashTimer = 0.0f;           // 대쉬 중 경과 (0이면 대쉬 아님)
    float m_fDashCooldownRemain = 0.0f;  // 쿨다운 잔여
    float m_fDashFlashTail = 0.0f;       // 대쉬 종료 후 HitFlash 잔상 타이머
    XMFLOAT3 m_xmf3DashDir = { 0, 0, 1 };// 대쉬 방향 (시작 시 고정)
    int   m_nDashEmitterId = -1;         // 플레이어 주변 블루 파티클 이미터 (최초 대쉬에서 지연 생성)

    static constexpr float kDashDuration      = 0.25f;  // 대쉬 지속
    static constexpr float kDashCooldown      = 1.2f;   // 쿨다운
    static constexpr float kDashSpeedMult     = 3.2f;   // 평상시 속도 대비 배율
    static constexpr float kDashFlashTail     = 0.15f;  // 대쉬 후 플래시 잔상 (페이드아웃)

    // 네트워크 회전 동기화용 (이전 프레임 Y 회전값)
    float m_fPrevYaw = 0.0f;
    static constexpr float YAW_SYNC_THRESHOLD = 1.0f;  // 1도 이상 변화 시 동기화

    // Flight mode (레일 슈팅: 보스 forward 기준 2D 평면 락온)
    bool m_bFlightMode = false;
    GameObject* m_pFlightCenter = nullptr;     // 추적할 보스
    float m_fFlightOffsetX = 0.0f;             // 보스 로컬 right 방향 오프셋
    float m_fFlightOffsetY = 0.0f;             // 월드 up 방향 오프셋
    float m_fFlightOffsetXVel = 0.0f;
    float m_fFlightOffsetYVel = 0.0f;
    static constexpr float kFlightTrailDist  = 30.0f;  // 보스 뒤 거리
    static constexpr float kFlightOffsetXMax = 18.0f;  // 좌우 무빙 한계
    static constexpr float kFlightOffsetYMax = 11.0f;  // 상하 무빙 한계
    static constexpr float kFlightOffsetYMin = -8.0f;  // 아래쪽 한계 (보스 시야 유지)
    static constexpr float kFlightAccel      = 110.0f; // m/s^2
    static constexpr float kFlightMaxSpeed   = 28.0f;  // m/s
    static constexpr float kFlightDrag       = 5.0f;   // 1/s — 키 떼면 빠르게 정지(무빙 정확도)
    static constexpr float kFlightBoostMult  = 1.6f;
    void UpdateFlightMode(float deltaTime, InputSystem* pInputSystem, CCamera* pCamera);

    void UpdateAnimation(float deltaTime, bool bMoving, bool bAttackTriggered, bool bDashStarted, bool bDashing);
};
