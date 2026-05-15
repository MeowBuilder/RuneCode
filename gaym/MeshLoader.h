
#pragma once



#include "GameObject.h"

#include "Mesh.h"

#include <string>
#include <unordered_map>
#include <vector>



class Scene; // Forward declaration



class MeshLoadInfo

{

public:

    MeshLoadInfo() { }

    ~MeshLoadInfo();



public:

    char                            m_pstrMeshName[256] = { 0 };



    UINT                            m_nType = 0x00;



    XMFLOAT3                        m_xmf3AABBCenter = XMFLOAT3(0.0f, 0.0f, 0.0f);

    XMFLOAT3                        m_xmf3AABBExtents = XMFLOAT3(0.0f, 0.0f, 0.0f);



    int                             m_nVertices = 0;

    XMFLOAT3* m_pxmf3Positions = NULL;

    XMFLOAT4* m_pxmf4Colors = NULL;

    XMFLOAT3* m_pxmf3Normals = NULL;

    XMFLOAT2* m_pxmf2TextureCoords0 = NULL;

    // Skinning Data
    XMINT4* m_pxmn4BoneIndices = NULL;
    XMFLOAT4* m_pxmf4BoneWeights = NULL;
    std::vector<std::string> m_vBoneNames;
    std::vector<XMFLOAT4X4> m_vBindPoses;

    int                             m_nIndices = 0;

    UINT* m_pnIndices = NULL;



    int                             m_nSubMeshes = 0;

    int* m_pnSubSetIndices = NULL;

    UINT** m_ppnSubSetIndices = NULL;

};



class MeshLoader

{

public:

    static GameObject* LoadGeometryFromFile(Scene* pScene, ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, const char* pstrFileName);



private:



    static GameObject* LoadFrameHierarchyFromFile(Scene* pScene, ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, FILE* pInFile, const std::string& strMeshDir);



    static MeshLoadInfo* LoadMeshInfoFromFile(FILE* pInFile);



    static void LoadMaterialsInfoFromFile(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, FILE* pInFile, GameObject* pGameObject, Scene* pScene, const std::string& strMeshDir);



    // ── 메쉬 GPU 리소스 공유 캐시 ─────────────────────────────────────────────
    //   같은 파일을 N번 로드해도 SkinnedMesh/MeshFromFile 등은 1번만 생성.
    //   <Mesh>: 디렉티브 순서대로 캐싱. t_meshCounter 가 frame 카운터.
    //   AddRef 로 재사용 → GameObject hierarchy 자체는 매번 새로 생성됨 (per-instance state OK).
    static std::unordered_map<std::string, std::vector<Mesh*>> s_meshCache;
    thread_local static std::string                            t_currentLoadPath;
    thread_local static int                                    t_meshCounter;
    thread_local static bool                                   t_isCacheHit;

};


