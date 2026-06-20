#include "stdafx.h"
#include "MegaBreathAttackBehavior.h"
#include "EnemyComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "PlayerComponent.h"
#include "ColliderComponent.h"
#include "RenderComponent.h"
#include "AnimationComponent.h"
#include "Room.h"
#include "Scene.h"
#include "Shader.h"
#include "Mesh.h"
#include "MathUtils.h"
#include "Dx12App.h"
#include "Camera.h"
#include "MapLoader.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"

MegaBreathAttackBehavior::MegaBreathAttackBehavior(
    float fDamagePerTick,
    float fTickInterval,
    float fMoveSpeed,
    float fMoveToWallTime,
    float fWindupTime,
    float fBreathDuration,
    float fRecoveryTime,
    float fCoverObjectSize)
    : m_fDamagePerTick(fDamagePerTick)
    , m_fTickInterval(fTickInterval)
    , m_fMoveSpeed(fMoveSpeed)
    , m_fMoveToWallTime(fMoveToWallTime)
    , m_fWindupTime(fWindupTime)
    , m_fBreathDuration(fBreathDuration)
    , m_fRecoveryTime(fRecoveryTime)
    , m_fCoverObjectSize(fCoverObjectSize)
{
}

void MegaBreathAttackBehavior::Execute(EnemyComponent* pEnemy)
{
    Reset();

    if (!pEnemy) return;

    // EnemyComponent에서 Room 참조 가져오기
    m_pRoom = pEnemy->GetRoom();
    if (!m_pRoom) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;

    TransformComponent* pTransform = pOwner->GetTransform();
    if (!pTransform) return;

    // 시작 위치 저장 (이 필드는 단계별로 덮어써짐)
    m_xmf3StartPosition = pTransform->GetPosition();
    // 원래 위치 — 공격 종료 후 복귀 목표 (덮어쓰지 않음)
    m_xmf3OriginalPosition = m_xmf3StartPosition;

    // 벽 위치 계산
    m_xmf3WallPosition = CalculateWallPosition();

    // 무적 설정
    pEnemy->SetInvincible(true);

    // 컷씬 중 용암 기믹 일시 중지 (화염 맵 전용 기믹이지만 컷씬 중엔 방해되므로 비활성화)
    m_pRoom->SetLavaGeyserEnabled(false);

    // 이륙 애니메이션 시작
    if (AnimationComponent* pAnim = pEnemy->GetAnimationComponent())
    {
        pAnim->CrossFade("Take Off", 0.15f, false);
    }

    m_ePhase = Phase::TakeOff;

}

