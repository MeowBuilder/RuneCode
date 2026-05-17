#include "stdafx.h"
#include "Mesh.h"
#include "Dx12App.h"
#include "MeshLoader.h"
#include <random>
#include <cmath>

Mesh::~Mesh()
{
}

/////////////////////////////////////////////////////////////////////////////////////////////////
//
MeshFromFile::MeshFromFile(ID3D12Device *pd3dDevice, ID3D12GraphicsCommandList *pd3dCommandList, MeshLoadInfo *pMeshInfo)
{
	m_nVertices = pMeshInfo->m_nVertices;
	m_nType = pMeshInfo->m_nType;

	m_pd3dPositionBuffer = Dx12App::CreateBufferResource(pMeshInfo->m_pxmf3Positions, sizeof(XMFLOAT3) * m_nVertices, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dPositionUploadBuffer);

	m_d3dPositionBufferView.BufferLocation = m_pd3dPositionBuffer->GetGPUVirtualAddress();
	m_d3dPositionBufferView.StrideInBytes = sizeof(XMFLOAT3);
	m_d3dPositionBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

	m_nSubMeshes = pMeshInfo->m_nSubMeshes;
	if (m_nSubMeshes > 0)
	{
		m_ppd3dSubSetIndexBuffers = new ComPtr<ID3D12Resource>[m_nSubMeshes];
		m_ppd3dSubSetIndexUploadBuffers = new ComPtr<ID3D12Resource>[m_nSubMeshes];
		m_pd3dSubSetIndexBufferViews = new D3D12_INDEX_BUFFER_VIEW[m_nSubMeshes];

		m_pnSubSetIndices = new int[m_nSubMeshes];

		for (int i = 0; i < m_nSubMeshes; i++)
		{
			m_pnSubSetIndices[i] = pMeshInfo->m_pnSubSetIndices[i];
			m_ppd3dSubSetIndexBuffers[i] = Dx12App::CreateBufferResource(pMeshInfo->m_ppnSubSetIndices[i], sizeof(UINT) * m_pnSubSetIndices[i], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER, &m_ppd3dSubSetIndexUploadBuffers[i]);

			m_pd3dSubSetIndexBufferViews[i].BufferLocation = m_ppd3dSubSetIndexBuffers[i]->GetGPUVirtualAddress();
			m_pd3dSubSetIndexBufferViews[i].Format = DXGI_FORMAT_R32_UINT;
			m_pd3dSubSetIndexBufferViews[i].SizeInBytes = sizeof(UINT) * pMeshInfo->m_pnSubSetIndices[i];
		}
	}
};

MeshFromFile::~MeshFromFile()
{
	if (m_nSubMeshes > 0)
	{
		if (m_ppd3dSubSetIndexBuffers) delete[] m_ppd3dSubSetIndexBuffers;
		if (m_pd3dSubSetIndexBufferViews) delete[] m_pd3dSubSetIndexBufferViews;

		if (m_pnSubSetIndices) delete[] m_pnSubSetIndices;
	}
}

void MeshFromFile::ReleaseUploadBuffers()
{
	Mesh::ReleaseUploadBuffers();

	if ((m_nSubMeshes > 0) && m_ppd3dSubSetIndexUploadBuffers)
	{
		delete[] m_ppd3dSubSetIndexUploadBuffers;
		m_ppd3dSubSetIndexUploadBuffers = NULL;
	}
}

