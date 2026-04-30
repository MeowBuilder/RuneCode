#pragma once
#include <vector>
#include <optional>
#include <DirectXMath.h>
#include "FluidParticle.h"  // FluidCPDesc
using namespace DirectX;

// 파티클 운동 모드
enum class ParticleMotionMode {
    ControlPoint,  // 기존: CP 인력 기반 SPH
    Gravity,       // 신규: 중력만 작용
    Beam,          // 신규: 시작->끝 선형 이동, 반복
    OrbitalCP,     // 신규: 마스터 CP + 공전 위성 CP군
};

// 중력 설정
struct GravityDesc {
    XMFLOAT3 gravity = { 0.f, -9.8f, 0.f };
    float initialSpeedMin = 2.f;
    float initialSpeedMax = 8.f;
};

// 빔 설정
struct BeamDesc {
    XMFLOAT3 startPos = {};
    XMFLOAT3 endPos   = {};
    float speedMin = 8.f;
    float speedMax = 16.f;
    float spreadRadius = 0.3f; // 빔 폭 (시작점 랜덤 오프셋 / swirlSpeed 사용 시 시작 공전 반경)
    float verticalScale = 1.0f; // 수직 배율 (기본 1.0 = 원형, 보스 브레스 등은 낮춰서 납작하게 만듦)
    bool  enableFlow = false;   // 입자 흐름 활성화 (false = 정적 레이저, true = 흐르는 브레스)
    float swirlSpeed  = 0.f;   // 공전 각속도 (rad/s). 0이면 기존 랜덤 오프셋 방식
    bool  swirlExpand = false; // true = 반경이 beamT에 따라 커짐(퍼짐), false = 작아짐(수렴)
    float swirlFadeEnd   = 0.f;  // 이 beamT 위치에서 크기→0 (0=페이드 없음)
    bool  swirlFadeInOut = false; // true=시작(0)→밝아짐→swirlFadeEnd/2 피크→사라짐 (삼각파)
    float beamLength     = 0.f;  // 빔 기하 길이(월드 거리). 0 이면 매니저 기본값/스위를페이드엔드 사용
    float swirlBaseT     = 0.f;  // 입 쪽 기본 폭 비율 (0..1, 0이면 기존 원뿔, 0.3이면 입에서도 30% 폭)
    XMFLOAT3 prevDir = { 0.f, 0.f, 1.f }; // 이전 프레임 방향 (빔 전체 회전 계산용)
};

// 박스 경계
struct ConfinementBoxDesc {
    XMFLOAT3 center      = {};
    XMFLOAT3 halfExtents = { 1.f, 1.f, 1.f };
    // 월드 공간 기준 OBB를 위한 로컬 축 (기본: 축정렬)
    XMFLOAT3 axisX = { 1,0,0 };
    XMFLOAT3 axisY = { 0,1,0 };
    XMFLOAT3 axisZ = { 0,0,1 };
    bool active = false;
};

// 위성 CP 설명자 (OrbitalCP 모드용)
struct SatelliteCPDesc {
    float orbitRadius;
    float orbitSpeed;     // rad/s
    float orbitPhase;     // 초기 위상
    float verticalOffset; // 마스터 CP 기준 Y 오프셋
    float attractionStrength;
    float sphereRadius;
    // 궤도 기울기: X축 기준으로 xz면을 회전 (0=수평, π/2=수직 yz면)
    float orbitTiltX = 0.f;
    // 궤도면 세차운동 속도 (rad/s): 0이면 고정, 양수/음수로 방향 제어
    float precessionSpeed = 0.f;

    // 궤도 반지름 호흡 (줄었다 늘었다)
    // R_eff = orbitRadius * (1 + breatheAmplitude * sin(elapsed * breatheSpeed + breathePhase))
    float breatheAmplitude = 0.f;  // 진폭 (0~1): 1이면 완전히 0까지 수축
    float breatheSpeed     = 0.f;  // 속도 (rad/s)
    float breathePhase     = 0.f;  // 초기 위상
};

// VFX 페이즈
struct VFXPhase {
    float startTime = 0.f;
    float duration  = 1.f;

    ParticleMotionMode motionMode = ParticleMotionMode::ControlPoint;

    // ConfinementBox (모든 모드에서 선택적 활성화 가능)
    ConfinementBoxDesc boxDesc;

    // Gravity 모드용
    GravityDesc gravityDesc;

    // Beam 모드용
    BeamDesc beamDesc;

    // ControlPoint 모드용 CP 목록 (비어있으면 CP 없음)
    std::vector<FluidCPDesc> cpDescs;

    // 박스 확장 방향 힘 (OBB 로컬 좌표계: x=right, y=up, z=forward)
    XMFLOAT3 expansionForce = { 0.f, 0.f, 0.f };
    float expansionForceStrength = 0.f;

