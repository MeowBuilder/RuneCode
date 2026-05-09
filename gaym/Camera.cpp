#include "stdafx.h"
#include "Camera.h"
#include "GameObject.h"
#include "TransformComponent.h"

using namespace DirectX;

CCamera::CCamera()
{
    // Initialize matrices
    XMStoreFloat4x4(&m_viewMatrix, XMMatrixIdentity());
    XMStoreFloat4x4(&m_projectionMatrix, XMMatrixIdentity());

    // Initialize position
    m_position = XMFLOAT3(0.0f, 0.0f, 0.0f);
}

CCamera::~CCamera()
{
}

void CCamera::SetLens(float fovY, float aspect, float zn, float zf)
{
    m_fBaseFovDeg = XMConvertToDegrees(fovY);
    m_fAspect = aspect;
    m_fNearZ  = zn;
    m_fFarZ   = zf;
    XMMATRIX P = XMMatrixPerspectiveFovLH(fovY, aspect, zn, zf);
    XMStoreFloat4x4(&m_projectionMatrix, P);
}

void CCamera::SetFovDegrees(float fovDeg)
{
    fovDeg = max(20.0f, min(120.0f, fovDeg));
    XMMATRIX P = XMMatrixPerspectiveFovLH(XMConvertToRadians(fovDeg), m_fAspect, m_fNearZ, m_fFarZ);
    XMStoreFloat4x4(&m_projectionMatrix, P);
}

void CCamera::ToggleFreeCam()
{
    m_bFreeCam = !m_bFreeCam;
    if (m_bFreeCam)
    {
        // FreeCam 진입 시 현재 카메라 위치 및 방향 이어받기
        m_freeCamPos = m_position;
        m_freeYaw   = m_yaw;
        m_freePitch = -m_pitch; // orbit pitch는 위를 양수로, freecam은 아래를 양수로
        OutputDebugString(L"[Camera] FreeCam ON (F2 to toggle)\n");
    }
    else
    {
        OutputDebugString(L"[Camera] FreeCam OFF\n");
    }
}

void CCamera::SetFlightMode(bool bEnabled, GameObject* pFlightCenter)
{
    m_bFlightMode = bEnabled;
    m_pFlightCenter = bEnabled ? pFlightCenter : nullptr;
    m_fFlightAimYaw = 0.0f;
    m_fFlightAimPitch = 0.0f;
    OutputDebugString(bEnabled ? L"[Camera] FlightMode ON\n" : L"[Camera] FlightMode OFF\n");
}