void MeshFromFile::Render(ID3D12GraphicsCommandList *pd3dCommandList, int nSubSet)
{
	pd3dCommandList->IASetPrimitiveTopology(m_d3dPrimitiveTopology);
	pd3dCommandList->IASetVertexBuffers(m_nSlot, 1, &m_d3dPositionBufferView);
	if ((m_nSubMeshes > 0) && (nSubSet < m_nSubMeshes))
	{
		pd3dCommandList->IASetIndexBuffer(&(m_pd3dSubSetIndexBufferViews[nSubSet]));
		pd3dCommandList->DrawIndexedInstanced(m_pnSubSetIndices[nSubSet], 1, 0, 0, 0);
	}
	else
	{
		pd3dCommandList->DrawInstanced(m_nVertices, 1, m_nOffset, 0);
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////////
//
MeshIlluminatedFromFile::MeshIlluminatedFromFile(ID3D12Device *pd3dDevice, ID3D12GraphicsCommandList *pd3dCommandList, MeshLoadInfo *pMeshInfo) : MeshFromFile(pd3dDevice, pd3dCommandList, pMeshInfo)
{
	m_pd3dNormalBuffer = Dx12App::CreateBufferResource(pMeshInfo->m_pxmf3Normals, sizeof(XMFLOAT3) * m_nVertices, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dNormalUploadBuffer);

	m_d3dNormalBufferView.BufferLocation = m_pd3dNormalBuffer->GetGPUVirtualAddress();
	m_d3dNormalBufferView.StrideInBytes = sizeof(XMFLOAT3);
	m_d3dNormalBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

	if (m_nType & VERTEXT_TEXTURE_COORD0)
	{
		m_pd3dTextureCoord0Buffer = Dx12App::CreateBufferResource(pMeshInfo->m_pxmf2TextureCoords0, sizeof(XMFLOAT2) * m_nVertices, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dTextureCoord0UploadBuffer);

		m_d3dTextureCoord0BufferView.BufferLocation = m_pd3dTextureCoord0Buffer->GetGPUVirtualAddress();
		m_d3dTextureCoord0BufferView.StrideInBytes = sizeof(XMFLOAT2);
		m_d3dTextureCoord0BufferView.SizeInBytes = sizeof(XMFLOAT2) * m_nVertices;
	}
}

MeshIlluminatedFromFile::~MeshIlluminatedFromFile()
{
}

void MeshIlluminatedFromFile::ReleaseUploadBuffers()
{
	MeshFromFile::ReleaseUploadBuffers();
	if (m_pd3dNormalUploadBuffer) m_pd3dNormalUploadBuffer = nullptr;
	if (m_pd3dTextureCoord0UploadBuffer) m_pd3dTextureCoord0UploadBuffer = nullptr;
}

void MeshIlluminatedFromFile::Render(ID3D12GraphicsCommandList *pd3dCommandList, int nSubSet)
{
	pd3dCommandList->IASetPrimitiveTopology(m_d3dPrimitiveTopology);
	D3D12_VERTEX_BUFFER_VIEW pVertexBufferViews[3] = { m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dTextureCoord0BufferView };
	pd3dCommandList->IASetVertexBuffers(m_nSlot, 3, pVertexBufferViews);
    	if ((m_nSubMeshes > 0) && (nSubSet < m_nSubMeshes))
    	{
    		pd3dCommandList->IASetIndexBuffer(&(m_pd3dSubSetIndexBufferViews[nSubSet]));
    		pd3dCommandList->DrawIndexedInstanced(m_pnSubSetIndices[nSubSet], 1, 0, 0, 0);
    	}
    	else
    	{
    		pd3dCommandList->DrawInstanced(m_nVertices, 1, m_nOffset, 0);
    	}
    }
    
    /////////////////////////////////////////////////////////////////////////////////////////////////
    //
    SkinnedMesh::SkinnedMesh(ID3D12Device *pd3dDevice, ID3D12GraphicsCommandList *pd3dCommandList, MeshLoadInfo *pMeshInfo) : MeshIlluminatedFromFile(pd3dDevice, pd3dCommandList, pMeshInfo)
    {
        m_vBoneNames = pMeshInfo->m_vBoneNames;
        m_vBindPoses = pMeshInfo->m_vBindPoses;
    
        m_pd3dBoneIndexBuffer = Dx12App::CreateBufferResource(pMeshInfo->m_pxmn4BoneIndices, sizeof(XMINT4) * m_nVertices, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dBoneIndexUploadBuffer);
        m_d3dBoneIndexBufferView.BufferLocation = m_pd3dBoneIndexBuffer->GetGPUVirtualAddress();
        m_d3dBoneIndexBufferView.StrideInBytes = sizeof(XMINT4);
        m_d3dBoneIndexBufferView.SizeInBytes = sizeof(XMINT4) * m_nVertices;
    
        m_pd3dBoneWeightBuffer = Dx12App::CreateBufferResource(pMeshInfo->m_pxmf4BoneWeights, sizeof(XMFLOAT4) * m_nVertices, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dBoneWeightUploadBuffer);
        m_d3dBoneWeightBufferView.BufferLocation = m_pd3dBoneWeightBuffer->GetGPUVirtualAddress();
        m_d3dBoneWeightBufferView.StrideInBytes = sizeof(XMFLOAT4);
        m_d3dBoneWeightBufferView.SizeInBytes = sizeof(XMFLOAT4) * m_nVertices;
    }
    
    SkinnedMesh::~SkinnedMesh()
    {
    }
    
    void SkinnedMesh::ReleaseUploadBuffers()
    {
        MeshIlluminatedFromFile::ReleaseUploadBuffers();
        if (m_pd3dBoneIndexUploadBuffer) m_pd3dBoneIndexUploadBuffer = nullptr;
        if (m_pd3dBoneWeightUploadBuffer) m_pd3dBoneWeightUploadBuffer = nullptr;
    }
    
    void SkinnedMesh::Render(ID3D12GraphicsCommandList *pd3dCommandList, int nSubSet)
    {
        pd3dCommandList->IASetPrimitiveTopology(m_d3dPrimitiveTopology);

        bool bHasTexCoord = (m_nType & VERTEXT_TEXTURE_COORD0);

        if (bHasTexCoord)
        {
            D3D12_VERTEX_BUFFER_VIEW pVertexBufferViews[5] = { m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dTextureCoord0BufferView, m_d3dBoneIndexBufferView, m_d3dBoneWeightBufferView };
            pd3dCommandList->IASetVertexBuffers(m_nSlot, 5, pVertexBufferViews);
        }
        else
        {
            D3D12_VERTEX_BUFFER_VIEW pVertexBufferViews[4] = { m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dBoneIndexBufferView, m_d3dBoneWeightBufferView };
            pd3dCommandList->IASetVertexBuffers(m_nSlot, 4, pVertexBufferViews);
        }

        if ((m_nSubMeshes > 0) && (nSubSet < m_nSubMeshes))
        {
            pd3dCommandList->IASetIndexBuffer(&(m_pd3dSubSetIndexBufferViews[nSubSet]));
            pd3dCommandList->DrawIndexedInstanced(m_pnSubSetIndices[nSubSet], 1, 0, 0, 0);
        }
        else
        {
            pd3dCommandList->DrawInstanced(m_nVertices, 1, m_nOffset, 0);
        }
    }

/////////////////////////////////////////////////////////////////////////////////////////////////
// ObjMesh - loaded from file

void ObjMesh::Build(ID3D12Device* pd3dDevice,
                    ID3D12GraphicsCommandList* pd3dCommandList,
                    const std::vector<XMFLOAT3>& positions,
                    const std::vector<XMFLOAT3>& normals,
                    const std::vector<XMFLOAT2>& uvs,
                    const std::vector<UINT>&     indices)
{
    m_nVertices = (UINT)positions.size();
    m_nIndices  = (UINT)indices.size();
    m_nType     = VERTEXT_POSITION | VERTEXT_NORMAL | VERTEXT_TEXTURE_COORD0;

    m_pd3dPositionBuffer = Dx12App::CreateBufferResource(
        (void*)positions.data(), sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        &m_pd3dPositionUploadBuffer);
    m_d3dPositionBufferView.BufferLocation = m_pd3dPositionBuffer->GetGPUVirtualAddress();
    m_d3dPositionBufferView.StrideInBytes  = sizeof(XMFLOAT3);
    m_d3dPositionBufferView.SizeInBytes    = sizeof(XMFLOAT3) * m_nVertices;

    const std::vector<XMFLOAT3>& nrmSrc = normals.empty()
        ? std::vector<XMFLOAT3>(m_nVertices, XMFLOAT3(0,1,0)) : normals;
    m_pd3dNormalBuffer = Dx12App::CreateBufferResource(
        (void*)nrmSrc.data(), sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        &m_pd3dNormalUploadBuffer);
    m_d3dNormalBufferView.BufferLocation = m_pd3dNormalBuffer->GetGPUVirtualAddress();
    m_d3dNormalBufferView.StrideInBytes  = sizeof(XMFLOAT3);
    m_d3dNormalBufferView.SizeInBytes    = sizeof(XMFLOAT3) * m_nVertices;

    const std::vector<XMFLOAT2>& uvSrc = uvs.empty()
        ? std::vector<XMFLOAT2>(m_nVertices, XMFLOAT2(0,0)) : uvs;
    m_pd3dTexCoordBuffer = Dx12App::CreateBufferResource(
        (void*)uvSrc.data(), sizeof(XMFLOAT2) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        &m_pd3dTexCoordUploadBuffer);
    m_d3dTexCoordBufferView.BufferLocation = m_pd3dTexCoordBuffer->GetGPUVirtualAddress();
    m_d3dTexCoordBufferView.StrideInBytes  = sizeof(XMFLOAT2);
    m_d3dTexCoordBufferView.SizeInBytes    = sizeof(XMFLOAT2) * m_nVertices;

    m_pd3dIndexBuffer = Dx12App::CreateBufferResource(
        (void*)indices.data(), sizeof(UINT) * m_nIndices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER,
        &m_pd3dIndexUploadBuffer);
    m_d3dIndexBufferView.BufferLocation = m_pd3dIndexBuffer->GetGPUVirtualAddress();
    m_d3dIndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
    m_d3dIndexBufferView.SizeInBytes    = sizeof(UINT) * m_nIndices;
}

void ObjMesh::ReleaseUploadBuffers()
{
    if (m_pd3dPositionUploadBuffer) m_pd3dPositionUploadBuffer = nullptr;
    if (m_pd3dNormalUploadBuffer)   m_pd3dNormalUploadBuffer   = nullptr;
    if (m_pd3dTexCoordUploadBuffer) m_pd3dTexCoordUploadBuffer = nullptr;
    if (m_pd3dIndexUploadBuffer)    m_pd3dIndexUploadBuffer    = nullptr;
}

void ObjMesh::Render(ID3D12GraphicsCommandList* pd3dCommandList, int /*nSubSet*/)
{
    pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbvs[3] = {
        m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dTexCoordBufferView };
    pd3dCommandList->IASetVertexBuffers(0, 3, vbvs);
    pd3dCommandList->IASetIndexBuffer(&m_d3dIndexBufferView);
    pd3dCommandList->DrawIndexedInstanced(m_nIndices, 1, 0, 0, 0);
}

/////////////////////////////////////////////////////////////////////////////////////////////////
// CubeMesh - Procedural cube mesh
CubeMesh::CubeMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, float fWidth, float fHeight, float fDepth)
{
    m_nVertices = 24; // 6 faces * 4 vertices
    m_nIndices = 36;  // 6 faces * 2 triangles * 3 vertices
    m_nType = VERTEXT_POSITION | VERTEXT_NORMAL | VERTEXT_TEXTURE_COORD0;

    float hw = fWidth * 0.5f;
    float hh = fHeight * 0.5f;
    float hd = fDepth * 0.5f;

    // Positions (24 vertices for proper normals per face)
    XMFLOAT3 positions[24] = {
        // Front face (Z+)
        { -hw, -hh,  hd }, {  hw, -hh,  hd }, {  hw,  hh,  hd }, { -hw,  hh,  hd },
        // Back face (Z-)
        {  hw, -hh, -hd }, { -hw, -hh, -hd }, { -hw,  hh, -hd }, {  hw,  hh, -hd },
        // Top face (Y+)
        { -hw,  hh,  hd }, {  hw,  hh,  hd }, {  hw,  hh, -hd }, { -hw,  hh, -hd },
        // Bottom face (Y-)
        { -hw, -hh, -hd }, {  hw, -hh, -hd }, {  hw, -hh,  hd }, { -hw, -hh,  hd },
        // Right face (X+)
        {  hw, -hh,  hd }, {  hw, -hh, -hd }, {  hw,  hh, -hd }, {  hw,  hh,  hd },
        // Left face (X-)
        { -hw, -hh, -hd }, { -hw, -hh,  hd }, { -hw,  hh,  hd }, { -hw,  hh, -hd }
    };

    // Normals
    XMFLOAT3 normals[24] = {
        // Front
        { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 },
        // Back
        { 0, 0, -1 }, { 0, 0, -1 }, { 0, 0, -1 }, { 0, 0, -1 },
        // Top
        { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 },
        // Bottom
        { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 },
        // Right
        { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 },
        // Left
        { -1, 0, 0 }, { -1, 0, 0 }, { -1, 0, 0 }, { -1, 0, 0 }
    };

    // Texture coordinates
    XMFLOAT2 texCoords[24] = {
        // Front
        { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 },
        // Back
        { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 },
        // Top
        { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 },
        // Bottom
        { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 },
        // Right
        { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 },
        // Left
        { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 }
    };

    // Indices
    UINT indices[36] = {
        0, 1, 2, 0, 2, 3,       // Front
        4, 5, 6, 4, 6, 7,       // Back
        8, 9, 10, 8, 10, 11,    // Top
        12, 13, 14, 12, 14, 15, // Bottom
        16, 17, 18, 16, 18, 19, // Right
        20, 21, 22, 20, 22, 23  // Left
    };

    // Create position buffer
    m_pd3dPositionBuffer = Dx12App::CreateBufferResource(positions, sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dPositionUploadBuffer);
    m_d3dPositionBufferView.BufferLocation = m_pd3dPositionBuffer->GetGPUVirtualAddress();
    m_d3dPositionBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dPositionBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    // Create normal buffer
    m_pd3dNormalBuffer = Dx12App::CreateBufferResource(normals, sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dNormalUploadBuffer);
    m_d3dNormalBufferView.BufferLocation = m_pd3dNormalBuffer->GetGPUVirtualAddress();
    m_d3dNormalBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dNormalBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    // Create texture coordinate buffer
    m_pd3dTexCoordBuffer = Dx12App::CreateBufferResource(texCoords, sizeof(XMFLOAT2) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dTexCoordUploadBuffer);
    m_d3dTexCoordBufferView.BufferLocation = m_pd3dTexCoordBuffer->GetGPUVirtualAddress();
    m_d3dTexCoordBufferView.StrideInBytes = sizeof(XMFLOAT2);
    m_d3dTexCoordBufferView.SizeInBytes = sizeof(XMFLOAT2) * m_nVertices;

    // Create index buffer
    m_pd3dIndexBuffer = Dx12App::CreateBufferResource(indices, sizeof(UINT) * m_nIndices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER, &m_pd3dIndexUploadBuffer);
    m_d3dIndexBufferView.BufferLocation = m_pd3dIndexBuffer->GetGPUVirtualAddress();
    m_d3dIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    m_d3dIndexBufferView.SizeInBytes = sizeof(UINT) * m_nIndices;
}

CubeMesh::~CubeMesh()
{
}

void CubeMesh::ReleaseUploadBuffers()
{
    if (m_pd3dPositionUploadBuffer) m_pd3dPositionUploadBuffer = nullptr;
    if (m_pd3dNormalUploadBuffer) m_pd3dNormalUploadBuffer = nullptr;
    if (m_pd3dTexCoordUploadBuffer) m_pd3dTexCoordUploadBuffer = nullptr;
    if (m_pd3dIndexUploadBuffer) m_pd3dIndexUploadBuffer = nullptr;
}

void CubeMesh::Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet)
{
    pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW pVertexBufferViews[3] = { m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dTexCoordBufferView };
    pd3dCommandList->IASetVertexBuffers(0, 3, pVertexBufferViews);
    pd3dCommandList->IASetIndexBuffer(&m_d3dIndexBufferView);
    pd3dCommandList->DrawIndexedInstanced(m_nIndices, 1, 0, 0, 0);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// RingMesh - flat ring on XZ plane

RingMesh::RingMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList,
                   float fOuterRadius, float fInnerRadius, int nSegments)
{
    m_nVertices = nSegments * 2;
    m_nIndices = nSegments * 6;
    m_nType = VERTEXT_POSITION | VERTEXT_NORMAL | VERTEXT_TEXTURE_COORD0;

    std::vector<XMFLOAT3> positions(m_nVertices);
    std::vector<XMFLOAT3> normals(m_nVertices);
    std::vector<XMFLOAT2> texCoords(m_nVertices);
    std::vector<UINT> indices(m_nIndices);

    for (int i = 0; i < nSegments; i++)
    {
        float angle = XMConvertToRadians(360.0f * i / nSegments);
        float cosA = cosf(angle);
        float sinA = sinf(angle);

        // Outer vertex
        int outerIdx = i * 2;
        positions[outerIdx] = XMFLOAT3(cosA * fOuterRadius, 0.0f, sinA * fOuterRadius);
        normals[outerIdx] = XMFLOAT3(0.0f, 1.0f, 0.0f);
        texCoords[outerIdx] = XMFLOAT2(static_cast<float>(i) / nSegments, 0.0f);

        // Inner vertex
        int innerIdx = i * 2 + 1;
        positions[innerIdx] = XMFLOAT3(cosA * fInnerRadius, 0.0f, sinA * fInnerRadius);
        normals[innerIdx] = XMFLOAT3(0.0f, 1.0f, 0.0f);
        texCoords[innerIdx] = XMFLOAT2(static_cast<float>(i) / nSegments, 1.0f);
    }

    for (int i = 0; i < nSegments; i++)
    {
        int cur = i * 2;
        int next = ((i + 1) % nSegments) * 2;
        int idx = i * 6;

        // Two triangles per segment
        indices[idx + 0] = cur;
        indices[idx + 1] = next;
        indices[idx + 2] = cur + 1;

        indices[idx + 3] = cur + 1;
        indices[idx + 4] = next;
        indices[idx + 5] = next + 1;
    }

    // Create position buffer
    m_pd3dPositionBuffer = Dx12App::CreateBufferResource(positions.data(), sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dPositionUploadBuffer);
    m_d3dPositionBufferView.BufferLocation = m_pd3dPositionBuffer->GetGPUVirtualAddress();
    m_d3dPositionBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dPositionBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    // Create normal buffer
    m_pd3dNormalBuffer = Dx12App::CreateBufferResource(normals.data(), sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dNormalUploadBuffer);
    m_d3dNormalBufferView.BufferLocation = m_pd3dNormalBuffer->GetGPUVirtualAddress();
    m_d3dNormalBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dNormalBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    // Create texture coordinate buffer
    m_pd3dTexCoordBuffer = Dx12App::CreateBufferResource(texCoords.data(), sizeof(XMFLOAT2) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dTexCoordUploadBuffer);
    m_d3dTexCoordBufferView.BufferLocation = m_pd3dTexCoordBuffer->GetGPUVirtualAddress();
    m_d3dTexCoordBufferView.StrideInBytes = sizeof(XMFLOAT2);
    m_d3dTexCoordBufferView.SizeInBytes = sizeof(XMFLOAT2) * m_nVertices;

    // Create index buffer
    m_pd3dIndexBuffer = Dx12App::CreateBufferResource(indices.data(), sizeof(UINT) * m_nIndices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER, &m_pd3dIndexUploadBuffer);
    m_d3dIndexBufferView.BufferLocation = m_pd3dIndexBuffer->GetGPUVirtualAddress();
    m_d3dIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    m_d3dIndexBufferView.SizeInBytes = sizeof(UINT) * m_nIndices;
}

