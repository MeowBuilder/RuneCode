#include "stdafx.h"
#include "PlayerComponent.h"
#include "CharacterData.h"
#include "InputSystem.h" // Needed for InputSystem
#include "GameObject.h" // Needed for GameObject
#include "TransformComponent.h" // Needed for TransformComponent
#include "Camera.h" // Needed for CCamera
#include "SkillComponent.h" // Needed for SkillComponent
#include "AnimationComponent.h"
#include "Dx12App.h" // For runtime window size
#include "NetworkManager.h" // For rotation sync
#include "Scene.h"
#include "VFXManager.h"
#include "VFXTypes.h"
#include "DamageNumberManager.h"

PlayerComponent::PlayerComponent(GameObject* pOwner)
    : Component(pOwner)
{
}

void PlayerComponent::SetElementType(ElementType e)
{
    m_elementType = e;
    const CharacterData& cd = GetCharacterData(e);

    m_fMaxHP       = cd.baseHP;
    m_fCurrentHP   = cd.baseHP;
    m_fMoveSpeed   = cd.moveSpeed;
    m_fDashCooldown  = cd.dashCooldown;
    m_fDashDuration  = cd.dashDuration;
    m_fDashSpeedMult = cd.dashSpeedMult;
    m_dashCoreColor  = cd.dashCoreColor;
    m_dashEdgeColor  = cd.dashEdgeColor;
}

