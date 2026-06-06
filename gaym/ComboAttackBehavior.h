#pragma once
#include "IAttackBehavior.h"
#include <DirectXMath.h>
#include <vector>
#include <string>

using namespace DirectX;

// Combo attack: boss performs a series of melee attacks in sequence
// shape variant 호 1개 spawn 설정 — ComboAttackBehavior 외부에서도 접근 가능.
struct ArcShapeCfg
{
    float coneDeg;
    float halfWidthMul;
    float radiusMul;
    float yOffset;
    float yawOffsetDeg;
    bool  bVertical;
};

class ComboAttackBehavior : public IAttackBehavior
{
public:
    // 검기 모양 variant — 같은 애니메이션이라도 다른 시각 효과.
    //   Default 외: hit 별 호 굵기/반경/각도/높이/추가 호 변동.
    enum class SwordEnergyShape : int
    {
        Default,    // 단일 호 (기본)
        Wide,       // 굵고 짧은 호 (heavy slam, 묵직)
        Slim,       // 좁고 가는 호 (quick jab, 잽)
        Long,       // 멀리 뻗는 가는 호 (reach, 견제)
        Double,     // 평행 2단 호 (위/아래 동시)
        Cross,      // X 자 교차 호 (90° 회전 secondary)
        Overhead,   // 수직 내려치기 — 호가 위→아래 plane
    };

    struct ComboHit
    {
        float fDamage = 10.0f;
        float fWindupTime = 0.15f;
        float fHitTime = 0.1f;
        float fRecoveryTime = 0.1f;
        float fHitRange = 4.0f;
        float fConeAngle = 90.0f;
        std::string strAnimation = "Attack 1";
        bool bTrackTarget = true;  // Re-face target before this hit

        // 전방 사각형 판정 모드 — 둘 다 >0 이면 cone 대신 사각형 사용
        float fRectWidthHalf = 0.0f;
        float fRectLength    = 0.0f;

        // 타격 시점에 보스 칼 끝에 스폰할 EffectRegistry 이펙트 이름.
        // 비어있으면 VFX 안 뜸. 원소 보스 짤패에 "칼날에 깃든 원소" 표현용.
        std::string strVFXOnHit;
        // 추가 임팩트 이펙트 (예: sub_blast_wave, sub_strike_spark). 비어있으면 안 띄움.
        std::string strVFXImpact;
        // VFX 스폰 오프셋 — 보스 본체 감싸는 모드 기본값 (DarkLord scale 10 기준).
        //   forward 0 = 보스 중심에서 스폰. Y 9 = 본체 중심 높이.
        //   "검기" 효과 원하면 forward 키워서 검 끝쪽으로 밀기 (예: 8~10).
        float fVFXForwardOffset = 0.0f;
        float fVFXYOffset       = 9.0f;
        // 파티클/사이즈 스케일 — 본체 감싸려면 5.0~8.0 추천
        float fVFXScale         = 6.0f;

        // 검기 모양 — 같은 애니라도 hit 별로 다양화.
        SwordEnergyShape eShape = SwordEnergyShape::Default;
    };

    ComboAttackBehavior(const std::vector<ComboHit>& hits);
    virtual ~ComboAttackBehavior();

    virtual void Execute(EnemyComponent* pEnemy) override;
    virtual void Update(float dt, EnemyComponent* pEnemy) override;
    virtual bool IsFinished() const override;
    virtual void Reset() override;
    // 첫 hit의 애니메이션을 반환 — EnemyComponent state 머신이 Attack 진입 시 올바른 클립 재생
    virtual const char* GetAnimClipName() const override {
        return m_vHits.empty() ? "" : m_vHits[0].strAnimation.c_str();
    }
    // 첫 hit의 windup 시간을 fill 기준으로 (이후 hit는 같은 애니 연속 재생이라 동일 텔레그래프 사용)
    virtual float GetTimeToHit() const override {
        return m_vHits.empty() ? 0.0f : m_vHits[0].fWindupTime;
    }

