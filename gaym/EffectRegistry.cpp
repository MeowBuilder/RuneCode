#include "stdafx.h"
#include "EffectRegistry.h"
#include <algorithm>
#include <cmath>

// ─── 등록 헬퍼: phases[0].motionMode → SPH_* EmitterType ───────────────────
namespace {
    EmitterType PickSPHEmitter(const std::vector<VFXPhase>& phases)
    {
        if (phases.empty())
            return EmitterType::SPH_Attract;
        switch (phases[0].motionMode)
        {
        case ParticleMotionMode::Gravity:    return EmitterType::SPH_Gravity;
        case ParticleMotionMode::OrbitalCP:  return EmitterType::SPH_Orbital;
        case ParticleMotionMode::Beam:       return EmitterType::SPH_Beam;
        case ParticleMotionMode::ControlPoint:
        default:                             return EmitterType::SPH_Attract;
        }
    }

    // 신규 SPH 레이어를 생성해 element/색상 기본값을 채움.
    // 이후 호출자가 sph.* 필드, coreColor/edgeColor 등을 직접 설정한다.
    EffectLayer MakeSPHLayer(ElementType element)
    {
        EffectLayer layer;
        layer.type    = EmitterType::SPH_Attract; // phases 채운 뒤 FinalizeSPHLayer로 보정
        layer.element = element;
        return layer;
    }

    // 등록 직전 호출: phases[0].motionMode 기반으로 EmitterType 확정
    void FinalizeSPHLayer(EffectLayer& layer)
    {
        layer.type = PickSPHEmitter(layer.sph.phases);
    }
}

// ─── EffectRegistry ──────────────────────────────────────────────────────────

EffectRegistry& EffectRegistry::Get() {
    static EffectRegistry instance;
    return instance;
}