void PlayerComponent::PlayerUpdate(float deltaTime, InputSystem* pInputSystem, CCamera* pCamera)
{
    if (!m_pOwner) return;

    TransformComponent* pTransform = m_pOwner->GetTransform();
    if (!pTransform) return;

    // === Flight Mode (4스테이지 바람 보스) — 중력/지면판정/스킬/네트워크 모두 우회 ===
    if (m_bFlightMode)
    {
        UpdateFlightMode(deltaTime, pInputSystem, pCamera);
        return;
    }

    // === Tornado Trap (4스테이지 ambient 회오리에 빨려들어감) — 입력/물리 모두 우회 ===
    if (m_bTornadoTrapped)
    {
        UpdateTornadoTrap(deltaTime);
        return;
    }

    // 피해감소 타이머
    if (m_fDamageReductionTimer > 0.f)
        m_fDamageReductionTimer = fmaxf(0.f, m_fDamageReductionTimer - deltaTime);

    // 무적 타이머
    if (m_fInvincibleTimer > 0.f)
        m_fInvincibleTimer = fmaxf(0.f, m_fInvincibleTimer - deltaTime);

    // 보복 타이머 (ABY_RVG)
    if (m_fVengeanceTimer > 0.f)
    {
        m_fVengeanceTimer -= deltaTime;
        if (m_fVengeanceTimer <= 0.f)
        {
            m_fVengeanceTimer  = 0.f;
            m_bVengeancePrimed = false;
        }
    }

    // Hit flash 페이드 — 대쉬 중이 아닐 때만 (대쉬는 자기 플래시 적용)
    if (m_fHitFlashTimer > 0.0f)
    {
        m_fHitFlashTimer = fmaxf(0.0f, m_fHitFlashTimer - deltaTime);
        if (!IsDashing() && m_fDashFlashTail <= 0.0f)
        {
            float f = m_fHitFlashTimer / kHitFlashDuration;
            m_pOwner->SetHitFlashAll(f);
            if (m_fHitFlashTimer <= 0.0f)
                m_pOwner->SetHitFlashAll(0.0f);
        }
    }

    // Intro Fly — 게임 시작 후 포탈에서 낙하 시퀀스. 입력/스킬 차단, 자유 낙하 + Levitating 애니.
    //   바닥 도달 시 Landing 짧게 → 자동 Idle 복귀.
    if (m_fIntroFlyTimer > 0.0f || m_fLandingHoldTimer > 0.0f)
    {
        XMFLOAT3 pos = pTransform->GetPosition();
        if (!m_bOnGround)
        {
            m_fVelocityY -= GRAVITY * deltaTime;
            pos.y += m_fVelocityY * deltaTime;
            if (pos.y <= m_fIntroGroundY)
            {
                pos.y = m_fIntroGroundY;
                m_fVelocityY = 0.0f;
                m_bOnGround = true;
                if (auto* pAnim = m_pOwner->GetComponent<AnimationComponent>())
                    pAnim->CrossFade("Landing", 0.05f, false, true);
                m_fLandingHoldTimer = 0.50f;
                m_fIntroFlyTimer    = 0.0f;
            }
            pTransform->SetPosition(pos);
        }
        else if (m_fLandingHoldTimer > 0.0f)
        {
            m_fLandingHoldTimer = fmaxf(0.0f, m_fLandingHoldTimer - deltaTime);
            if (m_fLandingHoldTimer <= 0.0f)
            {
                m_eAnimState = PlayerAnimState::Idle;
                if (auto* pAnim = m_pOwner->GetComponent<AnimationComponent>())
                    pAnim->CrossFade("Idle", 0.15f, true);
            }
        }
        if (m_fIntroFlyTimer > 0.0f)
            m_fIntroFlyTimer -= deltaTime;
        return;
    }

    // 사망 상태: 중력/수면은 유지하되 입력·스킬·전송 모두 차단 (데스 애니만 재생 중)
    if (m_bNetworkDead)
    {
        XMFLOAT3 deadPos = pTransform->GetPosition();
        if (!m_bOnGround)
        {
            m_fVelocityY -= GRAVITY * deltaTime;
            deadPos.y += m_fVelocityY * deltaTime;
            if (deadPos.y <= GROUND_Y)
            {
                deadPos.y = GROUND_Y;
                m_fVelocityY = 0.0f;
                m_bOnGround = true;
            }
            pTransform->SetPosition(deadPos);
        }
        return;
    }

    // Apply gravity
    XMFLOAT3 pos = pTransform->GetPosition();

    // Fall zone: safe AABB 바깥 = 낙하 허용, 안쪽 = 수면에 뜸(차오르는 물 따라 상승)
    bool bOutsideSafe = false;
    float effectiveGroundY = GROUND_Y;  // 기본 바닥(타일 Y=0)

    // 포탈 베이스 — XZ 영역 안이면 베이스 표면이 effectiveGroundY. 가장자리에서 안쪽으로 ramp 처리:
    //   가장자리(distXZ=radius)        = 평지 높이 (스냅 X)
    //   안쪽 50%(distXZ ≤ radius*0.5) = 풀 베이스 표면
    //   사이                            = 선형 ramp (경사로 느낌, 텔레포트 X)
    bool bInsideStand = false;
    if (m_fStandRadius > 0.0f)
    {
        float dx = pos.x - m_xmf3StandCenter.x;
        float dz = pos.z - m_xmf3StandCenter.z;
        float distXZ = sqrtf(dx * dx + dz * dz);
        bInsideStand = (distXZ <= m_fStandRadius);
        if (bInsideStand)
        {
            float rampInner = m_fStandRadius * 0.50f;
            float t = (distXZ <= rampInner) ? 1.0f
                    : (m_fStandRadius - distXZ) / (m_fStandRadius - rampInner);
            t = fmaxf(0.0f, fminf(1.0f, t));
            float standSurfaceY = GROUND_Y + t * (m_fIntroGroundY - GROUND_Y);
            effectiveGroundY = fmaxf(effectiveGroundY, standSurfaceY);
            // 표면 아래에서 영역에 들어오면 ramp 높이까지만 안착 (가장자리에선 거의 평지)
            if (pos.y < effectiveGroundY)
            {
                pos.y = effectiveGroundY;
                m_fVelocityY = 0.0f;
                m_bOnGround = true;
            }
        }
        else if (m_bOnGround && pos.y > GROUND_Y + 0.05f)
        {
            // 베이스 표면 위에 있었는데 영역 벗어남 → 자유낙하 시작
            m_bOnGround = false;
        }
    }
    if (m_bFallZoneActive)
    {
        float dx = pos.x - m_xmf3SafeCenter.x;
        float dz = pos.z - m_xmf3SafeCenter.z;
        bOutsideSafe = (fabsf(dx) > m_xmf3SafeExtents.x) || (fabsf(dz) > m_xmf3SafeExtents.z);
        if (bOutsideSafe)
        {
            m_bOnGround = false;  // 물 밖 = 지지 없음
        }
        else
        {
            // 안전존 내부에서는 수면이 타일 위로 올라오면 수면이 새 바닥
            effectiveGroundY = fmaxf(GROUND_Y, m_fFallZoneWaterY);
            if (pos.y < effectiveGroundY)
            {
                // 물이 플레이어 발밑을 넘었으니 수면에 띄움
                pos.y = effectiveGroundY;
                m_fVelocityY = 0.0f;
                m_bOnGround = true;
            }
        }
    }

    if (!m_bOnGround)
    {
        m_fVelocityY -= GRAVITY * deltaTime;
        pos.y += m_fVelocityY * deltaTime;

        if (!bOutsideSafe && pos.y <= effectiveGroundY)
        {
            pos.y = effectiveGroundY;
            m_fVelocityY = 0.0f;
            m_bOnGround = true;
        }
        pTransform->SetPosition(pos);

        // 낙사: 안전존 밖에서 사망 Y 이하 → 즉사
        if (bOutsideSafe && pos.y <= FALL_DEATH_Y && !IsDead())
        {
            TakeDamage(m_fMaxHP);
        }
    }
    else
    {
        // 수면 상승 반영 (이미 bOnGround=true 상태에서도 Y 업데이트)
        pTransform->SetPosition(pos);
    }

    // --- Aim-at-cursor Rotation Logic ---

    // 1. Get mouse position in screen space
    XMFLOAT2 mousePos = pInputSystem->GetMousePosition();

    // 2. Convert to Normalized Device Coordinates (NDC)
    // 런타임 윈도우 크기 사용 (고DPI/해상도 변경 대응)
    float windowWidth = static_cast<float>(Dx12App::GetInstance()->GetWindowWidth());
    float windowHeight = static_cast<float>(Dx12App::GetInstance()->GetWindowHeight());
    float ndcX = (2.0f * mousePos.x / windowWidth) - 1.0f;
    float ndcY = 1.0f - (2.0f * mousePos.y / windowHeight);

    // 3. Unproject from NDC to World Space to form a ray
    XMMATRIX viewMatrix = XMLoadFloat4x4(&pCamera->GetViewMatrix());
    XMMATRIX projMatrix = XMLoadFloat4x4(&pCamera->GetProjectionMatrix());
    XMMATRIX viewProjMatrix = viewMatrix * projMatrix;
    XMMATRIX invViewProjMatrix = XMMatrixInverse(nullptr, viewProjMatrix);

    XMVECTOR rayOrigin = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProjMatrix); // Near plane
    XMVECTOR rayEnd = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProjMatrix);    // Far plane
    XMVECTOR rayDir = XMVector3Normalize(rayEnd - rayOrigin);

    // 4. Define the ground plane at player's floor height
    XMVECTOR groundPlane = XMPlaneFromPointNormal(XMVectorSet(0.0f, GROUND_Y, 0.0f, 0.0f), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

    // 5. Find intersection of the ray and the ground plane
	// XMPlaneIntersectLine requires two points on the line, not a point and a direction.
    XMVECTOR intersectionPoint = XMPlaneIntersectLine(groundPlane, rayOrigin, rayOrigin + rayDir * 1000.0f);


    // 6. Make the player look at the intersection point
    XMVECTOR playerPos = XMLoadFloat3(&pTransform->GetPosition());
    XMVECTOR lookDir = intersectionPoint - playerPos;
    
    // Set Y-component of look direction to 0 to prevent character from tilting up/down
    lookDir = XMVectorSetY(lookDir, 0.0f); 
    lookDir = XMVector3Normalize(lookDir);

	// Check if the look direction is valid before setting it, to prevent generating NaNs
    bool bRotationChanged = false;
	if (XMVectorGetX(XMVector3LengthSq(lookDir)) > 0.001f)
	{
		// Convert look direction to a yaw angle
        float yawRad = atan2f(XMVectorGetX(lookDir), XMVectorGetZ(lookDir));
        float yawDeg = XMConvertToDegrees(yawRad);

        // 회전 변경 감지 (임계값 이상 변화 시)
        float yawDiff = fabsf(yawDeg - m_fPrevYaw);
        // 360도 경계 처리 (예: 359 -> 1도 변화는 2도로 처리)
        if (yawDiff > 180.0f) yawDiff = 360.0f - yawDiff;
        if (yawDiff >= YAW_SYNC_THRESHOLD)
        {
            bRotationChanged = true;
            m_fPrevYaw = yawDeg;
        }

        // Get current rotation, only overwrite yaw
        const XMFLOAT3& currentRot = pTransform->GetRotation();
        pTransform->SetRotation(currentRot.x, yawDeg, currentRot.z);
	}


    // --- Movement Logic (Camera-Relative) ---

    float moveSpeed = m_fMoveSpeed;

    // Get camera's axes and flatten them to the XZ plane (ground)
    // This makes WASD movement relative to the screen/camera view.
    XMVECTOR camLook = pCamera->GetLookDirection();
    XMVECTOR camRight = pCamera->GetRightDirection();

    camLook = XMVectorSetY(camLook, 0.0f);
    camRight = XMVectorSetY(camRight, 0.0f);

    camLook = XMVector3Normalize(camLook);
    camRight = XMVector3Normalize(camRight);

    XMVECTOR currentPosition = XMLoadFloat3(&pTransform->GetPosition());
    XMVECTOR displacement = XMVectorZero();

    // Keyboard input for movement (Camera-relative)
    XMVECTOR moveDir = XMVectorZero();
    if (pInputSystem->IsKeyDown('W')) moveDir += camLook;
    if (pInputSystem->IsKeyDown('S')) moveDir -= camLook;
    if (pInputSystem->IsKeyDown('A')) moveDir -= camRight;
    if (pInputSystem->IsKeyDown('D')) moveDir += camRight;

    bool bMoving = XMVectorGetX(XMVector3LengthSq(moveDir)) > 0.001f;

    // Normalize movement direction to keep speed consistent (even diagonally)
    if (bMoving)
    {
        moveDir = XMVector3Normalize(moveDir);
    }

    // --- Dash (Space) ---
    //   - Space pressed & cooldown=0 & 살아있음 → 대쉬 시작
    //   - 대쉬 중: 방향 고정, 속도*kDashSpeedMult, 피격 무시, Levitate 애니
    //   - 시작 시 이미시브 플래시(푸른빛) → 대쉬 끝날 때 복원
    if (m_fDashCooldownRemain > 0.0f)
        m_fDashCooldownRemain = fmaxf(0.0f, m_fDashCooldownRemain - deltaTime);

    // 대쉬 트레일 — LightEmitterSystem(Sphere) 단발 Burst를 주기적 스폰.
    //   대시 시작 시 큰 Burst 1회, 진행 중 짧은 주기로 작은 Sphere를 추가 스폰.
    auto SpawnDashBurst = [&](const XMFLOAT3& pos, int particleCount, float radius)
    {
        Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
        VFXManager* pVFX = pScene ? pScene->GetVFXManager() : nullptr;
        if (!pVFX) return;

        EffectLayer layer;
        layer.type          = EmitterType::Sphere;
        layer.particleCount = particleCount;
        layer.coreColor     = m_dashCoreColor;
        layer.edgeColor     = m_dashEdgeColor;
        layer.sizeScale     = 0.55f;
        layer.speedMin      = 1.5f;
        layer.speedMax      = 4.0f;
        layer.lifetimeMin   = 0.18f;
        layer.lifetimeMax   = 0.40f;
        layer.sphere.radius        = radius;
        layer.sphere.shellFraction = 0.f;     // 전체 채움
        layer.sphere.inward        = false;
        layer.sphere.rotationSpeed = 0.f;
        pVFX->SpawnLightLayer(pos, XMFLOAT3(0, 1, 0), layer, /*isPlayer*/true);
    };

    bool bDashStarted = false;
    if (!IsDead()
        && pInputSystem->IsKeyPressed(VK_SPACE)
        && m_fDashTimer <= 0.0f
        && m_fDashCooldownRemain <= 0.0f)
    {
        // 대쉬 방향: WASD 입력 있으면 그 방향, 없으면 캐릭터 정면
        XMVECTOR dashVec = bMoving ? moveDir : pTransform->GetLook();
        dashVec = XMVectorSetY(dashVec, 0.0f);
        if (XMVectorGetX(XMVector3LengthSq(dashVec)) > 0.001f)
        {
            dashVec = XMVector3Normalize(dashVec);
            XMStoreFloat3(&m_xmf3DashDir, dashVec);
            m_fDashTimer = m_fDashDuration;
            bDashStarted = true;

            // 시작 시 폭발적 Sphere Burst (즉시 18개 팡)
            {
                XMFLOAT3 startPos = pTransform->GetPosition();
                startPos.y += 2.0f;
                SpawnDashBurst(startPos, 36, 1.6f);
            }
            m_fDashTrailAccum = 0.0f;

            // 네트워크 연출 액션 — 대쉬 망토 펄럭임 시작 알림
            if (NetworkManager* pNetMgr = NetworkManager::GetInstance())
            {
                if (pNetMgr->IsConnected())
                {
                    XMFLOAT3 actionPos = pTransform->GetPosition();

                    pNetMgr->SendPlayerAction(
                        PLAYER_ACTION_DASH_CAPE_FLUTTER,
                        actionPos.x, actionPos.y, actionPos.z,
                        m_xmf3DashDir.x, m_xmf3DashDir.y, m_xmf3DashDir.z);
                }
            }
        }
    }

    bool bDashing = (m_fDashTimer > 0.0f);
    if (bDashing)
    {
        // 대쉬 진행: 방향 고정 + 부스트 속도
        m_fDashTimer -= deltaTime;
        XMVECTOR dashVec = XMLoadFloat3(&m_xmf3DashDir);
        displacement = dashVec * (moveSpeed * m_fDashSpeedMult) * deltaTime;

        // HitFlash 림 아웃라인으로 "블러/스피드" 연출 — 시작 즉시 풀 강도, 끝 직전에만 페이드
        //   t: 0 시작 → 1 끝. 80% 구간 1.0 유지, 마지막 20% easeOut
        float t = 1.0f - fmaxf(0.0f, m_fDashTimer) / m_fDashDuration;
        float flash = (t < 0.8f) ? 1.0f : (1.0f - (t - 0.8f) / 0.2f);
        m_pOwner->SetHitFlashAll(flash);

        // 진행 중 주기적 트레일 Burst — 0.04s 마다 작은 Sphere 1회 (≒ 25Hz)
        m_fDashTrailAccum += deltaTime;
        constexpr float kDashTrailInterval = 0.04f;
        while (m_fDashTrailAccum >= kDashTrailInterval)
        {
            m_fDashTrailAccum -= kDashTrailInterval;
            XMFLOAT3 p = pTransform->GetPosition();
            p.y += 2.0f;
            SpawnDashBurst(p, 6, 1.6f);
        }

        // 대쉬 끝난 프레임: 쿨다운 시작 + 잔상 타이머 시작
        if (m_fDashTimer <= 0.0f)
        {
            m_fDashTimer = 0.0f;
            m_fDashCooldownRemain = m_fDashCooldown;
            m_fDashFlashTail = kDashFlashTail;
            m_fDashTrailAccum = 0.0f;
        }
    }
    else if (m_fDashFlashTail > 0.0f)
    {
        // 대쉬 끝난 후 잔상 페이드 (0.15s 추가)
        m_fDashFlashTail = fmaxf(0.0f, m_fDashFlashTail - deltaTime);
        float tailFlash = m_fDashFlashTail / kDashFlashTail;
        m_pOwner->SetHitFlashAll(tailFlash * 0.5f);
        if (m_fDashFlashTail <= 0.0f) m_pOwner->SetHitFlashAll(0.0f);
    }
    else if (bMoving)
    {
        displacement = moveDir * moveSpeed * deltaTime;
    }

    // 스킬 대쉬 활성 중이면 displacement 를 스킬 방향으로 덮어씀
    if (m_fSkillDashTimer > 0.f)
    {
        m_fSkillDashTimer = fmaxf(0.f, m_fSkillDashTimer - deltaTime);
        XMVECTOR sDir = XMLoadFloat3(&m_xmf3SkillDashDir);
        displacement = sDir * m_fSkillDashSpeed * deltaTime;
    }

    // Apply displacement (keep current Y from gravity system)
    currentPosition += displacement;
    float currentY = pTransform->GetPosition().y;
    pTransform->SetPosition(XMFLOAT3(XMVectorGetX(currentPosition), currentY, XMVectorGetZ(currentPosition)));

    // --- 네트워크 동기화: 이동 또는 회전 변경 시 전송 ---
    bool bHasDisplacement = XMVectorGetX(XMVector3LengthSq(displacement)) > 0.000001f;

    if (bMoving || bRotationChanged || bDashStarted || bDashing || bHasDisplacement)
    {
        NetworkManager* pNetMgr = NetworkManager::GetInstance();
        if (pNetMgr && pNetMgr->IsConnected())
        {
            const XMFLOAT3& finalPos = pTransform->GetPosition();
            XMVECTOR lookVec = pTransform->GetLook();
            XMFLOAT3 lookDir3;
            XMStoreFloat3(&lookDir3, lookVec);

            pNetMgr->SendMove(finalPos.x, finalPos.y, finalPos.z, lookDir3.x, lookDir3.y, lookDir3.z);
        }
    }

    // --- Skill Input Processing ---
    bool bAttackTriggered = pInputSystem->IsKeyPressed('Q')
                         || pInputSystem->IsKeyPressed('E')
                         || pInputSystem->IsKeyPressed('R')
                         || pInputSystem->IsMouseButtonPressed(1);

    SkillComponent* pSkillComponent = m_pOwner->GetComponent<SkillComponent>();
    if (pSkillComponent)
    {
        pSkillComponent->ProcessSkillInput(pInputSystem, pCamera);
    }

    UpdateAnimation(deltaTime, bMoving, bAttackTriggered, bDashStarted, bDashing);
}

