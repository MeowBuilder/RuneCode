#pragma once
#include "stdafx.h"
#include <SpriteBatch.h>
#include <SpriteFont.h>
#include <vector>

struct DamageNumber
{
    DirectX::XMFLOAT3 worldPos;
    float damage;
    float lifetime;
    float riseOffset;
    wchar_t customText[32] = {};   // 비어있으면 damage 숫자 표시, 채워지면 텍스트 표시
    DirectX::XMFLOAT4 color = { 1.f, 0.9f, 0.15f, 1.f }; // 기본 노란색
};

class DamageNumberManager
{
public:
    static DamageNumberManager& Get();

    void AddNumber(const DirectX::XMFLOAT3& worldPos, float damage);
    void AddText(const DirectX::XMFLOAT3& worldPos, const wchar_t* text,
                 DirectX::XMFLOAT4 color = { 1.f, 0.2f, 0.2f, 1.f });
    void Update(float dt);
    void Render(DirectX::SpriteBatch* pBatch, DirectX::SpriteFont* pFont,
                const DirectX::XMFLOAT4X4& viewProj, int screenW, int screenH);

private:
    DamageNumberManager() = default;

    static constexpr float LIFETIME   = 1.5f;
    static constexpr float RISE_SPEED = 2.5f;

    std::vector<DamageNumber> m_numbers;
};
