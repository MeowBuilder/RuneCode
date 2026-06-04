#pragma once
#include <vector>
#include <functional>

// 단순 타임라인 큐. (t초, callback) 이벤트를 등록해두면 Tick 호출 시
// 경과 시간에 도달한 이벤트를 순서대로 발사한다. DarkLord 입장 컷신 등에 사용.
//
// 사용:
//   CinematicSequencer seq;
//   seq.Queue(0.0f, [&]{ /* 페이드 인 */ });
//   seq.Queue(7.0f, [&]{ /* 스크린 슬래시 */ });
//   seq.Queue(11.0f, [&]{ /* 입력 해제 */ });
//   seq.Start(true /* 입력 잠금 */);
//   매 프레임 seq.Tick(dt);
class CinematicSequencer
{
public:
    void Queue(float fTime, std::function<void()> fnCallback);
    void Clear();

    // Start: 타이머 0 으로 리셋, 큐는 그대로. bBlockInput 동안 게임플레이 입력 차단.
    void Start(bool bBlockInput = true);
    void Stop();

    void Tick(float dt);

    bool IsActive() const     { return m_bActive; }
    bool BlocksInput() const  { return m_bActive && m_bBlockInput; }
    bool IsFinished() const   { return !m_bActive; }
    float GetTime() const     { return m_fTimer; }

private:
    struct Event
    {
        float                 fTime    = 0.0f;
        std::function<void()> fnCallback;
        bool                  bFired   = false;
    };

    std::vector<Event> m_vEvents;
    float m_fTimer       = 0.0f;
    bool  m_bActive      = false;
    bool  m_bBlockInput  = false;
};