    // 단발/콤보 첫 hit 의 cone 이 충분히 넓으면 (≥120°) Circle 인디케이터로 override —
    //   DarkLord ForwardBox 기본(4.5×9.0) 은 실제 hit cone/range 와 크게 어긋남.
    //   좁은 cone 은 preset 의 ForwardBox 유지.
    virtual int GetIndicatorTypeOverride() const override;
    virtual float GetIndicatorRadius() const override;
    virtual float GetIndicatorLength() const override;
    virtual DirectX::XMFLOAT3 GetIndicatorTint() const override;

    // Builder helper for creating combos
    static ComboAttackBehavior* CreateLightCombo();   // Fast 3-hit combo
    static ComboAttackBehavior* CreateHeavyCombo();   // Slow 2-hit high damage
    static ComboAttackBehavior* CreateFuryCombo();    // Fast 5-hit flurry

private:
    void DealConeDamage(EnemyComponent* pEnemy, const ComboHit& hit);
    void SpawnHitVFX(EnemyComponent* pEnemy, const ComboHit& hit);
    // 검 본 자동 탐색 — DarkLord 등 sword-wielding 보스용. nullptr 면 fallback (boss forward + Y).
    class TransformComponent* FindSwordBone(EnemyComponent* pEnemy);

    // 검기/글로우 메쉬 lifecycle — 작은 piece 들이 검 본 trail 따라 emit, 각자 scale + emissive 페이드.
    struct SlashPiece
    {
        class GameObject* pObj = nullptr;
        float fAge      = 0.0f;
        float fLifetime = 0.30f;
        DirectX::XMFLOAT3 startScale = { 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT3 endScale   = { 1.6f, 1.6f, 1.6f };
        DirectX::XMFLOAT4 startEmissive = { 1.0f, 1.0f, 1.0f, 1.0f };
        class Scene* pScene = nullptr;   // cleanup 시 MarkForDeletion 에 사용
    };
    void UpdateSlashPieces(float dt);
    void CleanupAllSlashPieces();
    // Hit phase 동안 매 N 초마다 검 본 위치에 작은 glow piece 를 spawn — 진짜 trail 표현
    void UpdateTrailEmission(float dt, EnemyComponent* pEnemy);

private:
    // 본 탐색 1회 로그용 (per instance)
    bool m_bBoneLookupDone = false;
    class TransformComponent* m_pCachedSwordBone = nullptr;

    std::vector<SlashPiece> m_vSlashPieces;

    // Trail emission state — hit 발생 시 시작, fTrailDuration 동안 매 fTrailInterval 마다 piece spawn
    bool  m_bEmittingTrail        = false;
    float m_fTrailRemain          = 0.0f;
    float m_fTrailEmitAccum       = 0.0f;
    float m_fTrailEmitInterval    = 0.005f;     // 200Hz — 매우 촘촘, dash 들이 연속 띠 형성
    float m_fTrailPieceScale      = 1.0f;
    DirectX::XMFLOAT4 m_xmf4TrailColor    = { 1.0f, 1.0f, 1.0f, 1.0f };   // core (밝은 가운데)
    DirectX::XMFLOAT4 m_xmf4TrailEdgeColor= { 1.0f, 1.0f, 1.0f, 1.0f };   // edge (양 옆 색)
    // 검 본 이전 위치 — swing 방향(속도) 계산해서 piece 를 그 방향으로 stretch
    DirectX::XMFLOAT3 m_xmf3PrevSwordPos = { 0.0f, 0.0f, 0.0f };
    bool m_bHasPrevSwordPos       = false;
    // 누적 swing yaw — 매 frame fresh atan2 가 jitter 로 spike 생성 → smoothing 필요
    float m_fSmoothedYaw          = 0.0f;
    bool  m_bYawInitialized       = false;

    // ── Dynamic triangle strip ribbon mesh — 한 hit 당 1 mesh, 검 본 history 따라 매 frame 갱신.
    //   piece 누적 X → fan/boundary 없음. 진짜 검 swing arc 표현.
    class GameObject*       m_pTrailRibbon       = nullptr;
    class Scene*            m_pRibbonScene       = nullptr;
    class SwordTrailMesh*   m_pTrailMesh         = nullptr;   // Behavior owns; AddRef'd by GameObject
    std::vector<DirectX::XMFLOAT3> m_vSwordHistory;
    DirectX::XMFLOAT3       m_xmf3TrailStartPos  = { 0,0,0 };
    float              m_fRibbonFadeT       = 1.0f;
    bool               m_bRibbonFading      = false;
    void UpdateTrailMesh(float dt, EnemyComponent* pEnemy);
    void StopRibbon();