    // 전역 중력 강도 (모든 모드에서 적용 가능, 0이면 비활성)
    float globalGravityStrength = 0.f;

    // Phase 진입 시 forward(Z축) 방향 속도 제거
    bool cancelForwardVelocityOnEnter = false;

    // expansionForce를 양방향 분산으로 적용 (중심 기준으로 양쪽으로 밀기)
    // false: 모든 파티클에 동일 방향 (기존), true: 중심 기준 양방향
    bool useAxisSpreadForce = false;

    // Phase 진입 시 right 축 방향으로 랜덤 속도 부여 (양방향, 파도 확산용)
    // 0이면 비활성, 양수면 이 값 범위에서 랜덤 (-randomSidewaysImpulse ~ +randomSidewaysImpulse)
    float randomSidewaysImpulse = 0.f;

    // 투사체 이동 시 파티클도 같이 텔레포트할지 여부
    // true(기본): 파티클이 origin과 함께 이동 (뭉쳐서 이동)
    // false: CP만 이동, 파티클은 물리로 따라감 → 혜성 꼬리 효과
    bool offsetParticlesWithOrigin = true;

    // Phase 진입 시 maxParticleSpeed 오버라이드 (0=변경 없음)
    // 폭발 페이즈에서 속도 상한을 높여 빠른 방사 허용
    float phaseMaxSpeed = 0.f;

    // Phase 진입 시 ExplodeFade 모드 시작 (파티클이 작아지며 사라짐)
    // true이면 phase.duration 동안 fadeRatio 1→0으로 감소 후 슬롯 소멸
    bool triggerExplodeFadeOnEnter = false;
};

// VFX 룬 수식자
struct VFXModifier {
    float particleCountMult = 1.f;
    float strengthMult      = 1.f;
    float sizeScaleMult     = 1.f;
    float speedMult         = 1.f;
    std::optional<std::vector<VFXPhase>> phaseOverride;
};

// ─── 새 기반 이미터 시스템 ────────────────────────────────────────────────────

#include <unordered_map>
#include <string>

enum class EmitterType {
    // 경량 (컴퓨트 셰이더, SPH 없음)
    Linear,      // 직선 방출: 빔, 광선, 화살
    Cone,        // 원뿔 확산: 폭발, 화염, 연기
    Sphere,      // 구형: 오라, 폭발구
    Ring,        // 링/충격파
    Burst,       // 중력 영향 폭발 파편

    // SPH 물리 (기존 FluidParticleSystem 래핑)
    SPH_Attract, // ControlPoint 모드
    SPH_Gravity, // Gravity 모드
    SPH_Orbital, // OrbitalCP 모드
    SPH_Beam,    // Beam 모드

    // 텍스처 이펙트 (향후 구현)
    Decal,
    MagicCircle,
    Sprite,
};

// ── 경량 이미터 파라미터 ──────────────────────────────────────────────────────
// 공통 speedMin/speedMax, lifetimeMin/lifetimeMax 는 EffectLayer에서 관리

struct LinearEmitterParams {
    float length      = 10.f;   // 빔 총 길이
    float width       = 0.5f;   // 굵기 (반경)
    float recycleRate = 0.f;    // >0: 끝 도달 후 시작점 재스폰 (흐름 효과)
    float swirlSpeed  = 0.f;    // 공전 각속도 (rad/s)
};

struct ConeEmitterParams {
    float halfAngle     = 30.f;  // 반각 (도)
    float gravityScale  = 0.f;   // 0: 중력 없음, 1: 기본 중력 (-9.8m/s²)
    float startSizeMult = 1.f;   // 스폰 시 크기 배율
    float endSizeMult   = 0.3f;  // 소멸 시 크기 배율
    bool  fadeAlpha     = true;
    bool  fadeSize      = false;
};

struct SphereEmitterParams {
    float radius         = 2.f;
    float shellFraction  = 0.f;   // 0=전체 채움, 1=껍데기만
    bool  inward         = false;  // true: 중심 수렴, false: 발산
    float rotationSpeed  = 0.f;   // Y축 회전 (rad/s)
};

struct RingEmitterParams {
    float radius         = 3.f;
    float width          = 0.5f;  // 링 두께
    float expandSpeed    = 0.f;   // 반경 확장 속도 (units/s)
    float tiltX          = 0.f;   // X축 기울기 (rad)
    float rotateSpeed    = 0.f;   // 링 회전 속도 (rad/s)
    float normalSpeedMin = 0.f;   // 링 법선 방향 파티클 속도
    float normalSpeedMax = 1.f;
};

