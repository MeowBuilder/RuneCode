#pragma once
#include "Component.h"
#include "SkillTypes.h"  // ElementType

class InputSystem; // Forward declaration for InputSystem
class CCamera;     // Forward declaration for CCamera
class SkillComponent; // Forward declaration for SkillComponent

enum class PlayerAnimState { Idle, Walk, Attack, Dash, IntroFall };

class PlayerComponent : public Component
{
public:
    PlayerComponent(GameObject* pOwner);

    void PlayerUpdate(float deltaTime, InputSystem* pInputSystem, CCamera* pCamera);

    // 캐릭터 원소 설정 (씬 초기화 직후 호출)
    void SetElementType(ElementType e);
    ElementType GetElementType() const { return m_elementType; }

    // Dash state query (for VFX / i-frame 판정)
    bool IsDashing() const { return m_fDashTimer > 0.0f; }
    float GetDashCooldownRatio() const { return (m_fDashCooldown > 0.0f) ? (m_fDashCooldownRemain / m_fDashCooldown) : 0.0f; }

    // 스킬이 트리거하는 대쉬 이동 (GaleRushBehavior 등에서 호출)
    void StartSkillDash(const DirectX::XMFLOAT3& dir, float speed, float duration);
    bool IsSkillDashing() const { return m_fSkillDashTimer > 0.f; }

    // HP System
    void TakeDamage(float fDamage);
    void Heal(float fAmount);
    float GetHPRatio() const { return m_fCurrentHP / m_fMaxHP; }
    float GetCurrentHP() const { return m_fCurrentHP; }
    float GetMaxHP() const { return m_fMaxHP; }
    bool IsDead() const { return m_fCurrentHP <= 0.0f || m_bNetworkDead; }

    // 보호막 시스템 (보호막 룬 L07)
    void  AddShield(float amount);
    void  SetShield(float amount); // 네트워크 권위 — clamp [0, m_fMaxHP]
    float GetShield() const { return m_fShield; }
    float GetShieldRatio() const { return m_fMaxHP > 0.f ? m_fShield / m_fMaxHP : 0.f; }

    // 보복 룬 (ABY_RVG): 피격 시 활성화, 다음 스킬 시전 시 소모
    void  TriggerVengeance(float duration = 10.f);
    bool  ConsumeVengeance();
    bool  IsVengeancePrimed() const { return m_bVengeancePrimed; }

    // 피해감소 시스템 (대지의 갑옷 E스킬)
    void  SetDamageReduction(float ratio, float duration);
    float GetDamageReductionRatio() const { return m_fDamageReductionRatio; }
    bool  HasDamageReduction() const { return m_fDamageReductionTimer > 0.f; }

    // 무적 시스템 (대지의 갑옷 E스킬)
    void SetInvincible(float duration);
    void ClearInvincible() { m_fInvincibleTimer = 0.f; }
    bool IsInvincible() const { return m_fInvincibleTimer > 0.f; }


    // 네트워크 권위 HP 세팅 — 서버에서 S_PLAYER_DAMAGE 받아 호출. 로컬 TakeDamage 우회.
    void SetCurrentHP(float fHP);
    // 서버 데미지 알림 — HP 갱신은 SetCurrentHP 로 별도. 이건 피격 연출만 트리거.
    void TriggerHitFlash();
    // 서버 사망 알림 — 데스 애니 + 입력 차단
    void OnServerDeath();

    // 심연 룬 단발 VFX 트리거 (오프라인 ProjectileManager 와 멀티 NetworkManager 양쪽에서 호출)
    //   흡혈 회복 펄스 — 플레이어 머리 위 초록 파티클
    void TriggerLifestealVFX(float healAmount);
    //   시간역행(ABY_TIM) 시계 — 플레이어 머리 위, 추적(새 발동 시 교체)하여 멀티에서 겹침 방지
    void TriggerTimeRewindVFX();
    //   보호막 흡수 펄스 — 플레이어 주변 청백 충격파 (피격 흡수 순간)
    void TriggerShieldBreakVFX();

    // 네트워크 원격 플레이어용 룬 오라 갱신.
    // 원격 플레이어는 PlayerUpdate를 타지 않으므로 VFX만 별도로 갱신한다.
    void UpdateNetworkRuneVFX(float deltaTime);

