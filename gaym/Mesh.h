//------------------------------------------------------- ----------------------
// File: Mesh.h
//-----------------------------------------------------------------------------

#pragma once

#include <vector>
#include <string>

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
class Mesh
{
public:
	Mesh() { }
    virtual ~Mesh();

private:
	int								m_nReferences = 0;

public:
	// 로컬 공간 AABB — MeshLoader 가 .bin <Bounds>: 섹션에서 채워줌 (Unity-export 메쉬용).
	// ColliderComponent 가 월드 변환 시 사용. OBJ 메쉬는 0 (MapLoader 에서 별도 계산).
	XMFLOAT3 m_xmf3AABBCenter  = { 0.0f, 0.0f, 0.0f };
	XMFLOAT3 m_xmf3AABBExtents = { 0.0f, 0.0f, 0.0f };

	void AddRef() { m_nReferences++; }
	void Release() { if (--m_nReferences <= 0) delete this; }

	virtual void ReleaseUploadBuffers() { }

protected:
	D3D12_PRIMITIVE_TOPOLOGY		m_d3dPrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	UINT							m_nSlot = 0;
	UINT							m_nVertices = 0;
	UINT							m_nOffset = 0;

	UINT							m_nType = 0;

public:
	UINT GetType() { return(m_nType); }
	virtual void Render(ID3D12GraphicsCommandList *pd3dCommandList) { }
	virtual void Render(ID3D12GraphicsCommandList *pd3dCommandList, int nSubSet) { }
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
#define VERTEXT_POSITION			0x01
#define VERTEXT_COLOR				0x02
#define VERTEXT_NORMAL				0x04
#define VERTEXT_TEXTURE_COORD0		0x08
#define VERTEXT_BONE_INDEX_WEIGHT   0x10

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
class MeshFromFile : public Mesh
{
public:
	MeshFromFile(ID3D12Device *pd3dDevice, ID3D12GraphicsCommandList *pd3dCommandList, class MeshLoadInfo *pMeshInfo);
	virtual ~MeshFromFile();

public:
	virtual void ReleaseUploadBuffers();

protected:
	ComPtr<ID3D12Resource>				m_pd3dPositionBuffer;
	ComPtr<ID3D12Resource>				m_pd3dPositionUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW		m_d3dPositionBufferView;

	int								m_nSubMeshes = 0;
	int								*m_pnSubSetIndices = NULL;

	ComPtr<ID3D12Resource>					*m_ppd3dSubSetIndexBuffers = NULL;
	ComPtr<ID3D12Resource>					*m_ppd3dSubSetIndexUploadBuffers = NULL;
	D3D12_INDEX_BUFFER_VIEW			*m_pd3dSubSetIndexBufferViews = NULL;

public:
	virtual void Render(ID3D12GraphicsCommandList *pd3dCommandList, int nSubSet);
	int GetSubMeshCount() const { return m_nSubMeshes; }
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
class MeshIlluminatedFromFile : public MeshFromFile
{
public:
	MeshIlluminatedFromFile(ID3D12Device *pd3dDevice, ID3D12GraphicsCommandList *pd3dCommandList, class MeshLoadInfo *pMeshInfo);
	virtual ~MeshIlluminatedFromFile();

	virtual void ReleaseUploadBuffers();

protected:
	ComPtr<ID3D12Resource>				m_pd3dNormalBuffer;
	ComPtr<ID3D12Resource>				m_pd3dNormalUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW		m_d3dNormalBufferView;

	ComPtr<ID3D12Resource>				m_pd3dTextureCoord0Buffer;
	ComPtr<ID3D12Resource>				m_pd3dTextureCoord0UploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW		m_d3dTextureCoord0BufferView;

public:
	virtual void Render(ID3D12GraphicsCommandList *pd3dCommandList, int nSubSet);
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
class SkinnedMesh : public MeshIlluminatedFromFile
{
public:
	SkinnedMesh(ID3D12Device *pd3dDevice, ID3D12GraphicsCommandList *pd3dCommandList, class MeshLoadInfo *pMeshInfo);
	virtual ~SkinnedMesh();

	virtual void ReleaseUploadBuffers();
	virtual void Render(ID3D12GraphicsCommandList *pd3dCommandList, int nSubSet);

protected:
	ComPtr<ID3D12Resource>				m_pd3dBoneIndexBuffer;
	ComPtr<ID3D12Resource>				m_pd3dBoneIndexUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW			m_d3dBoneIndexBufferView;

	ComPtr<ID3D12Resource>				m_pd3dBoneWeightBuffer;
	ComPtr<ID3D12Resource>				m_pd3dBoneWeightUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW			m_d3dBoneWeightBufferView;

public:
	std::vector<std::string>		m_vBoneNames;
	std::vector<XMFLOAT4X4>			m_vBindPoses; // Inverse Bind Pose Matrices
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Simple procedural cube mesh
class CubeMesh : public Mesh
{
public:
	CubeMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, float fWidth = 1.0f, float fHeight = 1.0f, float fDepth = 1.0f);
	virtual ~CubeMesh();