struct BurstEmitterParams {
    float bounceCoeff = 0.f;      // 바닥 반사 계수 (0: 없음)
    float groundY     = -999.f;   // 바닥 높이
    bool  fadeOut     = true;     // 수명에 따라 알파 감소
    bool  fadeSize    = true;     // 수명에 따라 크기 감소
};

// ── SPH 이미터 래핑 파라미터 ──────────────────────────────────────────────────
// SPH 시퀀스 기반 이미터(ControlPoint/Gravity/OrbitalCP/Beam) 파라미터를 캡슐화

struct SPHEmitterParams {
    std::vector<VFXPhase>        phases;
    std::vector<FluidCPDesc>     cpDescs;
    std::vector<SatelliteCPDesc> satelliteCPs;

    int   particleCount        = 200;
    float spawnRadius          = 1.5f;

    float masterCPFallSpeed    = 15.f;
    float masterCPStrength     = 25.f;
    float masterCPSphereRadius = 5.f;

    float nucleusSpawnFraction = 0.f;
    float nucleusSpawnRadius   = 0.4f;

    float maxParticleSpeed     = 0.f;

    bool  overridePhysics      = false;
    float sphStiffness         = 50.f;
    float sphNearPressureMult  = 2.f;
    float sphRestDensity       = 7.f;
    float sphViscosity         = 0.25f;
    float sphSmoothingRadius   = 1.2f;

    float cardinalSpawnRadius  = 0.f;
    float cardinalInwardSpeed  = 12.f;

    float particleSize         = 0.f;

    bool  isWave               = false;
    float waveSpeed            = 20.f;
    float wavePushForce        = 60.f;
    float waveMaxDist          = 22.f;
    float waveHalfW            = 4.f;
    float waveHalfH            = 2.5f;
    float waveDepth            = 2.5f;
    float waveOscAmplitude     = 0.f;
    float waveOscFrequency     = 4.f;
    float waveOscWaveNumber    = 0.8f;
};

// ── 텍스처 이펙트 파라미터 (향후 구현) ────────────────────────────────────────

struct DecalParams {
    float radius      = 3.f;
    float duration    = 2.f;
    float fadeInTime  = 0.2f;
    float fadeOutTime = 0.5f;
    bool  rotate      = false;
    float rotateSpeed = 0.f;
    // std::string texturePath;  // 향후 텍스처 시스템 추가 시
};

struct MagicCircleParams {
    float radius      = 3.f;
    float rotateSpeed = 1.f;   // rad/s
    float duration    = 3.f;
    bool  pulse       = true;
    float pulseFreq   = 2.f;
    // std::string texturePath;
};

struct SpriteParams {
    int   frameCount = 1;
    float frameRate  = 24.f;
    bool  loop       = true;
    float duration   = -1.f;
    // std::string texturePath;
};

// ── EffectLayer: 단일 이미터 레이어 ──────────────────────────────────────────

struct EffectLayer {
    EmitterType  type          = EmitterType::Cone;
    ElementType  element       = ElementType::Fire; // SPH 레이어의 원소 타입
    int          particleCount = 100;   // 경량 이미터용 (SPH는 sph.particleCount 사용)

    XMFLOAT4     coreColor     = { 1, 1, 1, 1 };
    XMFLOAT4     edgeColor     = { 1, 1, 1, 0.5f };
    bool         overrideColors = false;  // true: coreColor/edgeColor를 강제 적용 (false면 element 기본 색상)
    float        sizeScale     = 1.f;

    // 공용 파라미터 (경량 이미터)
    float        speedMin      = 5.f;
    float        speedMax      = 15.f;
    float        lifetimeMin   = 0.5f;
    float        lifetimeMax   = 1.5f;

    float        emitDelay     = 0.f;   // 스킬 발동 후 이 레이어 시작 딜레이
    float        duration      = -1.f;  // -1: 무한, 양수: 이 레이어 유효 시간
    float        emitRate      = 0.f;   // 0: 즉시 전부, >0: 초당 방출 수

    bool         attachToProjectile = false; // 투사체 위치/방향 추적
    bool         useSSF             = false; // SSF 파이프라인 사용

    // 이미터별 파라미터 (type에 따라 해당 필드 사용)
    LinearEmitterParams   linear;
    ConeEmitterParams     cone;
    SphereEmitterParams   sphere;
    RingEmitterParams     ring;
    BurstEmitterParams    burst;
    SPHEmitterParams      sph;         // SPH_* 타입 전용
    DecalParams           decal;       // 향후
    MagicCircleParams     magicCircle; // 향후
    SpriteParams          sprite;      // 향후
};

// ── EffectDef: 레이어 조합으로 이루어진 이펙트 정의 ──────────────────────────

struct EffectDef {
    std::string              name;
    ElementType              element = ElementType::Fire;
    std::vector<EffectLayer> layers;
};
