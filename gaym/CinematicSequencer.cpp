#include "stdafx.h"
#include "CinematicSequencer.h"
#include <algorithm>

void CinematicSequencer::Queue(float fTime, std::function<void()> fnCallback)
{
    Event e;
    e.fTime = fTime;
    e.fnCallback = std::move(fnCallback);
    m_vEvents.push_back(std::move(e));
}

void CinematicSequencer::Clear()
{
    m_vEvents.clear();
    m_fTimer = 0.0f;
    m_bActive = false;
}

void CinematicSequencer::Start(bool bBlockInput)
{
    // 시간순 정렬 (Queue 호출 순서가 뒤죽박죽이어도 안전)
    std::sort(m_vEvents.begin(), m_vEvents.end(),
              [](const Event& a, const Event& b) { return a.fTime < b.fTime; });
    for (auto& e : m_vEvents) e.bFired = false;

    m_fTimer      = 0.0f;
    m_bActive     = !m_vEvents.empty();
    m_bBlockInput = bBlockInput;
}

void CinematicSequencer::Stop()
{
    m_bActive = false;
}

void CinematicSequencer::Tick(float dt)
{
    if (!m_bActive) return;
    m_fTimer += dt;

    bool bAllFired = true;
    for (auto& e : m_vEvents)
    {
        if (!e.bFired)
        {
            if (m_fTimer >= e.fTime)
            {
                e.bFired = true;
                if (e.fnCallback) e.fnCallback();
            }
            else
            {
                bAllFired = false;
            }
        }
    }
    if (bAllFired) m_bActive = false;
}
