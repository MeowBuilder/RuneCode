#include "stdafx.h"
#include "RenderComponent.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "Mesh.h"
#include "Shader.h"

RenderComponent::RenderComponent(GameObject* pOwner) : Component(pOwner)
{
}

RenderComponent::~RenderComponent()
{
    if (m_pOwnerShader)
        m_pOwnerShader->RemoveRenderComponent(this);
}

void RenderComponent::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList)
{
    // This component no longer needs to create its own constant buffer.
    // The GameObject now manages it.
}

void RenderComponent::Update(float deltaTime)
{
}

void RenderComponent::Render(ID3D12GraphicsCommandList* pCommandList)
{
    if (!m_pMesh || !m_bVisible)
        return;


    // Set the per-object descriptor table to root parameter 0 (CBV)
    pCommandList->SetGraphicsRootDescriptorTable(0, m_pOwner->GetGpuDescriptorHandle());

    if (m_pOwner->HasTexture())
        pCommandList->SetGraphicsRootDescriptorTable(2, m_pOwner->GetSrvDescriptorHandle());

    if (m_pOwner->HasNormalMap())
        pCommandList->SetGraphicsRootDescriptorTable(4, m_pOwner->GetNormalMapSrvHandle());

    if (m_pOwner->HasHeightMap())
        pCommandList->SetGraphicsRootDescriptorTable(5, m_pOwner->GetHeightMapSrvHandle());

    if (m_pOwner->HasEmissiveTexture())
        pCommandList->SetGraphicsRootDescriptorTable(6, m_pOwner->GetEmissiveSrvDescriptorHandle());

    if (m_pOwner->HasAOMap())
        pCommandList->SetGraphicsRootDescriptorTable(7, m_pOwner->GetAOMapSrvHandle());

    if (m_pOwner->HasRoughnessMap())
        pCommandList->SetGraphicsRootDescriptorTable(8, m_pOwner->GetRoughnessMapSrvHandle());

    // 서브메쉬가 여러 개면 모두 렌더 (스킨드 메쉬 다리 등)
    if (auto* pMFF = dynamic_cast<MeshFromFile*>(m_pMesh))
    {
        int nSubs = pMFF->GetSubMeshCount();
        if (nSubs > 1)
        {
            for (int i = 0; i < nSubs; i++)
                m_pMesh->Render(pCommandList, i);
            return;
        }
    }
    m_pMesh->Render(pCommandList, 0);
}