    // 검 자체 emissive 발광 — 검 GameObject 의 material.m_cEmissive 를 동적 boost
    class GameObject*  m_pSwordObj         = nullptr;
    bool               m_bSwordGlowing     = false;
    float              m_fSwordGlowRemain  = 0.0f;
    float              m_fSwordGlowMax     = 0.30f;
    DirectX::XMFLOAT4  m_xmf4SwordGlowColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    // 3단계 anticipation flash — prearm 시작 직후 흰색 부스트.
    //   "검에 원소가 깃드는 순간" 표현 — 흰빛 → element 색 전이.
    //   m_fSwordGlowFlashWhiteMax 는 set 한 초기값 보존 → 분모로 정확한 lerp.
    float              m_fSwordGlowFlashWhite = 0.0f;
    float              m_fSwordGlowFlashWhiteMax = 0.05f;
    void UpdateSwordGlow(float dt);
    void StopSwordGlow();

    // ── 2단계 Static crescent flash — Hit 진입 순간 1프레임 호 mesh 번쩍.
    //   동적 ribbon 과 별도의 static SwordTrailMesh 에 arc 점들을 미리 계산해 push.
    //   같은 sword-trail 셰이더 path 사용 → 톤 일치.
    class GameObject*       m_pCrescentObj    = nullptr;
    class Scene*            m_pCrescentScene  = nullptr;
    class SwordTrailMesh*   m_pCrescentMesh   = nullptr;
    // Secondary 호 — Double/Cross/Overhead shape 에서 추가 spawn.
    class GameObject*       m_pCrescentObj2   = nullptr;
    class SwordTrailMesh*   m_pCrescentMesh2  = nullptr;
    DirectX::XMFLOAT4       m_xmf4CrescentColor = { 1.0f, 1.0f, 1.0f, 3.0f };
    // Crescent alpha envelope — 게이지 fill 효과 폐기. 단순 fade-in → hold → fade-out.
    //   m_fCrescentTime    : spawn 후 경과 시간 (s).
    //   m_fCrescentFadeInDur  : 0 → 1 부드러운 등장.
    //   m_fCrescentHoldDur    : alpha 1 유지 (swing window).
    //   m_fCrescentFadeOutDur : 1 → 0 사라짐.
    float                   m_fCrescentTime        = 0.0f;
    float                   m_fCrescentFadeInDur   = 0.06f;
    float                   m_fCrescentHoldDur     = 0.30f;
    float                   m_fCrescentFadeOutDur  = 0.15f;
    void SpawnCrescentFlash(EnemyComponent* pEnemy, const ComboHit& hit);
    void SpawnSingleCrescent(EnemyComponent* pEnemy, const ComboHit& hit,
                              const struct ArcShapeCfg& cfg, float chestY, float yawDeg,
                              class GameObject** outObj, class SwordTrailMesh** outMesh);
    void UpdateCrescent(float dt);
    void StopCrescent();

private:
    std::vector<ComboHit> m_vHits;

    // Runtime state
    enum class HitPhase { Windup, Hit, Recovery };
    HitPhase m_eHitPhase = HitPhase::Windup;
    int m_nCurrentHit = 0;
    float m_fTimer = 0.0f;
    bool m_bHitDealt = false;
    bool m_bFinished = false;
    // Trail 이 이번 hit 의 windup 후반부에 미리 켜졌는지 — true 면 Hit 진입 시 재시작 skip
    bool m_bTrailStarted = false;
    // 현재 hit 의 cone 각도 — UpdateTrailMesh 가 회전 공격(>180°) 시 history 짧게 캡 (자기 교차 차단).
    float m_fCurrentConeAngle = 0.0f;
    // Crescent prearm 트리거 가드 — windup 동안 한 번만 spawn.
    bool m_bCrescentSpawned = false;
};
