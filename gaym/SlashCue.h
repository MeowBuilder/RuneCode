#pragma once
#include "SlashVFXDesc.h"
#include <DirectXMath.h>

class Scene;
class EnemyComponent;
class TransformComponent;

// ─── SlashCue ────────────────────────────────────────────────────────────────
//
// 보스 검기 한 발의 5-레이어 컷씬 중 L1 / L2 / L5 를 관리하는 stateful 헬퍼.
// L3 (Blade Body — ribbon + crescent) 와 데미지 판정은 ComboAttackBehavior 가
// 이미 처리하므로 여기서는 다루지 않는다.
//
//   1) Begin(scene, enemy, desc)    — 시작.
//   2) Tick(rawDt, enemy)           — 매 프레임. 차징 오라 위치 갱신.
//   3) Release(enemy)               — 발도 순간 1회. 화이트플래시 + 히트스톱 + 카메라 펄스.
//   4) Impact(enemy)                — 검기 충돌 시점 (현재는 짧은 히트스톱만, 향후 확장).
//   5) End()                        — 차징 오라 정리 + 내부 상태 리셋.
//
// raw dt 사용: 차징 오라 추적·카메라 펄스는 hit-stop 영향 X. ComboAttackBehavior 가
// 받는 effective dt 와는 별개 (Scene::GetRawDeltaTime).
class SlashCue
{
public:
    SlashCue() = default;
    ~SlashCue();

    // 비복사·비이동 (VFX slot 소유권 단순화)
    SlashCue(const SlashCue&)            = delete;
    SlashCue& operator=(const SlashCue&) = delete;

    // 검 끝 월드 좌표/방향을 caller(DarkLordSigilSlash 등) 가 계산해서 전달.
    //   검 본 캐싱 책임은 호출자에게 — SlashCue 는 ComboAttackBehavior 와 독립.
    void Begin(Scene* pScene, const SlashVFXDesc& desc,
               const DirectX::XMFLOAT3& swordTipPos,
               const DirectX::XMFLOAT3& swordDir);
    void Tick(float rawDt,
              const DirectX::XMFLOAT3& swordTipPos,
              const DirectX::XMFLOAT3& swordDir);
    void Release();
    // 실제 타격 모멘트. 검 끝/지면 위치 전달 — Ring/Spark VFX 가 거기 spawn.
    //   impactPos : 검기 끝점 (보스 정면 ~사거리 중간)
    //   groundPos : 동일 X/Z 의 지면 (Y=0 또는 보스 발치) — Ring 충격파용
    void Impact(const DirectX::XMFLOAT3& impactPos,
                const DirectX::XMFLOAT3& groundPos,
                const DirectX::XMFLOAT3& dir);
    void End();

    bool IsActive() const    { return m_bActive; }
    bool HasReleased() const { return m_bReleased; }

    // ── 원소 → EffectDef 이름 매핑 (외부에서도 직접 spawn 가능하도록 public) ──
    static const char* ChargeAuraEffectName(ElementType e);
    static const char* ImpactEffectName(ElementType e);
    static DirectX::XMFLOAT3 ImpactFlashColor(ElementType e);

private:

    Scene*       m_pScene   = nullptr;
    SlashVFXDesc m_desc;
    int          m_nChargeVFXSlot = -1;
    bool         m_bActive   = false;
    bool         m_bReleased = false;
};
