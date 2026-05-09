#pragma once
#include <DirectXMath.h>

class EnemyComponent;

class IAttackBehavior
{
public:
    virtual ~IAttackBehavior() = default;

    // Start the attack
    virtual void Execute(EnemyComponent* pEnemy) = 0;

    // Update the attack (called every frame while attacking)
    virtual void Update(float dt, EnemyComponent* pEnemy) = 0;

    // Check if the attack is finished
    virtual bool IsFinished() const = 0;

    // Reset the attack state for reuse
    virtual void Reset() = 0;

    // 이 공격에 맞는 애니메이션 클립 이름. 빈 문자열이면 m_strAttackClip 기본값 사용
    virtual const char* GetAnimClipName() const { return ""; }

    // 공격 시작(windup)부터 실제 데미지 타이밍까지의 시간(초).
    // 인디케이터 fill 진행도 계산에 사용. 0 반환 시 즉시 차오름/기본 1.0s 사용
    virtual float GetTimeToHit() const { return 0.0f; }

    // 지면 AoE 인디케이터(빨간 원)를 보여줄지 여부. 투사체/발사형은 false로 표시 억제
    virtual bool ShouldShowHitZone() const { return true; }

    // 이 공격의 지면 인디케이터 반경 override. 0 반환 시 EnemyComponent preset 기본값 사용.
    // Circle 에서는 반경, ForwardBox 에서는 half-width 로 사용됨
    virtual float GetIndicatorRadius() const { return 0.0f; }

    // 인디케이터 길이 override (ForwardBox 전용). 0 반환 시 preset 기본값 사용
    virtual float GetIndicatorLength() const { return 0.0f; }

    // 인디케이터 타입 override. 반환값이 음수이면 preset 기본값 사용 (일반)
    //   특정 패턴이 preset 과 다른 모양이 필요할 때 override (예: 원형 보스의 직사각형 공격)
    //   값은 int 로 받아서 IndicatorType 으로 캐스팅 — 헤더 의존성 축소
    virtual int GetIndicatorTypeOverride() const { return -1; }

    // 공격 애니메이션 루프 여부 override.
    //   true (default) : m_AnimConfig.m_bLoopAttack 사용
    //   false          : 애니를 1회만 재생하고 끝 (behavior 가 애니 종료 후 idle 전환 담당)
    virtual bool ShouldLoopAnim() const { return true; }

    // 인디케이터 색상 RGB 멀티플라이어 (1,1,1) 이면 기본 빨강 그대로.
    //   특수 패턴마다 색깔 구분에 사용 — 예: SpinDash=주황, FixatedCharge=보라/짙은빨강
    virtual DirectX::XMFLOAT3 GetIndicatorTint() const
    {
        return DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);
    }
};
