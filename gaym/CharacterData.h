#pragma once
#include <DirectXMath.h>
#include "SkillTypes.h"

using namespace DirectX;

struct CharacterData
{
    const wchar_t* name;
    ElementType    element;

    const char* meshPath;
    const char* animPath;

    float baseHP;
    float moveSpeed;
    float dashCooldown;
    float dashDuration;
    float dashSpeedMult;

    XMFLOAT4 dashCoreColor;
    XMFLOAT4 dashEdgeColor;
    XMFLOAT4 cardBgColor;
    XMFLOAT4 cardTextColor;
};

inline const CharacterData* GetAllCharacterData(int* outCount)
{
    static const CharacterData s_table[] =
    {
        {
            L"불꽃술사",
            ElementType::Fire,
            "Assets/Player/MageRed.bin",
            "Assets/Player/MageRed_Anim.bin",
            80.f, 22.f, 1.0f, 0.25f, 3.2f,
            { 1.0f, 0.50f, 0.05f, 1.0f },
            { 1.0f, 0.10f, 0.00f, 0.0f },
            { 0.45f, 0.10f, 0.02f, 0.82f },
            { 1.0f, 0.50f, 0.05f, 1.0f },
        },
        {
            L"물결술사",
            ElementType::Water,
            "Assets/Player/MageBlue.bin",
            "Assets/Player/MageBlue_Anim.bin",
            100.f, 20.f, 1.2f, 0.25f, 3.2f,
            { 0.35f, 0.75f, 1.0f, 1.0f },
            { 0.05f, 0.15f, 0.55f, 0.0f },
            { 0.04f, 0.12f, 0.40f, 0.82f },
            { 0.35f, 0.75f, 1.0f, 1.0f },
        },
        {
            L"바람술사",
            ElementType::Wind,
            "Assets/Player/MageGreen.bin",
            "Assets/Player/MageGreen_Anim.bin",
            70.f, 28.f, 0.8f, 0.20f, 3.6f,
            { 0.65f, 1.0f, 0.60f, 0.90f },
            { 0.20f, 0.80f, 0.15f, 0.0f },
            { 0.05f, 0.25f, 0.05f, 0.82f },
            { 0.65f, 1.0f, 0.50f, 1.0f },
        },
        {
            L"대지술사",
            ElementType::Earth,
            "Assets/Player/MageOrange.bin",
            "Assets/Player/MageOrange_Anim.bin",
            130.f, 16.f, 1.5f, 0.30f, 2.8f,
            { 0.65f, 0.42f, 0.18f, 1.0f },
            { 0.35f, 0.18f, 0.05f, 0.0f },
            { 0.22f, 0.14f, 0.04f, 0.82f },
            { 0.65f, 0.45f, 0.18f, 1.0f },
        },
    };

    if (outCount) *outCount = 4;
    return s_table;
}

inline const CharacterData& GetCharacterData(ElementType e)
{
    int count = 0;
    const CharacterData* table = GetAllCharacterData(&count);
    for (int i = 0; i < count; ++i)
        if (table[i].element == e) return table[i];
    return table[1];  // Water fallback
}
