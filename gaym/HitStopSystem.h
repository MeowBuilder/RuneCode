#pragma once

// ─── 전역 Hit-Stop 시스템 ───────────────────────────────────────────────────
//
// 검기 임팩트의 "찰나" 감각을 만드는 핵심. Request(seconds) 호출 후, 매 프레임
// Scene::Update 초입에서 Tick(rawDt)을 호출해 effective dt를 받는다. effective dt
// 는 게임 로직(애니메이션·이동·물리)에 전달, raw dt 는 카메라/UI/VFX 등 멈춰서는
// 안 되는 시스템에 그대로 전달.
//
//   HitStopSystem::Get().Request(0.05f);            // 50ms 정지
//   float effDt = HitStopSystem::Get().Tick(raw);   // 매 프레임 1회만 호출
//
// 주의:
//   - Tick은 프레임당 정확히 1번. (Scene::Update 시작점 권장)
//   - Request는 동시에 들어와도 안전 — 더 긴 시간이 우선.
//   - 카메라 셰이크/줌은 effective dt 가 아닌 raw dt 를 받아야 효과가 멈추지 않음.
class HitStopSystem
{
public:
    static HitStopSystem& Get();

    // 새 hit-stop 요청. 이미 활성이면 (현재 잔여) vs (새 요청) 중 더 긴 쪽 채택.
    void Request(float seconds);

    // 프레임당 1회. 반환: hit-stop 이 적용된 effective dt.
    // 내부 타이머는 raw dt 기준으로 감소 (실시간 흐름과 동기).
    float Tick(float rawDt);

    bool  IsActive() const  { return m_timer > 0.0f; }
    float Remaining() const { return m_timer; }
    void  Cancel()          { m_timer = 0.0f; }

private:
    HitStopSystem() = default;
    float m_timer = 0.0f;
};