void PlayerComponent::StartIntroFly(float duration, float groundY,
                                    const XMFLOAT3& standCenter, float standRadius)
{
    m_fIntroFlyTimer    = duration;
    m_fLandingHoldTimer = 0.0f;
    m_fIntroGroundY     = groundY;
    m_xmf3StandCenter   = standCenter;
    m_fStandRadius      = standRadius;
    m_fVelocityY        = 0.0f;
    m_bOnGround         = false;
    m_eAnimState        = PlayerAnimState::IntroFall;
    if (auto* pAnim = m_pOwner ? m_pOwner->GetComponent<AnimationComponent>() : nullptr)
        pAnim->CrossFade("Levitating", 0.10f, true, true);
    OutputDebugString(L"[Player] Intro Fly START\n");
}

void PlayerComponent::EnterTornadoTrap(const XMFLOAT3& tornadoCenter)
{
    if (!m_pOwner || !m_pOwner->GetTransform()) return;
    m_bTornadoTrapped   = true;
    m_xmf3TornadoCenter = tornadoCenter;
    m_fTornadoTime      = 0.0f;
    m_fTornadoStartY    = m_pOwner->GetTransform()->GetPosition().y;
    m_fVelocityY        = 0.0f;
    m_bOnGround         = false;
    OutputDebugString(L"[Player] Tornado Trap ENTER\n");
}