RingMesh::~RingMesh()
{
}

void RingMesh::ReleaseUploadBuffers()
{
    if (m_pd3dPositionUploadBuffer) m_pd3dPositionUploadBuffer = nullptr;
    if (m_pd3dNormalUploadBuffer) m_pd3dNormalUploadBuffer = nullptr;
    if (m_pd3dTexCoordUploadBuffer) m_pd3dTexCoordUploadBuffer = nullptr;
    if (m_pd3dIndexUploadBuffer) m_pd3dIndexUploadBuffer = nullptr;
}

void RingMesh::Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet)
{
    pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW pVertexBufferViews[3] = { m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dTexCoordBufferView };
    pd3dCommandList->IASetVertexBuffers(0, 3, pVertexBufferViews);
    pd3dCommandList->IASetIndexBuffer(&m_d3dIndexBufferView);
    pd3dCommandList->DrawIndexedInstanced(m_nIndices, 1, 0, 0, 0);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// LineMesh - flat thin rectangle on XZ plane, from (0,0,0) to (0,0,1)

LineMesh::LineMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, float fWidth)
{
    m_nVertices = 4;
    m_nIndices = 6;
    m_nType = VERTEXT_POSITION | VERTEXT_NORMAL | VERTEXT_TEXTURE_COORD0;

    float hw = fWidth * 0.5f;

    XMFLOAT3 positions[4] = {
        { -hw, 0.0f, 0.0f }, {  hw, 0.0f, 0.0f },
        {  hw, 0.0f, 1.0f }, { -hw, 0.0f, 1.0f }
    };

    XMFLOAT3 normals[4] = {
        { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }
    };

    XMFLOAT2 texCoords[4] = {
        { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }
    };

    UINT indices[6] = { 0, 1, 2, 0, 2, 3 };

    m_pd3dPositionBuffer = Dx12App::CreateBufferResource(positions, sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dPositionUploadBuffer);
    m_d3dPositionBufferView.BufferLocation = m_pd3dPositionBuffer->GetGPUVirtualAddress();
    m_d3dPositionBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dPositionBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    m_pd3dNormalBuffer = Dx12App::CreateBufferResource(normals, sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dNormalUploadBuffer);
    m_d3dNormalBufferView.BufferLocation = m_pd3dNormalBuffer->GetGPUVirtualAddress();
    m_d3dNormalBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dNormalBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    m_pd3dTexCoordBuffer = Dx12App::CreateBufferResource(texCoords, sizeof(XMFLOAT2) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dTexCoordUploadBuffer);
    m_d3dTexCoordBufferView.BufferLocation = m_pd3dTexCoordBuffer->GetGPUVirtualAddress();
    m_d3dTexCoordBufferView.StrideInBytes = sizeof(XMFLOAT2);
    m_d3dTexCoordBufferView.SizeInBytes = sizeof(XMFLOAT2) * m_nVertices;

    m_pd3dIndexBuffer = Dx12App::CreateBufferResource(indices, sizeof(UINT) * m_nIndices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER, &m_pd3dIndexUploadBuffer);
    m_d3dIndexBufferView.BufferLocation = m_pd3dIndexBuffer->GetGPUVirtualAddress();
    m_d3dIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    m_d3dIndexBufferView.SizeInBytes = sizeof(UINT) * m_nIndices;
}

