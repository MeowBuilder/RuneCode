#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <vector>

using namespace DirectX;

class GameObject;
class CRoom;
class FluidSkillVFXManager;

// 메가 브레스 공격: 보스가 벽으로 이동 후 맵 전체를 덮는 브레스 공격
// 플레이어는 엄폐물 뒤에 숨어서 회피해야 함
class MegaBreathAttackBehavior : public IAttackBehavior
{
public:
    MegaBreathAttackBehavior(
        float fDamagePerTick = 15.0f,
        float fTickInterval = 0.2f,
        float fMoveSpeed = 20.0f,
        float fMoveToWallTime = 2.0f,
        float fWindupTime = 2.0f,
        float fBreathDuration = 4.0f,
        float fRecoveryTime = 1.0f,
        float fCoverObjectSize = 3.0f
    );
    virtual ~MegaBreathAttackBehavior() = default;

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;

    // 메가브레스 SPH 화염 홍수 VFX 스폰 — 오프라인(SpawnFireWave)·네트워크 컷씬 공용.
    //   SPH_Gravity 제트 홍수 + 원기둥 충돌(포텐셜 흐름)을 한 곳에서 생성해 양 경로가 동일 비주얼.
    //   mouthOrigin: 비주얼 입 위치, dir: 수평 진행 단위벡터, floorY: 지면,
    //   roomCenter/roomExtents: 방 AABB, obstacles(xy=중심XZ, z=반경)/count: 엄폐 기둥,
    //   breathDuration: 분사 지속, jetSpeedOut: 데미지 front 계산용(오프라인만, null 허용).
    //   반환: VFX 슬롯 id (-1 실패).
    static int SpawnMegaBreathFloodVFX(
        FluidSkillVFXManager* pMgr,
        const XMFLOAT3& mouthOrigin, const XMFLOAT3& dir, float floorY,
        const XMFLOAT3& roomCenter, const XMFLOAT3& roomExtents,
        const XMFLOAT4* obstacles, int obstacleCount,
        float breathDuration, float* jetSpeedOut = nullptr);

    // 메가브레스 차지(수렴) VFX 스폰 — 오프라인(SpawnChargeVFX)·네트워크 Windup 공용.
    //   맵 전역의 작은 불씨가 입(mouthOrigin)으로 빨려드는 "곧 터진다" 예고 (Fire Wave 역재생).
    //   반환: VFX 슬롯 id (-1 실패).
    static int SpawnMegaBreathChargeVFX(
        FluidSkillVFXManager* pMgr,
        const XMFLOAT3& mouthOrigin, const XMFLOAT3& dir, float floorY,
        const XMFLOAT3& roomCenter, const XMFLOAT3& roomExtents,
        float windupTime);

private:
    enum class Phase
    {
        TakeOff,      // 이륙
        MoveToWall,   // 벽 끝으로 비행
        Landing,      // 착륙
        SpawnCover,   // 엄폐물 생성
        Windup,       // 브레스 준비
        Breath,       // 브레스 발사
        Recovery,     // 회복
        ReturnTakeOff,  // 복귀 이륙
        ReturnFly,      // 원 위치로 비행
        ReturnLand      // 원 위치 착륙
    };

    // 벽 이동 관련
    XMFLOAT3 CalculateWallPosition();
    void UpdateMoveToWall(float dt, EnemyComponent* pEnemy);

    // 엄폐물 관리
    void SpawnCoverObjects(EnemyComponent* pEnemy);
    void DestroyCoverObjects();

    // 엄폐 판정
    bool IsPlayerBehindCover(const XMFLOAT3& breathOrigin, const XMFLOAT3& playerPos);
    bool RayIntersectsAABB(const XMFLOAT3& rayOrigin, const XMFLOAT3& rayDir,
                          const BoundingBox& box, float maxDist);

    // 데미지 처리
    void ApplyBreathDamage(EnemyComponent* pEnemy);

private:
    // Room 참조
    CRoom* m_pRoom = nullptr;

    // 파라미터
    float m_fDamagePerTick = 15.0f;
    float m_fTickInterval = 0.2f;
    float m_fMoveSpeed = 20.0f;
    float m_fMoveToWallTime = 2.0f;
    float m_fWindupTime = 2.0f;
    float m_fBreathDuration = 4.0f;
    float m_fRecoveryTime = 1.0f;
    float m_fCoverObjectSize = 3.0f;

    // 비행 관련 (컷씬 페이싱 감안 — 너무 빠르게 끝나지 않도록)
    float m_fTakeOffTime = 0.9f;
    float m_fLandingTime = 0.7f;
    float m_fFlyHeight = 15.0f;