void PlayerComponent::ExitTornadoTrap()
{
    if (!m_bTornadoTrapped) return;
    m_bTornadoTrapped = false;
    m_fVelocityY      = 0.0f;
    m_bOnGround       = false;   // 낙하 시작
    OutputDebugString(L"[Player] Tornado Trap EXIT (descend)\n");
}

void PlayerComponent::UpdateTornadoTrap(float dt)
{
    auto* pT = m_pOwner ? m_pOwner->GetTransform() : nullptr;
    if (!pT) return;

    m_fTornadoTime += dt;

    const float orbitRadius = 2.5f;
    const float orbitSpeed  = 5.5f;     // rad/s, ~0.87 rev/s
    const float riseSpeed   = 5.5f;     // units/s
    const float maxLiftY    = 12.0f;    // 컬럼 높이만큼

    float angle = m_fTornadoTime * orbitSpeed;
    float x     = m_xmf3TornadoCenter.x + cosf(angle) * orbitRadius;
    float z     = m_xmf3TornadoCenter.z + sinf(angle) * orbitRadius;
    float y     = m_fTornadoStartY + m_fTornadoTime * riseSpeed;
    if (y > m_fTornadoStartY + maxLiftY) y = m_fTornadoStartY + maxLiftY;

    pT->SetPosition(x, y, z);

    // 시각 회전 — 접선 방향으로 정렬 (캐릭터가 회오리 따라 도는 모션)
    float tangentYawDeg = XMConvertToDegrees(angle) + 90.0f;
    pT->SetRotation(0.0f, tangentYawDeg, 0.0f);
}

