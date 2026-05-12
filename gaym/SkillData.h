#pragma once

#include "SkillTypes.h"
#include <string>

// Base data structure for skill configuration
struct SkillData
{
    std::string name;
    ElementType element = ElementType::None;
    ActivationType activationType = ActivationType::Instant;

    float damage = 0.0f;
    float cooldown = 1.0f;          // Seconds
    float castTime = 0.0f;          // Instant if 0
    float range = 10.0f;
    float radius = 0.0f;            // For AoE skills

    // Resource costs (for future use)
    float manaCost = 0.0f;
    float staminaCost = 0.0f;
};

// Preset skill data for fire element skills
namespace FireSkillPresets
{
    inline SkillData Fireball()
    {
        SkillData data;
        data.name = "Fireball";
        data.element = ElementType::Fire;
        data.activationType = ActivationType::Instant;
        data.damage = 30.0f;
        data.cooldown = 2.0f;
        data.castTime = 0.0f;
        data.range = 50.0f;
        data.radius = 3.0f;
        data.manaCost = 10.0f;
        return data;
    }

    inline SkillData FlameWave()
    {
        SkillData data;
        data.name = "Flame Wave";
        data.element = ElementType::Fire;
        data.activationType = ActivationType::Instant;
        data.damage = 20.0f;
        data.cooldown = 5.0f;
        data.castTime = 0.0f;
        data.range = 15.0f;
        data.radius = 8.0f;
        data.manaCost = 25.0f;
        return data;
    }

    inline SkillData Meteor()
    {
        SkillData data;
        data.name = "Meteor";
        data.element = ElementType::Fire;
        data.activationType = ActivationType::Instant;
        data.damage = 100.0f;
        data.cooldown = 30.0f;
        data.castTime = 1.5f;
        data.range = 40.0f;
        data.radius = 10.0f;
        data.manaCost = 50.0f;
        return data;
    }
}

namespace WaterSkillPresets
{
    inline SkillData WaterPuddle()
    {
        SkillData d; d.name = "WaterPuddle"; d.element = ElementType::Water;
        d.activationType = ActivationType::Instant;
        d.damage = 18.0f; d.cooldown = 6.0f; d.range = 20.0f; d.radius = 7.0f;
        return d;
    }
    inline SkillData WaterWave()
    {
        SkillData d; d.name = "WaterWave"; d.element = ElementType::Water;
        d.activationType = ActivationType::Instant;
        d.damage = 22.0f; d.cooldown = 3.5f; d.range = 18.0f; d.radius = 7.0f;
        return d;
    }
    inline SkillData WaterVortex()
    {
        SkillData d; d.name = "WaterVortex"; d.element = ElementType::Water;
        d.activationType = ActivationType::Instant;
        d.damage = 10.0f; d.cooldown = 8.0f; d.range = 20.0f; d.radius = 9.0f;
        return d;
    }
    inline SkillData TidalWave()
    {
        SkillData d; d.name = "TidalWave"; d.element = ElementType::Water;
        d.activationType = ActivationType::Instant;
        d.damage = 140.0f; d.cooldown = 25.0f; d.range = 35.0f; d.radius = 15.0f;
        return d;
    }
    inline SkillData WaterOrb()
    {
        SkillData d; d.name = "WaterOrb"; d.element = ElementType::Water;
        d.activationType = ActivationType::Instant;
        d.damage = 28.0f; d.cooldown = 2.2f; d.range = 50.0f; d.radius = 4.0f;
        return d;
    }
}

namespace WindSkillPresets
{
    inline SkillData WindCutter()
    {
        SkillData d; d.name = "WindCutter"; d.element = ElementType::Wind;
        d.activationType = ActivationType::Instant;
        d.damage = 30.0f; d.cooldown = 2.5f; d.range = 22.0f; d.radius = 2.0f;
        return d;
    }
    inline SkillData GaleRush()
    {
        SkillData d; d.name = "GaleRush"; d.element = ElementType::Wind;
        d.activationType = ActivationType::Instant;
        d.damage = 45.0f; d.cooldown = 5.0f; d.range = 15.0f; d.radius = 5.0f;
        return d;
    }
    inline SkillData Tornado()
    {
        SkillData d; d.name = "Tornado"; d.element = ElementType::Wind;
        d.activationType = ActivationType::Instant;
        d.damage = 15.0f; d.cooldown = 20.0f; d.range = 25.0f; d.radius = 10.0f;
        return d;
    }
    inline SkillData WindShot()
    {
        SkillData d; d.name = "WindShot"; d.element = ElementType::Wind;
        d.activationType = ActivationType::Instant;
        d.damage = 18.0f; d.cooldown = 1.5f; d.range = 60.0f; d.radius = 2.0f;
        return d;
    }
}

namespace EarthSkillPresets
{
    inline SkillData StoneSpikes()
    {
        SkillData d; d.name = "StoneSpikes"; d.element = ElementType::Earth;
        d.activationType = ActivationType::Instant;
        d.damage = 38.0f; d.cooldown = 4.0f; d.range = 20.0f; d.radius = 3.5f;
        return d;
    }
    inline SkillData RockThrow()
    {
        SkillData d; d.name = "RockThrow"; d.element = ElementType::Earth;
        d.activationType = ActivationType::Instant;
        d.damage = 75.0f; d.cooldown = 6.0f; d.range = 30.0f; d.radius = 6.0f;
        return d;
    }
    inline SkillData Earthquake()
    {
        SkillData d; d.name = "Earthquake"; d.element = ElementType::Earth;
        d.activationType = ActivationType::Instant;
        d.damage = 110.0f; d.cooldown = 25.0f; d.range = 14.0f; d.radius = 14.0f;
        return d;
    }
    inline SkillData EarthShard()
    {
        SkillData d; d.name = "EarthShard"; d.element = ElementType::Earth;
        d.activationType = ActivationType::Instant;
        d.damage = 42.0f; d.cooldown = 2.8f; d.range = 40.0f; d.radius = 3.0f;
        return d;
    }
}