    // Reset velocity when teleported — 중력이 플레이어를 바닥까지 끌어내리도록 onGround=false
    // (텔레포트 Y가 바닥보다 높으면 gravity가 자연스럽게 스냅; Y=0이면 즉시 ground 판정)
    void ResetGroundY() { m_fVelocityY = 0.0f; m_bOnGround = false; }

    // Intro Fly — 게임 시작 직후 포탈에서 낙하하는 시퀀스. 입력 차단 + Levitating 애니 + 자유낙하.
    //   바닥 도달 시 Landing 클립 짧게 후 자동 Idle 복귀. 타이머 안전망(duration).
    //   groundY: 착지 높이 (포탈 베이스 표면 등). 기본 0.
    //   standCenterXZ + standRadius: 베이스 영역 (XZ 평면). 영역 밖으로 이동하면 자유낙하 트리거.
    void StartIntroFly(float duration, float groundY = 0.0f,
                       const XMFLOAT3& standCenter = XMFLOAT3(0,0,0), float standRadius = 0.0f);

    // 포탈 Intro Fly 낙하/착지 연출 중인지 확인.
    // 보스 컷신 입력 차단 중에도 이 연출 업데이트는 허용해야 한다.
    bool IsIntroFlyPlaying() const
    {
        return m_fIntroFlyTimer > 0.0f || m_fLandingHoldTimer > 0.0f;
    }

    // Flight Mode (4스테이지 바람 보스 비행 슈팅): 보스 중심 구면 좌표 비행
    void EnterFlightMode(GameObject* pFlightCenter);
    void ExitFlightMode();
    bool IsFlightMode() const { return m_bFlightMode; }
    GameObject* GetFlightCenter() const { return m_pFlightCenter; }

    // Tornado Trap (4스테이지 ambient 회오리에 빨려들어감)
    //   진입: 회오리 중심 좌표 받아서 회전+상승 시작 (입력/물리 모두 우회)
    //   해제: ExitTornadoTrap → 다음 프레임부터 중력 작동, 자연 낙하
    void EnterTornadoTrap(const XMFLOAT3& tornadoCenter);
    void ExitTornadoTrap();
    bool IsTornadoTrapped() const { return m_bTornadoTrapped; }

    // Fall zone: safe AABB(center±extents) 안 = 수면에 뜸(차오르는 물 따라 상승),
    // 밖 = 중력 낙하 → y<FALL_DEATH_Y 도달 시 즉사. 크라켄 WaterRise/전투에서만 활성화.
    void EnableFallZone(const XMFLOAT3& safeCenter, const XMFLOAT3& safeExtents);
    void DisableFallZone() { m_bFallZoneActive = false; m_fFallZoneWaterY = -1e9f; }
    void SetFallZoneWaterY(float waterY) { m_fFallZoneWaterY = waterY; }
    bool IsFallZoneActive() const { return m_bFallZoneActive; }

private:
    // ─── 캐릭터 원소 ──────────────────────────────────────────────────────────
    ElementType m_elementType    = ElementType::Water;
    XMFLOAT4    m_dashCoreColor  = { 0.35f, 0.75f, 1.0f, 1.0f };
    XMFLOAT4    m_dashEdgeColor  = { 0.05f, 0.15f, 0.55f, 0.0f };
    float       m_fMoveSpeed     = 20.0f;   // 기본값; SetElementType 으로 덮어씀
    float       m_fDashCooldown  = 1.2f;    // 기본값 (Water); SetElementType 으로 덮어씀
    float       m_fDashDuration  = 0.25f;
    float       m_fDashSpeedMult = 3.2f;

    // ─── HP ───────────────────────────────────────────────────────────────────
    float m_fMaxHP = 100.0f;
    float m_fCurrentHP = 100.0f;
    float m_fShield    = 0.0f;
    // 최대 보호막 = 최대 생명력(m_fMaxHP)

    float m_fDamageReductionRatio = 0.f;
    float m_fDamageReductionTimer = 0.f;
    float m_fInvincibleTimer      = 0.f;

