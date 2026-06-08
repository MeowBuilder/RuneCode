#include "stdafx.h"
#include "HitStopSystem.h"
#include <algorithm>

HitStopSystem& HitStopSystem::Get()
{
    static HitStopSystem s;
    return s;
}

void HitStopSystem::Request(float seconds)
{
    if (seconds <= 0.0f) return;
    m_timer = (std::max)(m_timer, seconds);
}

float HitStopSystem::Tick(float rawDt)
{
    if (m_timer <= 0.0f)
        return rawDt;

    m_timer -= rawDt;
    if (m_timer <= 0.0f)
    {
        // 잔여 시간만큼 정지 → 나머지 dt 는 정상 흐름
        float leftover = -m_timer;  // 이 프레임의 raw dt 중 정지 후 남는 양
        m_timer = 0.0f;
        return leftover;
    }
    return 0.0f;  // 이 프레임 완전 정지
}