void PlayerComponent::EnterFlightMode(GameObject* pFlightCenter)
{
    if (!pFlightCenter || !m_pOwner || !m_pOwner->GetTransform()) return;
    m_bFlightMode = true;
    m_pFlightCenter = pFlightCenter;

    // 오프셋 0 으로 시작 (보스 정 뒤에서 출발)
    m_fFlightOffsetX = 0.0f;
    m_fFlightOffsetY = 0.0f;
    m_fFlightOffsetXVel = 0.0f;
    m_fFlightOffsetYVel = 0.0f;

    // 중력/지면 상태 리셋
    m_fVelocityY = 0.0f;
    m_bOnGround  = false;
    OutputDebugString(L"[Player] Flight Mode ENTER (rail)\n");
}

void PlayerComponent::ExitFlightMode()
{
    if (!m_bFlightMode) return;
    m_bFlightMode = false;
    m_pFlightCenter = nullptr;
    m_fVelocityY = 0.0f;
    m_bOnGround  = false;
    OutputDebugString(L"[Player] Flight Mode EXIT\n");
}

void PlayerComponent::UpdateFlightMode(float deltaTime, InputSystem* pInputSystem, CCamera* pCamera)
{
    if (!m_pFlightCenter || !m_pFlightCenter->GetTransform()) { ExitFlightMode(); return; }
    TransformComponent* pT = m_pOwner->GetTransform();
    if (!pT) return;

    // 입력 (D=오른쪽, A=왼쪽, W=위, S=아래)
    float xInput = 0.0f, yInput = 0.0f;
    bool bBoost = pInputSystem && pInputSystem->IsKeyDown(VK_SHIFT);
    if (pInputSystem)
    {
        if (pInputSystem->IsKeyDown('D')) xInput += 1.0f;
        if (pInputSystem->IsKeyDown('A')) xInput -= 1.0f;
        if (pInputSystem->IsKeyDown('W')) yInput += 1.0f;
        if (pInputSystem->IsKeyDown('S')) yInput -= 1.0f;
    }

    // 가속 + 항력 (선형 속도 적분)
    float accelMult = bBoost ? kFlightBoostMult : 1.0f;
    float maxSpd    = kFlightMaxSpeed * accelMult;
    m_fFlightOffsetXVel += xInput * kFlightAccel * accelMult * deltaTime;
    m_fFlightOffsetYVel += yInput * kFlightAccel * accelMult * deltaTime;
    float drag = expf(-kFlightDrag * deltaTime);
    m_fFlightOffsetXVel *= drag;
    m_fFlightOffsetYVel *= drag;
    m_fFlightOffsetXVel = max(-maxSpd, min(maxSpd, m_fFlightOffsetXVel));
    m_fFlightOffsetYVel = max(-maxSpd, min(maxSpd, m_fFlightOffsetYVel));

    // 적분 + 클램프 (경계 도달 시 속도 리셋)
    m_fFlightOffsetX += m_fFlightOffsetXVel * deltaTime;
    m_fFlightOffsetY += m_fFlightOffsetYVel * deltaTime;
    if (m_fFlightOffsetX >  kFlightOffsetXMax) { m_fFlightOffsetX =  kFlightOffsetXMax; m_fFlightOffsetXVel = 0.0f; }
    if (m_fFlightOffsetX < -kFlightOffsetXMax) { m_fFlightOffsetX = -kFlightOffsetXMax; m_fFlightOffsetXVel = 0.0f; }
    if (m_fFlightOffsetY >  kFlightOffsetYMax) { m_fFlightOffsetY =  kFlightOffsetYMax; m_fFlightOffsetYVel = 0.0f; }
    if (m_fFlightOffsetY <  kFlightOffsetYMin) { m_fFlightOffsetY =  kFlightOffsetYMin; m_fFlightOffsetYVel = 0.0f; }

    // 보스 forward / right 벡터 (보스 GameObject 의 회전 사용)
    // 보스 mesh가 motion 방향과 +180 회전 보정되어 있어 GetLook이 motion의 반대 방향임 → 부호 반전
    XMFLOAT3 bp = m_pFlightCenter->GetTransform()->GetPosition();
    XMVECTOR bossForward = m_pFlightCenter->GetTransform()->GetLook();
    bossForward = XMVectorScale(bossForward, -1.0f);
    bossForward = XMVectorSetY(bossForward, 0.0f);
    if (XMVectorGetX(XMVector3LengthSq(bossForward)) < 0.001f)
        bossForward = XMVectorSet(0, 0, 1, 0);
    bossForward = XMVector3Normalize(bossForward);
    XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    XMVECTOR bossRight = XMVector3Normalize(XMVector3Cross(worldUp, bossForward));

    // 플레이어 = 보스 - forward*trail + right*offsetX + up*offsetY
    XMVECTOR bossPos = XMLoadFloat3(&bp);
    XMVECTOR playerPos = bossPos
                       - bossForward * kFlightTrailDist
                       + bossRight   * m_fFlightOffsetX
                       + worldUp     * m_fFlightOffsetY;
    XMFLOAT3 newPos;
    XMStoreFloat3(&newPos, playerPos);
    pT->SetPosition(newPos);

    // 회전: 보스와 같은 방향(정면)을 바라봄
    float facingYaw = XMConvertToDegrees(atan2f(XMVectorGetX(bossForward), XMVectorGetZ(bossForward)));
    const XMFLOAT3& curRot = pT->GetRotation();
    pT->SetRotation(curRot.x, facingYaw, curRot.z);

    // 좌클릭 사격 (히트스캔)
    if (pInputSystem && pCamera && pInputSystem->IsMouseButtonPressed(0))
    {
        Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr;
        if (pScene)
        {
            XMFLOAT3 muzzlePos = newPos;
            muzzlePos.y += 1.6f;
            XMVECTOR camLook = pCamera->GetLookDirection();
            XMFLOAT3 dirF; XMStoreFloat3(&dirF, XMVector3Normalize(camLook));
            pScene->FlightShoot(muzzlePos, dirF);
        }
    }
}

