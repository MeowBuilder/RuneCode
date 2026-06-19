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

    // ─── 원소별 검기 색상 / 이름 helper (Boss_* 검기 일괄 등록용) ────────────
    struct ElementTone
    {
        XMFLOAT4 coreBright;   // L1 charge, L3 sigil core, L5 spark — 밝은 코어
        XMFLOAT4 edgeDark;     // 진한 외곽 (alpha 0 → 투명 소멸)
        XMFLOAT4 heatMid;      // L3 heat 레이어 — 중간 톤
        XMFLOAT4 heatDark;     // L3 heat edge
        XMFLOAT4 mistDark;     // Heavy mist — 어두운 안개
        XMFLOAT4 ringMid;      // L5 ring — 중간 톤
        const char* suffix;
    };

    ElementTone GetElementTone(ElementType e)
    {
        switch (e)
        {
        case ElementType::Fire:
            return {
                { 1.00f, 0.88f, 0.45f, 1.0f },   // 황백
                { 1.00f, 0.32f, 0.04f, 1.0f },
                { 1.05f, 0.45f, 0.10f, 0.90f },
                { 0.55f, 0.10f, 0.0f,  0.0f  },
                { 0.55f, 0.15f, 0.03f, 0.65f },
                { 1.00f, 0.45f, 0.10f, 0.0f  },
                "Fire"
            };
        case ElementType::Water:
            return {
                { 0.55f, 0.92f, 1.20f, 1.0f },   // 청백
                { 0.10f, 0.30f, 0.95f, 1.0f },
                { 0.20f, 0.55f, 1.10f, 0.90f },
                { 0.05f, 0.15f, 0.55f, 0.0f  },
                { 0.05f, 0.15f, 0.45f, 0.65f },
                { 0.30f, 0.70f, 1.10f, 0.0f  },
                "Water"
            };
        case ElementType::Wind:
            return {
                { 0.55f, 1.20f, 0.85f, 1.0f },   // 청록 민트
                { 0.10f, 0.55f, 0.30f, 1.0f },
                { 0.30f, 0.95f, 0.55f, 0.90f },
                { 0.05f, 0.40f, 0.18f, 0.0f  },
                { 0.10f, 0.30f, 0.18f, 0.55f },
                { 0.35f, 0.95f, 0.55f, 0.0f  },
                "Wind"
            };
        case ElementType::Earth:
            return {
                { 1.20f, 0.85f, 0.40f, 1.0f },   // 황갈
                { 0.55f, 0.30f, 0.05f, 1.0f },
                { 0.95f, 0.55f, 0.15f, 0.90f },
                { 0.40f, 0.20f, 0.05f, 0.0f  },
                { 0.30f, 0.18f, 0.05f, 0.65f },
                { 0.95f, 0.60f, 0.20f, 0.0f  },
                "Earth"
            };
        default:
            return GetElementTone(ElementType::Fire);
        }
    }

    std::string MakeBossEffectName(const char* base, ElementType e)
    {
        return std::string(base) + "_" + GetElementTone(e).suffix;
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
    // Boss_SwordCharge_Fire — Dark Lord 검 끝에 부착되는 차징 오라 (L1 Charge 레이어)
    //   inward=true → 검 끝으로 불씨가 빨려들어오는 차징 연출.
    //   원소별 4종 등록 예정 (Day 3 — 색상만 교체). Fire 부터 완성.
    //
    //   [튜닝 v2] v1 옅음 → v2 보스 전체 덮음. v3 는 응축감에 집중:
    //              검 끝 1.2m 반경 좁은 spawn + 빠른 수렴 + 작고 많은 불씨.
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.element        = ElementType::Fire;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f, 0.88f, 0.45f, 1.0f };  // 황백 코어 (HDR 살짝 boost)
        layer.edgeColor      = { 1.0f, 0.32f, 0.04f, 1.0f };  // 진홍 엣지
        layer.particleCount  = 110;        // 180 → 110 (밀도 적절)
        layer.sizeScale      = 1.4f;       // 5.5 → 1.4 (한 입자 작게 — 응축 가독성)
        layer.speedMin       = 6.0f;       // 2 → 6 (빠른 수렴 — "빨려들어감" 명확)
        layer.speedMax       = 11.0f;
        layer.lifetimeMin    = 0.06f;      // v4:0.08 → v5:0.06 (Stop 후 ~0.14s 클린업)
        layer.lifetimeMax    = 0.14f;      // v4:0.18 → v5:0.14
        layer.duration       = -1.f;       // SlashCue 가 명시적으로 정지/지속 제어
        layer.emitRate       = 130.f;      // v4:220 → v5:130 (잔재 줄이기, 응축 충분)

        layer.sphere.radius        = 1.2f; // 3.0 → 1.2 (보스 본체 안 덮음. 검 끝 주변만)
        layer.sphere.shellFraction = 0.90f; // 외곽 셸에서 거의 다 spawn → 가시적 수렴
        layer.sphere.inward        = true;  // ★ 검 끝으로 빨려들어가는 핵심 플래그
        layer.sphere.rotationSpeed = 2.5f;  // 회전감 ↑

        EffectDef def;
        def.name    = "Boss_SwordCharge_Fire";
        def.element = ElementType::Fire;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Boss_CrescentSigil_Fire — Dark Lord 검기 본체 (L3 Blade Body)
    //   기존: ComboHit 의 strVFXOnHit="status_burn" (보스 sphere DoT) 으로 메인 본체를
    //         대신했음 → 판정 면처럼 읽히는 문제.
    //   교체: Crescent 호 한 줄. 검 끝 위치에 spawn 되어 sweep — 검에 깃든 불꽃 한 줄.
    //   참고: Q_WindCutter (플레이어 윈드커터) 와 비슷한 골격, 색만 Fire.
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Boss_CrescentSigil_Fire";
        def.element = ElementType::Fire;

        // 코어 — 굵고 밝은 황금 칼날 (v2 강화: 두께·반경·duration ↑)
        EffectLayer core;
        core.type           = EmitterType::Crescent;
        core.element        = ElementType::Fire;
        core.overrideColors = true;
        core.coreColor      = { 1.20f, 0.95f, 0.50f, 1.0f };  // HDR boost — 1.10 → 1.20
        core.edgeColor      = { 1.10f, 0.32f, 0.05f, 0.0f };
        core.particleCount  = 480;     // 320 → 480 (밀도 ↑)
        core.sizeScale      = 4.5f;    // 2.6 → 4.5 (한 입자 굵게)
        core.lifetimeMin    = 0.30f;
        core.lifetimeMax    = 0.50f;
        core.duration       = 0.55f;   // 0.30 → 0.55 (Hit + Recovery 전반에 잔존)

        core.crescent.radius         = 5.2f;   // 3.2 → 5.2 (호 반경 ↑ — 검 sweep 폭)
        core.crescent.thickness      = 1.8f;   // 0.9 → 1.8 (두께 2배)
        core.crescent.arcAngle       = 160.0f;
        core.crescent.arcOffset      = -80.0f;
        core.crescent.tiltX          = 0.0f;
        core.crescent.normalSpeedMin = 32.f;
        core.crescent.normalSpeedMax = 48.f;
        core.crescent.rotateSpeed    = 0.0f;
        def.layers.push_back(core);

        // 외층 — 두껍고 어두운 적색 열기 — 코어 뒤 무게감 담당
        EffectLayer heat;
        heat.type           = EmitterType::Crescent;
        heat.element        = ElementType::Fire;
        heat.overrideColors = true;
        heat.coreColor      = { 1.00f, 0.42f, 0.10f, 0.95f };
        heat.edgeColor      = { 0.55f, 0.10f, 0.0f,  0.0f };
        heat.particleCount  = 280;     // 180 → 280
        heat.sizeScale      = 6.0f;    // 3.6 → 6.0 (외층 더 굵게)
        heat.lifetimeMin    = 0.40f;
        heat.lifetimeMax    = 0.65f;
        heat.duration       = 0.55f;

        heat.crescent.radius         = 5.8f;   // 3.5 → 5.8
        heat.crescent.thickness      = 2.6f;   // 1.4 → 2.6 (외층 두께 ↑)
        heat.crescent.arcAngle       = 180.0f;
        heat.crescent.arcOffset      = -90.0f;
        heat.crescent.normalSpeedMin = 18.f;
        heat.crescent.normalSpeedMax = 28.f;
        heat.crescent.rotateSpeed    = 0.0f;
        def.layers.push_back(heat);

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Boss_CrescentSigil_Fire_Heavy — 두껍고 무거운 검기 변형 (Slam/Overhead 톤)
    //   기존 Quick 톤은 그대로 보존. 이건 묵직한 일격용 (Wide/Overhead Shape 매칭).
    //   4 레이어 구조:
    //     1) 굵은 황금 코어      — 칼날 본체
    //     2) 두꺼운 적색 외층    — 무게감
    //     3) 검은 잔열 mist     — 호 뒤 lingering 잔열
    //     4) 불티 spark cloud   — 호 끝 분출 파편
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Boss_CrescentSigil_Fire_Heavy";
        def.element = ElementType::Fire;

        // [v2 튜닝 — 너무 cloud-like / 투박했음. sharper arc 강조: 입자 크기 ↓, count ↓,
        //   thickness ↓, arcAngle 좁힘. mist 대폭 축소. 무게감은 유지하되 호 라인 또렷.]

        // [L1] 굵은 황금 코어 — 칼날 본체 (sharp arc)
        EffectLayer core;
        core.type           = EmitterType::Crescent;
        core.element        = ElementType::Fire;
        core.overrideColors = true;
        core.coreColor      = { 1.35f, 1.05f, 0.55f, 1.0f };
        core.edgeColor      = { 1.15f, 0.30f, 0.05f, 0.0f };
        core.particleCount  = 420;     // 720 → 420
        core.sizeScale      = 4.0f;    // 6.5 → 4.0 (입자 sharp)
        core.lifetimeMin    = 0.32f;
        core.lifetimeMax    = 0.55f;
        core.duration       = 0.55f;

        core.crescent.radius         = 6.0f;
        core.crescent.thickness      = 1.6f;   // 2.8 → 1.6 (얇고 sharp)
        core.crescent.arcAngle       = 140.0f; // 170 → 140 (좁은 호)
        core.crescent.arcOffset      = -70.0f;
        core.crescent.tiltX          = 0.0f;
        core.crescent.normalSpeedMin = 28.f;
        core.crescent.normalSpeedMax = 42.f;
        core.crescent.rotateSpeed    = 0.0f;
        def.layers.push_back(core);

        // [L2] 두꺼운 적색 외층 — 무게감 (sharper, cloud 줄임)
        EffectLayer heat;
        heat.type           = EmitterType::Crescent;
        heat.element        = ElementType::Fire;
        heat.overrideColors = true;
        heat.coreColor      = { 1.05f, 0.45f, 0.10f, 0.90f };
        heat.edgeColor      = { 0.55f, 0.10f, 0.0f,  0.0f };
        heat.particleCount  = 280;     // 420 → 280
        heat.sizeScale      = 5.5f;    // 9.0 → 5.5
        heat.lifetimeMin    = 0.40f;
        heat.lifetimeMax    = 0.62f;
        heat.duration       = 0.55f;

        heat.crescent.radius         = 6.5f;
        heat.crescent.thickness      = 2.4f;   // 4.0 → 2.4
        heat.crescent.arcAngle       = 160.0f; // 195 → 160
        heat.crescent.arcOffset      = -80.0f;
        heat.crescent.normalSpeedMin = 18.f;
        heat.crescent.normalSpeedMax = 28.f;
        heat.crescent.rotateSpeed    = 0.0f;
        def.layers.push_back(heat);

        // [L3] 검은 잔열 mist — 대폭 축소 (cloud 형태의 주범이었음)
        EffectLayer mist;
        mist.type           = EmitterType::Crescent;
        mist.element        = ElementType::Fire;
        mist.overrideColors = true;
        mist.coreColor      = { 0.55f, 0.15f, 0.03f, 0.65f };
        mist.edgeColor      = { 0.15f, 0.04f, 0.0f,  0.0f };
        mist.particleCount  = 140;     // 240 → 140
        mist.sizeScale      = 6.0f;    // 11.0 → 6.0 (절반)
        mist.lifetimeMin    = 0.45f;
        mist.lifetimeMax    = 0.70f;   // 1.10 → 0.70
        mist.duration       = 0.55f;

        mist.crescent.radius         = 6.8f;
        mist.crescent.thickness      = 2.8f;   // 5.5 → 2.8 (절반)
        mist.crescent.arcAngle       = 180.0f; // 210 → 180
        mist.crescent.arcOffset      = -90.0f;
        mist.crescent.normalSpeedMin = 10.f;
        mist.crescent.normalSpeedMax = 16.f;
        mist.crescent.rotateSpeed    = 0.0f;
        def.layers.push_back(mist);

        // [L4] 불티 spark — 호 sweep 방향으로 sharp 분출
        EffectLayer spark;
        spark.type           = EmitterType::Cone;
        spark.element        = ElementType::Fire;
        spark.overrideColors = true;
        spark.coreColor      = { 1.0f, 0.92f, 0.50f, 1.0f };
        spark.edgeColor      = { 1.0f, 0.30f, 0.04f, 0.0f };
        spark.particleCount  = 80;     // 120 → 80
        spark.emitRate       = 160.f;
        spark.speedMin       = 8.f;
        spark.speedMax       = 22.f;
        spark.lifetimeMin    = 0.18f;
        spark.lifetimeMax    = 0.38f;
        spark.sizeScale      = 1.1f;   // 1.4 → 1.1 (sharp)
        spark.duration       = 0.32f;
        spark.cone.halfAngle     = 45.f;   // 60 → 45 (호 sweep 방향 강조)
        spark.cone.gravityScale  = 0.45f;
        spark.cone.startSizeMult = 1.0f;
        spark.cone.endSizeMult   = 0.0f;
        spark.cone.fadeAlpha     = true;
        spark.cone.fadeSize      = true;
        def.layers.push_back(spark);

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Boss_SigilImpact_Fire — 검기 충돌 임팩트 (L5 Impact)
    //   Ring 충격파 (지면 확장) + Cone spark burst (불티 사방 분출).
    //   참고: E_GaleRush_Ring (확장 링), sub_strike_spark (스파크 콘).
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Boss_SigilImpact_Fire";
        def.element = ElementType::Fire;

        // Ring — 지면 충격파 (얇은 황금 링이 빠르게 확장)
        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = ElementType::Fire;
        ring.overrideColors = true;
        ring.coreColor      = { 1.00f, 0.95f, 0.65f, 1.0f };
        ring.edgeColor      = { 1.00f, 0.45f, 0.10f, 0.0f };
        ring.particleCount  = 140;
        ring.duration       = 0.22f;
        ring.speedMin       = 3.f;  ring.speedMax    = 9.f;
        ring.lifetimeMin    = 0.18f; ring.lifetimeMax = 0.32f;
        ring.sizeScale      = 2.4f;

        ring.ring.radius         = 0.6f;
        ring.ring.width          = 1.8f;
        ring.ring.expandSpeed    = 32.f;   // 빠르게 확장
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 0.f;
        ring.ring.normalSpeedMin = 4.f;
        ring.ring.normalSpeedMax = 11.f;
        def.layers.push_back(ring);

        // Cone spark burst — 사방 불티
        EffectLayer spark;
        spark.type           = EmitterType::Cone;
        spark.element        = ElementType::Fire;
        spark.overrideColors = true;
        spark.coreColor      = { 1.0f, 0.95f, 0.55f, 1.0f };
        spark.edgeColor      = { 1.0f, 0.32f, 0.05f, 0.0f };
        spark.particleCount  = 80;
        spark.emitRate       = 0.f;        // duration 동안 한번에 분출 (burst)
        spark.speedMin       = 8.f;
        spark.speedMax       = 22.f;
        spark.lifetimeMin    = 0.16f;
        spark.lifetimeMax    = 0.34f;
        spark.sizeScale      = 1.0f;
        spark.duration       = 0.15f;
        spark.cone.halfAngle     = 75.f;   // 사방으로 — 넓은 cone
        spark.cone.gravityScale  = 0.3f;   // 살짝 떨어짐 (불티 ↓)
        spark.cone.startSizeMult = 1.1f;
        spark.cone.endSizeMult   = 0.0f;
        spark.cone.fadeAlpha     = true;
        spark.cone.fadeSize      = true;
        def.layers.push_back(spark);

        Register(std::move(def));
    }

    // ══════════════════════════════════════════════════════════════════════════
    // ─── 원소별 검기 EffectDef 일괄 등록 (Water / Wind / Earth) ──────────────
    //   Fire 정의 4개 (위 블록들) 는 v3 튜닝 완료 상태로 그대로 유지. 여기서는
    //   Water/Wind/Earth 변형을 동일 골격 + 색상만 교체로 추가 등록한다.
    //   GetElementTone(e) 가 원소별 색상 6종 (coreBright/edgeDark/heatMid/heatDark
    //   /mistDark/ringMid) 을 제공. 텍스처는 PickSlashTextureForElement 자동 매핑.
    // ══════════════════════════════════════════════════════════════════════════

    // L1 차징 오라 (Boss_SwordCharge_{Element})
    auto registerSwordCharge = [this](ElementType e)
    {
        const ElementTone t = GetElementTone(e);
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.element        = e;
        layer.overrideColors = true;
        layer.coreColor      = t.coreBright;
        layer.edgeColor      = t.edgeDark;
        layer.particleCount  = 110;
        layer.sizeScale      = 1.4f;
        layer.speedMin       = 6.0f;
        layer.speedMax       = 11.0f;
        layer.lifetimeMin    = 0.06f;
        layer.lifetimeMax    = 0.14f;
        layer.duration       = -1.f;
        layer.emitRate       = 130.f;
        layer.sphere.radius        = 1.2f;
        layer.sphere.shellFraction = 0.90f;
        layer.sphere.inward        = true;
        layer.sphere.rotationSpeed = 2.5f;

        EffectDef def;
        def.name    = MakeBossEffectName("Boss_SwordCharge", e);
        def.element = e;
        def.layers.push_back(std::move(layer));
        this->Register(std::move(def));
    };

    // L3 검기 본체 Quick (Boss_CrescentSigil_{Element}) — 2 레이어 (core + heat)
    auto registerCrescentSigil = [this](ElementType e)
    {
        const ElementTone t = GetElementTone(e);
        EffectDef def;
        def.name    = MakeBossEffectName("Boss_CrescentSigil", e);
        def.element = e;

        // 코어
        EffectLayer core;
        core.type           = EmitterType::Crescent;
        core.element        = e;
        core.overrideColors = true;
        core.coreColor      = { t.coreBright.x * 1.20f, t.coreBright.y * 1.10f, t.coreBright.z * 1.10f, 1.0f };
        core.edgeColor      = { t.edgeDark.x,   t.edgeDark.y,   t.edgeDark.z,   0.0f };
        core.particleCount  = 480;
        core.sizeScale      = 4.5f;
        core.lifetimeMin    = 0.30f;
        core.lifetimeMax    = 0.50f;
        core.duration       = 0.55f;
        core.crescent.radius         = 5.2f;
        core.crescent.thickness      = 1.8f;
        core.crescent.arcAngle       = 160.0f;
        core.crescent.arcOffset      = -80.0f;
        core.crescent.normalSpeedMin = 32.f;
        core.crescent.normalSpeedMax = 48.f;
        def.layers.push_back(core);

        // 외층 heat
        EffectLayer heat;
        heat.type           = EmitterType::Crescent;
        heat.element        = e;
        heat.overrideColors = true;
        heat.coreColor      = t.heatMid;
        heat.edgeColor      = t.heatDark;
        heat.particleCount  = 280;
        heat.sizeScale      = 6.0f;
        heat.lifetimeMin    = 0.40f;
        heat.lifetimeMax    = 0.65f;
        heat.duration       = 0.55f;
        heat.crescent.radius         = 5.8f;
        heat.crescent.thickness      = 2.6f;
        heat.crescent.arcAngle       = 180.0f;
        heat.crescent.arcOffset      = -90.0f;
        heat.crescent.normalSpeedMin = 18.f;
        heat.crescent.normalSpeedMax = 28.f;
        def.layers.push_back(heat);

        this->Register(std::move(def));
    };

    // L3 검기 본체 Heavy (Boss_CrescentSigil_{Element}_Heavy) — 4 레이어
    auto registerCrescentSigilHeavy = [this](ElementType e)
    {
        const ElementTone t = GetElementTone(e);
        EffectDef def;
        def.name    = MakeBossEffectName("Boss_CrescentSigil", e) + "_Heavy";
        def.element = e;

        // [L1] 코어
        EffectLayer core;
        core.type           = EmitterType::Crescent;
        core.element        = e;
        core.overrideColors = true;
        core.coreColor      = { t.coreBright.x * 1.35f, t.coreBright.y * 1.20f, t.coreBright.z * 1.20f, 1.0f };
        core.edgeColor      = { t.edgeDark.x,   t.edgeDark.y,   t.edgeDark.z,   0.0f };
        core.particleCount  = 420;
        core.sizeScale      = 4.0f;
        core.lifetimeMin    = 0.32f;
        core.lifetimeMax    = 0.55f;
        core.duration       = 0.55f;
        core.crescent.radius         = 6.0f;
        core.crescent.thickness      = 1.6f;
        core.crescent.arcAngle       = 140.0f;
        core.crescent.arcOffset      = -70.0f;
        core.crescent.normalSpeedMin = 28.f;
        core.crescent.normalSpeedMax = 42.f;
        def.layers.push_back(core);

        // [L2] heat
        EffectLayer heat;
        heat.type           = EmitterType::Crescent;
        heat.element        = e;
        heat.overrideColors = true;
        heat.coreColor      = t.heatMid;
        heat.edgeColor      = t.heatDark;
        heat.particleCount  = 280;
        heat.sizeScale      = 5.5f;
        heat.lifetimeMin    = 0.40f;
        heat.lifetimeMax    = 0.62f;
        heat.duration       = 0.55f;
        heat.crescent.radius         = 6.5f;
        heat.crescent.thickness      = 2.4f;
        heat.crescent.arcAngle       = 160.0f;
        heat.crescent.arcOffset      = -80.0f;
        heat.crescent.normalSpeedMin = 18.f;
        heat.crescent.normalSpeedMax = 28.f;
        def.layers.push_back(heat);

        // [L3] mist (어두운 잔열)
        EffectLayer mist;
        mist.type           = EmitterType::Crescent;
        mist.element        = e;
        mist.overrideColors = true;
        mist.coreColor      = t.mistDark;
        mist.edgeColor      = { t.mistDark.x * 0.3f, t.mistDark.y * 0.3f, t.mistDark.z * 0.3f, 0.0f };
        mist.particleCount  = 140;
        mist.sizeScale      = 6.0f;
        mist.lifetimeMin    = 0.45f;
        mist.lifetimeMax    = 0.70f;
        mist.duration       = 0.55f;
        mist.crescent.radius         = 6.8f;
        mist.crescent.thickness      = 2.8f;
        mist.crescent.arcAngle       = 180.0f;
        mist.crescent.arcOffset      = -90.0f;
        mist.crescent.normalSpeedMin = 10.f;
        mist.crescent.normalSpeedMax = 16.f;
        def.layers.push_back(mist);

        // [L4] spark cone (호 sweep 방향)
        EffectLayer spark;
        spark.type           = EmitterType::Cone;
        spark.element        = e;
        spark.overrideColors = true;
        spark.coreColor      = t.coreBright;
        spark.edgeColor      = { t.edgeDark.x, t.edgeDark.y, t.edgeDark.z, 0.0f };
        spark.particleCount  = 80;
        spark.emitRate       = 160.f;
        spark.speedMin       = 8.f;
        spark.speedMax       = 22.f;
        spark.lifetimeMin    = 0.18f;
        spark.lifetimeMax    = 0.38f;
        spark.sizeScale      = 1.1f;
        spark.duration       = 0.32f;
        spark.cone.halfAngle     = 45.f;
        spark.cone.gravityScale  = 0.45f;
        spark.cone.startSizeMult = 1.0f;
        spark.cone.endSizeMult   = 0.0f;
        spark.cone.fadeAlpha     = true;
        spark.cone.fadeSize      = true;
        def.layers.push_back(spark);

        this->Register(std::move(def));
    };

    // L3 Projectile-style Bolt (Boss_CrescentBolt_{Element})
    //   — 발사된 검기 톤. 좁고 sharp 한 호가 빠르게·멀리·오래 sweep.
    //     duration ↑↑, normalSpeed ↑↑, radius·thickness ↓ (sharp).
    auto registerCrescentBolt = [this](ElementType e)
    {
        const ElementTone t = GetElementTone(e);
        EffectDef def;
        def.name    = MakeBossEffectName("Boss_CrescentBolt", e);
        def.element = e;

        // [v2 튜닝 — 사용자 피드백: 더 크고 더 느리게]
        // 코어 — 크고 천천히 떠가는 발사 호
        EffectLayer core;
        core.type           = EmitterType::Crescent;
        core.element        = e;
        core.overrideColors = true;
        core.coreColor      = { t.coreBright.x * 1.30f, t.coreBright.y * 1.15f, t.coreBright.z * 1.15f, 1.0f };
        core.edgeColor      = { t.edgeDark.x, t.edgeDark.y, t.edgeDark.z, 0.0f };
        core.particleCount  = 720;       // 600 → 720 (잔상 ↑)
        core.sizeScale      = 5.5f;      // 3.2 → 5.5 (크게)
        core.lifetimeMin    = 1.20f;     // 0.80 → 1.20 (더 오래)
        core.lifetimeMax    = 1.90f;     // 1.30 → 1.90
        core.duration       = 2.00f;     // 1.50 → 2.00
        core.crescent.radius         = 6.0f;   // 3.8 → 6.0 (큰 호)
        core.crescent.thickness      = 2.0f;   // 1.1 → 2.0 (굵게)
        core.crescent.arcAngle       = 130.0f;
        core.crescent.arcOffset      = -65.0f;
        core.crescent.normalSpeedMin = 50.f;   // 90 → 50 (느리게)
        core.crescent.normalSpeedMax = 65.f;   // 105 → 65
        def.layers.push_back(core);

        // 트레일 잔열 — 발사 흔적 (더 크고 느리게)
        EffectLayer trail;
        trail.type           = EmitterType::Crescent;
        trail.element        = e;
        trail.overrideColors = true;
        trail.coreColor      = t.heatMid;
        trail.edgeColor      = { t.heatDark.x, t.heatDark.y, t.heatDark.z, 0.0f };
        trail.particleCount  = 400;       // 320 → 400
        trail.sizeScale      = 7.5f;      // 4.5 → 7.5
        trail.lifetimeMin    = 1.10f;
        trail.lifetimeMax    = 1.70f;
        trail.duration       = 2.00f;
        trail.crescent.radius         = 6.5f;
        trail.crescent.thickness      = 3.0f;
        trail.crescent.arcAngle       = 145.0f;
        trail.crescent.arcOffset      = -72.0f;
        trail.crescent.normalSpeedMin = 40.f;   // 75 → 40
        trail.crescent.normalSpeedMax = 55.f;   // 90 → 55
        def.layers.push_back(trail);

        this->Register(std::move(def));
    };

    // L4 지면 잔류 (Boss_GroundResidue_{Element})
    //   검기 hit 자리에 일정 시간 떠다니는 원소별 잔재 (불티/얼음조각/먼지/돌부스러기).
    //   Cone emitter (느린 spawn, 짧은 거리, 중력↓) + 긴 lifetime → 바닥 잔류.
    auto registerGroundResidue = [this](ElementType e)
    {
        const ElementTone t = GetElementTone(e);
        EffectDef def;
        def.name    = MakeBossEffectName("Boss_GroundResidue", e);
        def.element = e;

        EffectLayer ember;
        ember.type           = EmitterType::Cone;
        ember.element        = e;
        ember.overrideColors = true;
        ember.coreColor      = t.coreBright;
        ember.edgeColor      = { t.edgeDark.x, t.edgeDark.y, t.edgeDark.z, 0.0f };
        ember.particleCount  = 60;
        ember.emitRate       = 25.f;
        ember.speedMin       = 1.0f;
        ember.speedMax       = 3.5f;
        ember.lifetimeMin    = 1.20f;
        ember.lifetimeMax    = 2.40f;
        ember.sizeScale      = 1.6f;
        ember.duration       = 2.20f;
        ember.cone.halfAngle     = 35.f;
        ember.cone.gravityScale  = 0.30f;     // 약한 중력 (불티 떠다님)
        ember.cone.startSizeMult = 1.0f;
        ember.cone.endSizeMult   = 0.0f;
        ember.cone.fadeAlpha     = true;
        ember.cone.fadeSize      = true;
        def.layers.push_back(ember);

        this->Register(std::move(def));
    };

    // L5 임팩트 (Boss_SigilImpact_{Element}) — Ring + Spark
    auto registerSigilImpact = [this](ElementType e)
    {
        const ElementTone t = GetElementTone(e);
        EffectDef def;
        def.name    = MakeBossEffectName("Boss_SigilImpact", e);
        def.element = e;

        // Ring 충격파
        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = e;
        ring.overrideColors = true;
        ring.coreColor      = t.coreBright;
        ring.edgeColor      = t.ringMid;
        ring.particleCount  = 140;
        ring.duration       = 0.22f;
        ring.speedMin       = 3.f;  ring.speedMax    = 9.f;
        ring.lifetimeMin    = 0.18f; ring.lifetimeMax = 0.32f;
        ring.sizeScale      = 2.4f;
        ring.ring.radius         = 0.6f;
        ring.ring.width          = 1.8f;
        ring.ring.expandSpeed    = 32.f;
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 0.f;
        ring.ring.normalSpeedMin = 4.f;
        ring.ring.normalSpeedMax = 11.f;
        def.layers.push_back(ring);

        // Spark burst cone
        EffectLayer spark;
        spark.type           = EmitterType::Cone;
        spark.element        = e;
        spark.overrideColors = true;
        spark.coreColor      = t.coreBright;
        spark.edgeColor      = { t.edgeDark.x, t.edgeDark.y, t.edgeDark.z, 0.0f };
        spark.particleCount  = 80;
        spark.emitRate       = 0.f;
        spark.speedMin       = 8.f;
        spark.speedMax       = 22.f;
        spark.lifetimeMin    = 0.16f;
        spark.lifetimeMax    = 0.34f;
        spark.sizeScale      = 1.0f;
        spark.duration       = 0.15f;
        spark.cone.halfAngle     = 75.f;
        spark.cone.gravityScale  = 0.3f;
        spark.cone.startSizeMult = 1.1f;
        spark.cone.endSizeMult   = 0.0f;
        spark.cone.fadeAlpha     = true;
        spark.cone.fadeSize      = true;
        def.layers.push_back(spark);

        this->Register(std::move(def));
    };

    // ─── 4원소 일괄 등록 (Fire 는 위에 직접 정의됨, 여기서는 나머지 3원소) ──
    for (ElementType e : { ElementType::Water, ElementType::Wind, ElementType::Earth })
    {
        registerSwordCharge(e);
        registerCrescentSigil(e);
        registerCrescentSigilHeavy(e);
        registerSigilImpact(e);
    }
    // Bolt 는 Fire 도 helper 로 등록 (위 Fire 4종은 Heavy/Quick/Charge/Impact만)
    for (ElementType e : { ElementType::Fire, ElementType::Water, ElementType::Wind, ElementType::Earth })
    {
        registerCrescentBolt(e);
        registerGroundResidue(e);
    }

    // ─── Sanctum Pillar (Sanctum 컷씬 4 zone 의 떠오르는 원소 컬럼) ──────────
    //   기존 Wind_UpdraftSmall 흰 톤 → 원소별 풍성 vivid 톤. 2 레이어 (코어 + 외층).
    auto registerSanctumPillar = [this](ElementType e)
    {
        const ElementTone t = GetElementTone(e);
        EffectDef def;
        def.name    = MakeBossEffectName("Sanctum_Pillar", e);
        def.element = e;

        // [L1] 코어 — 밝은 vivid, 위로 떠오르는 입자 (Cone 위 방향)
        EffectLayer core;
        core.type           = EmitterType::Cone;
        core.element        = e;
        core.overrideColors = true;
        core.coreColor      = { t.coreBright.x * 1.20f, t.coreBright.y * 1.10f, t.coreBright.z * 1.10f, 1.0f };
        core.edgeColor      = { t.edgeDark.x, t.edgeDark.y, t.edgeDark.z, 0.0f };
        core.particleCount  = 220;
        core.emitRate       = 90.f;
        core.speedMin       = 3.5f;
        core.speedMax       = 7.5f;
        core.lifetimeMin    = 1.20f;
        core.lifetimeMax    = 2.20f;
        core.sizeScale      = 2.5f;
        core.duration       = -1.f;          // 무한 (CleanupElementalSanctum 가 명시적 stop)
        core.cone.halfAngle     = 12.f;       // 좁은 cone → 컬럼 형태
        core.cone.gravityScale  = -0.20f;     // 음수 = 위로 가속 (떠오름)
        core.cone.startSizeMult = 1.0f;
        core.cone.endSizeMult   = 0.0f;
        core.cone.fadeAlpha     = true;
        core.cone.fadeSize      = true;
        def.layers.push_back(core);

        // [L2] 외층 — 어두운 vivid 톤, 더 넓은 cone (조명감 ↑)
        EffectLayer aura;
        aura.type           = EmitterType::Cone;
        aura.element        = e;
        aura.overrideColors = true;
        aura.coreColor      = t.heatMid;
        aura.edgeColor      = { t.heatDark.x, t.heatDark.y, t.heatDark.z, 0.0f };
        aura.particleCount  = 120;
        aura.emitRate       = 50.f;
        aura.speedMin       = 1.5f;
        aura.speedMax       = 4.0f;
        aura.lifetimeMin    = 1.50f;
        aura.lifetimeMax    = 2.80f;
        aura.sizeScale      = 4.0f;          // 더 큰 입자 → 풍성한 안개감
        aura.duration       = -1.f;
        aura.cone.halfAngle     = 25.f;
        aura.cone.gravityScale  = -0.10f;
        aura.cone.startSizeMult = 1.2f;
        aura.cone.endSizeMult   = 0.0f;
        aura.cone.fadeAlpha     = true;
        aura.cone.fadeSize      = true;
        def.layers.push_back(aura);

        this->Register(std::move(def));
    };
    for (ElementType e : { ElementType::Fire, ElementType::Water, ElementType::Wind, ElementType::Earth })
        registerSanctumPillar(e);
    // ══════════════════════════════════════════════════════════════════════════

    // ──────────────────────────────────────────────────────────────────────────
    // 상태이상 VFX — 적 머리 위로 솟아오르는 연기/파티클 (루프)
    // verticalOffset=4.0f 위성 CP가 굴뚝 역할: 파티클이 위로 끌려 올라감
    // → 몬스터가 뭉쳐도 머리 위 컬럼이 보임
    // ──────────────────────────────────────────────────────────────────────────

    // status_burn: 화상 — 주황/붉은 파티클이 몬스터 외곽에서 랜덤 스폰
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.element        = ElementType::Fire;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f, 0.65f, 0.05f, 1.0f };
        layer.edgeColor      = { 1.0f, 0.20f, 0.0f,  1.0f };
        layer.particleCount  = 70;
        layer.sizeScale      = 4.0f;
        layer.speedMin       = 1.5f;
        layer.speedMax       = 3.5f;
        layer.lifetimeMin    = 0.5f;
        layer.lifetimeMax    = 1.2f;
        layer.duration       = -1.f;
        layer.emitRate       = 35.f;

        layer.sphere.radius        = 2.0f;
        layer.sphere.shellFraction = 0.6f;
        layer.sphere.inward        = false;

        EffectDef def;
        def.name    = "status_burn";
        def.element = ElementType::Fire;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // status_chill: 빙결 중첩 — 파란 냉기 파티클이 몬스터 외곽에서 천천히 퍼짐
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.element        = ElementType::Water;
        layer.overrideColors = true;
        layer.coreColor      = { 0.70f, 0.92f, 1.0f, 1.0f };
        layer.edgeColor      = { 0.20f, 0.55f, 1.0f, 1.0f };
        layer.particleCount  = 55;
        layer.sizeScale      = 3.8f;
        layer.speedMin       = 0.5f;
        layer.speedMax       = 1.8f;
        layer.lifetimeMin    = 1.0f;
        layer.lifetimeMax    = 2.0f;
        layer.duration       = -1.f;
        layer.emitRate       = 25.f;

        layer.sphere.radius        = 2.0f;
        layer.sphere.shellFraction = 0.6f;
        layer.sphere.inward        = false;
        layer.sphere.rotationSpeed = 0.5f;

        EffectDef def;
        def.name    = "status_chill";
        def.element = ElementType::Water;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // status_freeze: 완전 빙결 — 하얀 얼음 파편이 몬스터 외곽에서 발산
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.element        = ElementType::Water;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f, 1.0f, 1.0f, 1.0f };
        layer.edgeColor      = { 0.45f, 0.80f, 1.0f, 1.0f };
        layer.particleCount  = 75;
        layer.sizeScale      = 4.2f;
        layer.speedMin       = 1.0f;
        layer.speedMax       = 2.8f;
        layer.lifetimeMin    = 0.5f;
        layer.lifetimeMax    = 1.0f;
        layer.duration       = -1.f;
        layer.emitRate       = 30.f;

        layer.sphere.radius        = 2.0f;
        layer.sphere.shellFraction = 0.6f;
        layer.sphere.inward        = false;

        EffectDef def;
        def.name    = "status_freeze";
        def.element = ElementType::Water;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // status_fracture: 균열 — 흙먼지 파티클이 몬스터 외곽에서 회전하며 퍼짐
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.element        = ElementType::Earth;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f, 0.75f, 0.20f, 1.0f };
        layer.edgeColor      = { 0.65f, 0.42f, 0.08f, 1.0f };
        layer.particleCount  = 55;
        layer.sizeScale      = 3.5f;
        layer.speedMin       = 1.0f;
        layer.speedMax       = 2.5f;
        layer.lifetimeMin    = 0.8f;
        layer.lifetimeMax    = 1.5f;
        layer.duration       = -1.f;
        layer.emitRate       = 22.f;

        layer.sphere.radius        = 2.0f;
        layer.sphere.shellFraction = 0.6f;
        layer.sphere.inward        = false;
        layer.sphere.rotationSpeed = 2.5f;

        EffectDef def;
        def.name    = "status_fracture";
        def.element = ElementType::Earth;
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
    //   width 50 → 18 로 좁혀 좁은 빔/스트림 흐름으로 (이전: disk 반경 50 이라 바닥~천장
    //   까지 입자가 펼쳐져 "바닥 쓰는 sheet" 인상). swirl ↑ 로 회오리 강조, 속도 ↑ 로 휙휙
    //   지나가는 강풍감. 밀도/사이즈 ↓ 로 sheet 느낌 완화.
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Wind_DriftLeaves";
        def.element = ElementType::Wind;

        EffectLayer drift;
        drift.type           = EmitterType::Linear;
        drift.element        = ElementType::Wind;
        drift.particleCount  = 120;            // 180 → 120 (밀도 ↓, sheet 인상 완화)
        drift.overrideColors = true;
        drift.coreColor      = { 0.78f, 0.92f, 0.55f, 1.0f };  // 옅은 잎녹색
        drift.edgeColor      = { 0.38f, 0.55f, 0.20f, 0.45f };
        drift.sizeScale      = 0.8f;           // 1.1 → 0.8 (입자 작게)
        drift.speedMin       = 4.5f;           // 2.5 → 4.5 (휙휙 지나감)
        drift.speedMax       = 8.0f;           // 5.0 → 8.0
        drift.lifetimeMin    = 6.0f;           // 8 → 6 (빠르게 흘러도 거리 비슷)
        drift.lifetimeMax    = 11.0f;          // 14 → 11
        drift.duration       = -1.f;
        drift.linear.length      = 120.0f;     // 160 → 120
        drift.linear.width       = 18.0f;      // 50 → 18 (좁은 빔/스트림)
        drift.linear.recycleRate = 1.0f;
        drift.linear.swirlSpeed  = 2.8f;       // 1.2 → 2.8 (회오리 강조)
        def.layers.push_back(drift);

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Wind_DustMotes — 공기 중 떠 있는 작은 입자 (햇살 받는 dust). 느린 드리프트.
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Wind_DustMotes";
        def.element = ElementType::Wind;

        EffectLayer dust;
        dust.type           = EmitterType::Linear;
        dust.element        = ElementType::Wind;
        dust.particleCount  = 220;
        dust.overrideColors = true;
        dust.coreColor      = { 1.00f, 0.98f, 0.85f, 0.85f };   // 따뜻한 white-cream
        dust.edgeColor      = { 0.85f, 0.82f, 0.65f, 0.30f };
        dust.sizeScale      = 0.40f;                            // 작게 (drift 1.1 보다 훨씬)
        dust.speedMin       = 1.0f;
        dust.speedMax       = 2.2f;                             // 느림 (잎 2.5~5.0 보다)
        dust.lifetimeMin    = 12.0f;
        dust.lifetimeMax    = 20.0f;
        dust.duration       = -1.f;
        dust.linear.length      = 180.0f;
        dust.linear.width       = 35.0f;                        // 잎 width(50) 보다 좁은 빔
        dust.linear.recycleRate = 1.0f;
        dust.linear.swirlSpeed  = 0.4f;                         // 미세 swirl
        def.layers.push_back(dust);

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Wind_Petals — 꽃잎/씨앗 흩날림. 옅은 핑크-화이트 톤, drift 보다 가볍게.
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Wind_Petals";
        def.element = ElementType::Wind;

        EffectLayer pet;
        pet.type           = EmitterType::Linear;
        pet.element        = ElementType::Wind;
        pet.particleCount  = 120;
        pet.overrideColors = true;
        pet.coreColor      = { 1.00f, 0.85f, 0.92f, 0.95f };    // 옅은 핑크-화이트
        pet.edgeColor      = { 0.92f, 0.65f, 0.78f, 0.50f };
        pet.sizeScale      = 0.85f;
        pet.speedMin       = 2.0f;
        pet.speedMax       = 4.5f;
        pet.lifetimeMin    = 9.0f;
        pet.lifetimeMax    = 16.0f;
        pet.duration       = -1.f;
        pet.linear.length      = 150.0f;
        pet.linear.width       = 45.0f;
        pet.linear.recycleRate = 1.0f;
        pet.linear.swirlSpeed  = 2.2f;                          // 잎보다 더 빙글빙글
        def.layers.push_back(pet);

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Wind_GustBurst — 강풍 peak 에 풀숲에서 터지는 짧은 꽃가루 burst.
    //                  Sphere 1.4s, 따뜻한 노랑-화이트, 위로 살짝 퍼지면서 사라짐.
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Wind_GustBurst";
        def.element = ElementType::Wind;

        EffectLayer burst;
        burst.type           = EmitterType::Sphere;
        burst.element        = ElementType::Wind;
        burst.overrideColors = true;
        burst.coreColor      = { 1.00f, 0.94f, 0.65f, 1.00f };  // 꽃가루 노랑
        burst.edgeColor      = { 0.85f, 0.75f, 0.45f, 0.40f };
        burst.particleCount  = 80;
        burst.sizeScale      = 0.55f;
        burst.speedMin       = 1.5f;
        burst.speedMax       = 3.8f;
        burst.lifetimeMin    = 0.8f;
        burst.lifetimeMax    = 1.6f;
        burst.duration       = 0.6f;                            // 짧은 emission
        burst.emitRate       = 130.f;

        burst.sphere.radius        = 1.8f;
        burst.sphere.shellFraction = 0.4f;
        burst.sphere.inward        = false;

        def.layers.push_back(std::move(burst));
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
    // Portal_Ring — 인터랙션 큐브 포탈 회전 링 (듀얼 톤: 시안 외곽 + 마젠타 내부 림)
    //   2 레이어로 깊이감: outer 차가운 시안 (외곽 큰 띠) + inner 따뜻한 마젠타 (좁고 빠른 가까운 띠)
    //   대비되는 색 + 다른 속도 → 절차적 보라 단일 톤 인상 제거
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Portal_Ring";
        def.element = ElementType::Wind;

        // ── 외곽 밝은 보라 림 — 넓고 천천히 회전 (림 두께/외곽 분위기) ──────────
        EffectLayer outerRing;
        outerRing.type           = EmitterType::Ring;
        outerRing.element        = ElementType::Wind;
        outerRing.particleCount  = 200;
        outerRing.overrideColors = true;
        outerRing.coreColor      = { 0.80f, 0.55f, 1.00f, 0.85f };  // 밝은 보라
        outerRing.edgeColor      = { 0.25f, 0.08f, 0.55f, 0.30f };  // 딥 바이올렛 페이드
        outerRing.sizeScale      = 1.6f;
        outerRing.speedMin       = 0.2f;
        outerRing.speedMax       = 0.7f;
        outerRing.lifetimeMin    = 1.5f;
        outerRing.lifetimeMax    = 2.4f;
        outerRing.duration       = 9999.f;
        outerRing.emitRate       = 110.f;

        outerRing.ring.radius         = 8.7f;
        outerRing.ring.width          = 0.9f;
        outerRing.ring.expandSpeed    = 0.0f;
        outerRing.ring.tiltX          = 0.f;
        outerRing.ring.rotateSpeed    = -2.2f;   // 역방향 회전 — inner 와 대비
        outerRing.ring.normalSpeedMin = 0.15f;
        outerRing.ring.normalSpeedMax = 0.6f;

        def.layers.push_back(outerRing);

        // ── 내부 핫 라벤더 펄스 — 좁고 빠르게 회전 (강렬한 코어 림) ────────────
        EffectLayer innerRing;
        innerRing.type           = EmitterType::Ring;
        innerRing.element        = ElementType::Wind;
        innerRing.particleCount  = 160;
        innerRing.overrideColors = true;
        innerRing.coreColor      = { 1.00f, 0.75f, 1.00f, 0.95f };  // 라벤더-핑크
        innerRing.edgeColor      = { 0.55f, 0.15f, 0.85f, 0.40f };
        innerRing.sizeScale      = 1.0f;
        innerRing.speedMin       = 0.4f;
        innerRing.speedMax       = 1.0f;
        innerRing.lifetimeMin    = 0.8f;
        innerRing.lifetimeMax    = 1.4f;
        innerRing.duration       = 9999.f;
        innerRing.emitRate       = 130.f;

        innerRing.ring.radius         = 7.5f;
        innerRing.ring.width          = 0.4f;
        innerRing.ring.expandSpeed    = 0.0f;
        innerRing.ring.tiltX          = 0.f;
        innerRing.ring.rotateSpeed    = 5.5f;    // 빠른 정방향 — outer 와 반대로 회전 → 마찰감
        innerRing.ring.normalSpeedMin = 0.3f;
        innerRing.ring.normalSpeedMax = 1.0f;

        def.layers.push_back(innerRing);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Spawn_Portal — 일반 몬스터 웨이브 등장용 공중 포탈 (강화판)
    //   웨이브 시작 시각 신호. 크고 밝게 + 입자 위/아래로 분사하여 볼륨감.
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Spawn_Portal";
        def.element = ElementType::Wind;

        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = ElementType::Wind;
        ring.particleCount  = 320;
        ring.overrideColors = true;
        ring.coreColor      = { 1.00f, 0.80f, 1.00f, 1.0f  };  // 밝은 핑크-라벤더
        ring.edgeColor      = { 0.60f, 0.20f, 0.95f, 0.55f };
        ring.sizeScale      = 2.8f;
        ring.speedMin       = 1.0f;
        ring.speedMax       = 2.5f;
        ring.lifetimeMin    = 0.4f;
        ring.lifetimeMax    = 0.8f;
        ring.duration       = 9999.f;
        ring.emitRate       = 220.f;

        ring.ring.radius         = 5.0f;
        ring.ring.width          = 1.2f;
        ring.ring.expandSpeed    = 2.0f;    // 살짝 펄스
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 7.0f;
        ring.ring.normalSpeedMin = 0.6f;
        ring.ring.normalSpeedMax = 2.0f;    // 위로 솟구침 → 볼륨감

        def.layers.push_back(ring);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Spawn_PortalGround — 지면 경고 링 (적 착지 예정 지점 표시, 보조 인디케이터)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Spawn_PortalGround";
        def.element = ElementType::Wind;

        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = ElementType::Wind;
        ring.particleCount  = 140;
        ring.overrideColors = true;
        ring.coreColor      = { 1.00f, 0.40f, 0.70f, 0.9f  };
        ring.edgeColor      = { 0.50f, 0.05f, 0.50f, 0.0f  };
        ring.sizeScale      = 2.0f;
        ring.speedMin       = 0.4f;
        ring.speedMax       = 1.0f;
        ring.lifetimeMin    = 0.5f;
        ring.lifetimeMax    = 0.9f;
        ring.duration       = 9999.f;
        ring.emitRate       = 160.f;

        ring.ring.radius         = 3.5f;
        ring.ring.width          = 0.6f;
        ring.ring.expandSpeed    = 1.0f;
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 4.0f;
        ring.ring.normalSpeedMin = 0.2f;
        ring.ring.normalSpeedMax = 0.6f;

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
    // Q_WindCutter — 초승달 칼날 하나가 앞으로 날아감
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Q_WindCutter";
        def.element = ElementType::Wind;

        {
            EffectLayer blade;
            blade.type           = EmitterType::Crescent;
            blade.element        = ElementType::Wind;
            blade.overrideColors = true;
            blade.coreColor      = { 0.60f, 1.0f, 0.65f, 1.0f };  // 밝은 민트 초록
            blade.edgeColor      = { 0.05f, 0.50f, 0.15f, 0.0f };  // 진한 초록, 투명 소멸
            blade.particleCount  = 350;
            blade.sizeScale      = 3.5f;
            blade.lifetimeMin    = 0.55f; blade.lifetimeMax = 0.75f;
            blade.duration       = 0.90f;

            blade.crescent.radius         = 2.8f;    // 칼날 반경
            blade.crescent.thickness      = 1.0f;    // 날 두께 (radial + depth×2.5 에 영향)
            blade.crescent.arcAngle       = 200.0f;  // 200° → 뚜렷한 초승달
            blade.crescent.arcOffset      = -100.0f; // 카메라 공간 기준: 볼록 오른쪽(camRight)
            blade.crescent.tiltX          = 0.0f;    // 카메라 평면 모드에서 미사용
            blade.crescent.normalSpeedMin = 62.f;
            blade.crescent.normalSpeedMax = 62.f;
            blade.crescent.rotateSpeed    = 0.0f;
            def.layers.push_back(blade);
        }

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // E_GaleRush_Burst — 출발 시 몸통 주변 구체 폭발 (이제 전방 제트 대신)
    // E_GaleRush_Ring  — 출발 지면 충격파 링
    // E_GaleRush_Trail — 대쉬 중 후방 배기 분사 (로켓 추진력)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "E_GaleRush_Burst";
        def.element = ElementType::Wind;

        // 출발 구체 폭발 — 몸통에서 사방으로 바람이 터져나옴
        EffectLayer burst;
        burst.type           = EmitterType::Sphere;
        burst.element        = ElementType::Wind;
        burst.overrideColors = true;
        burst.coreColor      = { 0.90f, 1.0f,  0.92f, 1.0f  };
        burst.edgeColor      = { 0.18f, 0.70f, 0.28f, 0.0f  };
        burst.particleCount  = 180;
        burst.speedMin       = 14.f;  burst.speedMax    = 38.f;
        burst.lifetimeMin    = 0.14f; burst.lifetimeMax = 0.34f;
        burst.sizeScale      = 2.2f;
        burst.duration       = 0.28f;

        burst.sphere.radius        = 2.2f;
        burst.sphere.shellFraction = 0.55f;
        burst.sphere.inward        = false;
        burst.sphere.rotationSpeed = 14.f;
        def.layers.push_back(burst);
        Register(std::move(def));
    }
    {
        EffectDef def;
        def.name    = "E_GaleRush_Ring";
        def.element = ElementType::Wind;

        // 출발 지면 충격파 — 빠르게 바깥으로 퍼지는 링
        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = ElementType::Wind;
        ring.overrideColors = true;
        ring.coreColor      = { 0.88f, 1.0f,  0.88f, 1.0f  };
        ring.edgeColor      = { 0.25f, 0.72f, 0.32f, 0.0f  };
        ring.particleCount  = 260;
        ring.duration       = 0.55f;
        ring.speedMin       = 3.f;   ring.speedMax    = 8.f;
        ring.lifetimeMin    = 0.25f; ring.lifetimeMax = 0.50f;
        ring.sizeScale      = 2.8f;

        ring.ring.radius         = 1.0f;
        ring.ring.width          = 2.5f;
        ring.ring.expandSpeed    = 40.f;
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 3.5f;
        ring.ring.normalSpeedMin = 4.f;
        ring.ring.normalSpeedMax = 10.f;
        def.layers.push_back(ring);
        Register(std::move(def));
    }
    {
        EffectDef def;
        def.name    = "E_GaleRush_Trail";
        def.element = ElementType::Wind;

        // 후방 배기 분사 — 로켓 추진처럼 뒤로 집중 분출
        // [내층] 밝고 좁은 배기 코어
        {
            EffectLayer core;
            core.type           = EmitterType::Cone;
            core.element        = ElementType::Wind;
            core.overrideColors = true;
            core.coreColor      = { 0.95f, 1.0f,  0.95f, 1.0f  };  // 거의 흰색 고온 코어
            core.edgeColor      = { 0.20f, 0.78f, 0.30f, 0.0f  };
            core.particleCount  = 70;
            core.speedMin       = 12.f;  core.speedMax    = 32.f;
            core.lifetimeMin    = 0.08f; core.lifetimeMax = 0.22f;
            core.sizeScale      = 1.6f;
            core.duration       = 0.22f;

            core.cone.halfAngle     = 18.f;
            core.cone.gravityScale  = 0.0f;
            core.cone.startSizeMult = 2.2f;
            core.cone.endSizeMult   = 0.0f;
            core.cone.fadeAlpha     = true;
            core.cone.fadeSize      = true;
            def.layers.push_back(core);
        }
        // [외층] 넓게 퍼지는 초록 분사 연기
        {
            EffectLayer plume;
            plume.type           = EmitterType::Cone;
            plume.element        = ElementType::Wind;
            plume.overrideColors = true;
            plume.coreColor      = { 0.38f, 0.92f, 0.48f, 0.75f };
            plume.edgeColor      = { 0.08f, 0.40f, 0.14f, 0.0f  };
            plume.particleCount  = 55;
            plume.speedMin       = 6.f;   plume.speedMax    = 18.f;
            plume.lifetimeMin    = 0.14f; plume.lifetimeMax = 0.32f;
            plume.sizeScale      = 2.8f;
            plume.duration       = 0.22f;

            plume.cone.halfAngle     = 36.f;
            plume.cone.gravityScale  = 0.01f;
            plume.cone.startSizeMult = 1.4f;
            plume.cone.endSizeMult   = 0.0f;
            plume.cone.fadeAlpha     = true;
            plume.cone.fadeSize      = true;
            def.layers.push_back(plume);
        }
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
            outer.lifetimeMin    = 0.4f; outer.lifetimeMax = 0.7f;
            outer.duration       = -1.f;
            outer.linear.length      = 18.0f;
            outer.linear.width       = 4.5f;
            outer.linear.recycleRate = 1.0f;
            outer.linear.swirlSpeed  = 20.0f;
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
            inner.lifetimeMin    = 0.25f; inner.lifetimeMax = 0.45f;
            inner.duration       = -1.f;
            inner.linear.length      = 20.0f;
            inner.linear.width       = 1.8f;
            inner.linear.recycleRate = 1.0f;
            inner.linear.swirlSpeed  = 32.0f;
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
            burst.speedMin       = 6.f;  burst.speedMax  = 14.f;
            burst.lifetimeMin    = 0.3f; burst.lifetimeMax = 0.6f;
            burst.sizeScale      = 2.8f;

            burst.cone.halfAngle     = 45.f;
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
        ring.particleCount  = 500;
        ring.duration       = 2.0f;   // 20u / 12 u/s ≈ 1.67s 커버
        ring.speedMin       = 1.5f; ring.speedMax  = 4.0f;  // 산란 줄여 선명한 링
        ring.lifetimeMin    = 0.3f; ring.lifetimeMax = 0.65f;
        ring.sizeScale      = 4.5f;

        ring.ring.radius         = 0.f;    // 피격 링과 동일하게 중심에서 시작
        ring.ring.width          = 8.5f;   // 피격 두께(RING_THICKNESS*2=8)에 맞춤
        ring.ring.expandSpeed    = 12.f;   // RING_SPEED와 일치 — VFX·피격 동기화
        ring.ring.tiltX          = 0.f;
        ring.ring.rotateSpeed    = 0.8f;
        ring.ring.normalSpeedMin = 5.f;
        ring.ring.normalSpeedMax = 18.f;

        def.layers.push_back(ring);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // E_RockThrow — 바위 투척: 입자들이 뭉쳐 하나의 돌덩이처럼 날아가는 형태
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "E_RockThrow";
        def.element = ElementType::Earth;

        // 레이어 1: SPH 암석 코어 — ControlPoint 인력으로 뭉쳐 이동
        {
            EffectLayer core = MakeSPHLayer(ElementType::Earth);
            core.overrideColors = true;
            core.coreColor = { 0.85f, 0.68f, 0.40f, 1.0f };  // 모래빛 암석
            core.edgeColor = { 0.50f, 0.32f, 0.14f, 0.85f }; // 짙은 흙빛

            SPHEmitterParams& s = core.sph;
            s.particleCount        = 380;
            s.spawnRadius          = 0.28f;
            s.masterCPStrength     = 55.f;
            s.masterCPSphereRadius = 0.35f;
            s.masterCPFallSpeed    = 20.f;
            s.maxParticleSpeed     = 8.f;

            s.overridePhysics     = true;
            s.sphStiffness        = 65.f;
            s.sphNearPressureMult = 2.5f;
            s.sphRestDensity      = 11.f;
            s.sphViscosity        = 0.55f;
            s.sphSmoothingRadius  = 0.85f;

            VFXPhase p0;
            p0.startTime  = 0.f;
            p0.duration   = 99.f;
            p0.motionMode = ParticleMotionMode::ControlPoint;
            p0.offsetParticlesWithOrigin = true;

            FluidCPDesc nucleus;
            nucleus.orbitRadius        = 0.f;
            nucleus.orbitSpeed         = 0.f;
            nucleus.attractionStrength = 50.f;
            nucleus.sphereRadius       = 0.35f;
            p0.cpDescs.push_back(nucleus);
            s.phases.push_back(p0);

            FinalizeSPHLayer(core);
            def.layers.push_back(std::move(core));
        }

        // 레이어 2: 흙먼지 꼬리 — 바위 주위에서 먼지·파편 지속 방출
        {
            EffectLayer dust;
            dust.type           = EmitterType::Sphere;
            dust.element        = ElementType::Earth;
            dust.overrideColors = true;
            dust.coreColor      = { 0.72f, 0.58f, 0.35f, 0.70f };
            dust.edgeColor      = { 0.42f, 0.28f, 0.10f, 0.00f };
            dust.particleCount  = 160;
            dust.emitRate       = 70.f;
            dust.duration       = -1.f;
            dust.speedMin       = 0.8f;
            dust.speedMax       = 2.8f;
            dust.lifetimeMin    = 0.25f;
            dust.lifetimeMax    = 0.55f;
            dust.sizeScale      = 1.3f;

            dust.sphere.radius        = 0.65f;
            dust.sphere.shellFraction = 0.5f;
            dust.sphere.inward        = false;
            def.layers.push_back(dust);
        }

        Register(std::move(def));

        VFXModifier enhMod;
        enhMod.particleCountMult = 1.4f;
        enhMod.sizeScaleMult     = 1.2f;
        RegisterRuneMod("E_RockThrow", RUNE_ENHANCE, enhMod);
    }

    // ──────────────────────────────────────────────────────────────────────────
    // E_EarthArmor_Burst — 대지의 갑옷 발동: 발 아래에서 빠르게 퍼지는 링 폭발
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "E_EarthArmor_Burst";
        def.element = ElementType::Earth;

        // 링 폭발
        {
            EffectLayer ring;
            ring.type           = EmitterType::Ring;
            ring.element        = ElementType::Earth;
            ring.overrideColors = true;
            ring.coreColor      = { 0.88f, 0.72f, 0.32f, 1.0f };  // 황금빛 갈색
            ring.edgeColor      = { 0.55f, 0.35f, 0.12f, 0.0f };
            ring.particleCount  = 260;
            ring.duration       = 0.8f;
            ring.speedMin       = 1.5f;
            ring.speedMax       = 5.5f;
            ring.lifetimeMin    = 0.30f;
            ring.lifetimeMax    = 0.70f;
            ring.sizeScale      = 2.8f;

            ring.ring.radius         = 0.4f;
            ring.ring.width          = 2.5f;
            ring.ring.expandSpeed    = 22.f;
            ring.ring.normalSpeedMin = 3.f;
            ring.ring.normalSpeedMax = 10.f;
            ring.ring.rotateSpeed    = 0.5f;
            def.layers.push_back(ring);
        }

        // 파편 콘 (위로 튀는 암석 조각)
        {
            EffectLayer debris;
            debris.type           = EmitterType::Cone;
            debris.element        = ElementType::Earth;
            debris.overrideColors = true;
            debris.coreColor      = { 0.80f, 0.62f, 0.30f, 1.0f };
            debris.edgeColor      = { 0.48f, 0.28f, 0.10f, 0.0f };
            debris.particleCount  = 120;
            debris.duration       = 0.4f;
            debris.speedMin       = 5.f;
            debris.speedMax       = 16.f;
            debris.lifetimeMin    = 0.3f;
            debris.lifetimeMax    = 0.65f;
            debris.sizeScale      = 2.2f;

            debris.cone.halfAngle     = 60.f;
            debris.cone.gravityScale  = 0.7f;
            debris.cone.startSizeMult = 1.2f;
            debris.cone.endSizeMult   = 0.1f;
            debris.cone.fadeAlpha     = true;
            debris.cone.fadeSize      = true;
            def.layers.push_back(debris);
        }

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // E_EarthArmor_Aura — 대지의 갑옷 지속 오라: 캐릭터 주위를 감싸는 흙먼지
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "E_EarthArmor_Aura";
        def.element = ElementType::Earth;

        EffectLayer aura;
        aura.type           = EmitterType::Sphere;
        aura.element        = ElementType::Earth;
        aura.overrideColors = true;
        aura.coreColor      = { 0.78f, 0.62f, 0.30f, 0.65f };  // 황토 먼지
        aura.edgeColor      = { 0.48f, 0.30f, 0.12f, 0.00f };
        aura.particleCount  = 160;
        aura.emitRate       = 50.f;
        aura.duration       = -1.f;   // 무한 — Behavior에서 StopEffect로 종료
        aura.speedMin       = 0.5f;
        aura.speedMax       = 2.2f;
        aura.lifetimeMin    = 0.45f;
        aura.lifetimeMax    = 0.90f;
        aura.sizeScale      = 1.4f;

        aura.sphere.radius        = 1.6f;
        aura.sphere.shellFraction = 0.7f;
        aura.sphere.inward        = false;
        aura.sphere.rotationSpeed = 0.8f;   // 천천히 회전하는 먼지 구름
        def.layers.push_back(aura);

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // E_EarthArmor_Shield — 대지의 갑옷 보호막 오라: 황금빛 흙먼지 파티클이 캐릭터 주위를 회전
    //   status_fracture 패턴과 동일한 Sphere emitter — 보호막 지속 중에만 재생
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.element        = ElementType::Earth;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f, 0.85f, 0.30f, 1.0f };   // 밝은 황금빛
        layer.edgeColor      = { 0.75f, 0.50f, 0.10f, 1.0f };   // 따뜻한 갈색 가장자리
        layer.particleCount  = 80;
        layer.sizeScale      = 4.0f;
        layer.speedMin       = 1.0f;
        layer.speedMax       = 2.5f;
        layer.lifetimeMin    = 1.2f;
        layer.lifetimeMax    = 2.0f;
        layer.duration       = -1.f;
        layer.emitRate       = 30.f;

        layer.sphere.radius        = 2.8f;
        layer.sphere.shellFraction = 0.55f;
        layer.sphere.inward        = false;
        layer.sphere.rotationSpeed = 2.0f;

        EffectDef def;
        def.name    = "E_EarthArmor_Shield";
        def.element = ElementType::Earth;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Portal_Suction — 외곽에서 중심으로 빨려들어가는 흡입 입자 (강한 인력 + 시안 streak)
    //   ↑ 입자 수, ↑ 속도, ↓ 사이즈 → "실제로 빨아들이는" 격렬한 vortex 느낌
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Portal_Suction";
        def.element = ElementType::Wind;

        EffectLayer suck;
        suck.type           = EmitterType::Sphere;
        suck.element        = ElementType::Wind;
        suck.particleCount  = 100;           // 140→100 — 점 장식 인상 제거, 적지만 빠르게
        suck.overrideColors = true;
        suck.coreColor      = { 0.85f, 0.65f, 1.00f, 0.85f }; // 밝은 라벤더
        suck.edgeColor      = { 0.40f, 0.10f, 0.80f, 0.0f };  // 딥 바이올렛 페이드
        suck.sizeScale      = 0.38f;         // 0.30→0.38 — 조금 더 읽히는 streak
        suck.speedMin       = 4.5f;          // ↑ 격렬한 흡입
        suck.speedMax       = 7.5f;
        suck.lifetimeMin    = 0.45f;         // 짧게 → 속도감 살아남
        suck.lifetimeMax    = 0.85f;
        suck.duration       = 9999.f;
        suck.emitRate       = 70.f;          // 90→70 — 적은 입자 흐름

        suck.sphere.radius        = 9.0f;
        suck.sphere.shellFraction = 1.0f;
        suck.sphere.inward        = true;
        suck.sphere.rotationSpeed = 4.0f;    // 2.5→4.0 — 강한 vortex 회전

        def.layers.push_back(suck);
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Portal_Beam — 디스크에서 위로 솟는 수직 광주 (얇고 높게 — cinematic 광선)
    //   tornado 둘레 좁히고 더 높이 → 멀리서도 보이는 신호탑 같은 인상
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "Portal_Beam";
        def.element = ElementType::Wind;

        EffectLayer beam;
        beam.type           = EmitterType::Linear;
        beam.element        = ElementType::Wind;
        beam.particleCount  = 110;
        beam.overrideColors = true;
        beam.coreColor      = { 0.95f, 0.80f, 1.00f, 0.90f }; // 라벤더-화이트
        beam.edgeColor      = { 0.45f, 0.10f, 0.85f, 0.0f };  // 딥 바이올렛 페이드
        beam.sizeScale      = 0.4f;          // ↓ 0.5 → 0.4 — 얇은 광선
        beam.speedMin       = 2.5f;          // ↑ 더 빠르게 솟음
        beam.speedMax       = 4.0f;
        beam.lifetimeMin    = 1.5f;
        beam.lifetimeMax    = 2.3f;
        beam.duration       = 9999.f;
        beam.emitRate       = 65.f;

        beam.linear.length      = 11.0f;    // ↑ 7.5 → 11 — 더 높이 솟음
        beam.linear.width       = 1.2f;     // ↓ 2.2 → 1.2 — 좁은 토네이도 둘레
        beam.linear.recycleRate = 1.0f;
        beam.linear.swirlSpeed  = 5.5f;     // ↑ 3 → 5.5 — 격렬한 회전

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

    // ══════════════════════════════════════════════════════════════════════════
    // 변환 룬 서브 VFX — 투사체 외형을 룬 특성에 맞게 바꿈
    // ══════════════════════════════════════════════════════════════════════════

    // sub_frag_trail — TRF_MLT 다연발: 작은 금빛 파편 트레일 (분열 느낌)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Cone;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  0.85f, 0.4f,  1.0f };
        layer.edgeColor      = { 0.7f,  0.4f,  0.05f, 0.0f };
        layer.particleCount  = 45;
        layer.emitRate       = 55.f;
        layer.speedMin       = 4.f;
        layer.speedMax       = 12.f;
        layer.lifetimeMin    = 0.07f;
        layer.lifetimeMax    = 0.18f;
        layer.sizeScale      = 0.9f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.cone.halfAngle     = 28.f;
        layer.cone.gravityScale  = 0.12f;
        layer.cone.startSizeMult = 1.1f;
        layer.cone.endSizeMult   = 0.0f;
        layer.cone.fadeAlpha     = true;
        layer.cone.fadeSize      = true;
        EffectDef def; def.name = "sub_frag_trail";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_pierce_trail — TRF_PRC 관통: 좁고 빠른 흰 트레일 (예리한 관통 느낌)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Cone;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  1.0f,  1.0f,  1.0f };
        layer.edgeColor      = { 0.75f, 0.9f,  1.0f,  0.0f };
        layer.particleCount  = 80;
        layer.emitRate       = 130.f;
        layer.speedMin       = 10.f;
        layer.speedMax       = 22.f;
        layer.lifetimeMin    = 0.06f;
        layer.lifetimeMax    = 0.16f;
        layer.sizeScale      = 1.1f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.cone.halfAngle     = 4.f;
        layer.cone.gravityScale  = 0.0f;
        layer.cone.startSizeMult = 1.0f;
        layer.cone.endSizeMult   = 0.08f;
        layer.cone.fadeAlpha     = true;
        layer.cone.fadeSize      = true;
        EffectDef def; def.name = "sub_pierce_trail";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_homing_spiral — TRF_HOM 유도: 청백 나선형 파티클 (추적 느낌)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Cone;
        layer.overrideColors = true;
        layer.coreColor      = { 0.80f, 0.95f, 1.0f, 1.0f };
        layer.edgeColor      = { 0.35f, 0.65f, 1.0f, 0.0f };
        layer.particleCount  = 60;
        layer.emitRate       = 80.f;
        layer.speedMin       = 2.f;
        layer.speedMax       = 7.f;
        layer.lifetimeMin    = 0.22f;
        layer.lifetimeMax    = 0.5f;
        layer.sizeScale      = 1.6f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.cone.halfAngle     = 72.f;
        layer.cone.gravityScale  = 0.0f;
        layer.cone.startSizeMult = 1.0f;
        layer.cone.endSizeMult   = 0.0f;
        layer.cone.fadeAlpha     = true;
        layer.cone.fadeSize      = true;
        EffectDef def; def.name = "sub_homing_spiral";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_chain_arc — TRF_CHA 연쇄: 노란/흰 전기 스파크 (번개 아크 느낌)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  0.96f, 0.45f, 1.0f };
        layer.edgeColor      = { 1.0f,  1.0f,  1.0f,  0.0f };
        layer.particleCount  = 55;
        layer.emitRate       = 80.f;
        layer.speedMin       = 4.f;
        layer.speedMax       = 12.f;
        layer.lifetimeMin    = 0.04f;
        layer.lifetimeMax    = 0.14f;
        layer.sizeScale      = 0.85f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.sphere.radius        = 1.2f;
        layer.sphere.shellFraction = 0.85f;
        layer.sphere.inward        = false;
        EffectDef def; def.name = "sub_chain_arc";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_orbital_halo — TRF_ORB 궤도: 기울어진 금빛 링 (공전 경로 표시)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Ring;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  0.93f, 0.55f, 1.0f };
        layer.edgeColor      = { 0.92f, 0.68f, 0.05f, 0.0f };
        layer.particleCount  = 90;
        layer.duration       = 0.55f;
        layer.speedMin       = 1.0f;
        layer.speedMax       = 3.5f;
        layer.lifetimeMin    = 0.4f;
        layer.lifetimeMax    = 0.9f;
        layer.sizeScale      = 2.8f;
        layer.ring.radius         = 0.4f;
        layer.ring.width          = 3.5f;
        layer.ring.expandSpeed    = 12.f;
        layer.ring.tiltX          = 50.f;
        layer.ring.rotateSpeed    = 2.5f;
        layer.ring.normalSpeedMin = 0.5f;
        layer.ring.normalSpeedMax = 2.0f;
        EffectDef def; def.name = "sub_orbital_halo";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_echo_ghost — TRF_ECH 반향: 희미한 청백 잔상 (메아리 느낌)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.overrideColors = true;
        layer.coreColor      = { 0.78f, 0.90f, 1.0f,  0.65f };
        layer.edgeColor      = { 0.45f, 0.65f, 1.0f,  0.0f  };
        layer.particleCount  = 40;
        layer.emitRate       = 28.f;
        layer.speedMin       = 0.3f;
        layer.speedMax       = 1.8f;
        layer.lifetimeMin    = 0.35f;
        layer.lifetimeMax    = 0.75f;
        layer.sizeScale      = 3.2f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.sphere.radius        = 1.6f;
        layer.sphere.shellFraction = 0.35f;
        layer.sphere.inward        = false;
        EffectDef def; def.name = "sub_echo_ghost";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // ══════════════════════════════════════════════════════════════════════════
    // 증폭 룬 서브 VFX — 각 수치 강화의 시각적 표현
    // ══════════════════════════════════════════════════════════════════════════

    // sub_strike_spark — AMP_DMG1 강타: 금빛 타격 스파크 (충격 강조)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Cone;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  0.90f, 0.18f, 1.0f };
        layer.edgeColor      = { 1.0f,  0.52f, 0.0f,  0.0f };
        layer.particleCount  = 50;
        layer.emitRate       = 60.f;
        layer.speedMin       = 5.f;
        layer.speedMax       = 14.f;
        layer.lifetimeMin    = 0.05f;
        layer.lifetimeMax    = 0.12f;
        layer.sizeScale      = 1.0f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.cone.halfAngle     = 42.f;
        layer.cone.gravityScale  = 0.08f;
        layer.cone.startSizeMult = 1.2f;
        layer.cone.endSizeMult   = 0.0f;
        layer.cone.fadeAlpha     = true;
        layer.cone.fadeSize      = true;
        EffectDef def; def.name = "sub_strike_spark";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_range_pulse — AMP_RAD1 범위: 확장하는 얇은 흰 링 (반경 시각화)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Ring;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  1.0f,  0.88f, 1.0f };
        layer.edgeColor      = { 0.80f, 0.85f, 0.45f, 0.0f };
        layer.particleCount  = 80;
        layer.duration       = 0.65f;
        layer.speedMin       = 2.0f;
        layer.speedMax       = 5.0f;
        layer.lifetimeMin    = 0.4f;
        layer.lifetimeMax    = 0.8f;
        layer.sizeScale      = 1.6f;
        layer.ring.radius         = 0.2f;
        layer.ring.width          = 2.5f;
        layer.ring.expandSpeed    = 28.f;
        layer.ring.tiltX          = 0.f;
        layer.ring.rotateSpeed    = 0.f;
        layer.ring.normalSpeedMin = 1.0f;
        layer.ring.normalSpeedMax = 3.5f;
        EffectDef def; def.name = "sub_range_pulse";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_speed_streak — AMP_SPD1 신속: 고속 흰 스트릭 트레일 (속도감)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Cone;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  1.0f,  1.0f,  1.0f };
        layer.edgeColor      = { 0.65f, 0.88f, 1.0f,  0.0f };
        layer.particleCount  = 100;
        layer.emitRate       = 160.f;
        layer.speedMin       = 12.f;
        layer.speedMax       = 28.f;
        layer.lifetimeMin    = 0.05f;
        layer.lifetimeMax    = 0.11f;
        layer.sizeScale      = 0.75f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.cone.halfAngle     = 7.f;
        layer.cone.gravityScale  = 0.0f;
        layer.cone.startSizeMult = 0.8f;
        layer.cone.endSizeMult   = 0.04f;
        layer.cone.fadeAlpha     = true;
        layer.cone.fadeSize      = true;
        EffectDef def; def.name = "sub_speed_streak";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_persist_glow — AMP_DUR1 지속: 온기 있는 황백 발광 오라 (지속 표현)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  0.94f, 0.72f, 0.85f };
        layer.edgeColor      = { 0.88f, 0.72f, 0.35f, 0.0f  };
        layer.particleCount  = 38;
        layer.emitRate       = 22.f;
        layer.speedMin       = 0.3f;
        layer.speedMax       = 1.1f;
        layer.lifetimeMin    = 0.6f;
        layer.lifetimeMax    = 1.3f;
        layer.sizeScale      = 2.8f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.sphere.radius        = 2.0f;
        layer.sphere.shellFraction = 0.5f;
        layer.sphere.inward        = false;
        EffectDef def; def.name = "sub_persist_glow";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_shatter_shard — AMP_DMG2 파괴: 주황/붉은 파편 (폭발적 피해 표현)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  0.52f, 0.08f, 1.0f };
        layer.edgeColor      = { 0.92f, 0.08f, 0.0f,  0.0f };
        layer.particleCount  = 70;
        layer.emitRate       = 95.f;
        layer.speedMin       = 5.f;
        layer.speedMax       = 16.f;
        layer.lifetimeMin    = 0.07f;
        layer.lifetimeMax    = 0.2f;
        layer.sizeScale      = 1.25f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.sphere.radius        = 1.6f;
        layer.sphere.shellFraction = 0.65f;
        layer.sphere.inward        = false;
        EffectDef def; def.name = "sub_shatter_shard";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_blast_wave — AMP_RAD2 광역: 넓은 황금 충격파 링 (광역 반경 표현)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Ring;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  0.98f, 0.75f, 1.0f };
        layer.edgeColor      = { 0.88f, 0.72f, 0.15f, 0.0f };
        layer.particleCount  = 120;
        layer.duration       = 0.85f;
        layer.speedMin       = 3.0f;
        layer.speedMax       = 8.0f;
        layer.lifetimeMin    = 0.5f;
        layer.lifetimeMax    = 1.1f;
        layer.sizeScale      = 2.8f;
        layer.ring.radius         = 0.5f;
        layer.ring.width          = 5.5f;
        layer.ring.expandSpeed    = 38.f;
        layer.ring.tiltX          = 0.f;
        layer.ring.rotateSpeed    = 0.f;
        layer.ring.normalSpeedMin = 1.5f;
        layer.ring.normalSpeedMax = 5.5f;
        EffectDef def; def.name = "sub_blast_wave";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_cool_mist — AMP_CD1 냉각: 청록 냉기 미스트 (빠른 쿨다운 표현)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.overrideColors = true;
        layer.coreColor      = { 0.35f, 0.95f, 1.0f,  0.82f };
        layer.edgeColor      = { 0.08f, 0.58f, 0.92f, 0.0f  };
        layer.particleCount  = 50;
        layer.emitRate       = 42.f;
        layer.speedMin       = 0.5f;
        layer.speedMax       = 2.8f;
        layer.lifetimeMin    = 0.4f;
        layer.lifetimeMax    = 1.0f;
        layer.sizeScale      = 2.3f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.sphere.radius        = 1.9f;
        layer.sphere.shellFraction = 0.4f;
        layer.sphere.inward        = false;
        EffectDef def; def.name = "sub_cool_mist";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_mana_wisp — AMP_MCO 절약: 부드러운 파란 마나 위스프 (마나 절약 표현)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Sphere;
        layer.overrideColors = true;
        layer.coreColor      = { 0.28f, 0.52f, 1.0f,  0.78f };
        layer.edgeColor      = { 0.08f, 0.22f, 0.88f, 0.0f  };
        layer.particleCount  = 35;
        layer.emitRate       = 24.f;
        layer.speedMin       = 0.2f;
        layer.speedMax       = 1.3f;
        layer.lifetimeMin    = 0.5f;
        layer.lifetimeMax    = 1.1f;
        layer.sizeScale      = 2.1f;
        layer.duration       = -1.f;
        layer.attachToProjectile = true;
        layer.sphere.radius        = 1.5f;
        layer.sphere.shellFraction = 0.3f;
        layer.sphere.inward        = true;
        EffectDef def; def.name = "sub_mana_wisp";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // sub_overload_surge — AMP_DMG3 과부하: 보라/흰 전하 폭주 (Ring + Sphere)
    {
        EffectDef def; def.name = "sub_overload_surge";

        // Layer 0: 보라 전기 링 (폭발 펄스)
        {
            EffectLayer ring;
            ring.type           = EmitterType::Ring;
            ring.overrideColors = true;
            ring.coreColor      = { 0.88f, 0.28f, 1.0f, 1.0f };
            ring.edgeColor      = { 1.0f,  0.80f, 1.0f, 0.0f };
            ring.particleCount  = 100;
            ring.duration       = 0.5f;
            ring.speedMin       = 6.f;
            ring.speedMax       = 18.f;
            ring.lifetimeMin    = 0.18f;
            ring.lifetimeMax    = 0.5f;
            ring.sizeScale      = 2.6f;
            ring.ring.radius         = 0.5f;
            ring.ring.width          = 3.8f;
            ring.ring.expandSpeed    = 32.f;
            ring.ring.tiltX          = 30.f;
            ring.ring.rotateSpeed    = 1.8f;
            ring.ring.normalSpeedMin = 3.0f;
            ring.ring.normalSpeedMax = 9.0f;
            def.layers.push_back(ring);
        }

        // Layer 1: 흰/보라 전기 스파크 (지속)
        {
            EffectLayer spark;
            spark.type           = EmitterType::Sphere;
            spark.overrideColors = true;
            spark.coreColor      = { 1.0f,  0.88f, 1.0f,  1.0f };
            spark.edgeColor      = { 0.70f, 0.18f, 1.0f,  0.0f };
            spark.particleCount  = 80;
            spark.emitRate       = 125.f;
            spark.speedMin       = 5.f;
            spark.speedMax       = 20.f;
            spark.lifetimeMin    = 0.04f;
            spark.lifetimeMax    = 0.14f;
            spark.sizeScale      = 1.05f;
            spark.duration       = -1.f;
            spark.attachToProjectile = true;
            spark.sphere.radius        = 1.1f;
            spark.sphere.shellFraction = 0.9f;
            spark.sphere.inward        = false;
            def.layers.push_back(spark);
        }

        Register(std::move(def));
    }

    // sub_expand_corona — AMP_RAD3 확장: 금빛 대형 코로나 링 (거대한 반경 표현)
    {
        EffectLayer layer;
        layer.type           = EmitterType::Ring;
        layer.overrideColors = true;
        layer.coreColor      = { 1.0f,  0.84f, 0.08f, 1.0f };
        layer.edgeColor      = { 0.97f, 0.58f, 0.0f,  0.0f };
        layer.particleCount  = 160;
        layer.duration       = 1.1f;
        layer.speedMin       = 5.f;
        layer.speedMax       = 12.f;
        layer.lifetimeMin    = 0.65f;
        layer.lifetimeMax    = 1.3f;
        layer.sizeScale      = 4.5f;
        layer.ring.radius         = 1.2f;
        layer.ring.width          = 7.0f;
        layer.ring.expandSpeed    = 55.f;
        layer.ring.tiltX          = 0.f;
        layer.ring.rotateSpeed    = 0.f;
        layer.ring.normalSpeedMin = 2.5f;
        layer.ring.normalSpeedMax = 9.0f;
        EffectDef def; def.name = "sub_expand_corona";
        def.layers.push_back(std::move(layer));
        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Enemy Markers — 적 타입 식별용 상시 표시 마커 (헤드 + 발밑)
    //   다크판타지 톤: 보라/회 베이스 + 자폭만 적색 위험 신호 + 저격은 금색
    //   색이 아닌 "형태/반경/회전속도"로 적 타입 식별. duration=-1 무한 루프.
    //   EnemyComponent::TrackMarkerVFX 가 매 프레임 위치 갱신.
    // ──────────────────────────────────────────────────────────────────────────
    {
        // 다크판타지 톤 유지하되 밝은 환경(용암/화염)에서도 가시성 확보를 위해 코어는 거의 백색에 가까운 라벤더
        const XMFLOAT4 kPurpleCore = { 0.95f, 0.85f, 1.00f, 1.00f };
        const XMFLOAT4 kPurpleEdge = { 0.55f, 0.30f, 0.85f, 0.85f };
        const XMFLOAT4 kDangerCore = { 1.00f, 0.40f, 0.35f, 1.00f };
        const XMFLOAT4 kDangerEdge = { 0.70f, 0.05f, 0.05f, 0.90f };
        const XMFLOAT4 kGoldCore   = { 1.00f, 0.95f, 0.55f, 1.00f };
        const XMFLOAT4 kGoldEdge   = { 0.65f, 0.40f, 0.05f, 0.85f };

        // 헤드: Sphere shell (껍데기만, speed=0, 매우 짧은 lifetime + 매우 높은 emitRate)
        //   → 매 프레임 같은 위치에 재생성 → 잔상 없이 적과 단단히 따라다니는 정적 마크
        auto makeHeadSphere = [this](const char* name,
                                     float radius,
                                     XMFLOAT4 core, XMFLOAT4 edge,
                                     int particleCount, float sizeScale)
        {
            EffectLayer layer;
            layer.type           = EmitterType::Sphere;
            layer.element        = ElementType::Wind;
            layer.overrideColors = true;
            layer.coreColor      = core;
            layer.edgeColor      = edge;
            layer.particleCount  = particleCount;
            layer.sizeScale      = sizeScale;
            layer.speedMin       = 0.f;
            layer.speedMax       = 0.f;
            layer.lifetimeMin    = 0.18f;
            layer.lifetimeMax    = 0.22f;
            layer.duration       = -1.f;
            layer.emitRate       = particleCount / 0.20f;  // 동시 활성 ≈ particleCount, 매 0.2초 회전
            layer.sphere.radius        = radius;
            layer.sphere.shellFraction = 1.0f;            // 껍데기만 → 윤곽선 원
            layer.sphere.inward        = false;
            layer.sphere.rotationSpeed = 0.f;
            EffectDef def;
            def.name    = name;
            def.element = ElementType::Wind;
            def.layers.push_back(std::move(layer));
            Register(std::move(def));
        };

        // 발밑: Ring (정적, speed=0, 매우 짧은 lifetime + 높은 emitRate)
        //   → 지면 위에 박힌 윤곽선 원. 회전 없음, 적과 함께 평행 이동만.
        auto makeFootRing = [this](const char* name,
                                   float radius, float width,
                                   XMFLOAT4 core, XMFLOAT4 edge,
                                   int particleCount, float sizeScale)
        {
            EffectLayer layer;
            layer.type           = EmitterType::Ring;
            layer.element        = ElementType::Wind;
            layer.overrideColors = true;
            layer.coreColor      = core;
            layer.edgeColor      = edge;
            layer.particleCount  = particleCount;
            layer.sizeScale      = sizeScale;
            layer.speedMin       = 0.f;
            layer.speedMax       = 0.f;
            layer.lifetimeMin    = 0.18f;
            layer.lifetimeMax    = 0.22f;
            layer.duration       = -1.f;
            layer.emitRate       = particleCount / 0.20f;
            layer.ring.radius         = radius;
            layer.ring.width          = width;
            layer.ring.expandSpeed    = 0.f;
            layer.ring.tiltX          = 0.f;
            layer.ring.rotateSpeed    = 0.f;
            layer.ring.normalSpeedMin = 0.f;
            layer.ring.normalSpeedMax = 0.f;
            EffectDef def;
            def.name    = name;
            def.element = ElementType::Wind;
            def.layers.push_back(std::move(layer));
            Register(std::move(def));
        };

        // ─── 헤드 마커 (Sphere shell — radius 크기로 차별화) ──
        makeHeadSphere("marker_head_rushfront",   1.3f, kPurpleCore, kPurpleEdge, 80,  4.0f);
        makeHeadSphere("marker_head_rushaoe",     1.8f, kPurpleCore, kPurpleEdge, 110, 4.0f);
        makeHeadSphere("marker_head_quickjab",    1.0f, kPurpleCore, kPurpleEdge, 60,  3.5f);
        makeHeadSphere("marker_head_suicide",     1.5f, kDangerCore, kDangerEdge, 100, 5.0f);
        makeHeadSphere("marker_head_ranged",      1.3f, kPurpleCore, kPurpleEdge, 80,  3.5f);
        makeHeadSphere("marker_head_chargedshot", 1.0f, kGoldCore,   kGoldEdge,   70,  4.0f);
        makeHeadSphere("marker_head_grenade",     1.8f, kPurpleCore, kPurpleEdge, 100, 4.0f);

        // ─── 발밑 데칼 (Ring — radius/width로 차별화) ──
        makeFootRing("marker_foot_rushfront",   1.6f, 0.40f, kPurpleCore, kPurpleEdge, 130, 4.0f);
        makeFootRing("marker_foot_rushaoe",     2.2f, 0.35f, kPurpleCore, kPurpleEdge, 160, 4.0f);
        makeFootRing("marker_foot_quickjab",    1.3f, 0.25f, kPurpleCore, kPurpleEdge, 110, 3.5f);
        makeFootRing("marker_foot_suicide",     2.0f, 0.50f, kDangerCore, kDangerEdge, 170, 5.0f);
        makeFootRing("marker_foot_ranged",      1.7f, 0.20f, kPurpleCore, kPurpleEdge, 110, 3.5f);
        makeFootRing("marker_foot_chargedshot", 1.9f, 0.20f, kGoldCore,   kGoldEdge,   120, 4.0f);
        makeFootRing("marker_foot_grenade",     2.2f, 0.25f, kPurpleCore, kPurpleEdge, 140, 4.0f);
    }

    // ──────────────────────────────────────────────────────────────────────────
    // charge_gather — 차지 중 캐릭터 주변 에너지 결집 VFX (지속형, 단계마다 재스폰)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "charge_gather";
        def.element = ElementType::None;

        // Layer 0 — Ring: 캐릭터를 공전하는 에너지 입자
        {
            EffectLayer ring;
            ring.type           = EmitterType::Ring;
            ring.element        = ElementType::None;
            ring.overrideColors = true;
            ring.coreColor      = { 1.0f, 0.95f, 0.6f, 1.0f };
            ring.edgeColor      = { 0.8f, 0.55f, 0.1f, 0.0f };
            ring.particleCount  = 100;
            ring.duration       = 4.0f;
            ring.speedMin       = 1.f;
            ring.speedMax       = 4.f;
            ring.lifetimeMin    = 0.5f;
            ring.lifetimeMax    = 1.0f;
            ring.sizeScale      = 2.5f;

            ring.ring.radius         = 2.5f;
            ring.ring.width          = 1.2f;
            ring.ring.rotateSpeed    = 3.5f;
            ring.ring.normalSpeedMin = 1.f;
            ring.ring.normalSpeedMax = 3.f;

            def.layers.push_back(ring);
        }

        // Layer 1 — Sphere: 바깥에서 캐릭터 방향으로 수렴하는 입자
        {
            EffectLayer sphere;
            sphere.type           = EmitterType::Sphere;
            sphere.element        = ElementType::None;
            sphere.overrideColors = true;
            sphere.coreColor      = { 1.0f, 1.0f, 0.8f, 1.0f };
            sphere.edgeColor      = { 0.9f, 0.65f, 0.2f, 0.0f };
            sphere.particleCount  = 70;
            sphere.duration       = 4.0f;
            sphere.speedMin       = 5.f;
            sphere.speedMax       = 14.f;
            sphere.lifetimeMin    = 0.4f;
            sphere.lifetimeMax    = 0.8f;
            sphere.sizeScale      = 2.0f;

            sphere.sphere.radius        = 3.5f;
            sphere.sphere.shellFraction = 0.8f;
            sphere.sphere.inward        = true;
            sphere.sphere.rotationSpeed = 1.5f;

            def.layers.push_back(sphere);
        }

        Register(std::move(def));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // charge_pulse — 차지 단계 전환 시 발생하는 에너지 파동 링 (순간형)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectDef def;
        def.name    = "charge_pulse";
        def.element = ElementType::None;

        EffectLayer ring;
        ring.type           = EmitterType::Ring;
        ring.element        = ElementType::None;
        ring.overrideColors = true;
        ring.coreColor      = { 1.0f, 0.95f, 0.7f, 1.0f };
        ring.edgeColor      = { 0.7f, 0.4f, 0.0f, 0.0f };
        ring.particleCount  = 180;
        ring.duration       = 0.6f;
        ring.speedMin       = 3.f;
        ring.speedMax       = 8.f;
        ring.lifetimeMin    = 0.25f;
        ring.lifetimeMax    = 0.5f;
        ring.sizeScale      = 3.5f;

        ring.ring.radius         = 0.5f;
        ring.ring.width          = 1.5f;
        ring.ring.expandSpeed    = 18.f;
        ring.ring.normalSpeedMin = 2.f;
        ring.ring.normalSpeedMax = 6.f;

        def.layers.push_back(ring);
        Register(std::move(def));
    }

    // ══════════════════════════════════════════════════════════════════════════
    // ▌ 룬 텍스처 이펙트 (MagicCircle / Sprite → VFXSpriteManager 위임)
    // ══════════════════════════════════════════════════════════════════════════

    // ABY_ECO_Trigger — 메아리 룬 발 아래 회전 마법진
    {
        EffectDef def;
        def.name    = "ABY_ECO_Trigger";
        def.element = ElementType::None;

        EffectLayer layer;
        layer.type = EmitterType::MagicCircle;
        layer.magicCircle.texId       = "magic3";
        layer.magicCircle.screenSize  = 140.f;
        layer.magicCircle.rotateSpeed = 1.5f;
        layer.magicCircle.duration    = 2.5f;
        layer.magicCircle.color       = { 1.f, 1.f, 1.f, 1.f }; // 색상은 호출 시 원소별로 오버라이드
        def.layers.push_back(layer);
        Register(std::move(def));
    }

    // ABY_EXC_Kill — 처형자 룬 처형 킬 skull 마커
    {
        EffectDef def;
        def.name    = "ABY_EXC_Kill";
        def.element = ElementType::None;

        EffectLayer layer;
        layer.type = EmitterType::Sprite;
        layer.sprite.texId      = "skull";
        layer.sprite.screenSize = 96.f;
        layer.sprite.duration   = 1.0f;
        layer.sprite.color      = { 0.9f, 0.15f, 0.1f, 1.f };
        layer.sprite.skullPop   = true;
        def.layers.push_back(layer);
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