void CCamera::Update(float mouseDeltaX, float mouseDeltaY, float scrollDelta, float deltaTime,
                     bool bForward, bool bBackward, bool bLeft, bool bRight, bool bUp, bool bDown)
{
    // === Flight Mode (보스 중심 3인칭 비행 카메라) ===
    if (m_bFlightMode && m_pTarget)
    {
        // 마우스 = 조준점 누적 보정 (자동 복귀 살짝)
        m_fFlightAimYaw   += mouseDeltaX * m_fFlightAimSpeed;
        m_fFlightAimPitch += mouseDeltaY * m_fFlightAimSpeed;
        m_fFlightAimYaw   = max(-45.0f, min(45.0f, m_fFlightAimYaw));
        m_fFlightAimPitch = max(-30.0f, min(30.0f, m_fFlightAimPitch));
        // 자동 복귀 (감쇠)
        if (deltaTime > 0.0f)
        {
            float decay = expf(-2.0f * deltaTime);
            m_fFlightAimYaw   *= decay;
            m_fFlightAimPitch *= decay;
        }

        TransformComponent* pPlayerT = m_pTarget->GetTransform();
        XMVECTOR playerPos = XMLoadFloat3(&pPlayerT->GetPosition());
        XMVECTOR bossPos   = m_pFlightCenter
            ? XMLoadFloat3(&m_pFlightCenter->GetTransform()->GetPosition())
            : playerPos + XMVectorSet(0, 0, 30, 0);

        // "비행 forward" = 보스의 진행 방향
        // 보스 mesh가 motion과 +180 회전 보정되어 있어 GetLook이 motion 반대 방향 → 부호 반전
        XMVECTOR flightForward = m_pFlightCenter
            ? XMVectorScale(m_pFlightCenter->GetTransform()->GetLook(), -1.0f)
            : XMVectorSet(0, 0, 1, 0);
        flightForward = XMVectorSetY(flightForward, 0.0f);
        if (XMVectorGetX(XMVector3LengthSq(flightForward)) < 0.001f)
            flightForward = XMVectorSet(0, 0, 1, 0);
        flightForward = XMVector3Normalize(flightForward);

        // 마우스 보정용 회전 (yaw 만 forward 회전, pitch 는 height 보정으로)
        float aimYawRad = XMConvertToRadians(m_fFlightAimYaw);
        XMMATRIX yawRot = XMMatrixRotationY(aimYawRad);
        XMVECTOR adjustedForward = XMVector3TransformNormal(flightForward, yawRot);

        // 카메라 = 플레이어 - forward*back + up*height(+pitch 보정)
        XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        float pitchOffset = -m_fFlightAimPitch * 0.08f;
        XMVECTOR camPos = playerPos
            - adjustedForward * m_fFlightCamBack
            + worldUp * (m_fFlightCamHeight + pitchOffset);

        // LookAt = 플레이어보다 약간 앞쪽 (forward * lookAhead) + 보스 쪽 약간 보정
        // 이러면 카메라가 항상 진행 방향을 바라보고, 보스도 화면 중앙에 잡힘
        const float kLookAhead = 30.0f;
        XMVECTOR forwardLook = playerPos + flightForward * kLookAhead;
        XMVECTOR lookAtPoint = forwardLook + (bossPos - forwardLook) * m_fFlightLookBias
                             + worldUp * 1.5f;

        // 카메라 셰이크 적용
        if (m_bShaking && deltaTime > 0.0f)
        {
            m_fShakeTimer += deltaTime;
            if (m_fShakeTimer >= m_fShakeDuration) StopShake();
            else
            {
                float fRem = 1.0f - (m_fShakeTimer / m_fShakeDuration);
                float fInt = m_fShakeIntensity * fRem;
                m_shakeOffset.x = ((float)(rand() % 1000) / 500.0f - 1.0f) * fInt;
                m_shakeOffset.y = ((float)(rand() % 1000) / 500.0f - 1.0f) * fInt;
                m_shakeOffset.z = ((float)(rand() % 1000) / 500.0f - 1.0f) * fInt * 0.5f;
            }
            camPos = camPos + XMLoadFloat3(&m_shakeOffset);
        }

        XMStoreFloat3(&m_position, camPos);
        XMMATRIX view = XMMatrixLookAtLH(camPos, lookAtPoint, worldUp);
        XMStoreFloat4x4(&m_viewMatrix, view);
        return;
    }

    // === Free Camera Mode ===
    if (m_bFreeCam)
    {
        // Mouse rotation
        m_freeYaw   += mouseDeltaX * m_freeRotSpeed;
        m_freePitch += mouseDeltaY * m_freeRotSpeed;
        m_freePitch  = max(-89.0f, min(89.0f, m_freePitch));

        // Build look/right/up from yaw+pitch
        float yawRad   = XMConvertToRadians(m_freeYaw);
        float pitchRad = XMConvertToRadians(m_freePitch);

        XMVECTOR look = XMVector3Normalize(XMVectorSet(
            cosf(pitchRad) * sinf(yawRad),
           -sinf(pitchRad),
            cosf(pitchRad) * cosf(yawRad),
            0.0f));
        XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        XMVECTOR right   = XMVector3Normalize(XMVector3Cross(worldUp, look));
        XMVECTOR up      = XMVector3Cross(look, right);

        // WASD + QE movement
        float speed = m_freeMoveSpeed * deltaTime;
        XMVECTOR pos = XMLoadFloat3(&m_freeCamPos);
        if (bForward)  pos = pos + look  * speed;
        if (bBackward) pos = pos - look  * speed;
        if (bRight)    pos = pos + right * speed;
        if (bLeft)     pos = pos - right * speed;
        if (bUp)       pos = pos + worldUp * speed;
        if (bDown)     pos = pos - worldUp * speed;
        XMStoreFloat3(&m_freeCamPos, pos);
        XMStoreFloat3(&m_position, pos);

        // Build view matrix
        XMVECTOR target = pos + look;
        XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
        XMStoreFloat4x4(&m_viewMatrix, view);
        return;
    }

    // === Orbit Camera Mode (original) ===
    // Update yaw and pitch from mouse input
    //m_yaw += mouseDeltaX * m_rotationSpeed;
    //m_pitch += mouseDeltaY * m_rotationSpeed;

    // Clamp pitch to avoid flipping
    m_pitch = max(-89.0f, min(89.0f, m_pitch));

    // Update distance from scroll wheel input
    m_distance -= scrollDelta * m_zoomSpeed;
    m_distance = max(m_minDistance, min(m_maxDistance, m_distance));

    // 임시 orbit 거리 부스트 lerp (보스 텔레그래프 중 pull-back 등)
    if (deltaTime > 0.0f)
    {
        m_fExtraOrbitDist += (m_fExtraOrbitDistTarget - m_fExtraOrbitDist) * 4.0f * deltaTime;
    }

    // Update camera shake
    if (m_bShaking && deltaTime > 0.0f)
    {
        m_fShakeTimer += deltaTime;
        if (m_fShakeTimer >= m_fShakeDuration)
        {
            StopShake();
        }
        else
        {
            // Shake intensity decreases over time
            float fRemainingRatio = 1.0f - (m_fShakeTimer / m_fShakeDuration);
            float fCurrentIntensity = m_fShakeIntensity * fRemainingRatio;

            // Random offset
            m_shakeOffset.x = ((float)(rand() % 1000) / 500.0f - 1.0f) * fCurrentIntensity;
            m_shakeOffset.y = ((float)(rand() % 1000) / 500.0f - 1.0f) * fCurrentIntensity;
            m_shakeOffset.z = ((float)(rand() % 1000) / 500.0f - 1.0f) * fCurrentIntensity * 0.5f;
        }
    }

    // Recalculate the view matrix
    UpdateViewMatrix();
}