void EffectRegistry::Initialize()
{
    // ──────────────────────────────────────────────────────────────────────────
    // Q_WaveSlash — 웨이브 슬래시 (일정 폭으로 앞으로 나아가는 파도)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Fire);
        layer.overrideColors = true;
        layer.coreColor = { 0.88f, 0.12f, 0.01f, 1.0f };
        layer.edgeColor = { 0.45f, 0.03f, 0.0f,  0.90f };
        layer.useSSF    = true;

        SPHEmitterParams& s   = layer.sph;
        s.particleCount       = 600;
        s.spawnRadius         = 1.0f;
        s.isWave              = true;
        s.waveSpeed           = 10.f;
        s.wavePushForce       = 50.f;
        s.waveMaxDist         = 20.f;
        s.waveHalfW           = 5.0f;
        s.waveHalfH           = 2.5f;
        s.waveOscAmplitude    = 12.f;
        s.waveOscFrequency    = 5.f;
        s.waveOscWaveNumber   = 0.7f;
        s.maxParticleSpeed    = 20.f;
        s.overridePhysics     = true;
        s.sphStiffness        = 20.f;
        s.sphNearPressureMult = 0.5f;
        s.sphRestDensity      = 0.0f;
        s.sphViscosity        = 0.4f;
        s.sphSmoothingRadius  = 1.8f;

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "Q_WaveSlash";
        def.element = ElementType::Fire;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));

        VFXModifier enhMod;
        enhMod.particleCountMult = 1.5f;
        enhMod.sizeScaleMult     = 1.3f;
        RegisterRuneMod("Q_WaveSlash", RUNE_ENHANCE, enhMod);

        VFXModifier chgMod;
        chgMod.particleCountMult = 1.4f;
        RegisterRuneMod("Q_WaveSlash", RUNE_CHARGE, chgMod);
    }

    // ──────────────────────────────────────────────────────────────────────────
    // E_FireBeam_Core — 화염 빔 코어
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Fire);
        layer.overrideColors = true;
        layer.coreColor = { 1.0f, 0.97f, 0.78f, 1.0f };
        layer.edgeColor = { 1.0f, 0.55f, 0.08f, 0.9f };

        SPHEmitterParams& s = layer.sph;
        s.particleCount     = 900;
        s.spawnRadius       = 0.12f;

        VFXPhase p0;
        p0.startTime  = 0.f;
        p0.duration   = 99.f;
        p0.motionMode = ParticleMotionMode::Beam;
        p0.beamDesc.speedMin     = 30.f;
        p0.beamDesc.speedMax     = 52.f;
        p0.beamDesc.spreadRadius = 0.14f;
        p0.beamDesc.enableFlow   = true;
        s.phases.push_back(p0);

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "E_FireBeam_Core";
        def.element = ElementType::Fire;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));

        VFXModifier chanMod;
        chanMod.particleCountMult = 1.5f;
        chanMod.speedMult         = 1.2f;
        RegisterRuneMod("E_FireBeam_Core", RUNE_CHANNEL, chanMod);

        VFXModifier enhMod;
        enhMod.particleCountMult = 1.4f;
        enhMod.speedMult         = 1.3f;
        RegisterRuneMod("E_FireBeam_Core", RUNE_ENHANCE, enhMod);
    }

    // R_Meteor는 제거 — 낙하 중에는 큐브 메쉬 + Trail만 사용

    // ──────────────────────────────────────────────────────────────────────────
    // R_MeteorTrail — 불타는 파편 코어: 낙하 중 튀는 뜨거운 잔해들
    // halfAngle 65° + 높은 중력 → 파편이 사방으로 튀며 곡선으로 낙하
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer inner;
        inner.type           = EmitterType::Cone;
        inner.element        = ElementType::Fire;
        inner.overrideColors = true;
        inner.coreColor      = { 1.00f, 0.88f, 0.55f, 1.00f };  // 밝은 황금-오렌지 (뜨거운 파편)
        inner.edgeColor      = { 0.90f, 0.22f, 0.02f, 0.00f };  // 진한 주황-적 → 투명 소멸
        inner.particleCount  = 700;
        inner.speedMin       = 5.f;
        inner.speedMax       = 30.f;
        inner.lifetimeMin    = 0.25f;
        inner.lifetimeMax    = 0.65f;
        inner.sizeScale      = 2.8f;   // 0.35 * 2.8 * 1.5 ≈ 1.47 유닛
        inner.duration       = -1.f;
        inner.attachToProjectile = true;

        inner.cone.halfAngle     = 65.f;
        inner.cone.gravityScale  = 0.40f;
        inner.cone.startSizeMult = 1.5f;
        inner.cone.endSizeMult   = 0.08f;
        inner.cone.fadeAlpha     = true;
        inner.cone.fadeSize      = true;
        inner.cone.spawnRadius   = 3.0f;  // 큐브 크기(3.5) 이내 랜덤 위치에서 스폰

        EffectDef def;
        def.name    = "R_MeteorTrail";
        def.element = ElementType::Fire;
        def.layers.push_back(std::move(inner));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // R_MeteorTrailOuter — 잔불 스파크: 더 넓게, 더 작게 퍼지는 불씨들
    // halfAngle 82° + 강한 중력 → 큐브 주변 전방위로 작은 불씨 산란
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer outer;
        outer.type           = EmitterType::Cone;
        outer.element        = ElementType::Fire;
        outer.overrideColors = true;
        outer.coreColor      = { 1.00f, 0.50f, 0.08f, 0.85f };  // 주황 불씨
        outer.edgeColor      = { 0.50f, 0.03f, 0.00f, 0.00f };  // 어두운 적 → 투명
        outer.particleCount  = 500;
        outer.speedMin       = 3.f;
        outer.speedMax       = 20.f;
        outer.lifetimeMin    = 0.30f;
        outer.lifetimeMax    = 0.75f;
        outer.sizeScale      = 1.8f;   // 0.35 * 1.8 * 1.2 ≈ 0.76 유닛
        outer.duration       = -1.f;
        outer.attachToProjectile = true;

        outer.cone.halfAngle     = 82.f;
        outer.cone.gravityScale  = 0.50f;
        outer.cone.startSizeMult = 1.2f;
        outer.cone.endSizeMult   = 0.0f;
        outer.cone.fadeAlpha     = true;
        outer.cone.fadeSize      = true;
        outer.cone.spawnRadius   = 4.0f;  // 큐브 표면 너머까지 넓게 분산

        EffectDef def;
        def.name    = "R_MeteorTrailOuter";
        def.element = ElementType::Fire;
        def.layers.push_back(std::move(outer));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // R_MeteorImpact — 충격파 링 + 구형 폭발 + 화염 기둥 (3레이어)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "R_MeteorImpact";
        def.element = ElementType::Fire;

        // Layer 0 — 충격파 링: 빠르게 퍼지는 불꽃 고리
        {
            EffectLayer ring;
            ring.type           = EmitterType::Ring;
            ring.element        = ElementType::Fire;
            ring.overrideColors = true;
            ring.coreColor      = { 1.00f, 0.85f, 0.45f, 1.00f };
            ring.edgeColor      = { 0.90f, 0.15f, 0.00f, 0.00f };
            ring.particleCount  = 500;
            ring.duration       = 1.5f;
            ring.speedMin       = 5.f;
            ring.speedMax       = 22.f;
            ring.lifetimeMin    = 0.7f;
            ring.lifetimeMax    = 1.5f;
            ring.sizeScale      = 5.0f;  // 0.35 * 5.0 = 1.75 유닛/파티클

            ring.ring.radius         = 0.5f;
            ring.ring.width          = 4.0f;
            ring.ring.expandSpeed    = 40.f;
            ring.ring.tiltX          = 0.f;
            ring.ring.rotateSpeed    = 0.f;
            ring.ring.normalSpeedMin = 10.f;
            ring.ring.normalSpeedMax = 28.f;

            def.layers.push_back(ring);
        }

        // Layer 1 — 구형 폭발: 전방위 순간 팽창
        {
            EffectLayer burst;
            burst.type           = EmitterType::Sphere;
            burst.element        = ElementType::Fire;
            burst.overrideColors = true;
            burst.coreColor      = { 1.00f, 0.96f, 0.70f, 1.00f };
            burst.edgeColor      = { 1.00f, 0.35f, 0.00f, 0.30f };
            burst.particleCount  = 400;
            burst.duration       = 0.5f;
            burst.speedMin       = 30.f;
            burst.speedMax       = 70.f;
            burst.lifetimeMin    = 0.20f;
            burst.lifetimeMax    = 0.50f;
            burst.sizeScale      = 7.0f;  // 0.35 * 7.0 = 2.45 유닛/파티클

            burst.sphere.radius        = 1.5f;
            burst.sphere.shellFraction = 0.5f;
            burst.sphere.inward        = false;

            def.layers.push_back(burst);
        }

        // Layer 2 — 수직 화염 기둥
        {
            EffectLayer pillar;
            pillar.type           = EmitterType::Cone;
            pillar.element        = ElementType::Fire;
            pillar.overrideColors = true;
            pillar.coreColor      = { 1.00f, 0.75f, 0.20f, 1.00f };
            pillar.edgeColor      = { 0.80f, 0.08f, 0.00f, 0.00f };
            pillar.particleCount  = 350;
            pillar.duration       = 2.0f;
            pillar.emitRate       = 250.f;
            pillar.speedMin       = 18.f;
            pillar.speedMax       = 42.f;
            pillar.lifetimeMin    = 0.50f;
            pillar.lifetimeMax    = 1.10f;
            pillar.sizeScale      = 5.0f;  // 0.35 * 5.0 * 1.6 ≈ 2.8 유닛/파티클

            pillar.cone.halfAngle     = 22.f;
            pillar.cone.gravityScale  = 0.20f;
            pillar.cone.startSizeMult = 1.6f;
            pillar.cone.endSizeMult   = 0.15f;
            pillar.cone.fadeAlpha     = true;
            pillar.cone.fadeSize      = true;

            def.layers.push_back(pillar);
        }

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // R_MeteorGroundFire — 충돌 후 남는 잔불 (짧게)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "R_MeteorGroundFire";
        def.element = ElementType::Fire;

        EffectLayer cone;
        cone.type           = EmitterType::Cone;
        cone.element        = ElementType::Fire;
        cone.overrideColors = true;
        cone.coreColor      = { 1.00f, 0.50f, 0.10f, 1.00f };
        cone.edgeColor      = { 0.55f, 0.04f, 0.00f, 0.00f };
        cone.particleCount  = 300;
        cone.duration       = 2.5f;
        cone.emitRate       = 100.f;
        cone.speedMin       = 3.f;
        cone.speedMax       = 10.f;
        cone.lifetimeMin    = 0.8f;
        cone.lifetimeMax    = 2.5f;
        cone.sizeScale      = 4.0f;  // 0.35 * 4.0 * 1.4 ≈ 1.96 유닛/파티클

        cone.cone.halfAngle     = 50.f;
        cone.cone.gravityScale  = 0.35f;
        cone.cone.startSizeMult = 1.4f;
        cone.cone.endSizeMult   = 0.15f;
        cone.cone.fadeAlpha     = true;
        cone.cone.fadeSize      = true;

        def.layers.push_back(cone);
        Register(std::move(def));
    }

    // R_MeteorFrontFire는 제거 — Trail로 충분

    // ──────────────────────────────────────────────────────────────────────────
    // R_MeteorSmallTrail — 소형 메테오 낙하 Trail (R_MeteorTrail 축소판)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer inner;
        inner.type           = EmitterType::Cone;
        inner.element        = ElementType::Fire;
        inner.overrideColors = true;
        inner.coreColor      = { 1.00f, 0.80f, 0.40f, 1.00f };
        inner.edgeColor      = { 0.80f, 0.18f, 0.02f, 0.00f };
        inner.particleCount  = 250;
        inner.speedMin       = 4.f;
        inner.speedMax       = 18.f;
        inner.lifetimeMin    = 0.15f;
        inner.lifetimeMax    = 0.40f;
        inner.sizeScale      = 1.5f;
        inner.duration       = -1.f;
        inner.attachToProjectile = true;

        inner.cone.halfAngle     = 55.f;
        inner.cone.gravityScale  = 0.35f;
        inner.cone.startSizeMult = 1.3f;
        inner.cone.endSizeMult   = 0.06f;
        inner.cone.fadeAlpha     = true;
        inner.cone.fadeSize      = true;
        inner.cone.spawnRadius   = 1.5f;

        EffectDef def;
        def.name    = "R_MeteorSmallTrail";
        def.element = ElementType::Fire;
        def.layers.push_back(std::move(inner));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // R_MeteorSmallImpact — 소형 메테오 착지 폭발 (링 + 버스트)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "R_MeteorSmallImpact";
        def.element = ElementType::Fire;

        // Layer 0 — 작은 충격파 링
        {
            EffectLayer ring;
            ring.type           = EmitterType::Ring;
            ring.element        = ElementType::Fire;
            ring.overrideColors = true;
            ring.coreColor      = { 1.00f, 0.80f, 0.35f, 1.00f };
            ring.edgeColor      = { 0.80f, 0.12f, 0.00f, 0.00f };
            ring.particleCount  = 150;
            ring.duration       = 0.6f;
            ring.speedMin       = 4.f;
            ring.speedMax       = 14.f;
            ring.lifetimeMin    = 0.3f;
            ring.lifetimeMax    = 0.7f;
            ring.sizeScale      = 2.5f;

            ring.ring.radius         = 0.3f;
            ring.ring.width          = 2.0f;
            ring.ring.expandSpeed    = 20.f;
            ring.ring.tiltX          = 0.f;
            ring.ring.rotateSpeed    = 0.f;
            ring.ring.normalSpeedMin = 5.f;
            ring.ring.normalSpeedMax = 14.f;

            def.layers.push_back(ring);
        }

        // Layer 1 — 순간 방사 버스트
        {
            EffectLayer burst;
            burst.type           = EmitterType::Sphere;
            burst.element        = ElementType::Fire;
            burst.overrideColors = true;
            burst.coreColor      = { 1.00f, 0.90f, 0.55f, 1.00f };
            burst.edgeColor      = { 1.00f, 0.30f, 0.00f, 0.20f };
            burst.particleCount  = 120;
            burst.duration       = 0.3f;
            burst.speedMin       = 15.f;
            burst.speedMax       = 35.f;
            burst.lifetimeMin    = 0.15f;
            burst.lifetimeMax    = 0.35f;
            burst.sizeScale      = 3.5f;

            burst.sphere.radius        = 0.8f;
            burst.sphere.shellFraction = 0.4f;
            burst.sphere.inward        = false;

            def.layers.push_back(burst);
        }

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // RC_Fireball — 우클릭 화염구 투사체
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "RC_Fireball";
        def.element = ElementType::Fire;

        EffectLayer layer;
        layer.type       = EmitterType::SPH_Attract;
        layer.element    = ElementType::Fire;
        layer.overrideColors = true;
        layer.coreColor  = { 1.0f, 0.42f, 0.02f, 1.0f };
        layer.edgeColor  = { 0.80f, 0.05f, 0.00f, 0.92f };
        layer.sizeScale  = 1.f;
        layer.attachToProjectile = true;

        SPHEmitterParams& s = layer.sph;
        s.particleCount      = 420;
        s.spawnRadius        = 0.8f;
        s.maxParticleSpeed   = 6.0f;
        s.cardinalSpawnRadius = 4.0f;
        s.cardinalInwardSpeed = 0.0f;
        s.overridePhysics    = true;
        s.sphStiffness       = 30.0f;
        s.sphNearPressureMult = 0.5f;
        s.sphRestDensity     = 5.0f;
        s.sphViscosity       = 1.5f;
        s.sphSmoothingRadius = 1.3f;

        FluidCPDesc cp;
        cp.forwardBias        = 0.0f;
        cp.attractionStrength = 8.0f;
        cp.sphereRadius       = 1.2f;
        s.cpDescs.push_back(cp);

        VFXPhase p0;
        p0.startTime  = 0.f;
        p0.duration   = 99.f;
        p0.motionMode = ParticleMotionMode::ControlPoint;
        p0.offsetParticlesWithOrigin = true;
        s.phases.push_back(p0);

        def.layers.push_back(layer);
        Register(std::move(def));

        VFXModifier chgMod;
        chgMod.particleCountMult = 1.5f;
        chgMod.sizeScaleMult     = 1.3f;
        RegisterRuneMod("RC_Fireball", RUNE_CHARGE, chgMod);

        VFXModifier enhMod;
        enhMod.particleCountMult = 1.3f;
        enhMod.strengthMult      = 1.2f;
        RegisterRuneMod("RC_Fireball", RUNE_ENHANCE, enhMod);
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Dragon_MegaBreath — 보스 드래곤 메가 브레스 (Beam 모드, 대규모 파티클)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Fire);
        // 색상 오버라이드 미지정 — element 기본 색상 사용

        SPHEmitterParams& s = layer.sph;
        s.particleCount     = 4096;
        s.spawnRadius       = 8.0f;

        VFXPhase p0;
        p0.startTime  = 0.f;
        p0.duration   = 99.f;
        p0.motionMode = ParticleMotionMode::Beam;
        p0.beamDesc.speedMin      = 55.f;
        p0.beamDesc.speedMax      = 110.f;
        p0.beamDesc.spreadRadius  = 35.0f;
        p0.beamDesc.verticalScale = 0.15f;
        p0.randomSidewaysImpulse  = 25.0f;
        p0.globalGravityStrength  = 45.0f;
        s.phases.push_back(p0);

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "Dragon_MegaBreath";
        def.element = ElementType::Fire;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // 서브 파티클 VFX (룬 원소 강화 시 메인 옆에 추가 스폰)
    // ──────────────────────────────────────────────────────────────────────────

    // sub_water
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Water);
        layer.overrideColors = true;
        layer.coreColor = { 0.55f, 0.85f, 1.0f, 1.0f };
        layer.edgeColor = { 0.05f, 0.25f, 0.75f, 0.75f };

        SPHEmitterParams& s = layer.sph;
        s.particleCount        = 100;
        s.spawnRadius          = 2.0f;
        s.masterCPFallSpeed    = 0.f;
        s.masterCPStrength     = 18.f;
        s.masterCPSphereRadius = 2.5f;

        SatelliteCPDesc sat;
        sat.orbitRadius        = 3.0f;
        sat.orbitSpeed         = 1.2f;
        sat.orbitPhase         = 0.f;
        sat.verticalOffset     = 0.f;
        sat.attractionStrength = 10.f;
        sat.sphereRadius       = 1.2f;
        sat.orbitTiltX         = 0.5f;
        s.satelliteCPs.push_back(sat);

        // OrbitalCP phase 추가 (위성 CP 동작용)
        VFXPhase p0;
        p0.startTime  = 0.f;
        p0.duration   = 99.f;
        p0.motionMode = ParticleMotionMode::OrbitalCP;
        s.phases.push_back(p0);

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "sub_water";
        def.element = ElementType::Water;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_fire
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Fire);
        layer.overrideColors = true;
        layer.coreColor = { 1.0f, 0.75f, 0.05f, 1.0f };
        layer.edgeColor = { 0.9f, 0.20f, 0.0f,  0.80f };

        SPHEmitterParams& s = layer.sph;
        s.particleCount        = 80;
        s.spawnRadius          = 0.5f;
        s.masterCPFallSpeed    = 0.f;
        s.masterCPStrength     = 20.f;
        s.masterCPSphereRadius = 1.8f;
        s.cardinalSpawnRadius  = 2.5f;
        s.cardinalInwardSpeed  = 8.f;

        // 기본 OrbitalCP phase로 origin 추적 (서브 파티클은 메인 옆을 따라다님)
        VFXPhase p0;
        p0.startTime  = 0.f;
        p0.duration   = 99.f;
        p0.motionMode = ParticleMotionMode::OrbitalCP;
        s.phases.push_back(p0);

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "sub_fire";
        def.element = ElementType::Fire;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_earth
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Earth);
        layer.overrideColors = true;
        layer.coreColor = { 0.70f, 0.55f, 0.25f, 1.0f };
        layer.edgeColor = { 0.35f, 0.22f, 0.08f, 0.80f };

        SPHEmitterParams& s = layer.sph;
        s.particleCount        = 80;
        s.spawnRadius          = 2.5f;
        s.masterCPFallSpeed    = 0.f;
        s.masterCPStrength     = 22.f;
        s.masterCPSphereRadius = 2.0f;

        SatelliteCPDesc satA;
        satA.orbitRadius        = 2.5f; satA.orbitSpeed = 0.8f;
        satA.orbitPhase         = 0.f;  satA.verticalOffset = 0.3f;
        satA.attractionStrength = 12.f; satA.sphereRadius   = 1.0f;
        satA.orbitTiltX         = 0.3f;
        s.satelliteCPs.push_back(satA);
        SatelliteCPDesc satB = satA;
        satB.orbitPhase = 3.14159f; satB.verticalOffset = -0.3f;
        s.satelliteCPs.push_back(satB);

        VFXPhase p0;
        p0.startTime  = 0.f;
        p0.duration   = 99.f;
        p0.motionMode = ParticleMotionMode::OrbitalCP;
        s.phases.push_back(p0);

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "sub_earth";
        def.element = ElementType::Earth;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_wind
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Wind);
        layer.overrideColors = true;
        layer.coreColor = { 0.80f, 0.98f, 0.70f, 1.0f };
        layer.edgeColor = { 0.25f, 0.75f, 0.35f, 0.65f };

        SPHEmitterParams& s = layer.sph;
        s.particleCount        = 120;
        s.spawnRadius          = 2.0f;
        s.masterCPFallSpeed    = 0.f;
        s.masterCPStrength     = 15.f;
        s.masterCPSphereRadius = 2.8f;

        SatelliteCPDesc satA;
        satA.orbitRadius        = 3.2f; satA.orbitSpeed = 3.5f;
        satA.orbitPhase         = 0.f;  satA.verticalOffset = 0.f;
        satA.attractionStrength = 8.f;  satA.sphereRadius   = 1.3f;
        satA.orbitTiltX         = 1.2f; satA.precessionSpeed = 0.5f;
        s.satelliteCPs.push_back(satA);
        SatelliteCPDesc satB = satA;
        satB.orbitPhase = 3.14159f;
        s.satelliteCPs.push_back(satB);

        VFXPhase p0;
        p0.startTime  = 0.f;
        p0.duration   = 99.f;
        p0.motionMode = ParticleMotionMode::OrbitalCP;
        s.phases.push_back(p0);

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "sub_wind";
        def.element = ElementType::Wind;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Demon_Tornado — 4스테이지 데몬 회오리 (Linear 수직 emitter + swirl)
    //   light_emitter.hlsl 에 swirl rotation 추가 후 Linear 가 진짜 helix 흐름 생성
    //   [Outer]  넓은 반경 느린 swirl → 외곽 흙먼지
    //   [Inner]  좁은 반경 빠른 swirl → 코어 streak
    // ──────────────────────────────────────────────────────────────────────────
    {
    EffectDef def;
    def.name    = "Demon_Tornado";
    def.element = ElementType::Wind;

    auto BuildLinearSwirlLayer = [](float length, float width, float swirlSpeed,
                                     float speedMin, float speedMax,
                                     float lifetimeMin, float lifetimeMax,
                                     int particleCount, float sizeScale,
                                     const XMFLOAT4& core, const XMFLOAT4& edge) -> EffectLayer
    {
        EffectLayer layer;
        layer.type           = EmitterType::Linear;
        layer.element        = ElementType::Wind;
        layer.particleCount  = particleCount;
        layer.overrideColors = true;
        layer.coreColor      = core;
        layer.edgeColor      = edge;
        layer.sizeScale      = sizeScale;
        layer.speedMin       = speedMin;
        layer.speedMax       = speedMax;
        layer.lifetimeMin    = lifetimeMin;
        layer.lifetimeMax    = lifetimeMax;
        layer.duration       = -1.f;        // 무한
        layer.emitRate       = 0.f;         // 즉시 전부 (recycle 로 순환)

        layer.linear.length      = length;
        layer.linear.width       = width;
        layer.linear.recycleRate = 1.0f;
        layer.linear.swirlSpeed  = swirlSpeed;

        return layer;
    };

    // [Layer 0] Outer column — 외곽 옅은 민트 그린, 넓은 반경, 천천히 회전
    def.layers.push_back(BuildLinearSwirlLayer(
        12.0f /*length*/, 3.0f /*width*/, 5.0f /*swirl rad/s ~0.8 rev/s*/,
        4.0f, 8.0f /*speedMin/Max — 위로 흐름*/,
        2.0f, 3.5f /*lifetime — 길게 흐름 보임*/,
        800 /*particleCount*/, 1.4f /*sizeScale*/,
        XMFLOAT4(0.72f, 0.92f, 0.76f, 1.00f),   // 옅은 민트 그린 (코어)
        XMFLOAT4(0.30f, 0.55f, 0.35f, 0.30f))); // 살짝 짙은 그린 (엣지)

    // [Layer 1] Inner core — 좁은 반경, 빠른 회전, 화이트-민트
    def.layers.push_back(BuildLinearSwirlLayer(
        13.0f /*length 살짝 더 길게*/, 1.3f /*width — 좁음*/,
        9.5f /*swirl 더 빠름*/,
        6.0f, 11.0f /*speed 빠름*/,
        1.2f, 2.0f /*lifetime*/,
        500 /*particleCount*/, 0.9f /*size 작게*/,
        XMFLOAT4(0.92f, 1.00f, 0.92f, 1.00f),   // 밝은 화이트-민트 (코어)
        XMFLOAT4(0.55f, 0.85f, 0.62f, 0.55f))); // 부드러운 민트 (엣지)

    Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Demon_Tornado_Big — 배경용 거대 토네이도 (멀리 보이는 ambient)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Demon_Tornado_Big";
        def.element = ElementType::Wind;

        EffectLayer outer;
        outer.type           = EmitterType::Linear;
        outer.element        = ElementType::Wind;
        outer.particleCount  = 1000;
        outer.overrideColors = true;
        outer.coreColor      = { 0.70f, 0.90f, 0.74f, 1.0f };
        outer.edgeColor      = { 0.28f, 0.50f, 0.32f, 0.30f };
        outer.sizeScale      = 2.2f;
        outer.speedMin       = 5.f;
        outer.speedMax       = 10.f;
        outer.lifetimeMin    = 3.0f;
        outer.lifetimeMax    = 5.0f;
        outer.duration       = -1.f;
        outer.linear.length      = 28.0f;   // ~2.5x demon
        outer.linear.width       = 6.5f;    // ~2x demon
        outer.linear.recycleRate = 1.0f;
        outer.linear.swirlSpeed  = 4.0f;    // 좀 더 느림
        def.layers.push_back(outer);

        EffectLayer inner;
        inner.type           = EmitterType::Linear;
        inner.element        = ElementType::Wind;
        inner.particleCount  = 500;
        inner.overrideColors = true;
        inner.coreColor      = { 0.92f, 1.00f, 0.92f, 1.0f };
        inner.edgeColor      = { 0.55f, 0.85f, 0.62f, 0.55f };
        inner.sizeScale      = 1.4f;
        inner.speedMin       = 7.f;
        inner.speedMax       = 13.f;
        inner.lifetimeMin    = 2.0f;
        inner.lifetimeMax    = 3.5f;
        inner.duration       = -1.f;
        inner.linear.length      = 30.0f;
        inner.linear.width       = 2.8f;
        inner.linear.recycleRate = 1.0f;
        inner.linear.swirlSpeed  = 7.5f;
        def.layers.push_back(inner);

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Wind_UpdraftSmall — 작은 업드래프트 기둥 (맵 곳곳 고정 배치용)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Wind_UpdraftSmall";
        def.element = ElementType::Wind;

        EffectLayer up;
        up.type           = EmitterType::Linear;
        up.element        = ElementType::Wind;
        up.particleCount  = 200;
        up.overrideColors = true;
        up.coreColor      = { 0.85f, 0.97f, 0.88f, 1.0f };
        up.edgeColor      = { 0.45f, 0.75f, 0.55f, 0.40f };
        up.sizeScale      = 1.0f;
        up.speedMin       = 5.f;
        up.speedMax       = 9.f;
        up.lifetimeMin    = 1.5f;
        up.lifetimeMax    = 2.5f;
        up.duration       = -1.f;
        up.linear.length      = 8.0f;
        up.linear.width       = 1.0f;
        up.linear.recycleRate = 1.0f;
        up.linear.swirlSpeed  = 3.5f;
        def.layers.push_back(up);

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Wind_DriftLeaves — 수평 드리프트 (맵 가로질러 떠다니는 잎/먼지)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Wind_DriftLeaves";
        def.element = ElementType::Wind;

        EffectLayer drift;
        drift.type           = EmitterType::Linear;
        drift.element        = ElementType::Wind;
        drift.particleCount  = 180;            // 인스턴스당 수 줄이고 인스턴스 수 늘림
        drift.overrideColors = true;
        drift.coreColor      = { 0.78f, 0.92f, 0.55f, 1.0f };  // 옅은 잎녹색
        drift.edgeColor      = { 0.38f, 0.55f, 0.20f, 0.45f };
        drift.sizeScale      = 1.1f;
        drift.speedMin       = 2.5f;
        drift.speedMax       = 5.0f;
        drift.lifetimeMin    = 8.0f;
        drift.lifetimeMax    = 14.0f;
        drift.duration       = -1.f;
        drift.linear.length      = 160.0f;
        drift.linear.width       = 50.0f;      // 너비 살짝 키움
        drift.linear.recycleRate = 1.0f;
        drift.linear.swirlSpeed  = 1.2f;
        def.layers.push_back(drift);

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Wind_TornadoWarning — 큰 토네이도 등장 직전 바닥 경고 링 (2s 펄스)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Wind_TornadoWarning";
        def.element = ElementType::Wind;

        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = ElementType::Wind;
        ring.particleCount  = 250;
        ring.overrideColors = true;
        // 빨강-주황 경고색 — ambient 민트와 분명히 구분
        ring.coreColor      = { 1.00f, 0.35f, 0.10f, 1.00f };
        ring.edgeColor      = { 0.70f, 0.10f, 0.00f, 0.40f };
        ring.sizeScale      = 2.5f;
        ring.speedMin       = 0.5f;
        ring.speedMax       = 1.8f;
        ring.lifetimeMin    = 0.6f;
        ring.lifetimeMax    = 1.0f;
        ring.duration       = 2.2f;        // warning 페이즈와 매칭 (2s + 약간)
        ring.emitRate       = 150.f;

        ring.ring.radius         = 5.5f;   // tornado outer width 와 매칭
        ring.ring.width          = 0.8f;
        ring.ring.expandSpeed    = 1.5f;   // 천천히 확장
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 8.0f;   // 빠른 회전 (긴급함)
        ring.ring.normalSpeedMin = 0.3f;
        ring.ring.normalSpeedMax = 1.2f;

        def.layers.push_back(ring);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Portal_Ring — 인터랙션 큐브 포탈 회전 링 (보라 자기장, 무한 루프)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Portal_Ring";
        def.element = ElementType::Wind; // 가장 가벼운 element 재사용

        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = ElementType::Wind;
        ring.particleCount  = 220;
        ring.overrideColors = true;
        // 라벤더 코어 + 딥 바이올렛 엣지 — 디스크와 같은 보라 톤으로 통일
        ring.coreColor      = { 1.00f, 0.80f, 0.95f, 0.95f };
        ring.edgeColor      = { 0.50f, 0.25f, 0.85f, 0.50f };
        ring.sizeScale      = 1.4f;
        ring.speedMin       = 0.3f;
        ring.speedMax       = 0.9f;
        ring.lifetimeMin    = 1.2f;
        ring.lifetimeMax    = 2.0f;
        ring.duration       = 9999.f;       // 무한 루프 (포탈 활성 동안 계속)
        ring.emitRate       = 120.f;

        ring.ring.radius         = 8.5f;     // 디스크 반경 9 와 매칭 (외곽 림 위에 입자)
        ring.ring.width          = 1.0f;
        ring.ring.expandSpeed    = 0.0f;     // 제자리 회전
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 3.0f;     // 회전 속도 약간 낮춤 (반경 커져서 외형상 속도는 유지)
        ring.ring.normalSpeedMin = 0.2f;
        ring.ring.normalSpeedMax = 0.8f;     // 살짝 위로 떠오르는 입자

        def.layers.push_back(ring);
        Register(std::move(def));
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // ▌ 물결술사 스킬 VFX
    // ═══════════════════════════════════════════════════════════════════════════

    // ──────────────────────────────────────────────────────────────────────────
    // Q_WaterWave — 넓은 물 파도 (선명한 틸/시안, 맑은 물 느낌)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Water);
        layer.overrideColors = true;
        layer.coreColor = { 0.00f, 0.78f, 0.88f, 1.0f };   // 선명한 시안/틸
        layer.edgeColor = { 0.00f, 0.28f, 0.65f, 0.80f };   // 짙은 파란 테두리
        layer.useSSF    = true;

        SPHEmitterParams& s    = layer.sph;
        s.particleCount        = 700;
        s.spawnRadius          = 1.2f;
        s.isWave               = true;
        s.waveSpeed            = 8.f;
        s.wavePushForce        = 45.f;
        s.waveMaxDist          = 18.f;
        s.waveHalfW            = 7.0f;
        s.waveHalfH            = 2.5f;
        s.waveOscAmplitude     = 15.f;
        s.waveOscFrequency     = 4.f;
        s.waveOscWaveNumber    = 0.6f;
        s.maxParticleSpeed     = 18.f;
        s.overridePhysics      = true;
        s.sphStiffness         = 18.f;
        s.sphNearPressureMult  = 0.5f;
        s.sphRestDensity       = 0.0f;
        s.sphViscosity         = 0.5f;
        s.sphSmoothingRadius   = 1.8f;

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "Q_WaterWave";
        def.element = ElementType::Water;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));

        VFXModifier enhMod; enhMod.particleCountMult = 1.5f; enhMod.sizeScaleMult = 1.2f;
        RegisterRuneMod("Q_WaterWave", RUNE_ENHANCE, enhMod);
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Q_WaterFall — 공중에서 쏟아지는 물기둥 (Cone 이미터, 아래 방향)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer fall;
        fall.type           = EmitterType::Cone;
        fall.element        = ElementType::Water;
        fall.overrideColors = true;
        fall.coreColor      = { 0.55f, 0.90f, 1.0f, 1.0f };
        fall.edgeColor      = { 0.10f, 0.50f, 0.90f, 0.0f };
        fall.particleCount  = 280;
        fall.speedMin       = 10.f;
        fall.speedMax       = 24.f;
        fall.lifetimeMin    = 0.25f;
        fall.lifetimeMax    = 0.60f;
        fall.sizeScale      = 2.2f;
        fall.duration       = -1.f;   // 행동에서 수동 종료 (FALL_DURATION 후 StopEffect)

        fall.cone.halfAngle     = 20.f;   // 좁은 집중 낙하 기둥
        fall.cone.gravityScale  = 0.35f;
        fall.cone.startSizeMult = 1.0f;
        fall.cone.endSizeMult   = 0.12f;
        fall.cone.fadeAlpha     = true;
        fall.cone.fadeSize      = true;
        fall.cone.spawnRadius   = 1.0f;   // 낙하 시작점 분산

        EffectDef def;
        def.name    = "Q_WaterFall";
        def.element = ElementType::Water;
        def.layers.push_back(std::move(fall));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Q_WaterPuddle — 유체 웅덩이 2페이즈
    // Phase0: 박스 없음 → 뭉친 채 자유낙하
    // Phase1: 바닥 슬랩(y=0) 활성 → 낙하 운동량 + SPH 압력으로 XZ 폭발 확산
    // origin=(x,2.5,z) + dir=(0,-1,0) → 박스 center=(x,0,z)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Water);
        layer.overrideColors = true;
        layer.coreColor = { 0.20f, 0.68f, 1.0f, 1.0f };
        layer.edgeColor = { 0.04f, 0.22f, 0.62f, 0.65f };
        layer.useSSF    = true;

        SPHEmitterParams& s     = layer.sph;
        s.particleCount         = 750;
        s.spawnRadius           = 1.5f;    // 뭉친 물방울처럼 작게 스폰
        s.masterCPFallSpeed     = 0.f;
        s.masterCPStrength      = 0.f;
        s.masterCPSphereRadius  = 0.f;
        s.maxParticleSpeed      = 30.f;  // 기본 12.0f → SPH 확산 속도 허용

        s.overridePhysics       = true;
        s.sphStiffness          = 80.f;    // 충돌 시 폭발적 반발
        s.sphNearPressureMult   = 4.0f;
        s.sphRestDensity        = 0.5f;    // 항상 밀려나려는 상태
        s.sphViscosity          = 0.05f;   // 거의 점성 없음 → 자유 확산
        s.sphSmoothingRadius    = 3.0f;

        // Phase 0: 박스 없이 자유낙하 (뭉친 물방울)
        VFXPhase p0;
        p0.startTime  = 0.f;
        p0.duration   = 0.7f;
        p0.motionMode = ParticleMotionMode::Gravity;
        p0.gravityDesc.gravity         = { 0.f, -22.f, 0.f };
        p0.gravityDesc.initialSpeedMin = 0.f;
        p0.gravityDesc.initialSpeedMax = 0.f;
        p0.boxDesc.active      = false;
        p0.boxDesc.halfExtents = { 50.f, 50.f, 0.5f }; // lerp 시작점 = Phase 1과 동일, XZ 충분히 크게 → 원형 확산
        s.phases.push_back(p0);

        // Phase 1: 바닥 슬랩 활성 → 충격 운동량 + SPH로 폭발 확산
        VFXPhase p1;
        p1.startTime  = 0.7f;
        p1.duration   = 99.f;
        p1.motionMode = ParticleMotionMode::Gravity;
        p1.gravityDesc.gravity         = { 0.f, -6.f, 0.f };  // 가벼운 중력으로 바닥 유지
        p1.gravityDesc.initialSpeedMin = 0.f;
        p1.gravityDesc.initialSpeedMax = 0.f;
        p1.boxDesc.active      = true;
        p1.boxDesc.halfExtents = { 50.f, 50.f, 0.5f }; // XZ 사실상 무한 → 파티클이 벽에 안 닿아 원형 유지
        s.phases.push_back(p1);

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "Q_WaterPuddle";
        def.element = ElementType::Water;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // E_WaterVortex — 물 소용돌이 (OrbitalCP, 3개 위성이 안쪽으로 나선형)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Water);
        layer.overrideColors = true;
        layer.coreColor = { 0.45f, 0.80f, 1.0f, 1.0f };
        layer.edgeColor = { 0.05f, 0.30f, 0.75f, 0.80f };
        layer.useSSF    = true;

        SPHEmitterParams& s     = layer.sph;
        s.particleCount         = 500;
        s.spawnRadius           = 3.0f;
        s.masterCPFallSpeed     = 0.f;   // 장판 스킬 — 낙하 없음
        s.masterCPStrength      = 20.f;
        s.masterCPSphereRadius  = 3.5f;

        auto makeSat = [](float r, float spd, float phase, float tilt) {
            SatelliteCPDesc sat;
            sat.orbitRadius = r; sat.orbitSpeed = spd; sat.orbitPhase = phase;
            sat.verticalOffset = 0.f; sat.attractionStrength = 12.f;
            sat.sphereRadius = 1.4f; sat.orbitTiltX = tilt;
            sat.breatheAmplitude = 0.3f; sat.breatheSpeed = 1.5f;
            return sat;
        };
        s.satelliteCPs.push_back(makeSat(3.5f, 4.0f, 0.f,      0.4f));
        s.satelliteCPs.push_back(makeSat(3.5f, 4.0f, 2.094f,   0.4f));
        s.satelliteCPs.push_back(makeSat(3.5f, 4.0f, 4.189f,   0.4f));

        VFXPhase p0;
        p0.startTime  = 0.f;
        p0.duration   = 99.f;
        p0.motionMode = ParticleMotionMode::OrbitalCP;
        s.phases.push_back(p0);

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "E_WaterVortex";
        def.element = ElementType::Water;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // R_TidalWave — 거대 해일 (다크 네이비 본체 + 흰 거품 테두리, 심해 쓰나미)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Water);
        layer.overrideColors = true;
        layer.coreColor = { 0.02f, 0.06f, 0.38f, 1.0f };   // 다크 네이비 (파도 본체)
        layer.edgeColor = { 0.68f, 0.92f, 1.0f, 0.95f };   // 흰 거품 (edge/얇은 부분)
        layer.useSSF    = true;

        SPHEmitterParams& s    = layer.sph;
        s.particleCount        = 1400;
        s.spawnRadius          = 2.5f;
        s.isWave               = true;
        s.waveSpeed            = 8.f;
        s.wavePushForce        = 65.f;
        s.waveMaxDist          = 35.f;
        s.waveHalfW            = 16.0f;
        s.waveHalfH            = 6.5f;   // 더 높은 파도
        s.waveOscAmplitude     = 30.f;   // 더 격렬한 진동
        s.waveOscFrequency     = 2.5f;
        s.waveOscWaveNumber    = 0.3f;
        s.maxParticleSpeed     = 20.f;
        s.overridePhysics      = true;
        s.sphStiffness         = 14.f;
        s.sphNearPressureMult  = 0.35f;
        s.sphRestDensity       = 0.0f;
        s.sphViscosity         = 0.5f;
        s.sphSmoothingRadius   = 2.5f;   // 입자 크기 증가 → 더 묵직해 보임

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "R_TidalWave";
        def.element = ElementType::Water;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // R_TidalWave_Foam — 해일 선두에서 위로 분출하는 흰 거품 (TidalWaveBehavior::DropFoam 사용)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "R_TidalWave_Foam";
        def.element = ElementType::Water;

        EffectLayer foam;
        foam.type           = EmitterType::Cone;
        foam.element        = ElementType::Water;
        foam.overrideColors = true;
        foam.coreColor      = { 1.0f, 1.0f, 1.0f, 0.92f };   // 흰 포말
        foam.edgeColor      = { 0.55f, 0.88f, 1.0f, 0.0f };   // 투명 시안 에지
        foam.particleCount  = 90;
        foam.duration       = 0.35f;   // 짧은 분출
        foam.sizeScale      = 2.6f;
        foam.speedMin       = 4.f;  foam.speedMax  = 12.f;
        foam.lifetimeMin    = 0.3f; foam.lifetimeMax = 0.65f;
        foam.cone.halfAngle     = 55.f;   // 위로 퍼지는 분무
        foam.cone.gravityScale  = 0.10f;
        foam.cone.startSizeMult = 1.3f;
        foam.cone.endSizeMult   = 0.0f;
        foam.cone.fadeAlpha     = true;
        foam.cone.fadeSize      = true;
        def.layers.push_back(foam);
        Register(std::move(def));
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // ▌ 바람술사 스킬 VFX
    // ═══════════════════════════════════════════════════════════════════════════

    // ──────────────────────────────────────────────────────────────────────────
    // Q_WindCutter — 좁고 빠른 진공 커터 (Wave 모드, halfW=2 좁음)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Wind);
        layer.overrideColors = true;
        layer.coreColor = { 0.90f, 1.0f,  0.92f, 1.0f };
        layer.edgeColor = { 0.40f, 0.85f, 0.50f, 0.70f };
        layer.useSSF    = false;  // 바람 커터는 빌보드 렌더 (얇고 빠름)

        SPHEmitterParams& s    = layer.sph;
        s.particleCount        = 380;
        s.spawnRadius          = 0.5f;
        s.isWave               = true;
        s.waveSpeed            = 20.f;
        s.wavePushForce        = 80.f;
        s.waveMaxDist          = 22.f;
        s.waveHalfW            = 2.0f;
        s.waveHalfH            = 2.0f;
        s.waveOscAmplitude     = 0.f;   // 직선 커터 — 진동 없음
        s.maxParticleSpeed     = 35.f;
        s.overridePhysics      = true;
        s.sphStiffness         = 30.f;
        s.sphNearPressureMult  = 0.4f;
        s.sphRestDensity       = 0.0f;
        s.sphViscosity         = 0.2f;
        s.sphSmoothingRadius   = 1.2f;

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "Q_WindCutter";
        def.element = ElementType::Wind;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // E_GaleRush_Cone — 전방 폭풍 콘 (Cone 타입)
    // E_GaleRush_Ring — 충격파 링 (Ring 타입)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "E_GaleRush_Cone";
        def.element = ElementType::Wind;

        EffectLayer cone;
        cone.type           = EmitterType::Cone;
        cone.element        = ElementType::Wind;
        cone.overrideColors = true;
        cone.coreColor      = { 0.88f, 1.0f,  0.88f, 1.0f  };
        cone.edgeColor      = { 0.35f, 0.80f, 0.40f, 0.0f  };
        cone.particleCount  = 600;
        cone.speedMin       = 20.f; cone.speedMax = 50.f;
        cone.lifetimeMin    = 0.2f; cone.lifetimeMax = 0.5f;
        cone.sizeScale      = 2.2f;
        cone.duration       = 0.5f;

        cone.cone.halfAngle     = 50.f;
        cone.cone.gravityScale  = 0.05f;
        cone.cone.startSizeMult = 1.4f;
        cone.cone.endSizeMult   = 0.0f;
        cone.cone.fadeAlpha     = true;
        cone.cone.fadeSize      = true;

        def.layers.push_back(cone);
        Register(std::move(def));
    }
    {
        EffectDef def;
        def.name    = "E_GaleRush_Ring";
        def.element = ElementType::Wind;

        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = ElementType::Wind;
        ring.overrideColors = true;
        ring.coreColor      = { 0.85f, 1.0f,  0.85f, 0.85f };
        ring.edgeColor      = { 0.30f, 0.75f, 0.35f, 0.0f  };
        ring.particleCount  = 200;
        ring.duration       = 0.6f;
        ring.speedMin       = 2.f; ring.speedMax = 5.f;
        ring.lifetimeMin    = 0.3f; ring.lifetimeMax = 0.6f;
        ring.sizeScale      = 2.5f;

        ring.ring.radius         = 0.5f;
        ring.ring.width          = 2.0f;
        ring.ring.expandSpeed    = 25.f;
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 2.0f;
        ring.ring.normalSpeedMin = 2.f;
        ring.ring.normalSpeedMax = 6.f;

        def.layers.push_back(ring);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Demon_RushWind — 돌진 궤적 양옆 평행 트레일 단위 (behaviors 가 좌+우 두 개 스폰)
    //   Spawn pos = 보스 위치 ± perpendicular * offset (좌/우 오프셋)
    //   Spawn dir = 진행 방향의 **반대** (뒤로 흘러 보스 지나간 자리에 잔류)
    //   → 보스가 직진할 때 양옆에 평행한 두 줄의 바람 트레일이 깔림.
    //   좁은 콘 + 긴 lifetime + 느린 속도 = 길게 늘어지는 wake.
    // ──────────────────────────────────────────────────────────────────────────
    // 트레일 도달거리 = (rushSpeed + driftSpeed) × lifetime
    //   RushFront 최대 ~44u (긴 돌진), FixatedCharge 최대 110u → 두 케이스 다 커버하도록
    //   _Big 쪽을 더 길게 잡음.
    {
        EffectDef def;
        def.name    = "Demon_RushWind";
        def.element = ElementType::Wind;

        EffectLayer cone;
        cone.type           = EmitterType::Cone;
        cone.element        = ElementType::Wind;
        cone.overrideColors = true;
        cone.coreColor      = { 0.95f, 1.00f, 0.95f, 1.0f  };
        cone.edgeColor      = { 0.30f, 0.70f, 0.40f, 0.0f  };
        cone.particleCount  = 500;
        cone.speedMin       = 25.f; cone.speedMax = 45.f;
        cone.lifetimeMin    = 0.7f;  cone.lifetimeMax = 1.4f;   // (42+45)*1.4 = ~120u 도달
        cone.sizeScale      = 2.4f;
        cone.duration       = -1.f;

        cone.cone.halfAngle     = 9.f;      // 좀 더 좁게 — 라인 더 또렷
        cone.cone.gravityScale  = 0.02f;
        cone.cone.startSizeMult = 1.5f;
        cone.cone.endSizeMult   = 0.0f;
        cone.cone.fadeAlpha     = true;
        cone.cone.fadeSize      = true;

        def.layers.push_back(cone);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Demon_RushWind_Big — 강화 평행 트레일 (FixatedCharge / 페이즈2)
    //   FixatedCharge 110u 풀 커버 목표: (58+60)*2.0 = ~236u 여유
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Demon_RushWind_Big";
        def.element = ElementType::Wind;

        EffectLayer cone;
        cone.type           = EmitterType::Cone;
        cone.element        = ElementType::Wind;
        cone.overrideColors = true;
        cone.coreColor      = { 0.98f, 1.00f, 0.95f, 1.0f  };
        cone.edgeColor      = { 0.25f, 0.65f, 0.35f, 0.0f  };
        cone.particleCount  = 850;
        cone.speedMin       = 38.f; cone.speedMax = 60.f;
        cone.lifetimeMin    = 1.1f;  cone.lifetimeMax = 2.0f;
        cone.sizeScale      = 3.0f;
        cone.duration       = -1.f;

        cone.cone.halfAngle     = 10.f;
        cone.cone.gravityScale  = 0.02f;
        cone.cone.startSizeMult = 1.7f;
        cone.cone.endSizeMult   = 0.0f;
        cone.cone.fadeAlpha     = true;
        cone.cone.fadeSize      = true;

        def.layers.push_back(cone);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // R_TornadoPlayer — 플레이어 소환 토네이도 (Demon_Tornado 강화판, 더 크고 강렬)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "R_TornadoPlayer";
        def.element = ElementType::Wind;

        // [Outer] 넓은 외곽 선회
        {
            EffectLayer outer;
            outer.type           = EmitterType::Linear;
            outer.element        = ElementType::Wind;
            outer.particleCount  = 900;
            outer.overrideColors = true;
            outer.coreColor      = { 0.75f, 0.95f, 0.78f, 1.0f };
            outer.edgeColor      = { 0.30f, 0.60f, 0.38f, 0.30f };
            outer.sizeScale      = 1.8f;
            outer.speedMin       = 5.f;  outer.speedMax  = 10.f;
            outer.lifetimeMin    = 2.2f; outer.lifetimeMax = 3.8f;
            outer.duration       = -1.f;
            outer.linear.length      = 18.0f;
            outer.linear.width       = 4.5f;
            outer.linear.recycleRate = 1.0f;
            outer.linear.swirlSpeed  = 6.0f;
            def.layers.push_back(outer);
        }
        // [Inner] 좁고 빠른 코어
        {
            EffectLayer inner;
            inner.type           = EmitterType::Linear;
            inner.element        = ElementType::Wind;
            inner.particleCount  = 550;
            inner.overrideColors = true;
            inner.coreColor      = { 0.95f, 1.0f,  0.95f, 1.0f };
            inner.edgeColor      = { 0.55f, 0.88f, 0.62f, 0.55f };
            inner.sizeScale      = 1.0f;
            inner.speedMin       = 8.f;  inner.speedMax  = 14.f;
            inner.lifetimeMin    = 1.3f; inner.lifetimeMax = 2.2f;
            inner.duration       = -1.f;
            inner.linear.length      = 20.0f;
            inner.linear.width       = 1.8f;
            inner.linear.recycleRate = 1.0f;
            inner.linear.swirlSpeed  = 11.0f;
            def.layers.push_back(inner);
        }
        Register(std::move(def));
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // ▌ 대지술사 스킬 VFX
    // ═══════════════════════════════════════════════════════════════════════════

    // ──────────────────────────────────────────────────────────────────────────
    // Q_StoneSpike — 바위 기둥 단위 VFX (Burst + Cone)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Q_StoneSpike";
        def.element = ElementType::Earth;

        // 파편 폭발
        {
            EffectLayer burst;
            burst.type           = EmitterType::Cone;
            burst.element        = ElementType::Earth;
            burst.overrideColors = true;
            burst.coreColor      = { 0.75f, 0.60f, 0.28f, 1.0f };
            burst.edgeColor      = { 0.45f, 0.30f, 0.10f, 0.0f };
            burst.particleCount  = 280;
            burst.duration       = 0.6f;
            burst.speedMin       = 8.f;  burst.speedMax  = 28.f;
            burst.lifetimeMin    = 0.3f; burst.lifetimeMax = 0.8f;
            burst.sizeScale      = 2.8f;

            burst.cone.halfAngle     = 75.f;
            burst.cone.gravityScale  = 0.55f;
            burst.cone.startSizeMult = 1.5f;
            burst.cone.endSizeMult   = 0.05f;
            burst.cone.fadeAlpha     = true;
            burst.cone.fadeSize      = true;
            def.layers.push_back(burst);
        }
        // 먼지 기둥
        {
            EffectLayer pillar;
            pillar.type           = EmitterType::Cone;
            pillar.element        = ElementType::Earth;
            pillar.overrideColors = true;
            pillar.coreColor      = { 0.68f, 0.55f, 0.35f, 0.85f };
            pillar.edgeColor      = { 0.35f, 0.22f, 0.08f, 0.0f  };
            pillar.particleCount  = 180;
            pillar.duration       = 1.2f;
            pillar.emitRate       = 200.f;
            pillar.speedMin       = 5.f;  pillar.speedMax  = 15.f;
            pillar.lifetimeMin    = 0.5f; pillar.lifetimeMax = 1.3f;
            pillar.sizeScale      = 3.5f;

            pillar.cone.halfAngle     = 20.f;
            pillar.cone.gravityScale  = 0.15f;
            pillar.cone.startSizeMult = 1.2f;
            pillar.cone.endSizeMult   = 0.1f;
            pillar.cone.fadeAlpha     = true;
            pillar.cone.fadeSize      = true;
            def.layers.push_back(pillar);
        }
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // R_Earthquake_Burst — 지진 초기 중심 폭발 (Sphere 방사)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "R_Earthquake_Burst";
        def.element = ElementType::Earth;

        EffectLayer burst;
        burst.type           = EmitterType::Sphere;
        burst.element        = ElementType::Earth;
        burst.overrideColors = true;
        burst.coreColor      = { 0.80f, 0.65f, 0.30f, 1.0f };
        burst.edgeColor      = { 0.50f, 0.30f, 0.10f, 0.3f };
        burst.particleCount  = 350;
        burst.duration       = 0.4f;
        burst.speedMin       = 20.f; burst.speedMax  = 55.f;
        burst.lifetimeMin    = 0.3f; burst.lifetimeMax = 0.7f;
        burst.sizeScale      = 5.5f;

        burst.sphere.radius        = 1.0f;
        burst.sphere.shellFraction = 0.4f;
        burst.sphere.inward        = false;

        def.layers.push_back(burst);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // R_Earthquake_Ring — 지진 충격파 링 (반복 스폰)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "R_Earthquake_Ring";
        def.element = ElementType::Earth;

        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = ElementType::Earth;
        ring.overrideColors = true;
        ring.coreColor      = { 0.85f, 0.70f, 0.35f, 1.0f };
        ring.edgeColor      = { 0.55f, 0.32f, 0.10f, 0.0f };
        ring.particleCount  = 350;
        ring.duration       = 1.8f;
        ring.speedMin       = 3.f; ring.speedMax  = 10.f;
        ring.lifetimeMin    = 0.5f; ring.lifetimeMax = 1.2f;
        ring.sizeScale      = 4.5f;

        ring.ring.radius         = 0.5f;
        ring.ring.width          = 3.0f;
        ring.ring.expandSpeed    = 30.f;
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 1.0f;
        ring.ring.normalSpeedMin = 5.f;
        ring.ring.normalSpeedMax = 18.f;

        def.layers.push_back(ring);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Portal_Suction — 외곽에서 중심으로 빨려들어가는 흡입 입자 (차원문 인력 연출)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Portal_Suction";
        def.element = ElementType::Wind;

        EffectLayer suck;
        suck.type           = EmitterType::Sphere;
        suck.element        = ElementType::Wind;
        suck.particleCount  = 70;            // 약간 늘림 — 흐름이 너무 빈약하지 않게
        suck.overrideColors = true;
        suck.coreColor      = { 1.00f, 0.75f, 0.95f, 0.80f };  // 라벤더
        suck.edgeColor      = { 0.30f, 0.10f, 0.60f, 0.0f };   // 딥 바이올렛 페이드
        suck.sizeScale      = 0.45f;
        suck.speedMin       = 1.5f;          // ↓ 느리게 — 빨려들어가는 속도가 자연스러워짐
        suck.speedMax       = 2.8f;
        suck.lifetimeMin    = 1.2f;          // ↑ 길게 — trail 이 더 부드럽게 보임
        suck.lifetimeMax    = 1.8f;
        suck.duration       = 9999.f;
        suck.emitRate       = 40.f;

        suck.sphere.radius        = 8.5f;    // 디스크 외곽 직전 (반경 9)
        suck.sphere.shellFraction = 1.0f;
        suck.sphere.inward        = true;
        suck.sphere.rotationSpeed = 0.5f;    // 부드러운 나선 (큰 반경에 맞춰 약간 느리게)

        def.layers.push_back(suck);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Portal_Beam — 디스크에서 위로 솟는 수직 광주 (멀리서도 인지)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Portal_Beam";
        def.element = ElementType::Wind;

        // Linear emitter — 위로 솟는 빛줄기 + swirl(공전) 으로 토네이도/소용돌이 형태
        // recycle 켜져 있어 위쪽 끝 도달 시 바닥에서 재스폰 → 끊김 없는 흐름
        EffectLayer beam;
        beam.type           = EmitterType::Linear;
        beam.element        = ElementType::Wind;
        beam.particleCount  = 90;            // 토네이도 흐름은 입자 수 필요
        beam.overrideColors = true;
        beam.coreColor      = { 1.00f, 0.80f, 0.95f, 0.85f }; // 라벤더
        beam.edgeColor      = { 0.40f, 0.15f, 0.75f, 0.0f };  // 딥 바이올렛 페이드
        beam.sizeScale      = 0.5f;
        beam.speedMin       = 1.8f;
        beam.speedMax       = 3.0f;
        beam.lifetimeMin    = 1.8f;
        beam.lifetimeMax    = 2.6f;
        beam.duration       = 9999.f;
        beam.emitRate       = 50.f;

        beam.linear.length      = 7.5f;     // 디스크 위로 7.5u 솟음 (디스크 커져서 비례)
        beam.linear.width       = 2.2f;     // 토네이도 둘레 반경
        beam.linear.recycleRate = 1.0f;     // 끝 도달 후 재스폰 (지속 흐름)
        beam.linear.swirlSpeed  = 3.0f;     // 회전 — 진짜 소용돌이 느낌

        def.layers.push_back(beam);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Wind_GroundRipple — 바닥 풍압 ripple (주기 spawn)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Wind_GroundRipple";
        def.element = ElementType::Wind;

        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = ElementType::Wind;
        ring.particleCount  = 80;
        ring.overrideColors = true;
        ring.coreColor      = { 0.85f, 0.98f, 0.88f, 0.7f };
        ring.edgeColor      = { 0.50f, 0.78f, 0.55f, 0.0f };
        ring.sizeScale      = 1.5f;
        ring.speedMin       = 0.5f;
        ring.speedMax       = 1.5f;
        ring.lifetimeMin    = 1.0f;
        ring.lifetimeMax    = 1.6f;
        ring.duration       = 1.6f;            // 짧게 끝남

        ring.ring.radius         = 1.5f;
        ring.ring.width          = 0.4f;
        ring.ring.expandSpeed    = 8.0f;       // 빠르게 외곽으로 확장
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 0.f;
        ring.ring.normalSpeedMin = 0.2f;
        ring.ring.normalSpeedMax = 0.8f;

        def.layers.push_back(ring);
        Register(std::move(def));
    }
}