LineMesh::~LineMesh()
{
}

void LineMesh::ReleaseUploadBuffers()
{
    if (m_pd3dPositionUploadBuffer) m_pd3dPositionUploadBuffer = nullptr;
    if (m_pd3dNormalUploadBuffer) m_pd3dNormalUploadBuffer = nullptr;
    if (m_pd3dTexCoordUploadBuffer) m_pd3dTexCoordUploadBuffer = nullptr;
    if (m_pd3dIndexUploadBuffer) m_pd3dIndexUploadBuffer = nullptr;
}

void LineMesh::Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet)
{
    pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW pVertexBufferViews[3] = { m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dTexCoordBufferView };
    pd3dCommandList->IASetVertexBuffers(0, 3, pVertexBufferViews);
    pd3dCommandList->IASetIndexBuffer(&m_d3dIndexBufferView);
    pd3dCommandList->DrawIndexedInstanced(m_nIndices, 1, 0, 0, 0);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// FanMesh - sector/pie on XZ plane, unit radius, angle around +Z axis

FanMesh::FanMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList,
                 float fAngleDeg, int nSegments)
{
    // nSegments+1 arc vertices + 1 center vertex
    m_nVertices = nSegments + 2;
    m_nIndices = nSegments * 3;
    m_nType = VERTEXT_POSITION | VERTEXT_NORMAL | VERTEXT_TEXTURE_COORD0;

    std::vector<XMFLOAT3> positions(m_nVertices);
    std::vector<XMFLOAT3> normals(m_nVertices);
    std::vector<XMFLOAT2> texCoords(m_nVertices);
    std::vector<UINT> indices(m_nIndices);

    // Center vertex at origin
    positions[0] = XMFLOAT3(0.0f, 0.0f, 0.0f);
    normals[0] = XMFLOAT3(0.0f, 1.0f, 0.0f);
    texCoords[0] = XMFLOAT2(0.5f, 0.5f);

    float halfAngleRad = XMConvertToRadians(fAngleDeg * 0.5f);

    for (int i = 0; i <= nSegments; i++)
    {
        // Angle from -halfAngle to +halfAngle, centered around +Z axis
        float t = static_cast<float>(i) / nSegments;
        float angle = -halfAngleRad + t * 2.0f * halfAngleRad;

        // +Z is forward, X is right
        float x = sinf(angle);
        float z = cosf(angle);

        positions[i + 1] = XMFLOAT3(x, 0.0f, z);
        normals[i + 1] = XMFLOAT3(0.0f, 1.0f, 0.0f);
        texCoords[i + 1] = XMFLOAT2(0.5f + x * 0.5f, 0.5f - z * 0.5f);
    }

    // Triangles: center → arc[i] → arc[i+1]
    for (int i = 0; i < nSegments; i++)
    {
        indices[i * 3 + 0] = 0;
        indices[i * 3 + 1] = i + 1;
        indices[i * 3 + 2] = i + 2;
    }

    m_pd3dPositionBuffer = Dx12App::CreateBufferResource(positions.data(), sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dPositionUploadBuffer);
    m_d3dPositionBufferView.BufferLocation = m_pd3dPositionBuffer->GetGPUVirtualAddress();
    m_d3dPositionBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dPositionBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    m_pd3dNormalBuffer = Dx12App::CreateBufferResource(normals.data(), sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dNormalUploadBuffer);
    m_d3dNormalBufferView.BufferLocation = m_pd3dNormalBuffer->GetGPUVirtualAddress();
    m_d3dNormalBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dNormalBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    m_pd3dTexCoordBuffer = Dx12App::CreateBufferResource(texCoords.data(), sizeof(XMFLOAT2) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dTexCoordUploadBuffer);
    m_d3dTexCoordBufferView.BufferLocation = m_pd3dTexCoordBuffer->GetGPUVirtualAddress();
    m_d3dTexCoordBufferView.StrideInBytes = sizeof(XMFLOAT2);
    m_d3dTexCoordBufferView.SizeInBytes = sizeof(XMFLOAT2) * m_nVertices;

    m_pd3dIndexBuffer = Dx12App::CreateBufferResource(indices.data(), sizeof(UINT) * m_nIndices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER, &m_pd3dIndexUploadBuffer);
    m_d3dIndexBufferView.BufferLocation = m_pd3dIndexBuffer->GetGPUVirtualAddress();
    m_d3dIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    m_d3dIndexBufferView.SizeInBytes = sizeof(UINT) * m_nIndices;
}

