#pragma once
#include "Component.h"
#include "Mesh.h"
#include <memory>

class Shader;

class RenderComponent : public Component
{
public:
    RenderComponent(GameObject* pOwner);
    ~RenderComponent();

    virtual void Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList) override;
    virtual void Update(float deltaTime) override;
    virtual void Render(ID3D12GraphicsCommandList* pCommandList) override;

    void SetMesh(Mesh* pMesh) { m_pMesh = pMesh; }
    Mesh* GetMesh() { return m_pMesh; }

    void SetCastsShadow(bool bCasts) { m_bCastsShadow = bCasts; }
    bool CastsShadow() const { return m_bCastsShadow; }

    void SetTransparent(bool bTransparent) { m_bTransparent = bTransparent; }
    bool IsTransparent() const { return m_bTransparent; }

    // UI/오버레이처럼 depth test 무시하고 항상 맨 위에 그리기 (공격 인디케이터 등)
    void SetOverlay(bool bOverlay) { m_bOverlay = bOverlay; }
    bool IsOverlay() const { return m_bOverlay; }

    void SetOwnerShader(Shader* pShader) { m_pOwnerShader = pShader; }

    // 컷씬 등에서 일시적으로 mesh 렌더 skip — Shader::Render 가 이 플래그 보고 건너뜀.
    void SetVisible(bool b) { m_bVisible = b; }
    bool IsVisible() const  { return m_bVisible; }

private:
    Mesh* m_pMesh = nullptr;
    bool m_bCastsShadow = false;
    bool m_bTransparent = false;
    bool m_bOverlay = false;
    bool m_bVisible = true;
    Shader* m_pOwnerShader = nullptr;
};