void MegaBreathAttackBehavior::Update(float dt, EnemyComponent* pEnemy)
{
    if (m_bFinished) return;
    if (!pEnemy) return;

    m_fTimer += dt;

    // 매 프레임 시네마틱 카메라 업데이트 (페이즈에 맞춰 앵글/타겟 결정 + 부드럽게 블렌드)
    UpdateCinematicCamera(dt, pEnemy);

    switch (m_ePhase)
    {
    case Phase::TakeOff:
        // 이륙 중 - 높이 상승
        {
            GameObject* pOwner = pEnemy->GetOwner();
            if (pOwner)
            {
                TransformComponent* pTransform = pOwner->GetTransform();
                if (pTransform)
                {
                    float t = m_fTimer / m_fTakeOffTime;
                    t = min(t, 1.0f);
                    float smoothT = t * t;  // easeIn

                    XMFLOAT3 pos = m_xmf3StartPosition;
                    pos.y = m_xmf3StartPosition.y + m_fFlyHeight * smoothT;
                    pTransform->SetPosition(pos);
                }
            }
        }

        if (m_fTimer >= m_fTakeOffTime)
        {
            // 비행 애니메이션으로 전환
            if (AnimationComponent* pAnim = pEnemy->GetAnimationComponent())
            {
                pAnim->CrossFade("Fly Glide", 0.2f, true);
            }

            m_ePhase = Phase::MoveToWall;
            m_fTimer = 0.0f;

            // 시작 위치 업데이트 (현재 높이 포함)
            if (GameObject* pOwner = pEnemy->GetOwner())
            {
                if (TransformComponent* pTransform = pOwner->GetTransform())
                {
                    m_xmf3StartPosition = pTransform->GetPosition();
                    m_xmf3WallPosition.y = m_xmf3StartPosition.y;  // 비행 높이 유지
                }
            }

        }
        break;

    case Phase::MoveToWall:
        UpdateMoveToWall(dt, pEnemy);
        if (m_fTimer >= m_fMoveToWallTime)
        {
            // 착륙 애니메이션
            if (AnimationComponent* pAnim = pEnemy->GetAnimationComponent())
            {
                pAnim->CrossFade("Land", 0.15f, false);
            }

            // 착륙 시작 위치 저장
            if (GameObject* pOwner = pEnemy->GetOwner())
            {
                if (TransformComponent* pTransform = pOwner->GetTransform())
                {
                    m_xmf3StartPosition = pTransform->GetPosition();
                }
            }

            m_ePhase = Phase::Landing;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::Landing:
        // 착륙 중 - 높이 하강
        {
            GameObject* pOwner = pEnemy->GetOwner();
            if (pOwner)
            {
                TransformComponent* pTransform = pOwner->GetTransform();
                if (pTransform)
                {
                    float t = m_fTimer / m_fLandingTime;
                    t = min(t, 1.0f);
                    float smoothT = 1.0f - (1.0f - t) * (1.0f - t);  // easeOut

                    XMFLOAT3 pos = m_xmf3StartPosition;
                    pos.y = m_xmf3StartPosition.y - m_fFlyHeight * smoothT;
                    pTransform->SetPosition(pos);
                }
            }
        }

        if (m_fTimer >= m_fLandingTime)
        {
            m_ePhase = Phase::SpawnCover;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::SpawnCover:
        if (!m_bCoverSpawned)
        {
            SpawnCoverObjects(pEnemy);
            m_bCoverSpawned = true;
        }
        // 엄폐 기둥이 눈에 보이도록 짧게 대기 (카메라가 기둥 생성 잡아줌)
        if (m_fTimer >= m_fCoverRevealTime)
        {
            m_ePhase = Phase::Windup;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::Windup:
        // 플레이어를 바라봄 (방 중앙 방향)
        {
            const BoundingBox& roomBox = m_pRoom->GetBoundingBox();
            GameObject* pOwner = pEnemy->GetOwner();
            if (pOwner)
            {
                TransformComponent* pTransform = pOwner->GetTransform();
                if (pTransform)
                {
                    XMFLOAT3 pos = pTransform->GetPosition();
                    XMFLOAT3 center = { roomBox.Center.x, pos.y, roomBox.Center.z };
                    XMFLOAT2 dir = MathUtils::Direction2D(pos, center);
                    float yaw = atan2f(dir.x, dir.y) * (180.0f / XM_PI);
                    pTransform->SetRotation(0.0f, yaw, 0.0f);
                }
            }
        }

        // Windup 진입 직후 집결 VFX 스폰 — 플레이어에게 "곧 터진다" 예고
        if (m_nChargeVFXId < 0) SpawnChargeVFX(pEnemy);

        if (m_fTimer >= m_fWindupTime)
        {
            m_ePhase = Phase::Breath;
            m_fTimer = 0.0f;
            m_fDamageTickTimer = 0.0f;

            // 브레스 애니메이션 시작 (루프)
            AnimationComponent* pAnim = pEnemy->GetAnimationComponent();
            if (pAnim)
            {
                pAnim->CrossFade("Flame Attack", 0.2f, true);  // 루프로 재생
                m_bBreathAnimStarted = true;
            }

            // 카메라 쉐이킹 시작 + Fire Wave 스폰
            if (Scene* pScene = m_pRoom->GetScene())
            {
                if (CCamera* pCamera = pScene->GetCamera())
                {
                    pCamera->StartShake(2.5f, m_fBreathDuration);  // 거대 파도 강렬한 진동
                }
            }
            // 수렴 charge VFX 정지 후 메인 분사 시작 (입으로 모였다가 → 쫙 분사)
            DestroyChargeVFX();
            SpawnFireWave(pEnemy);

        }
        break;

    case Phase::Breath:
        // Fire Wave 진행 업데이트 (위치 전진 + ember 파티클 에미터 추적)
        UpdateFireWave(dt, pEnemy);

        // 데미지 틱: 파도 AABB 안에 있으면 맞음 (엄폐물이 파도 전면↔플레이어 사이 차단하면 세이프)
        m_fDamageTickTimer += dt;
        if (m_fDamageTickTimer >= m_fTickInterval)
        {
            ApplyBreathDamage(pEnemy);
            m_fDamageTickTimer = 0.0f;
        }

        if (m_fTimer >= m_fBreathDuration)
        {
            m_ePhase = Phase::Recovery;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::Recovery:
        // 첫 프레임에만 정리 작업 수행
        if (m_bBreathAnimStarted)
        {
            // 카메라 쉐이킹 중지
            if (Scene* pScene = m_pRoom->GetScene())
            {
                if (CCamera* pCamera = pScene->GetCamera())
                {
                    pCamera->StopShake();
                }
            }

            // Fire Wave 정리 (벽 mesh 숨김 + ember 에미터 정지)
            DestroyFireWave();

            // 중앙 기둥 제거 — 브레스 끝난 직후 바로 사라지게
            DestroyCoverObjects();

            m_bBreathAnimStarted = false;
        }

        if (m_fTimer >= m_fRecoveryTime)
        {
            // 무적 해제 — 복귀 비행 중엔 보스가 노출 (플레이어가 공격 기회)
            pEnemy->SetInvincible(false);

            // ReturnTakeOff 로 진입 — 복귀 비행 시작
            if (AnimationComponent* pAnim = pEnemy->GetAnimationComponent())
            {
                pAnim->CrossFade("Take Off", 0.15f, false);
            }

            // 이륙 시작 위치 저장 (현재 지면 위치)
            if (GameObject* pOwner = pEnemy->GetOwner())
                if (TransformComponent* pTransform = pOwner->GetTransform())
                    m_xmf3StartPosition = pTransform->GetPosition();

            m_ePhase = Phase::ReturnTakeOff;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::ReturnTakeOff:
        {
            // 현재 위치에서 수직으로 비행 높이까지 상승
            GameObject* pOwner = pEnemy->GetOwner();
            if (pOwner)
            {
                TransformComponent* pTransform = pOwner->GetTransform();
                if (pTransform)
                {
                    float t = m_fTimer / m_fTakeOffTime;
                    t = min(t, 1.0f);
                    float smoothT = t * t;

                    XMFLOAT3 pos = m_xmf3StartPosition;
                    pos.y = m_xmf3StartPosition.y + m_fFlyHeight * smoothT;
                    pTransform->SetPosition(pos);
                }
            }
        }
        if (m_fTimer >= m_fTakeOffTime)
        {
            // 비행 애니메이션으로 전환
            if (AnimationComponent* pAnim = pEnemy->GetAnimationComponent())
                pAnim->CrossFade("Fly Glide", 0.2f, true);

            // 비행 시작점 = 현재 공중 위치
            if (GameObject* pOwner = pEnemy->GetOwner())
                if (TransformComponent* pTransform = pOwner->GetTransform())
                    m_xmf3StartPosition = pTransform->GetPosition();

            m_ePhase = Phase::ReturnFly;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::ReturnFly:
        {
            GameObject* pOwner = pEnemy->GetOwner();
            if (pOwner)
            {
                TransformComponent* pTransform = pOwner->GetTransform();
                if (pTransform)
                {
                    float t = m_fTimer / m_fMoveToWallTime;
                    t = min(t, 1.0f);
                    float smoothT = 1.0f - (1.0f - t) * (1.0f - t);  // easeOut

                    // 공중에서 원위치(의 공중 높이)까지 이동
                    XMFLOAT3 targetAir = m_xmf3OriginalPosition;
                    targetAir.y = m_xmf3StartPosition.y;  // 비행 높이 유지

                    XMFLOAT3 pos;
                    pos.x = m_xmf3StartPosition.x + (targetAir.x - m_xmf3StartPosition.x) * smoothT;
                    pos.y = m_xmf3StartPosition.y + (targetAir.y - m_xmf3StartPosition.y) * smoothT;
                    pos.z = m_xmf3StartPosition.z + (targetAir.z - m_xmf3StartPosition.z) * smoothT;
                    pTransform->SetPosition(pos);

                    // 이동 방향 바라보기
                    if (t < 1.0f)
                    {
                        XMFLOAT2 dir = MathUtils::Direction2D(pos, targetAir);
                        if (dir.x != 0.0f || dir.y != 0.0f)
                        {
                            float yaw = atan2f(dir.x, dir.y) * (180.0f / XM_PI);
                            pTransform->SetRotation(0.0f, yaw, 0.0f);
                        }
                    }
                }
            }
        }
        if (m_fTimer >= m_fMoveToWallTime)
        {
            if (AnimationComponent* pAnim = pEnemy->GetAnimationComponent())
                pAnim->CrossFade("Land", 0.15f, false);

            if (GameObject* pOwner = pEnemy->GetOwner())
                if (TransformComponent* pTransform = pOwner->GetTransform())
                    m_xmf3StartPosition = pTransform->GetPosition();

            m_ePhase = Phase::ReturnLand;
            m_fTimer = 0.0f;
        }
        break;

    case Phase::ReturnLand:
        {
            GameObject* pOwner = pEnemy->GetOwner();
            if (pOwner)
            {
                TransformComponent* pTransform = pOwner->GetTransform();
                if (pTransform)
                {
                    float t = m_fTimer / m_fLandingTime;
                    t = min(t, 1.0f);
                    float smoothT = 1.0f - (1.0f - t) * (1.0f - t);

                    XMFLOAT3 pos = m_xmf3StartPosition;
                    pos.y = m_xmf3StartPosition.y - m_fFlyHeight * smoothT;
                    pTransform->SetPosition(pos);
                }
            }
        }
        if (m_fTimer >= m_fLandingTime)
        {
            // 완전히 원위치로 스냅
            if (GameObject* pOwner = pEnemy->GetOwner())
                if (TransformComponent* pTransform = pOwner->GetTransform())
                    pTransform->SetPosition(m_xmf3OriginalPosition);

            // 용암 기믹 재활성화
            if (m_pRoom) m_pRoom->SetLavaGeyserEnabled(true);

            // 시네마틱 카메라 종료 (보험)
            if (m_bCinematicActive && m_pRoom)
            {
                if (Scene* pScene = m_pRoom->GetScene())
                    if (CCamera* pCam = pScene->GetCamera())
                        pCam->StopCinematic();
                m_bCinematicActive = false;
            }

            m_bFinished = true;
        }
        break;
    }
}

// ─── 시네마틱 카메라 연출 ───────────────────────────────────────────────────
//  Phase 별로 3단 구성:
//   1) TakeOff/MoveToWall/Landing  → 하늘의 용을 추적하는 와이드 숏 (용이 프레임에 다 들어오게)
//   2) SpawnCover                  → 방 중앙 기둥 위로 내려다보는 숏
//   3) Windup/Breath               → 플레이어 뒤 어깨 너머 (용 쪽을 향해)
//  목표값을 매 프레임 계산해 현재 카메라 값에서 지수 블렌드 → 급격한 점프 제거
void MegaBreathAttackBehavior::UpdateCinematicCamera(float dt, EnemyComponent* pEnemy)
{
    if (!pEnemy || !m_pRoom) return;
    Scene* pScene = m_pRoom->GetScene();
    if (!pScene) return;
    CCamera* pCam = pScene->GetCamera();
    if (!pCam) return;

    GameObject* pOwner  = pEnemy->GetOwner();
    GameObject* pPlayer = pEnemy->GetTarget();
    if (!pOwner) return;
    TransformComponent* pOwnerT = pOwner->GetTransform();
    if (!pOwnerT) return;

    XMFLOAT3 dragonPos = pOwnerT->GetPosition();
    XMFLOAT3 playerPos = dragonPos;
    if (pPlayer)
        if (auto* pPT = pPlayer->GetTransform())
            playerPos = pPT->GetPosition();

    const BoundingBox& roomBox = m_pRoom->GetBoundingBox();

    // Recovery: cinematic 유지한 채 기본 orbit 값으로 부드럽게 블렌드 → Recovery 막바지에 StopCinematic
    //   기본 orbit: dist 50, pitch 60, yaw 45, lookAt = player
    if (m_ePhase == Phase::Recovery)
    {
        if (!m_bCinematicActive) return;

        // 남은 시간 0.15s 이하면 StopCinematic (막판에 부드럽게 끊기)
        if (m_fTimer >= m_fRecoveryTime - 0.15f)
        {
            pCam->StopCinematic();
            m_bCinematicActive = false;
            return;
        }

        XMFLOAT3 tgtLookAt = { playerPos.x, playerPos.y + 1.0f, playerPos.z };
        float tgtDist  = 50.f;
        float tgtPitch = 60.f;
        float tgtYaw   = 45.f;

        // Recovery 는 빠르게 수렴해야 해서 더 높은 rate 사용
        const float kBlendOut = 5.5f;
        float rate = 1.0f - expf(-dt * kBlendOut);
        rate = (rate < 0.f) ? 0.f : ((rate > 1.f) ? 1.f : rate);

        m_xmf3CamLookAt.x += (tgtLookAt.x - m_xmf3CamLookAt.x) * rate;
        m_xmf3CamLookAt.y += (tgtLookAt.y - m_xmf3CamLookAt.y) * rate;
        m_xmf3CamLookAt.z += (tgtLookAt.z - m_xmf3CamLookAt.z) * rate;
        m_fCamDist  += (tgtDist  - m_fCamDist)  * rate;
        m_fCamPitch += (tgtPitch - m_fCamPitch) * rate;
        float yawDelta = tgtYaw - m_fCamYaw;
        while (yawDelta >  180.f) yawDelta -= 360.f;
        while (yawDelta < -180.f) yawDelta += 360.f;
        m_fCamYaw += yawDelta * rate;

        pCam->SetCinematicLookAt(m_xmf3CamLookAt);
        pCam->SetCinematicOrbit(m_fCamDist, m_fCamPitch, m_fCamYaw);
        return;
    }

    // 복귀 비행 페이즈들: cinematic off — 일반 탑뷰로 진행
    if (m_ePhase == Phase::ReturnTakeOff
     || m_ePhase == Phase::ReturnFly
     || m_ePhase == Phase::ReturnLand)
    {
        if (m_bCinematicActive)
        {
            pCam->StopCinematic();
            m_bCinematicActive = false;
        }
        return;
    }

    // ── 페이즈별 목표 파라미터 결정 ──
    XMFLOAT3 tgtLookAt = { 0, 0, 0 };
    float tgtDist  = 30.f;
    float tgtPitch = 25.f;
    float tgtYaw   = 0.f;

    switch (m_ePhase)
    {
    case Phase::TakeOff:
    case Phase::MoveToWall:
    case Phase::Landing:
    {
        // 용이 화면에 충분히 크게, 하늘이 배경이 되도록 카메라 높이 크게 + 거리 확보
        tgtLookAt = { dragonPos.x, dragonPos.y + 4.0f, dragonPos.z };

        // 방 중앙 기준 용 방향을 기준 yaw로 (카메라는 방 바깥쪽에 위치)
        float dx = dragonPos.x - roomBox.Center.x;
        float dz = dragonPos.z - roomBox.Center.z;
        float radius = sqrtf(dx * dx + dz * dz);
        float baseYaw = (radius > 0.5f)
                      ? atan2f(dx, dz) * (180.0f / XM_PI)
                      : 0.f;

        // 다음 단계(SpawnCover/Windup) 에서 쓸 playerDir yaw
        float ddx = playerPos.x - dragonPos.x;
        float ddz = playerPos.z - dragonPos.z;
        float playerDirYaw = atan2f(ddx, ddz) * (180.0f / XM_PI);

        // 비행 진행도 전반에 걸쳐 완만한 30도 팬 (-15 → +15)
        float totalFlight = m_fTakeOffTime + m_fMoveToWallTime + m_fLandingTime;
        float stageBase   = 0.f;
        if (m_ePhase == Phase::MoveToWall) stageBase = m_fTakeOffTime;
        else if (m_ePhase == Phase::Landing) stageBase = m_fTakeOffTime + m_fMoveToWallTime;
        float globalT = (stageBase + m_fTimer) / (totalFlight > 0.001f ? totalFlight : 1.f);
        float yawOffset = (globalT - 0.5f) * 30.0f;
        float flightYaw = baseYaw + 45.0f + yawOffset;

        // Landing 단계: flightYaw → playerDirYaw 로 부드럽게 수렴 (다음 SpawnCover 와 yaw 일치)
        //   landingT = 0 이면 flightYaw, 1 이면 playerDirYaw
        if (m_ePhase == Phase::Landing)
        {
            float landingT = m_fTimer / (m_fLandingTime > 0.001f ? m_fLandingTime : 1.f);
            landingT = (landingT < 0.f) ? 0.f : ((landingT > 1.f) ? 1.f : landingT);
            // 최단 각도 경로로 보간
            float diff = playerDirYaw - flightYaw;
            while (diff >  180.f) diff -= 360.f;
            while (diff < -180.f) diff += 360.f;
            tgtYaw = flightYaw + diff * landingT;
        }
        else
        {
            tgtYaw = flightYaw;
        }

        tgtPitch = 28.0f;
        tgtDist  = 48.0f;
        break;
    }

    case Phase::SpawnCover:
    {
        // "와이드 establishing 숏" — Windup 과 동일 구도지만 거리/각도 큼
        //   플레이어 중심 + 용 반대편에서 내려다봐 기둥/용/플레이어 전부 프레임에 들어옴
        //   Windup 에 그대로 이어지면서 pitch/dist 만 조금 바뀌는 "zoom in" 느낌으로 매끄러움
        float ddx = playerPos.x - dragonPos.x;
        float ddz = playerPos.z - dragonPos.z;
        float yaw = atan2f(ddx, ddz) * (180.0f / XM_PI);

        // lookAt 은 플레이어 — Windup 과 연속. y 는 조금 더 띄워 와이드감 보강
        tgtLookAt = { playerPos.x, playerPos.y + 2.5f, playerPos.z };
        tgtDist   = 78.0f;   // Windup(62) 보다 훨씬 멀리 — 방 전체 조감
        tgtPitch  = 50.0f;   // Windup(55) 보다 살짝 낮게 → SpawnCover→Windup 은 pitch 상승 + zoom-in 으로 연출
        tgtYaw    = yaw;
        break;
    }

    case Phase::Windup:
    case Phase::Breath:
    {
        // 어깨 너머 숏: 카메라 offset 방향 = (player - dragon) — 플레이어 뒤편
        //   pitch 높여서 브레스 전체 모양이 화면에 담기도록, dist 도 살짝 증가
        //   lookAt 을 플레이어와 용 사이 25% 지점으로 옮겨 프레이밍 중심을 브레스 쪽으로
        float ddx = playerPos.x - dragonPos.x;
        float ddz = playerPos.z - dragonPos.z;
        float yaw = atan2f(ddx, ddz) * (180.0f / XM_PI);

        // 기본 탑뷰와 동일한 프레이밍(플레이어 중심), 거리만 길게 + 각도는 살짝만 조절
        //   기본 orbit: dist 50, pitch 60, yaw 45
        //   여기선: dist 62, pitch 55 (조금 덜 수직 → 전방 시야 살짝 확보), yaw는 용 반대편
        tgtLookAt = { playerPos.x, playerPos.y + 1.0f, playerPos.z };
        tgtDist   = 62.0f;
        tgtPitch  = 55.0f;
        tgtYaw    = yaw;
        break;
    }

    default: break;
    }

    // ── 첫 프레임: 현재 상태를 목표값으로 초기화 + StartCinematic ──
    if (!m_bCinematicActive || !m_bCamStateInit)
    {
        m_xmf3CamLookAt = tgtLookAt;
        m_fCamDist      = tgtDist;
        m_fCamPitch     = tgtPitch;
        m_fCamYaw       = tgtYaw;
        pCam->StartCinematic(m_xmf3CamLookAt, m_fCamDist, m_fCamPitch, m_fCamYaw);
        m_bCinematicActive = true;
        m_bCamStateInit    = true;
        return;
    }

    // ── 지수 블렌드 (반감기 ~0.35s) ──
    //   rate = 1 - exp(-dt * k), k 클수록 빠르게 따라옴
    //   SpawnCover (1.2s) 와 Windup 초반에 걸쳐 완만한 pan/zoom 이 보이도록 k=2
    const float kBlend = 2.0f;
    float rate = 1.0f - expf(-dt * kBlend);
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 1.0f) rate = 1.0f;

    m_xmf3CamLookAt.x += (tgtLookAt.x - m_xmf3CamLookAt.x) * rate;
    m_xmf3CamLookAt.y += (tgtLookAt.y - m_xmf3CamLookAt.y) * rate;
    m_xmf3CamLookAt.z += (tgtLookAt.z - m_xmf3CamLookAt.z) * rate;
    m_fCamDist  += (tgtDist  - m_fCamDist)  * rate;
    m_fCamPitch += (tgtPitch - m_fCamPitch) * rate;

    // yaw: 360도 래핑 처리 — 최단 각도로 보간
    float yawDelta = tgtYaw - m_fCamYaw;
    while (yawDelta >  180.f) yawDelta -= 360.f;
    while (yawDelta < -180.f) yawDelta += 360.f;
    m_fCamYaw += yawDelta * rate;

    pCam->SetCinematicLookAt(m_xmf3CamLookAt);
    pCam->SetCinematicOrbit(m_fCamDist, m_fCamPitch, m_fCamYaw);
}

bool MegaBreathAttackBehavior::IsFinished() const
{
    return m_bFinished;
}

void MegaBreathAttackBehavior::Reset()
{
    m_ePhase = Phase::TakeOff;
    m_fTimer = 0.0f;
    m_fDamageTickTimer = 0.0f;
    m_bFinished = false;
    m_xmf3WallPosition = { 0.0f, 0.0f, 0.0f };
    m_xmf3StartPosition = { 0.0f, 0.0f, 0.0f };
    m_nWallDirection = 0;
    m_bBreathAnimStarted = false;
    m_bCoverSpawned = false;
    m_bCinematicActive = false;
    m_bCamStateInit    = false;

    // 이전 공격이 중단됐을 수도 있으니 용암 기믹을 안전하게 복구
    if (m_pRoom) m_pRoom->SetLavaGeyserEnabled(true);

    // Fire Wave / Charge VFX 정리
    DestroyFireWave();
    DestroyChargeVFX();

    // 남아있는 엄폐물이 있으면 정리
    DestroyCoverObjects();
}

XMFLOAT3 MegaBreathAttackBehavior::CalculateWallPosition()
{
    if (!m_pRoom) return XMFLOAT3(0.0f, 0.0f, 0.0f);

    const BoundingBox& roomBox = m_pRoom->GetBoundingBox();

    // 방의 경계 계산
    float minX = roomBox.Center.x - roomBox.Extents.x;
    float maxX = roomBox.Center.x + roomBox.Extents.x;
    float minZ = roomBox.Center.z - roomBox.Extents.z;
    float maxZ = roomBox.Center.z + roomBox.Extents.z;

    // 4방향 중 랜덤 선택
    m_nWallDirection = rand() % 4;

    XMFLOAT3 wallPos = { roomBox.Center.x, 0.0f, roomBox.Center.z };
    const float WALL_OFFSET = 15.0f;  // 벽에서 충분히 안쪽

    switch (m_nWallDirection)
    {
    case 0: // +X (오른쪽 벽)
        wallPos.x = maxX - WALL_OFFSET;
        break;
    case 1: // -X (왼쪽 벽)
        wallPos.x = minX + WALL_OFFSET;
        break;
    case 2: // +Z (앞쪽 벽)
        wallPos.z = maxZ - WALL_OFFSET;
        break;
    case 3: // -Z (뒤쪽 벽)
        wallPos.z = minZ + WALL_OFFSET;
        break;
    }

    return wallPos;
}

void MegaBreathAttackBehavior::UpdateMoveToWall(float dt, EnemyComponent* pEnemy)
{
    if (!pEnemy) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;

    TransformComponent* pTransform = pOwner->GetTransform();
    if (!pTransform) return;

    // 현재 위치에서 목표 위치로 보간
    float t = m_fTimer / m_fMoveToWallTime;
    t = min(t, 1.0f);

    // 부드러운 이동 (easeOutQuad)
    float smoothT = 1.0f - (1.0f - t) * (1.0f - t);

    XMFLOAT3 newPos;
    newPos.x = m_xmf3StartPosition.x + (m_xmf3WallPosition.x - m_xmf3StartPosition.x) * smoothT;
    newPos.y = m_xmf3StartPosition.y + (m_xmf3WallPosition.y - m_xmf3StartPosition.y) * smoothT;
    newPos.z = m_xmf3StartPosition.z + (m_xmf3WallPosition.z - m_xmf3StartPosition.z) * smoothT;

    pTransform->SetPosition(newPos);

    // 이동 방향을 바라봄
    if (t < 1.0f)
    {
        XMFLOAT2 dir = MathUtils::Direction2D(pTransform->GetPosition(), m_xmf3WallPosition);
        if (dir.x != 0.0f || dir.y != 0.0f)
        {
            float yaw = atan2f(dir.x, dir.y) * (180.0f / XM_PI);
            pTransform->SetRotation(0.0f, yaw, 0.0f);
        }
    }
}

void MegaBreathAttackBehavior::SpawnCoverObjects(EnemyComponent* pEnemy)
{
    if (!m_pRoom) return;

    Scene* pScene = m_pRoom->GetScene();
    if (!pScene) return;

    Dx12App* pApp = Dx12App::GetInstance();
    if (!pApp) return;

    ID3D12Device* pDevice = pApp->GetDevice();
    ID3D12GraphicsCommandList* pCommandList = pApp->GetCommandList();
    Shader* pShader = pScene->GetDefaultShader();

    const BoundingBox& roomBox = m_pRoom->GetBoundingBox();
    XMFLOAT3 center = { roomBox.Center.x, 0.0f, roomBox.Center.z };

    // 방 크기에 비례한 엄폐물 배치 거리
    float coverDist = min(roomBox.Extents.x, roomBox.Extents.z) * 0.5f;

    // 4개의 엄폐물 위치 (방 중앙 주변)
    XMFLOAT3 coverPositions[4] = {
        { center.x + coverDist, 0.0f, center.z },
        { center.x - coverDist, 0.0f, center.z },
        { center.x, 0.0f, center.z + coverDist },
        { center.x, 0.0f, center.z - coverDist }
    };

    m_vObstacles.clear();
    m_vCoverObjects.clear();

    for (int i = 0; i < 4; ++i)
    {
        // 엄폐물의 BoundingBox를 장애물 목록에 추가
        BoundingBox coverBox;
        coverBox.Center = { coverPositions[i].x, coverPositions[i].y + m_fCoverObjectSize, coverPositions[i].z };
        coverBox.Extents = { m_fCoverObjectSize, m_fCoverObjectSize * 2.0f, m_fCoverObjectSize };
        m_vObstacles.push_back(coverBox);

        // 실제 GameObject 생성
        if (pDevice && pCommandList && pShader)
        {
            // Room을 현재 방으로 설정하여 오브젝트가 방에 추가되도록 함
            CRoom* pPrevRoom = pScene->GetCurrentRoom();
            pScene->SetCurrentRoom(m_pRoom);

            GameObject* pCover = pScene->CreateGameObject(pDevice, pCommandList);
            if (pCover)
            {
                // 위치/스케일 설정
                TransformComponent* pTransform = pCover->GetTransform();
                if (pTransform)
                {
                    pTransform->SetPosition(coverPositions[i].x, coverPositions[i].y, coverPositions[i].z);
                    // 우물 모델에 맞는 거대한 스케일 설정
                    pTransform->SetScale(5.0f, 5.0f, 5.0f);
                }

                // WellSmall_001.obj 모델 로드
                Mesh* pWellMesh = MapLoader::LoadMesh("Assets/MapData/meshes/ColumnBig_001.obj", pDevice, pCommandList);
                if (pWellMesh)
                {
                    pWellMesh->AddRef();
                    pCover->SetMesh(pWellMesh);
                }

                // 돌 느낌 재질
                MATERIAL material;
                material.m_cAmbient = XMFLOAT4(0.3f, 0.3f, 0.3f, 1.0f);
                material.m_cDiffuse = XMFLOAT4(0.7f, 0.7f, 0.7f, 1.0f);
                material.m_cSpecular = XMFLOAT4(0.2f, 0.2f, 0.2f, 8.0f);
                material.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                pCover->SetMaterial(material);

                // 렌더 컴포넌트
                auto* pRenderComp = pCover->AddComponent<RenderComponent>();
                if (pWellMesh) pRenderComp->SetMesh(pWellMesh);
                pShader->AddRenderComponent(pRenderComp);

                // 콜라이더 (Wall 레이어) - 모델 비주얼에 맞춰 히트박스 축소
                auto* pCollider = pCover->AddComponent<ColliderComponent>();
                // 가로 폭을 4.5 -> 1.5로 줄여 이동 방해 최소화, 높이는 유지
                pCollider->SetExtents(1.5f, 6.0f, 1.5f);
                pCollider->SetCenter(0.0f, 3.0f, 0.0f);
                pCollider->SetLayer(CollisionLayer::Wall);
                pCollider->SetCollisionMask(CollisionMask::Wall);

                // 엄폐 판정용 리스트에도 업데이트된 크기 반영
                coverBox.Extents = { 1.5f, 6.0f, 1.5f };
                m_vObstacles.back() = coverBox;

                // 나중에 Destroy 할 수 있도록 트랙 (이전엔 추가 안 해서 기둥이 안 사라졌음)
                m_vCoverObjects.push_back(pCover);

                pScene->SetCurrentRoom(pPrevRoom);
            }
        }

        // 방 내 기존 장애물(Wall 레이어)도 장애물 목록에 추가
        const auto& gameObjects = m_pRoom->GetGameObjects();
        for (const auto& pObj : gameObjects)
        {
            if (!pObj) continue;

            ColliderComponent* pCollider = pObj->GetComponent<ColliderComponent>();
            if (pCollider && pCollider->GetLayer() == CollisionLayer::Wall)
            {
                const BoundingOrientedBox& obb = pCollider->GetBoundingBox();
                // OBB를 AABB로 변환 (간단화)
                BoundingBox aabb;
                aabb.Center = obb.Center;
                aabb.Extents = obb.Extents;
                m_vObstacles.push_back(aabb);
            }
        }

    }
}
void MegaBreathAttackBehavior::DestroyCoverObjects()
{
    // Scene::MarkForDeletion 으로 프레임 끝에서 안전하게 삭제 (iteration 중 Room vector erase 시 UB 방지)
    //   부가로 Shader render component 도 해제해서 남은 프레임 잔상 렌더 방지
    Scene* pScene = m_pRoom ? m_pRoom->GetScene() : nullptr;
    Shader* pShader = pScene ? pScene->GetDefaultShader() : nullptr;

    for (GameObject* pCover : m_vCoverObjects)
    {
        if (!pCover) continue;

        if (pShader)
        {
            if (auto* pRenderComp = pCover->GetComponent<RenderComponent>())
                pShader->RemoveRenderComponent(pRenderComp);
        }
        // 시각적으로 즉시 숨기기 위해 땅 아래로 (이번 프레임 남은 렌더 보험)
        if (auto* pT = pCover->GetTransform())
            pT->SetPosition(0.0f, -1000.0f, 0.0f);

        if (pScene) pScene->MarkForDeletion(pCover);
    }

    m_vCoverObjects.clear();
    m_vObstacles.clear();

}

bool MegaBreathAttackBehavior::IsPlayerBehindCover(const XMFLOAT3& breathOrigin, const XMFLOAT3& playerPos)
{
    UNREFERENCED_PARAMETER(breathOrigin);
    // 평행 커튼 그림자 판정 — 불길이 진행 방향(jetDir)으로 평행하게 흐르므로
    //   안전지대는 각 기둥의 "진행방향 하류" 그림자뿐. 유체 원기둥과 동일 반경.
    //   기둥은 수직이라 높이 무시 (XZ 평면).
    float fx = m_xmf3BeamDirection.x;
    float fz = m_xmf3BeamDirection.z;
    float flen = sqrtf(fx * fx + fz * fz);
    if (flen < 0.001f) return false;
    fx /= flen; fz /= flen;

    // VFX 매칭: 안전지대는 기둥 바로 뒤 "포켓"으로 제한. 불길이 포켓 뒤에서 다시 합쳐지므로
    //   하류로 갈수록 안전 폭이 줄어드는 쐐기형이고, 포켓 길이 너머는 더 이상 안전이 아니다.
    const float kPocketLength = 16.0f;   // 포켓 길이 — 이 거리 뒤는 불길 재합류 → 노출
    const float kStartRadius  = 6.0f;    // 기둥 바로 뒤 안전 반폭 (강한 갈라짐 반영, 넓게)

    for (GameObject* pCover : m_vCoverObjects)
    {
        if (!pCover) continue;
        TransformComponent* pT = pCover->GetTransform();
        if (!pT) continue;
        XMFLOAT3 c = pT->GetPosition();

        // 기둥 → 플레이어 벡터를 진행축으로 분해
        float vx = playerPos.x - c.x;
        float vz = playerPos.z - c.z;
        float alongFlow = vx * fx + vz * fz;          // >0: 플레이어가 기둥 하류(포켓 쪽)
        if (alongFlow <= 0.0f || alongFlow > kPocketLength) continue; // 기둥 앞 / 포켓 너머 → 안전 아님

        float perpX = vx - fx * alongFlow;
        float perpZ = vz - fz * alongFlow;
        float perp = sqrtf(perpX * perpX + perpZ * perpZ);

        // 하류로 갈수록 좁아지는 안전 폭 (불길 재합류) — 포켓 끝에서 0
        float safeR = kStartRadius * (1.0f - alongFlow / kPocketLength);
        if (perp < safeR) return true;                // 포켓 안 → 안전
    }

    return false;  // 노출됨
}

bool MegaBreathAttackBehavior::RayIntersectsAABB(const XMFLOAT3& rayOrigin, const XMFLOAT3& rayDir,
                                                  const BoundingBox& box, float maxDist)
{
    // Slab method를 사용한 Ray-AABB 교차 검사
    float tmin = 0.0f;
    float tmax = maxDist;

    // X축
    float boxMin = box.Center.x - box.Extents.x;
    float boxMax = box.Center.x + box.Extents.x;
    if (fabsf(rayDir.x) < 0.0001f)
    {
        if (rayOrigin.x < boxMin || rayOrigin.x > boxMax)
            return false;
    }
    else
    {
        float invD = 1.0f / rayDir.x;
        float t1 = (boxMin - rayOrigin.x) * invD;
        float t2 = (boxMax - rayOrigin.x) * invD;
        if (t1 > t2) std::swap(t1, t2);
        tmin = max(tmin, t1);
        tmax = min(tmax, t2);
        if (tmin > tmax) return false;
    }

    // Y축
    boxMin = box.Center.y - box.Extents.y;
    boxMax = box.Center.y + box.Extents.y;
    if (fabsf(rayDir.y) < 0.0001f)
    {
        if (rayOrigin.y < boxMin || rayOrigin.y > boxMax)
            return false;
    }
    else
    {
        float invD = 1.0f / rayDir.y;
        float t1 = (boxMin - rayOrigin.y) * invD;
        float t2 = (boxMax - rayOrigin.y) * invD;
        if (t1 > t2) std::swap(t1, t2);
        tmin = max(tmin, t1);
        tmax = min(tmax, t2);
        if (tmin > tmax) return false;
    }

    // Z축
    boxMin = box.Center.z - box.Extents.z;
    boxMax = box.Center.z + box.Extents.z;
    if (fabsf(rayDir.z) < 0.0001f)
    {
        if (rayOrigin.z < boxMin || rayOrigin.z > boxMax)
            return false;
    }
    else
    {
        float invD = 1.0f / rayDir.z;
        float t1 = (boxMin - rayOrigin.z) * invD;
        float t2 = (boxMax - rayOrigin.z) * invD;
        if (t1 > t2) std::swap(t1, t2);
        tmin = max(tmin, t1);
        tmax = min(tmax, t2);
        if (tmin > tmax) return false;
    }

    return true;
}

void MegaBreathAttackBehavior::ApplyBreathDamage(EnemyComponent* pEnemy)
{
    if (!pEnemy) return;

    GameObject* pTarget = pEnemy->GetTarget();
    if (!pTarget) return;

    TransformComponent* pTargetTransform = pTarget->GetTransform();
    if (!pTargetTransform) return;

    XMFLOAT3 playerPos = pTargetTransform->GetPosition();
    playerPos.y += 1.0f;

    // ── 평행 커튼 판정: 진행축으로 쓸고 지나가는 불의 장막 ───────────────────
    //   히트 = (커튼이 도달한 진행축 구간) AND (커튼 반폭 안) AND (기둥 그림자 아님)
    if (m_fBeamLength <= 0.0f) return;

    float fx = m_xmf3BeamDirection.x;
    float fz = m_xmf3BeamDirection.z;
    float flen = sqrtf(fx * fx + fz * fz);
    if (flen < 0.001f) return;
    fx /= flen; fz /= flen;

    float rx = playerPos.x - m_xmf3BeamOrigin.x;
    float rz = playerPos.z - m_xmf3BeamOrigin.z;

    float along = rx * fx + rz * fz;             // 진행축 좌표 (0=시작 라인)
    float perpX = rx - fx * along;
    float perpZ = rz - fz * along;
    float lateral = sqrtf(perpX * perpX + perpZ * perpZ);

    // 커튼 선두 위치 = 속도 × 경과시간 (+ 약간 여유). m_fTimer = Breath 경과.
    float front = m_fJetSpeed * m_fTimer + 6.0f;

    if (along < -4.0f || along > m_fBeamLength) return; // 진행축 범위 밖 (뒤/끝 너머)
    if (along > front) return;                          // 아직 불길이 안 닿음
    if (lateral > m_fBeamEndRadius + 2.0f) return;      // 커튼 폭 밖

    // 엄폐 판정 — 기둥 진행방향 그림자 안이면 safe
    if (IsPlayerBehindCover(m_xmf3BeamOrigin, playerPos))
    {
        return;
    }

    // ── 3. 데미지 적용 ───────────────────────────────────────────────
    PlayerComponent* pPlayer = pTarget->GetComponent<PlayerComponent>();
    if (pPlayer)
    {
        pPlayer->TakeDamage(m_fDamagePerTick);
    }
}

// ─── Fire Wave (SPH 플루이드) ──────────────────────────────────────────────
void MegaBreathAttackBehavior::SpawnFireWave(EnemyComponent* pEnemy)
{
    if (!pEnemy || !m_pRoom) return;

    Scene* pScene = m_pRoom->GetScene();
    if (!pScene) return;

    // 플레이어 매니저 사용 — SSF 파이프라인(RenderDepth→Blur→Composite)을 타야 매끈한 빔.
    //  enemy 매니저는 RenderEnemyEffects(빌보드)만 호출돼 구슬처럼 보임.
    //  e51dda1 per-pixel 색상 아키텍처로 플레이어 스킬과 색상 충돌 근본 해결됨.
    m_pFluidVFXManager = pScene->GetFluidVFXManager();
    if (!m_pFluidVFXManager) return;

    // 방 바운드 + 벽 방향 기반 파도 경로 계산
    const BoundingBox& rb = m_pRoom->GetBoundingBox();
    // (방 AABB 는 m_pRoom->GetBoundingBox() 로 필요 시 참조)

    // 보스 실제 위치 + 방향 (Windup 단계에서 보스는 방 중앙을 향해 회전되어 있음)
    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;
    auto* pT = pOwner->GetTransform();
    if (!pT) return;

    XMFLOAT3 bossPos = pT->GetPosition();
    XMFLOAT3 bossRot = pT->GetRotation();
    float yawRad = XMConvertToRadians(bossRot.y);

    // 보스 입 위치 (전방 17u, 머리 높이 7u — charge VFX 와 동일 지점에서 발사)
    m_xmf3BeamOrigin.x = bossPos.x + sinf(yawRad) * 17.0f;
    m_xmf3BeamOrigin.y = bossPos.y + 7.0f;
    m_xmf3BeamOrigin.z = bossPos.z + cosf(yawRad) * 17.0f;

    m_xmf3BeamDirection = { sinf(yawRad), 0.0f, cosf(yawRad) };

    // 진행축 사거리(입→맵 반대편) + 커튼 반폭. 화염은 맵 전체를 균일하게 덮고
    //   안전지대는 기둥 진행방향 그림자뿐 (평행 커튼 모델).
    float diagX = rb.Extents.x * 2.0f;
    float diagZ = rb.Extents.z * 2.0f;
    m_fBeamLength   = sqrtf(diagX * diagX + diagZ * diagZ) * 1.15f;
    // 커튼 반폭 — 진행 방향에 수직인 방 반폭의 0.95배 (벽 안쪽 → 벽 충돌 squirt 방지)
    float perpExtent = (fabsf(m_xmf3BeamDirection.x) > 0.5f) ? rb.Extents.z : rb.Extents.x;
    m_fBeamEndRadius = perpExtent * 0.95f;

    // ════════════════════════════════════════════════════════════════════════
    //  진짜 SPH 화염 홍수: 입에서 분사된 유체가 바닥을 타고 흐르며,
    //  엄폐 기둥(수직 원기둥)에 부딪혀 갈라지고 옆으로 퍼진다.
    //  - 단일 SPH_Gravity 이미터 + GPU 제트 분사/재순환
    //  - 컴퓨트 셰이더가 원기둥 충돌·바닥·방 경계를 처리 (FluidParticleSystem)
    // ════════════════════════════════════════════════════════════════════════
    for (int i = 0; i < NUM_BEAMS; ++i) m_nFluidVFXIds[i] = -1;

    // 바닥 높이 = 실제 지면(보스 발치). 방 AABB 바닥(rb.Center.y-Extents.y)은 지면보다
    //   훨씬 아래라 그걸 floor로 쓰면 화염이 지면 밑으로 가라앉아 안 보임 → bossPos.y 사용.
    const float floorY = bossPos.y;
    const float inset  = 2.0f;
    XMFLOAT3 roomMin = { rb.Center.x - rb.Extents.x + inset, floorY,
                         rb.Center.z - rb.Extents.z + inset };
    XMFLOAT3 roomMax = { rb.Center.x + rb.Extents.x - inset, floorY + 30.0f,
                         rb.Center.z + rb.Extents.z - inset };

    // 분사 파라미터 — 맵 전체를 덮도록 부채꼴로 넓게, 빠르고 멀리 분사
    //   제트 사거리는 방 대각선 전체(여유 1.15배)로 잡아 반대편 벽·구석까지 도달.
    const float roomDiag    = sqrtf((rb.Extents.x * 2.0f) * (rb.Extents.x * 2.0f)
                                  + (rb.Extents.z * 2.0f) * (rb.Extents.z * 2.0f));
    const float jetLength   = roomDiag * 1.15f;                           // 축방향 사거리 (구석까지)
    const float jetSpeed    = jetLength / (m_fBreathDuration * 0.26f);    // 축방향 도달 속도 (빠르게)
    const float jetLifetime = m_fBreathDuration * 0.55f;                  // 짧은 수명 → 라인에서 계속 재방출
    const float jetVertJit  = 2.0f;              // 재방출 시 높이 산포 (작게 → 낮게 깔리게)
    const float curtainHalfW = m_fBeamEndRadius; // 커튼 반폭 = 맵 폭 전체
    m_fJetSpeed             = jetSpeed;           // 데미지 front 계산용 저장

    // ── 단일 SPH_Gravity 화염 홍수 EffectDef ──
    EffectDef def;
    def.name    = "Dragon_MegaBreath";
    def.element = ElementType::Fire;

    EffectLayer layer;
    layer.type       = EmitterType::SPH_Gravity;
    layer.element    = ElementType::Fire;
    layer.coreColor  = { 1.0f, 0.5f, 0.12f, 1.0f };
    layer.edgeColor  = { 0.9f, 0.22f, 0.04f, 0.9f };
    layer.overrideColors = true;
    layer.useSSF     = true;

    SPHEmitterParams& s = layer.sph;
    s.particleCount    = 12288;         // 최대치 — 입자 구분 안 되게 조밀하게
    s.spawnRadius      = curtainHalfW;  // 초기 스폰 폭 (SetSPHJet에서 커튼으로 재배치)
    s.particleSize     = 1.7f;          // 수가 2배라 약간 작게도 빈틈 없이 연속 화염
    s.maxParticleSpeed = jetSpeed * 1.8f;
    // 화염은 점성 낮고 잘 흩어지게 — 흐르는 용암 느낌. 응집/점성 낮춰 기둥 옆 정체 완화.
    s.overridePhysics     = true;
    s.sphStiffness        = 45.0f;
    s.sphNearPressureMult = 1.2f;       // 근접 반발 ↓ → 기둥 옆에 빽빽이 고이지 않게
    s.sphRestDensity      = 4.0f;       // 자기 응집 ↓ → 정지된 덩어리로 안 뭉침
    s.sphViscosity        = 0.05f;      // 점성 ↓ → 더 빠르게 흘러감 (느린 흐름 완화)
    s.sphSmoothingRadius  = 2.0f;

    VFXPhase p;
    p.startTime              = 0.f;
    p.duration               = m_fBreathDuration + 0.5f;
    p.motionMode             = ParticleMotionMode::Gravity;
    p.gravityDesc.gravity    = { 0.f, -16.0f, 0.f };  // 중력 ↑ → 공중에 흩어지지 않고 바닥에 깔린 조밀한 홍수
    p.gravityDesc.initialSpeedMin = 0.f;              // 초기 방사 폭발 비활성 (제트가 속도 부여)
    p.gravityDesc.initialSpeedMax = 0.f;
    p.globalGravityStrength  = 0.f;                   // 중복 중력 방지 (gravityDesc만 사용)
    p.boxDesc.active         = false;                 // 경계는 제트 방 클램프로 처리
    s.phases.push_back(p);

    def.layers.push_back(std::move(layer));

    int id = m_pFluidVFXManager->SpawnEffectDef(m_xmf3BeamOrigin, m_xmf3BeamDirection, def, true);
    m_nFluidVFXIds[0] = id;
    if (id < 0) return;

    // 제트 분사/재순환 + 방 경계 설정
    // spawnRadius 인자 = 높이 산포(jetVertJit), spread 인자 = 커튼 반폭(curtainHalfW)
    //   분사 높이는 입(비주얼)보다 낮은 바닥+4u에서 → 공중에 안 뜨고 바닥에 깔린 홍수.
    XMFLOAT3 jetOrigin = m_xmf3BeamOrigin;
    jetOrigin.y = floorY + 3.0f;   // 분사 높이 ↓ → 플레이어 높이로 깔림
    m_pFluidVFXManager->SetEffectJet(
        id, jetOrigin, m_xmf3BeamDirection,
        jetSpeed, jetLength, jetVertJit, curtainHalfW, jetLifetime,
        roomMin, roomMax);

    // 엄폐 기둥을 수직 원기둥 장애물로 등록 — 유체가 부딪혀 갈라지고 옆으로 퍼짐.
    //   콜라이더(반경 1.5)보다 크게 잡아 거대 모델 비주얼을 감싸며 명확히 갈라지게.
    XMFLOAT4 obstacles[4];
    int oc = 0;
    for (GameObject* pCover : m_vCoverObjects)
    {
        if (oc >= 4) break;
        if (!pCover) continue;
        TransformComponent* pCT = pCover->GetTransform();
        if (!pCT) continue;
        XMFLOAT3 cp = pCT->GetPosition();
        obstacles[oc] = { cp.x, cp.z, 3.0f, 0.0f };  // xy=중심XZ, z=반경 (후류 dead zone 축소)
        ++oc;
    }
    if (oc > 0) m_pFluidVFXManager->SetEffectObstacles(id, obstacles, oc);

}

void MegaBreathAttackBehavior::UpdateFireWave(float dt, EnemyComponent* pEnemy)
{
    UNREFERENCED_PARAMETER(dt);
    UNREFERENCED_PARAMETER(pEnemy);
    // Heat trail / shockwave 제거됨 — Fire Wave(SPH 화염 홍수) 단독 운용.
    // GPU 제트 재순환이 분사를 지속하므로 별도 업데이트 불필요.
}

// ─── Charge VFX: 입 주변 파티클 집결 "곧 뭔가 온다" ────────────────────────
// 플레이어 우클릭(Fireball) VFX 의 "사방에서 수렴하는 혜성" 느낌을 가져와서
// 보스 스케일로 키움 — 발사(origin 이동)는 없음. origin 을 입 위치로 고정해 그 자리에 맺힘.
void MegaBreathAttackBehavior::SpawnChargeVFX(EnemyComponent* pEnemy)
{
    if (!pEnemy || !m_pRoom) return;
    if (m_nChargeVFXId >= 0) return;  // 이미 스폰됨

    Scene* pScene = m_pRoom->GetScene();
    if (!pScene) return;

    // Breath 와 동일 매니저 사용 (player = SSF 파이프라인) — manager mismatch 버그 방지
    if (!m_pFluidVFXManager)
        m_pFluidVFXManager = pScene->GetFluidVFXManager();
    if (!m_pFluidVFXManager) return;

    GameObject* pOwner = pEnemy->GetOwner();
    if (!pOwner) return;
    auto* pT = pOwner->GetTransform();
    if (!pT) return;

    // 입 위치 근사 (Windup 단계의 보스 회전 기준) — beam origin 과 동일
    XMFLOAT3 bossPos = pT->GetPosition();
    XMFLOAT3 bossRot = pT->GetRotation();
    float yawRad = XMConvertToRadians(bossRot.y);

    XMFLOAT3 mouth;
    mouth.x = bossPos.x + sinf(yawRad) * 17.0f;
    mouth.y = bossPos.y + 7.0f;
    mouth.z = bossPos.z + cosf(yawRad) * 17.0f;
    XMFLOAT3 forward = { sinf(yawRad), 0.0f, cosf(yawRad) };

    // ════════════════════════════════════════════════════════════════════════
    //  수렴 화염: Fire Wave(분사)를 반대로 재생 — 맵 전역에 흩어진 작은 불씨들이
    //  입으로 빨려 들어오는 "곧 터진다" 예고. 같은 SPH_Gravity + 제트(converge) 사용.
    //  입자 크기↓ 수↓ 로 분사 본체와 시각적으로 구분.
    // ════════════════════════════════════════════════════════════════════════
    const BoundingBox& rb = m_pRoom->GetBoundingBox();
    const float floorY = bossPos.y;   // 실제 지면(보스 발치) — AABB 바닥 쓰면 지면 밑으로 감
    const float inset  = 2.0f;
    XMFLOAT3 roomMin = { rb.Center.x - rb.Extents.x + inset, floorY,
                         rb.Center.z - rb.Extents.z + inset };
    XMFLOAT3 roomMax = { rb.Center.x + rb.Extents.x - inset, floorY + 20.0f,  // 지면~머리 위 부피
                         rb.Center.z + rb.Extents.z - inset };

    const float roomDiag   = sqrtf((rb.Extents.x * 2.f) * (rb.Extents.x * 2.f)
                                 + (rb.Extents.z * 2.f) * (rb.Extents.z * 2.f));
    const float jetLength  = roomDiag * 1.15f;
    const float perpExtent = (fabsf(forward.x) > 0.5f) ? rb.Extents.z : rb.Extents.x;
    const float curtainHalfW = perpExtent * 0.95f;   // 맵 폭 전체에서 빨려듦
    const float vertJit    = 6.0f;                    // 높이 산포 (바닥~위)
    // 수렴 속도: Windup 동안 맵 끝 불씨가 입까지 도달 → 재순환 반복
    const float convSpeed   = jetLength / (m_fWindupTime * 0.45f);
    const float convLifetime = m_fWindupTime * 0.9f;

    EffectDef def;
    def.name    = "Dragon_MegaBreath_Charge";
    def.element = ElementType::Fire;

    EffectLayer layer;
    layer.type       = EmitterType::SPH_Gravity;
    layer.element    = ElementType::Fire;
    layer.coreColor  = { 1.0f, 0.55f, 0.15f, 1.0f };
    layer.edgeColor  = { 0.9f, 0.25f, 0.05f, 0.85f };
    layer.overrideColors = true;
    layer.useSSF     = true;

    SPHEmitterParams& s = layer.sph;
    s.particleCount    = 150;      // 듬성듬성 — 개별 불씨가 띄엄띄엄 빨려드는 느낌
    s.spawnRadius      = curtainHalfW;
    s.particleSize     = 1.4f;     // 수가 적은 만큼 약간 키워 개별 불씨로 보이게
    s.maxParticleSpeed = convSpeed * 1.8f;
    // 응집 거의 없음 → 일자 스트림으로 뭉치지 않고 흩어진 입자로 유지
    s.overridePhysics     = true;
    s.sphStiffness        = 12.0f;
    s.sphNearPressureMult = 0.5f;
    s.sphRestDensity      = 0.0f;  // 0 = 반발만(인력 없음) → 서로 안 들러붙음
    s.sphViscosity        = 0.05f;
    s.sphSmoothingRadius  = 1.2f;

    VFXPhase p;
    p.startTime              = 0.f;
    p.duration               = m_fWindupTime + 0.5f;
    p.motionMode             = ParticleMotionMode::Gravity;
    p.gravityDesc.gravity    = { 0.f, 0.f, 0.f };   // 중력 없이 입으로 곧장 빨려듦
    p.gravityDesc.initialSpeedMin = 0.f;
    p.gravityDesc.initialSpeedMax = 0.f;
    p.globalGravityStrength  = 0.f;
    p.boxDesc.active         = false;
    s.phases.push_back(p);

    def.layers.push_back(std::move(layer));

    int id = m_pFluidVFXManager->SpawnEffectDef(mouth, forward, def, true);
    m_nChargeVFXId = id;
    if (id < 0) return;

    // 수렴 제트 활성화 (converge=true) — 장애물 없음(예고 연출이라 기둥 충돌 불필요)
    m_pFluidVFXManager->SetEffectJet(
        id, mouth, forward,
        convSpeed, jetLength, vertJit, curtainHalfW, convLifetime,
        roomMin, roomMax, true);
}

void MegaBreathAttackBehavior::UpdateChargeVFX(EnemyComponent* /*pEnemy*/)
{
    // origin이 offsetParticlesWithOrigin 으로 따라감 — 여기선 별도 처리 없음
    // (필요 시 SetEffectOrigin 같은 API 통해 보스 머리 따라갈 수 있음)
}

void MegaBreathAttackBehavior::DestroyChargeVFX()
{
    if (m_pFluidVFXManager && m_nChargeVFXId >= 0)
    {
        m_pFluidVFXManager->StopEffect(m_nChargeVFXId);
        m_nChargeVFXId = -1;
    }
}

void MegaBreathAttackBehavior::DestroyFireWave()
{
    if (m_pFluidVFXManager)
    {
        for (int i = 0; i < NUM_BEAMS; ++i)
        {
            if (m_nFluidVFXIds[i] >= 0)
            {
                m_pFluidVFXManager->StopEffect(m_nFluidVFXIds[i]);
                m_nFluidVFXIds[i] = -1;
            }
        }

        // Trail 은 lifetime 기반 자연 소멸이 원칙이지만 강제 중단 시에는 정리
        for (int id : m_vTrailVFXIds) if (id >= 0) m_pFluidVFXManager->StopEffect(id);
    }
    m_vTrailVFXIds.clear();
    m_fTrailDropTimer = 0.f;
    m_pFluidVFXManager = nullptr;
}

// (2) Windup → Breath 전환 shockwave: 집결된 charge 파티클을 입 위치에서 방사형 폭발시켜
//     시각적 "빵!" 임팩트 제공. ExplodeEffect 가 CP 를 해제하고 파티클을 바깥으로 튕겨 보냄.
void MegaBreathAttackBehavior::TriggerMouthShockwave()
{
    if (!m_pFluidVFXManager) return;

    if (m_nChargeVFXId >= 0)
    {
        // ExplodeEffect: 모인 파티클을 입 위치 기준 방사형으로 튕김 → shockwave 연출
        m_pFluidVFXManager->ExplodeEffect(m_nChargeVFXId, m_xmf3BeamOrigin);
        // 파티클이 흩어지며 자연 소멸하도록 ID 만 해제 (StopEffect 하면 즉시 사라져서 효과 없음)
        m_nChargeVFXId = -1;
    }
}

// (3) 파도 전진 위치에 잔불 자국 드롭. WaveSlash 의 DropFireTrail 흉내.
//     5-fan beam 의 "대표 진행도" 는 timer 비율로 근사 — 파티클 실제 위치가 아니라
//     시간에 맞춰 beam 방향을 따라 앞으로 밀리는 가상 커서.
void MegaBreathAttackBehavior::DropHeatTrail()
{
    if (!m_pFluidVFXManager) return;
    if (m_fBeamLength <= 0.f) return;

    // 진행도: Breath 시작 후 경과 시간 / 총 지속 → 0~1
    float progress = m_fTimer / m_fBreathDuration;
    if (progress > 1.0f) progress = 1.0f;

    // 앞쪽에서부터 차곡차곡 자국이 깔리도록 진행도에 따라 전진
    float fwdDist = progress * m_fBeamLength;
    XMFLOAT3 trailPos;
    trailPos.x = m_xmf3BeamOrigin.x + m_xmf3BeamDirection.x * fwdDist;
    trailPos.y = 0.f;  // 바닥
    trailPos.z = m_xmf3BeamOrigin.z + m_xmf3BeamDirection.z * fwdDist;

    // 진행 방향의 직교 벡터 (파도 폭 방향)
    XMFLOAT3 waveRight;
    waveRight.x =  m_xmf3BeamDirection.z;
    waveRight.y = 0.f;
    waveRight.z = -m_xmf3BeamDirection.x;

    // 파도 폭 비례 반폭 — 5-fan end radius 의 대략 절반을 쓰면 맵 그을린 느낌 적당
    float halfW = m_fBeamEndRadius * 0.5f;

    int id = m_pFluidVFXManager->SpawnFireTrailEffect(trailPos, waveRight, halfW, TRAIL_LIFETIME);
    if (id >= 0) m_vTrailVFXIds.push_back(id);
}

