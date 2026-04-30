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

    // ──────────────────────────────────────────────────────────────────────────
    // R_Meteor — 메테오 (단일 거대 덩어리 낙하 + 충돌 폭발)
    // ──────────────────────────────────────────────────────────────────────────
    {
        EffectLayer layer = MakeSPHLayer(ElementType::Fire);
        layer.overrideColors = true;
        layer.coreColor = { 1.0f, 0.92f, 0.45f, 1.0f };
        layer.edgeColor = { 0.72f, 0.04f, 0.01f, 0.65f };

        SPHEmitterParams& s = layer.sph;
        s.particleCount        = 2400;
        s.spawnRadius          = 3.f;
        s.nucleusSpawnFraction = 0.85f;
        s.nucleusSpawnRadius   = 1.5f;
        s.masterCPStrength     = 65.f;
        s.masterCPSphereRadius = 6.f;
        s.masterCPFallSpeed    = 22.f;

        VFXPhase p0;
        p0.startTime             = 0.f;
        p0.duration              = 3.f;
        p0.motionMode            = ParticleMotionMode::OrbitalCP;
        p0.globalGravityStrength = 18.f;
        s.phases.push_back(p0);

        VFXPhase p1;
        p1.startTime  = 3.f;
        p1.duration   = 2.5f;
        p1.motionMode = ParticleMotionMode::Gravity;
        p1.gravityDesc.gravity         = { 0.f, -18.f, 0.f };
        p1.gravityDesc.initialSpeedMin = 22.f;
        p1.gravityDesc.initialSpeedMax = 55.f;
        p1.phaseMaxSpeed               = 80.f;
        p1.triggerExplodeFadeOnEnter   = true;
        s.phases.push_back(p1);

        FinalizeSPHLayer(layer);

        EffectDef def;
        def.name    = "R_Meteor";
        def.element = ElementType::Fire;
        def.layers.push_back(std::move(layer));
        Register(std::move(def));

        VFXModifier enhMod;
        enhMod.particleCountMult = 1.6f;
        enhMod.strengthMult      = 1.3f;
        enhMod.sizeScaleMult     = 1.4f;
        RegisterRuneMod("R_Meteor", RUNE_ENHANCE, enhMod);
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