void PlayerComponent::StartSkillDash(const XMFLOAT3& dir, float speed, float duration)
{
    m_xmf3SkillDashDir = dir;
    m_fSkillDashSpeed  = speed;
    m_fSkillDashTimer  = duration;
}

void PlayerComponent::EnableFallZone(const XMFLOAT3& safeCenter, const XMFLOAT3& safeExtents)
{
    m_bFallZoneActive = true;
    m_xmf3SafeCenter  = safeCenter;
    m_xmf3SafeExtents = safeExtents;
}

void PlayerComponent::TakeDamage(float fDamage)
{
    if (fDamage <= 0.0f || IsDead()) return;
    if (IsDashing()) return;     // 대쉬 중 i-frame
    if (IsInvincible()) return;  // 무적 중 피격 무시

    // 보호막이 있으면 먼저 흡수
    if (m_fShield > 0.f)
    {
        float absorbed = min(m_fShield, fDamage);
        m_fShield  -= absorbed;
        fDamage    -= absorbed;
        if (fDamage <= 0.f) return;
    }

    // 피해감소 적용 (대지의 갑옷 E스킬)
    if (m_fDamageReductionTimer > 0.f && fDamage > 0.f)
        fDamage *= (1.f - m_fDamageReductionRatio);

    m_fCurrentHP -= fDamage;
    if (m_fCurrentHP < 0.0f)
    {
        m_fCurrentHP = 0.0f;
    }

    // 피격 연출 — 네트워크 모드 ProcessPlayerDamage 와 동일한 플래시/데미지 넘버/카메라 셰이크.
    // 오프라인에선 이 세 가지가 누락되어 있어 체감상 "맞는지 모르겠음".
    TriggerHitFlash();

    // 보복 룬 (ABY_RVG): 피격 시 보복 상태 활성화 — 멀티는 서버 권위
    {
        NetworkManager* pNetMgr = NetworkManager::GetInstance();
        if (!(pNetMgr && pNetMgr->IsConnected()))
            TriggerVengeance(10.f);
    }

    if (m_pOwner && m_pOwner->GetTransform())
    {
        DirectX::XMFLOAT3 pos = m_pOwner->GetTransform()->GetPosition();
        pos.y += 3.0f;
        DamageNumberManager::Get().AddNumber(pos, fDamage);
    }

    if (Scene* pScene = Dx12App::GetInstance() ? Dx12App::GetInstance()->GetScene() : nullptr)
    {
        if (CCamera* pCam = pScene->GetCamera())
            pCam->StartShake(3.0f, 0.35f);  // 체감상 보이도록 세기·지속 up
    }
}