FanMesh::~FanMesh()
{
}

void FanMesh::ReleaseUploadBuffers()
{
    if (m_pd3dPositionUploadBuffer) m_pd3dPositionUploadBuffer = nullptr;
    if (m_pd3dNormalUploadBuffer) m_pd3dNormalUploadBuffer = nullptr;
    if (m_pd3dTexCoordUploadBuffer) m_pd3dTexCoordUploadBuffer = nullptr;
    if (m_pd3dIndexUploadBuffer) m_pd3dIndexUploadBuffer = nullptr;
}

void FanMesh::Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet)
{
    pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW pVertexBufferViews[3] = { m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dTexCoordBufferView };
    pd3dCommandList->IASetVertexBuffers(0, 3, pVertexBufferViews);
    pd3dCommandList->IASetIndexBuffer(&m_d3dIndexBufferView);
    pd3dCommandList->DrawIndexedInstanced(m_nIndices, 1, 0, 0, 0);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GridPlaneMesh - subdivided plane on XZ for water vertex displacement

GridPlaneMesh::GridPlaneMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList,
                             float fWidth, float fDepth, int nGridX, int nGridZ)
{
    int nVertsX = nGridX + 1;
    int nVertsZ = nGridZ + 1;
    m_nVertices = nVertsX * nVertsZ;
    m_nIndices = nGridX * nGridZ * 6;
    m_nType = VERTEXT_POSITION | VERTEXT_NORMAL | VERTEXT_TEXTURE_COORD0;

    std::vector<XMFLOAT3> positions(m_nVertices);
    std::vector<XMFLOAT3> normals(m_nVertices);
    std::vector<XMFLOAT2> texCoords(m_nVertices);
    std::vector<UINT> indices(m_nIndices);

    float hw = fWidth * 0.5f;
    float hd = fDepth * 0.5f;

    // Generate vertices
    for (int z = 0; z < nVertsZ; z++)
    {
        float fZ = -hd + (static_cast<float>(z) / nGridZ) * fDepth;
        float v = static_cast<float>(z) / nGridZ;

        for (int x = 0; x < nVertsX; x++)
        {
            float fX = -hw + (static_cast<float>(x) / nGridX) * fWidth;
            float u = static_cast<float>(x) / nGridX;

            int idx = z * nVertsX + x;
            positions[idx] = XMFLOAT3(fX, 0.0f, fZ);
            normals[idx] = XMFLOAT3(0.0f, 1.0f, 0.0f);
            texCoords[idx] = XMFLOAT2(u, v);
        }
    }

    // Generate indices (two triangles per grid cell)
    int idx = 0;
    for (int z = 0; z < nGridZ; z++)
    {
        for (int x = 0; x < nGridX; x++)
        {
            int topLeft = z * nVertsX + x;
            int topRight = topLeft + 1;
            int bottomLeft = (z + 1) * nVertsX + x;
            int bottomRight = bottomLeft + 1;

            // First triangle
            indices[idx++] = topLeft;
            indices[idx++] = bottomLeft;
            indices[idx++] = topRight;

            // Second triangle
            indices[idx++] = topRight;
            indices[idx++] = bottomLeft;
            indices[idx++] = bottomRight;
        }
    }

    // Create position buffer
    m_pd3dPositionBuffer = Dx12App::CreateBufferResource(positions.data(), sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dPositionUploadBuffer);
    m_d3dPositionBufferView.BufferLocation = m_pd3dPositionBuffer->GetGPUVirtualAddress();
    m_d3dPositionBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dPositionBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    // Create normal buffer
    m_pd3dNormalBuffer = Dx12App::CreateBufferResource(normals.data(), sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dNormalUploadBuffer);
    m_d3dNormalBufferView.BufferLocation = m_pd3dNormalBuffer->GetGPUVirtualAddress();
    m_d3dNormalBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dNormalBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    // Create texture coordinate buffer
    m_pd3dTexCoordBuffer = Dx12App::CreateBufferResource(texCoords.data(), sizeof(XMFLOAT2) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dTexCoordUploadBuffer);
    m_d3dTexCoordBufferView.BufferLocation = m_pd3dTexCoordBuffer->GetGPUVirtualAddress();
    m_d3dTexCoordBufferView.StrideInBytes = sizeof(XMFLOAT2);
    m_d3dTexCoordBufferView.SizeInBytes = sizeof(XMFLOAT2) * m_nVertices;

    // Create index buffer
    m_pd3dIndexBuffer = Dx12App::CreateBufferResource(indices.data(), sizeof(UINT) * m_nIndices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER, &m_pd3dIndexUploadBuffer);
    m_d3dIndexBufferView.BufferLocation = m_pd3dIndexBuffer->GetGPUVirtualAddress();
    m_d3dIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    m_d3dIndexBufferView.SizeInBytes = sizeof(UINT) * m_nIndices;
}