    // 런타임 상태
    Phase m_ePhase = Phase::TakeOff;
    float m_fTimer = 0.0f;
    float m_fDamageTickTimer = 0.0f;
    bool m_bFinished = false;

    // 벽 이동
    XMFLOAT3 m_xmf3WallPosition = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 m_xmf3StartPosition = { 0.0f, 0.0f, 0.0f };    // 현재 단계의 시작점 (단계 진입마다 덮어씀)
    XMFLOAT3 m_xmf3OriginalPosition = { 0.0f, 0.0f, 0.0f }; // 공격 시작 전 원래 위치 (복귀 목표)
    int m_nWallDirection = 0;  // 0: +X, 1: -X, 2: +Z, 3: -Z

    // 엄폐물 및 장애물
    std::vector<GameObject*> m_vCoverObjects;
    std::vector<BoundingBox> m_vObstacles;

    // 애니메이션 플래그
    bool m_bBreathAnimStarted = false;

    // 엄폐물 생성 플래그 + 엄폐물 노출 시간 (카메라가 기둥 세팅 보여주는 동안 유지)
    //   카메라가 Landing(dragon-follow) → SpawnCover(establishing) → Windup(over-shoulder)로
    //   크게 전환하는 구간이라 체류 시간을 여유롭게 잡아 블렌드 페이싱 확보
    bool  m_bCoverSpawned    = false;
    float m_fCoverRevealTime = 2.0f;

    // ── 시네마틱 카메라 ────────────────────────────────────────────────────
    //   페이즈마다 목표 orbit/lookAt을 계산하고, 현재 값에서 지수 블렌드로 접근 → 부드러운 전환
    void UpdateCinematicCamera(float dt, EnemyComponent* pEnemy);
    bool     m_bCinematicActive = false;
    bool     m_bCamStateInit    = false;   // 첫 프레임에 목표 = 현재로 초기화
    XMFLOAT3 m_xmf3CamLookAt    = { 0.0f, 0.0f, 0.0f };
    float    m_fCamDist         = 30.0f;
    float    m_fCamPitch        = 25.0f;
    float    m_fCamYaw          = 0.0f;

    // ── Fire Wave 이펙트 ────────────────────────────────────────────────────
    // 거대 파도형 화염 벽 — 보스 벽 위치에서 반대편 벽까지 스윕
    void SpawnFireWave(EnemyComponent* pEnemy);
    void UpdateFireWave(float dt, EnemyComponent* pEnemy);
    void DestroyFireWave();

    // (2) Windup→Breath 전환 순간 입에서 수렴된 charge 파티클을 방사형으로 폭발시키는 shockwave
    void TriggerMouthShockwave();

    // (3) Breath 중 파도가 스쳐간 바닥에 heat trail(잔불) 주기 드롭 — WaveSlash DropFireTrail 흉내
    void DropHeatTrail();
    std::vector<int>   m_vTrailVFXIds;       // 살아있는 trail IDs (Reset 시 정리)
    float              m_fTrailDropTimer = 0.f;
    static constexpr float TRAIL_DROP_INTERVAL = 0.2f;   // 드롭 간격 (초)
    static constexpr float TRAIL_LIFETIME      = 3.0f;   // 각 자국 지속 (초)

    // SPH Beam 기반 거대 화염 분사 — 입에서 끝까지 연속 분사 (cone)
    //  여러 빔을 좌우로 팬-아웃 스폰해 cone 영역을 더 넓고 조밀하게 채움
    //  5개 빔: -12°, -6°, 0°, +6°, +12° — 사이사이 간격 최소화
    class FluidSkillVFXManager* m_pFluidVFXManager   = nullptr;
    static constexpr int        NUM_BEAMS            = 5;
    int                         m_nFluidVFXIds[NUM_BEAMS] = { -1, -1, -1, -1, -1 };

    // Windup 단계 집결 VFX — 보스 입 주변에 파티클이 모여들어 "곧 뭔가 온다" 예고
    void SpawnChargeVFX(EnemyComponent* pEnemy);
    void UpdateChargeVFX(EnemyComponent* pEnemy);
    void DestroyChargeVFX();
    int  m_nChargeVFXId = -1;
    XMFLOAT3                    m_xmf3BeamOrigin     = { 0.0f, 0.0f, 0.0f };  // 보스 입(커튼 시작 라인 중심)
    XMFLOAT3                    m_xmf3BeamDirection  = { 0.0f, 0.0f, 1.0f };  // 커튼 진행 방향
    float                       m_fBeamLength        = 0.0f;  // 진행축 사거리 (입 → 맵 반대편)
    float                       m_fBeamEndRadius     = 0.0f;  // 커튼 측면 반폭 (half-width)
    float                       m_fJetSpeed          = 0.0f;  // 커튼 진행 속도 (데미지 front 계산용)
};