void PlayerComponent::AddShield(float amount)
{
    if (amount <= 0.f) return;
    m_fShield = min(m_fShield + amount, MAX_SHIELD);
}

void PlayerComponent::SetShield(float amount)
{
    if (amount < 0.f) amount = 0.f;
    if (amount > MAX_SHIELD) amount = MAX_SHIELD;
    m_fShield = amount;
}

void PlayerComponent::SetDamageReduction(float ratio, float duration)
{
    m_fDamageReductionRatio = max(0.f, min(ratio, 1.f));
    m_fDamageReductionTimer = max(0.f, duration);
}

void PlayerComponent::SetInvincible(float duration)
{
    m_fInvincibleTimer = max(m_fInvincibleTimer, duration);  // 갱신 시 더 긴 쪽 유지
}

void PlayerComponent::SetCurrentHP(float fHP)
{
    m_fCurrentHP = (fHP < 0.0f) ? 0.0f : (fHP > m_fMaxHP ? m_fMaxHP : fHP);
}

void PlayerComponent::TriggerHitFlash()
{
    m_fHitFlashTimer = kHitFlashDuration;
}

void PlayerComponent::TriggerVengeance(float duration)
{
    m_bVengeancePrimed = true;
    m_fVengeanceTimer  = duration;
}

bool PlayerComponent::ConsumeVengeance()
{
    if (!m_bVengeancePrimed) return false;
    m_bVengeancePrimed = false;
    m_fVengeanceTimer  = 0.f;
    return true;
}