	virtual void ReleaseUploadBuffers() override;
	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet = 0) override;

protected:
	ComPtr<ID3D12Resource> m_pd3dPositionBuffer;
	ComPtr<ID3D12Resource> m_pd3dPositionUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dPositionBufferView;

	ComPtr<ID3D12Resource> m_pd3dNormalBuffer;
	ComPtr<ID3D12Resource> m_pd3dNormalUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dNormalBufferView;

	ComPtr<ID3D12Resource> m_pd3dTexCoordBuffer;
	ComPtr<ID3D12Resource> m_pd3dTexCoordUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dTexCoordBufferView;

	ComPtr<ID3D12Resource> m_pd3dIndexBuffer;
	ComPtr<ID3D12Resource> m_pd3dIndexUploadBuffer;
	D3D12_INDEX_BUFFER_VIEW m_d3dIndexBufferView;

	UINT m_nIndices = 0;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// OBJ mesh loaded from file – same buffer layout as CubeMesh/RingMesh
class ObjMesh : public Mesh
{
public:
    ObjMesh() {}
    virtual ~ObjMesh() {}

    // Called by MapLoader after parsing the OBJ file
    void Build(ID3D12Device* pd3dDevice,
               ID3D12GraphicsCommandList* pd3dCommandList,
               const std::vector<XMFLOAT3>& positions,
               const std::vector<XMFLOAT3>& normals,
               const std::vector<XMFLOAT2>& uvs,
               const std::vector<UINT>&     indices);

    virtual void ReleaseUploadBuffers() override;
    virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet = 0) override;

protected:
    ComPtr<ID3D12Resource> m_pd3dPositionBuffer;
    ComPtr<ID3D12Resource> m_pd3dPositionUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_d3dPositionBufferView{};

    ComPtr<ID3D12Resource> m_pd3dNormalBuffer;
    ComPtr<ID3D12Resource> m_pd3dNormalUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_d3dNormalBufferView{};

    ComPtr<ID3D12Resource> m_pd3dTexCoordBuffer;
    ComPtr<ID3D12Resource> m_pd3dTexCoordUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_d3dTexCoordBufferView{};

    ComPtr<ID3D12Resource> m_pd3dIndexBuffer;
    ComPtr<ID3D12Resource> m_pd3dIndexUploadBuffer;
    D3D12_INDEX_BUFFER_VIEW m_d3dIndexBufferView{};

    UINT m_nIndices = 0;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Flat line (thin rectangle) on XZ plane for rush path visualization
// Unit length along +Z (0 to 1), width along X. Scale Z to match rush distance.
class LineMesh : public Mesh
{
public:
	LineMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList,
	         float fWidth = 0.5f);
	virtual ~LineMesh();

	virtual void ReleaseUploadBuffers() override;
	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet = 0) override;

protected:
	ComPtr<ID3D12Resource> m_pd3dPositionBuffer;
	ComPtr<ID3D12Resource> m_pd3dPositionUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dPositionBufferView;

	ComPtr<ID3D12Resource> m_pd3dNormalBuffer;
	ComPtr<ID3D12Resource> m_pd3dNormalUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dNormalBufferView;

	ComPtr<ID3D12Resource> m_pd3dTexCoordBuffer;
	ComPtr<ID3D12Resource> m_pd3dTexCoordUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dTexCoordBufferView;

	ComPtr<ID3D12Resource> m_pd3dIndexBuffer;
	ComPtr<ID3D12Resource> m_pd3dIndexUploadBuffer;
	D3D12_INDEX_BUFFER_VIEW m_d3dIndexBufferView;

	UINT m_nIndices = 0;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Fan/sector mesh on XZ plane for cone attack visualization
// Unit radius, centered at origin, spanning configurable angle around +Z axis
class FanMesh : public Mesh
{
public:
	FanMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList,
	        float fAngleDeg = 90.0f, int nSegments = 24);
	virtual ~FanMesh();

	virtual void ReleaseUploadBuffers() override;
	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet = 0) override;

protected:
	ComPtr<ID3D12Resource> m_pd3dPositionBuffer;
	ComPtr<ID3D12Resource> m_pd3dPositionUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dPositionBufferView;

	ComPtr<ID3D12Resource> m_pd3dNormalBuffer;
	ComPtr<ID3D12Resource> m_pd3dNormalUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dNormalBufferView;

	ComPtr<ID3D12Resource> m_pd3dTexCoordBuffer;
	ComPtr<ID3D12Resource> m_pd3dTexCoordUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dTexCoordBufferView;

	ComPtr<ID3D12Resource> m_pd3dIndexBuffer;
	ComPtr<ID3D12Resource> m_pd3dIndexUploadBuffer;
	D3D12_INDEX_BUFFER_VIEW m_d3dIndexBufferView;

	UINT m_nIndices = 0;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Flat ring mesh on XZ plane (for range indicators)
class RingMesh : public Mesh
{
public:
	RingMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList,
	         float fOuterRadius = 1.0f, float fInnerRadius = 0.92f, int nSegments = 48);
	virtual ~RingMesh();

