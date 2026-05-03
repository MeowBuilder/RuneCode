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