void EffectRegistry::Register(EffectDef def) {
    m_Effects[def.name] = std::move(def);
}

void EffectRegistry::RegisterRuneMod(const std::string& name, uint32_t runeFlag, VFXModifier mod) {
    m_RuneMods[name][runeFlag] = mod;
}

EffectDef EffectRegistry::GetEffect(const std::string& name, uint32_t runeFlags) const
{
    auto it = m_Effects.find(name);
    if (it == m_Effects.end()) return {};

    EffectDef result = it->second;

    auto modsIt = m_RuneMods.find(name);
    if (modsIt != m_RuneMods.end())
        result = ApplyMods(result, modsIt->second, runeFlags);

    return result;
}

bool EffectRegistry::HasEffect(const std::string& name) const {
    return m_Effects.count(name) > 0;
}

EffectDef EffectRegistry::ApplyMods(EffectDef def,
    const std::unordered_map<uint32_t, VFXModifier>& mods,
    uint32_t runeFlags) const
{
    for (const auto& [flag, mod] : mods) {
        if (!(runeFlags & flag)) continue;

        for (auto& layer : def.layers) {
            layer.sizeScale *= mod.sizeScaleMult;

            bool isSPH = (layer.type >= EmitterType::SPH_Attract &&
                          layer.type <= EmitterType::SPH_Beam);

            if (isSPH) {
                layer.sph.particleCount = int(layer.sph.particleCount * mod.particleCountMult);

                for (auto& sat : layer.sph.satelliteCPs) {
                    sat.attractionStrength *= mod.strengthMult;
                    sat.orbitRadius        *= mod.sizeScaleMult;
                }
                for (auto& phase : layer.sph.phases) {
                    if (phase.motionMode == ParticleMotionMode::Beam) {
                        phase.beamDesc.speedMin     *= mod.speedMult;
                        phase.beamDesc.speedMax     *= mod.speedMult;
                        phase.beamDesc.spreadRadius *= mod.sizeScaleMult;
                    }
                    if (phase.motionMode == ParticleMotionMode::Gravity) {
                        phase.gravityDesc.initialSpeedMin *= mod.speedMult;
                        phase.gravityDesc.initialSpeedMax *= mod.speedMult;
                    }
                }
                if (mod.phaseOverride.has_value())
                    layer.sph.phases = mod.phaseOverride.value();
            } else {
                layer.particleCount = int(layer.particleCount * mod.particleCountMult);
                layer.speedMin      *= mod.speedMult;
                layer.speedMax      *= mod.speedMult;
            }
        }
    }
    return def;
}