GridPlaneMesh::~GridPlaneMesh()
{
}

void GridPlaneMesh::ReleaseUploadBuffers()
{
    if (m_pd3dPositionUploadBuffer) m_pd3dPositionUploadBuffer = nullptr;
    if (m_pd3dNormalUploadBuffer) m_pd3dNormalUploadBuffer = nullptr;
    if (m_pd3dTexCoordUploadBuffer) m_pd3dTexCoordUploadBuffer = nullptr;
    if (m_pd3dIndexUploadBuffer) m_pd3dIndexUploadBuffer = nullptr;
}

void GridPlaneMesh::Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet)
{
    pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW pVertexBufferViews[3] = { m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dTexCoordBufferView };
    pd3dCommandList->IASetVertexBuffers(0, 3, pVertexBufferViews);
    pd3dCommandList->IASetIndexBuffer(&m_d3dIndexBufferView);
    pd3dCommandList->DrawIndexedInstanced(m_nIndices, 1, 0, 0, 0);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GrassClumpMesh — N개 십자(cross) quad blade를 통합한 단일 mesh
// 각 blade: 8 verts (quad 2개), 12 인덱스(quad 1개당 2 tri × 3 idx = 6 idx). 두 quad = 12 idx, 4 tri/blade
//   기존 패턴(GridPlaneMesh) 그대로: position/normal/texcoord 분리 VB + R32_UINT IB

GrassClumpMesh::GrassClumpMesh(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd,
                               int nBladeCount, float fAreaRadius,
                               float fBladeHeight, float fBladeWidth,
                               unsigned int seed, bool bTaper)
{
    using namespace DirectX;
    if (nBladeCount <= 0) nBladeCount = 1;

    // Cross = 2 quad. 각 quad는 3 행(base/mid/top) × 2 열 = 6 verts, 4 tris.
    // → 한 blade(cross): 12 verts, 8 tris(24 idx). 중간 행 덕분에 sway가 직선 pivot 아닌 곡선 휨.
    const int kVertsPerBlade = 12;
    const int kIndicesPerBlade = 24;

    UINT nTotalVerts = static_cast<UINT>(nBladeCount) * kVertsPerBlade;
    UINT nTotalIndices = static_cast<UINT>(nBladeCount) * kIndicesPerBlade;
    m_nVertices = nTotalVerts;
    m_nIndices = nTotalIndices;
    m_nType = VERTEXT_POSITION | VERTEXT_NORMAL | VERTEXT_TEXTURE_COORD0;

    std::vector<XMFLOAT3> positions(nTotalVerts);
    std::vector<XMFLOAT3> normals(nTotalVerts);
    std::vector<XMFLOAT2> texCoords(nTotalVerts);
    std::vector<UINT> indices(nTotalIndices);

    std::mt19937 rng(seed ? seed : 0xC0FFEEu);
    std::uniform_real_distribution<float> distAngle(0.0f, XM_2PI);
    std::uniform_real_distribution<float> distR(0.0f, 1.0f);          // disc 분포용 (sqrt 적용)
    std::uniform_real_distribution<float> distYaw(0.0f, XM_2PI);
    std::uniform_real_distribution<float> distH(0.6f, 1.4f);          // ±40% 높이 변동 (짧은~긴 갈대 mix)
    std::uniform_real_distribution<float> distW(0.7f, 1.3f);          // ±30% 폭 변동
    std::uniform_real_distribution<float> distLean(-0.30f, 0.30f);    // 기울기 (배율 of h, world-space)

    // Taper: 위쪽 두 정점은 X축 방향 0.4배 좁게 (텍스처 사용 시 bTaper=false → 1.0배 유지)
    const float kTipNarrow = bTaper ? 0.40f : 1.0f;

    UINT vCursor = 0;
    UINT iCursor = 0;

    for (int b = 0; b < nBladeCount; ++b)
    {
        // disc 균일 분포: r = areaRadius * sqrt(u)
        float u = distR(rng);
        float r = fAreaRadius * std::sqrt(u);
        float a = distAngle(rng);
        float ox = std::cos(a) * r;
        float oz = std::sin(a) * r;

        float yaw = distYaw(rng);
        float h = fBladeHeight * distH(rng);
        float halfW = fBladeWidth * 0.5f * distW(rng);
        // 정지 상태 기울기 (world-space) — 갈대마다 다른 방향으로 살짝 휘어 있음
        float leanWX = distLean(rng) * h;
        float leanWZ = distLean(rng) * h;

        // 십자 = 2 quad: yaw, yaw + 90°
        float yaws[2] = { yaw, yaw + XM_PIDIV2 };

        for (int q = 0; q < 2; ++q)
        {
            float cy = std::cos(yaws[q]);
            float sy = std::sin(yaws[q]);

            // local quad 정점 (yaw=0 기준): 3행(base/mid/top) × 2열(left/right)
            // 정점 순서: 0=좌하, 1=우하, 2=좌중, 3=우중, 4=좌상(taper), 5=우상(taper)
            float xL = -halfW;
            float xR = +halfW;
            // mid: base와 tip 사이 (taper 보간)
            float midShrink = 0.5f * (1.0f + kTipNarrow);   // taper면 ~0.7, 아니면 1.0
            float xLM = -halfW * midShrink;
            float xRM = +halfW * midShrink;
            float xLT = -halfW * kTipNarrow;
            float xRT = +halfW * kTipNarrow;

            float lx[6] = { xL,   xR,   xLM,    xRM,    xLT,  xRT  };
            float ly[6] = { 0.0f, 0.0f, h*0.5f, h*0.5f, h,    h    };
            float u_uv[6] = { 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f };
            float v_uv[6] = { 0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f };
            // world-space lean — mid는 절반, top은 전체 적용 (정지 시 곡선 휨 X, sway가 곡선 만듦)
            float topLeanX[6] = { 0.0f, 0.0f, leanWX*0.5f, leanWX*0.5f, leanWX, leanWX };
            float topLeanZ[6] = { 0.0f, 0.0f, leanWZ*0.5f, leanWZ*0.5f, leanWZ, leanWZ };

            // face normal: yaw=0이면 (0,0,1). 회전 후: (-sin(yaw), 0, cos(yaw))
            XMFLOAT3 faceN(-sy, 0.0f, cy);

            UINT base = vCursor;
            for (int k = 0; k < 6; ++k)
            {
                // yaw rotation around Y: (x', z') = (cy*x - sy*z, sy*x + cy*z); local z=0
                float rx = cy * lx[k];
                float rz = sy * lx[k];
                positions[vCursor] = XMFLOAT3(ox + rx + topLeanX[k], ly[k], oz + rz + topLeanZ[k]);
                normals[vCursor]   = faceN;
                texCoords[vCursor] = XMFLOAT2(u_uv[k], v_uv[k]);
                ++vCursor;
            }

            // 4 tri (CCW): 하단 quad 0→2→3, 0→3→1, 상단 quad 2→4→5, 2→5→3
            indices[iCursor++] = base + 0;
            indices[iCursor++] = base + 2;
            indices[iCursor++] = base + 3;
            indices[iCursor++] = base + 0;
            indices[iCursor++] = base + 3;
            indices[iCursor++] = base + 1;
            indices[iCursor++] = base + 2;
            indices[iCursor++] = base + 4;
            indices[iCursor++] = base + 5;
            indices[iCursor++] = base + 2;
            indices[iCursor++] = base + 5;
            indices[iCursor++] = base + 3;
        }
    }

    // VB/IB 업로드 (GridPlaneMesh 패턴 그대로)
    m_pd3dPositionBuffer = Dx12App::CreateBufferResource(positions.data(), sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dPositionUploadBuffer);
    m_d3dPositionBufferView.BufferLocation = m_pd3dPositionBuffer->GetGPUVirtualAddress();
    m_d3dPositionBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dPositionBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    m_pd3dNormalBuffer = Dx12App::CreateBufferResource(normals.data(), sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dNormalUploadBuffer);
    m_d3dNormalBufferView.BufferLocation = m_pd3dNormalBuffer->GetGPUVirtualAddress();
    m_d3dNormalBufferView.StrideInBytes = sizeof(XMFLOAT3);
    m_d3dNormalBufferView.SizeInBytes = sizeof(XMFLOAT3) * m_nVertices;

    m_pd3dTexCoordBuffer = Dx12App::CreateBufferResource(texCoords.data(), sizeof(XMFLOAT2) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dTexCoordUploadBuffer);
    m_d3dTexCoordBufferView.BufferLocation = m_pd3dTexCoordBuffer->GetGPUVirtualAddress();
    m_d3dTexCoordBufferView.StrideInBytes = sizeof(XMFLOAT2);
    m_d3dTexCoordBufferView.SizeInBytes = sizeof(XMFLOAT2) * m_nVertices;

    m_pd3dIndexBuffer = Dx12App::CreateBufferResource(indices.data(), sizeof(UINT) * m_nIndices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER, &m_pd3dIndexUploadBuffer);
    m_d3dIndexBufferView.BufferLocation = m_pd3dIndexBuffer->GetGPUVirtualAddress();
    m_d3dIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    m_d3dIndexBufferView.SizeInBytes = sizeof(UINT) * m_nIndices;
}

GrassClumpMesh::~GrassClumpMesh()
{
}

void GrassClumpMesh::ReleaseUploadBuffers()
{
    if (m_pd3dPositionUploadBuffer) m_pd3dPositionUploadBuffer = nullptr;
    if (m_pd3dNormalUploadBuffer) m_pd3dNormalUploadBuffer = nullptr;
    if (m_pd3dTexCoordUploadBuffer) m_pd3dTexCoordUploadBuffer = nullptr;
    if (m_pd3dIndexUploadBuffer) m_pd3dIndexUploadBuffer = nullptr;
}

void GrassClumpMesh::Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet)
{
    pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW pVertexBufferViews[3] = { m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dTexCoordBufferView };
    pd3dCommandList->IASetVertexBuffers(0, 3, pVertexBufferViews);
    pd3dCommandList->IASetIndexBuffer(&m_d3dIndexBufferView);
    pd3dCommandList->DrawIndexedInstanced(m_nIndices, 1, 0, 0, 0);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// QuadMesh — 1x1 flat quad on XZ plane for ground decals

QuadMesh::QuadMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList)
{
    m_nVertices = 4;
    m_nIndices  = 6;
    m_nType = VERTEXT_POSITION | VERTEXT_NORMAL | VERTEXT_TEXTURE_COORD0;

    XMFLOAT3 positions[4] = {
        { -0.5f, 0.f, -0.5f }, {  0.5f, 0.f, -0.5f },
        {  0.5f, 0.f,  0.5f }, { -0.5f, 0.f,  0.5f }
    };
    XMFLOAT3 normals[4] = {
        { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }
    };
    XMFLOAT2 texCoords[4] = {
        { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 }
    };
    UINT indices[6] = { 0, 1, 2, 0, 2, 3 };

    m_pd3dPositionBuffer = Dx12App::CreateBufferResource(positions, sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dPositionUploadBuffer);
    m_d3dPositionBufferView.BufferLocation = m_pd3dPositionBuffer->GetGPUVirtualAddress();
    m_d3dPositionBufferView.StrideInBytes  = sizeof(XMFLOAT3);
    m_d3dPositionBufferView.SizeInBytes    = sizeof(XMFLOAT3) * m_nVertices;

    m_pd3dNormalBuffer = Dx12App::CreateBufferResource(normals, sizeof(XMFLOAT3) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dNormalUploadBuffer);
    m_d3dNormalBufferView.BufferLocation = m_pd3dNormalBuffer->GetGPUVirtualAddress();
    m_d3dNormalBufferView.StrideInBytes  = sizeof(XMFLOAT3);
    m_d3dNormalBufferView.SizeInBytes    = sizeof(XMFLOAT3) * m_nVertices;

    m_pd3dTexCoordBuffer = Dx12App::CreateBufferResource(texCoords, sizeof(XMFLOAT2) * m_nVertices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &m_pd3dTexCoordUploadBuffer);
    m_d3dTexCoordBufferView.BufferLocation = m_pd3dTexCoordBuffer->GetGPUVirtualAddress();
    m_d3dTexCoordBufferView.StrideInBytes  = sizeof(XMFLOAT2);
    m_d3dTexCoordBufferView.SizeInBytes    = sizeof(XMFLOAT2) * m_nVertices;

    m_pd3dIndexBuffer = Dx12App::CreateBufferResource(indices, sizeof(UINT) * m_nIndices,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_INDEX_BUFFER, &m_pd3dIndexUploadBuffer);
    m_d3dIndexBufferView.BufferLocation = m_pd3dIndexBuffer->GetGPUVirtualAddress();
    m_d3dIndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
    m_d3dIndexBufferView.SizeInBytes    = sizeof(UINT) * m_nIndices;
}

QuadMesh::~QuadMesh()
{
}

void QuadMesh::ReleaseUploadBuffers()
{
    if (m_pd3dPositionUploadBuffer)  m_pd3dPositionUploadBuffer  = nullptr;
    if (m_pd3dNormalUploadBuffer)    m_pd3dNormalUploadBuffer    = nullptr;
    if (m_pd3dTexCoordUploadBuffer)  m_pd3dTexCoordUploadBuffer  = nullptr;
    if (m_pd3dIndexUploadBuffer)     m_pd3dIndexUploadBuffer     = nullptr;
}

void QuadMesh::Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet)
{
    pd3dCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW pVertexBufferViews[3] = {
        m_d3dPositionBufferView, m_d3dNormalBufferView, m_d3dTexCoordBufferView
    };
    pd3dCommandList->IASetVertexBuffers(0, 3, pVertexBufferViews);
    pd3dCommandList->IASetIndexBuffer(&m_d3dIndexBufferView);
    pd3dCommandList->DrawIndexedInstanced(m_nIndices, 1, 0, 0, 0);
}