    bool  m_bVengeancePrimed = false;
    float m_fVengeanceTimer  = 0.f;

    // ─── 심연 룬 추적 VFX (보호막 오라 / 보복 오라) ──────────────────────────
    //   짧은 lifetime 으로 매 프레임 위치 추적 + 만료 직전 재스폰 패턴 (m_overheatStackVFX 와 동일).
    int   m_shieldVFXSlot         = -1;
    float m_shieldRefreshTimer    = 0.f;   // 0이 되면 다음 프레임에서 재스폰
    float m_prevShieldAmount      = 0.f;   // 흡수 펄스 트리거용 (TakeDamage 후 감소 감지)
    int   m_vengeanceVFXSlot      = -1;
    float m_vengeanceRefreshTimer = 0.f;

    // 흡혈 룬(ABY_VMP) 송곳니 VFX 추적 (플레이어 따라 이동, 새 발동 시 교체)
    int   m_lifestealVFXSlot      = -1;
    float m_lifestealVFXTimer     = 0.f;

    // 시간역행 룬(ABY_TIM) 시계 VFX 추적 (위와 동일 패턴)
    int   m_timeRewindVFXSlot     = -1;
    float m_timeRewindVFXTimer    = 0.f;

    // 보호막/보복 오라 추적 — PlayerUpdate 에서 매 프레임 호출
    void UpdateAbyssAuraVFX(float deltaTime);

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

    // Intro Fly — 포탈 진입 후 낙하 시퀀스
    float    m_fIntroFlyTimer    = 0.0f;   // > 0 동안 입력 차단 + 강제 비행 모션
    float    m_fLandingHoldTimer = 0.0f;   // 바닥 도달 후 Landing 클립 유지 시간
    float    m_fIntroGroundY     = 0.0f;   // 포탈 베이스 표면 등 착지 높이
    XMFLOAT3 m_xmf3StandCenter   = { 0, 0, 0 };  // 베이스 영역 중심 (XZ)
    float    m_fStandRadius      = 0.0f;   // > 0 동안 활성. 영역 밖 = 자유낙하.
    static constexpr float kAttackAnimDuration = 0.92f;  // Attack1 clip duration

    // Dash (Space key) — 짧은 무적 이동기. LevitateStart 애니 재사용 + 이미시브 플래시
    float m_fDashTimer = 0.0f;           // 대쉬 중 경과 (0이면 대쉬 아님)
    float m_fDashCooldownRemain = 0.0f;  // 쿨다운 잔여
    float m_fDashFlashTail = 0.0f;       // 대쉬 종료 후 HitFlash 잔상 타이머
    XMFLOAT3 m_xmf3DashDir = { 0, 0, 1 };// 대쉬 방향 (시작 시 고정)
    float m_fDashTrailAccum = 0.0f;      // 대쉬 트레일 LightEmitter 재스폰 누적 타이머

    // 스킬 대쉬 (GaleRush 등) — 스페이스 대쉬와 독립적으로 이동만 처리
    float     m_fSkillDashTimer = 0.f;
    float     m_fSkillDashSpeed = 0.f;
    DirectX::XMFLOAT3 m_xmf3SkillDashDir = { 0.f, 0.f, 1.f };

    // 대쉬 파라미터 — SetElementType()으로 원소별 값으로 덮어씀
    // (m_fDashDuration / m_fDashCooldown / m_fDashSpeedMult 가 실제 사용)
    static constexpr float kDashFlashTail = 0.15f;  // 대쉬 후 플래시 잔상 (페이드아웃)

    // 네트워크 회전 동기화용 (이전 프레임 Y 회전값)
    float m_fPrevYaw = 0.0f;
    static constexpr float YAW_SYNC_THRESHOLD = 1.0f;  // 1도 이상 변화 시 동기화

    // Tornado Trap (4스테이지 ambient 회오리)
    bool      m_bTornadoTrapped     = false;
    XMFLOAT3  m_xmf3TornadoCenter   = { 0.0f, 0.0f, 0.0f };
    float     m_fTornadoTime        = 0.0f;       // trap 진입 후 경과
    float     m_fTornadoStartY      = 0.0f;       // 진입 시점 Y
    void      UpdateTornadoTrap(float dt);

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
