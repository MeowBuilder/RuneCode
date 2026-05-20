#pragma once

// 원소별 상태이상 중첩 상태
// EnemyComponent가 원소마다 하나씩 보유
struct ElementStatusState
{
    int   stacks      = 0;
    int   maxStacks   = 3;
    float timer       = 0.f;     // 현재 중첩의 남은 지속시간 (새 중첩 추가 시 리셋)
    float tickTimer   = 0.f;     // 틱 간격 타이머 (Burn 전용)
    float tickDmgBase = 0.f;     // 틱 데미지 기준값 (Burn 전용)

    bool IsActive() const { return stacks > 0; }

    void Clear()
    {
        stacks      = 0;
        timer       = 0.f;
        tickTimer   = 0.f;
        tickDmgBase = 0.f;
    }
};