	virtual void ReleaseUploadBuffers() override;
	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet = 0) override;

protected:
	ComPtr<ID3D12Resource> m_pd3dPositionBuffer;
	ComPtr<ID3D12Resource> m_pd3dPositionUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dPositionBufferView;

	ComPtr<ID3D12Resource> m_pd3dNormalBuffer;
	ComPtr<ID3D12Resource> m_pd3dNormalUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dNormalBufferView;

	ComPtr<ID3D12Resource> m_pd3dTexCoordBuffer;
	ComPtr<ID3D12Resource> m_pd3dTexCoordUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dTexCoordBufferView;

	ComPtr<ID3D12Resource> m_pd3dIndexBuffer;
	ComPtr<ID3D12Resource> m_pd3dIndexUploadBuffer;
	D3D12_INDEX_BUFFER_VIEW m_d3dIndexBufferView;

	UINT m_nIndices = 0;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Procedural grass clump — N개의 십자 quad blade를 하나의 mesh로 통합
// 한 군집 = 1 GameObject = 1 mesh = 수백 blade. 텍스처 없음(셰이더에서 vertex 그라데이션).
class GrassClumpMesh : public Mesh
{
public:
    GrassClumpMesh(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCmd,
                   int nBladeCount, float fAreaRadius,
                   float fBladeHeight, float fBladeWidth,
                   unsigned int seed, bool bTaper = false);
    virtual ~GrassClumpMesh();

    virtual void ReleaseUploadBuffers() override;
    virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet = 0) override;

protected:
    ComPtr<ID3D12Resource> m_pd3dPositionBuffer;
    ComPtr<ID3D12Resource> m_pd3dPositionUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_d3dPositionBufferView{};

    ComPtr<ID3D12Resource> m_pd3dNormalBuffer;
    ComPtr<ID3D12Resource> m_pd3dNormalUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_d3dNormalBufferView{};

    ComPtr<ID3D12Resource> m_pd3dTexCoordBuffer;
    ComPtr<ID3D12Resource> m_pd3dTexCoordUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_d3dTexCoordBufferView{};

    ComPtr<ID3D12Resource> m_pd3dIndexBuffer;
    ComPtr<ID3D12Resource> m_pd3dIndexUploadBuffer;
    D3D12_INDEX_BUFFER_VIEW m_d3dIndexBufferView{};

    UINT m_nIndices = 0;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Subdivided grid plane mesh on XZ plane for water vertex displacement
class GridPlaneMesh : public Mesh
{
public:
	GridPlaneMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList,
	              float fWidth = 1.0f, float fDepth = 1.0f, int nGridX = 64, int nGridZ = 64);
	virtual ~GridPlaneMesh();

	virtual void ReleaseUploadBuffers() override;
	virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet = 0) override;

protected:
	ComPtr<ID3D12Resource> m_pd3dPositionBuffer;
	ComPtr<ID3D12Resource> m_pd3dPositionUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dPositionBufferView;

	ComPtr<ID3D12Resource> m_pd3dNormalBuffer;
	ComPtr<ID3D12Resource> m_pd3dNormalUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dNormalBufferView;

	ComPtr<ID3D12Resource> m_pd3dTexCoordBuffer;
	ComPtr<ID3D12Resource> m_pd3dTexCoordUploadBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_d3dTexCoordBufferView;

	ComPtr<ID3D12Resource> m_pd3dIndexBuffer;
	ComPtr<ID3D12Resource> m_pd3dIndexUploadBuffer;
	D3D12_INDEX_BUFFER_VIEW m_d3dIndexBufferView;

	UINT m_nIndices = 0;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Flat 1x1 quad on XZ plane for ground decals (UV 0,0 top-left to 1,1 bottom-right)
class QuadMesh : public Mesh
{
public:
    QuadMesh(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList);
    virtual ~QuadMesh();

    virtual void ReleaseUploadBuffers() override;
    virtual void Render(ID3D12GraphicsCommandList* pd3dCommandList, int nSubSet = 0) override;

protected:
    ComPtr<ID3D12Resource> m_pd3dPositionBuffer;
    ComPtr<ID3D12Resource> m_pd3dPositionUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_d3dPositionBufferView{};

    ComPtr<ID3D12Resource> m_pd3dNormalBuffer;
    ComPtr<ID3D12Resource> m_pd3dNormalUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_d3dNormalBufferView{};

    ComPtr<ID3D12Resource> m_pd3dTexCoordBuffer;
    ComPtr<ID3D12Resource> m_pd3dTexCoordUploadBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_d3dTexCoordBufferView{};

    ComPtr<ID3D12Resource> m_pd3dIndexBuffer;
    ComPtr<ID3D12Resource> m_pd3dIndexUploadBuffer;
    D3D12_INDEX_BUFFER_VIEW m_d3dIndexBufferView{};

    UINT m_nIndices = 0;
};