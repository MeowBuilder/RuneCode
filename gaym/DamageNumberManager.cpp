#include "stdafx.h"
#include "DamageNumberManager.h"
#include <algorithm>

using namespace DirectX;

DamageNumberManager& DamageNumberManager::Get()
{
    static DamageNumberManager instance;
    return instance;
}

void DamageNumberManager::AddNumber(const XMFLOAT3& worldPos, float damage)
{
    DamageNumber n;
    n.worldPos   = worldPos;
    n.damage     = damage;
    n.lifetime   = LIFETIME;
    n.riseOffset = 0.f;
    m_numbers.push_back(n);
}

void DamageNumberManager::AddText(const XMFLOAT3& worldPos, const wchar_t* text, XMFLOAT4 color)
{
    DamageNumber n;
    n.worldPos   = worldPos;
    n.damage     = 0.f;
    n.lifetime   = LIFETIME;
    n.riseOffset = 0.f;
    n.color      = color;
    wcscpy_s(n.customText, text);
    m_numbers.push_back(n);
}

void DamageNumberManager::Update(float dt)
{
    for (auto& n : m_numbers)
    {
        n.lifetime   -= dt;
        n.riseOffset += RISE_SPEED * dt;
    }

    m_numbers.erase(
        std::remove_if(m_numbers.begin(), m_numbers.end(),
            [](const DamageNumber& n) { return n.lifetime <= 0.f; }),
        m_numbers.end());
}

void DamageNumberManager::Render(SpriteBatch* pBatch, SpriteFont* pFont,
                                  const XMFLOAT4X4& viewProj, int screenW, int screenH)
{
    if (m_numbers.empty()) return;

    XMMATRIX mVP = XMLoadFloat4x4(&viewProj);

    for (const auto& n : m_numbers)
    {
        // Apply rise offset to world position
        XMFLOAT3 pos = n.worldPos;
        pos.y += n.riseOffset;

        // Project world → NDC via ViewProj
        XMVECTOR worldV = XMLoadFloat3(&pos);
        XMVECTOR ndc    = XMVector3TransformCoord(worldV, mVP);

        float w = XMVectorGetW(XMVector3Transform(worldV, mVP));
        if (w <= 0.01f) continue;  // behind or at camera

        float cx = XMVectorGetX(ndc);
        float cy = XMVectorGetY(ndc);

        // NDC → screen pixels
        float sx = (cx * 0.5f + 0.5f) * (float)screenW;
        float sy = (-cy * 0.5f + 0.5f) * (float)screenH;

        // Clip if outside screen
        if (sx < -50.f || sx > (float)screenW + 50.f) continue;
        if (sy < -50.f || sy > (float)screenH + 50.f) continue;

        // Fade-in briefly, then fade out
        float t     = n.lifetime / LIFETIME;   // 1 → 0 over lifetime
        float alpha = (t > 0.8f) ? ((1.f - t) / 0.2f) : t;  // fade-in first 0.2s, fade-out rest
        if (alpha < 0.f) alpha = 0.f;
        if (alpha > 1.f) alpha = 1.f;

        float scale = 1.0f + t * 0.25f;

        XMVECTORF32 color = { n.color.x, n.color.y, n.color.z, alpha };

        wchar_t buf[32];
        if (n.customText[0] != L'\0')
            wcscpy_s(buf, n.customText);
        else
            swprintf_s(buf, L"%.0f", n.damage);

        XMVECTOR textSize = pFont->MeasureString(buf);
        float halfW = XMVectorGetX(textSize) * scale * 0.5f;
        float halfH = XMVectorGetY(textSize) * scale * 0.5f;

        pFont->DrawString(pBatch, buf,
            XMFLOAT2(sx - halfW, sy - halfH),
            color,
            0.f,                  // rotation
            XMFLOAT2(0.f, 0.f),  // origin (already centered via sx/sy offset)
            scale);
    }
}