void CCamera::StartShake(float fIntensity, float fDuration)
{
    m_bShaking = true;
    m_fShakeIntensity = fIntensity;
    m_fShakeDuration = fDuration;
    m_fShakeTimer = 0.0f;
}

void CCamera::StopShake()
{
    m_bShaking = false;
    m_fShakeTimer = 0.0f;
    m_shakeOffset = { 0.0f, 0.0f, 0.0f };
}

void CCamera::UpdateViewMatrix()
{
    XMVECTOR lookAtPoint;
    float yawRad, pitchRad, dist;

    if (m_bCinematic)
    {
        // Orbit around fixed world-space point
        lookAtPoint = XMLoadFloat3(&m_cinLookAt);
        yawRad   = XMConvertToRadians(m_fCinYaw);
        pitchRad = XMConvertToRadians(m_fCinPitch);
        dist     = m_fCinDist;
    }
    else
    {
        if (!m_pTarget) return;
        yawRad   = XMConvertToRadians(m_yaw);
        pitchRad = XMConvertToRadians(m_pitch);
        dist     = m_distance + m_fExtraOrbitDist;   // 임시 pull-back 적용

        TransformComponent* pTransform = m_pTarget->GetTransform();
        XMVECTOR targetPos = XMLoadFloat3(&pTransform->GetPosition());
        lookAtPoint = targetPos + XMLoadFloat3(&m_lookAtOffset);
    }

    // Calculate the camera's position based on orbit parameters
    float x = dist * cosf(pitchRad) * sinf(yawRad);
    float y = dist * sinf(pitchRad);
    float z = dist * cosf(pitchRad) * cosf(yawRad);

    XMVECTOR cameraPos = lookAtPoint + XMVectorSet(x, y, z, 0.0f);

    // Apply shake offset
    if (m_bShaking)
    {
        cameraPos = cameraPos + XMLoadFloat3(&m_shakeOffset);
    }

    XMStoreFloat3(&m_position, cameraPos);

    // Build the view matrix
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX view = XMMatrixLookAtLH(cameraPos, lookAtPoint, up);
    XMStoreFloat4x4(&m_viewMatrix, view);
}

void CCamera::StartCinematic(const XMFLOAT3& lookAt, float distance, float pitch, float yaw)
{
    m_bCinematic = true;
    m_cinLookAt  = lookAt;
    m_fCinDist   = distance;
    m_fCinPitch  = pitch;
    m_fCinYaw    = yaw;
}

void CCamera::StopCinematic()
{
    m_bCinematic = false;
}

XMVECTOR CCamera::GetLookDirection() const
{
    XMMATRIX viewMatrix = XMLoadFloat4x4(&m_viewMatrix);
    XMMATRIX inverseViewMatrix = XMMatrixInverse(nullptr, viewMatrix);
    return inverseViewMatrix.r[2]; // Z-axis of the inverse view matrix is the look direction
}

XMVECTOR CCamera::GetRightDirection() const
{
    XMMATRIX viewMatrix = XMLoadFloat4x4(&m_viewMatrix);
    XMMATRIX inverseViewMatrix = XMMatrixInverse(nullptr, viewMatrix);
    return inverseViewMatrix.r[0]; // X-axis of the inverse view matrix is the right direction
}