void PlayerComponent::OnServerDeath()
{
    if (m_bNetworkDead) return;
    m_bNetworkDead = true;
    m_fCurrentHP = 0.0f;

    // 데스 애니메이션 — 클립 이름은 MageBlue_Anim.bin 목록 기준("Death1"/"Death2" 존재)
    if (AnimationComponent* pAnim = m_pOwner->GetComponent<AnimationComponent>())
    {
        pAnim->CrossFade("Death1", 0.15f, false, true);
    }
    OutputDebugString(L"[Player] OnServerDeath\n");
}

void PlayerComponent::Heal(float fAmount)
{
    if (fAmount <= 0.0f || IsDead()) return;

    m_fCurrentHP += fAmount;
    if (m_fCurrentHP > m_fMaxHP)
    {
        m_fCurrentHP = m_fMaxHP;
    }
}

void PlayerComponent::UpdateAnimation(float deltaTime, bool bMoving, bool bAttackTriggered, bool bDashStarted, bool bDashing)
{
    AnimationComponent* pAnim = m_pOwner->GetComponent<AnimationComponent>();
    if (!pAnim) return;

    // Tick down attack timer
    if (m_fAttackTimer > 0.0f)
        m_fAttackTimer -= deltaTime;

    // 대쉬 시작: LevitateStart 1회 재생. 대쉬 중엔 상태 유지 (중간에 공격/이동 애니로 튀지 않게)
    if (bDashStarted)
    {
        m_eAnimState = PlayerAnimState::Dash;
        pAnim->CrossFade("Run", 0.05f, true, true);
        return;
    }
    if (bDashing)
    {
        m_eAnimState = PlayerAnimState::Dash;
        return;  // 대쉬 중엔 다른 애니로 전환 안 함
    }

    // If attack triggered, always restart attack animation
    if (bAttackTriggered)
    {
        m_fAttackTimer = kAttackAnimDuration;
        m_eAnimState = PlayerAnimState::Attack;
        pAnim->CrossFade("Attack1", 0.1f, false, true);  // forceRestart = true
        return;
    }

    // Determine desired state (Attack > Walk > Idle)
    PlayerAnimState desiredState;
    if (m_fAttackTimer > 0.0f)
    {
        desiredState = PlayerAnimState::Attack;
    }
    else if (bMoving)
    {
        desiredState = PlayerAnimState::Walk;
    }
    else
    {
        desiredState = PlayerAnimState::Idle;
    }

    if (desiredState == m_eAnimState) return;
    m_eAnimState = desiredState;

    switch (m_eAnimState)
    {
    case PlayerAnimState::Idle:
        pAnim->CrossFade("Idle", 0.2f, true);
        break;
    case PlayerAnimState::Walk:
        pAnim->CrossFade("Walk", 0.15f, true);
        break;
    case PlayerAnimState::Attack:
        pAnim->CrossFade("Attack1", 0.1f, false);
        break;
    case PlayerAnimState::Dash:
        // 대쉬는 bDashStarted 분기에서 이미 처리됨
        break;
    }
}
