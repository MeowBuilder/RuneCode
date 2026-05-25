#include "stdafx.h"
#include "MeshLoader.h"
#include "GameObject.h"
#include "Mesh.h"
#include "Scene.h" // Include Scene.h

MeshLoadInfo::~MeshLoadInfo()
{
	if (m_pxmf3Positions) delete[] m_pxmf3Positions;
	if (m_pxmf4Colors) delete[] m_pxmf4Colors;
	if (m_pxmf3Normals) delete[] m_pxmf3Normals;
	if (m_pxmf2TextureCoords0) delete[] m_pxmf2TextureCoords0;
    if (m_pxmn4BoneIndices) delete[] m_pxmn4BoneIndices;
    if (m_pxmf4BoneWeights) delete[] m_pxmf4BoneWeights;

	if (m_pnIndices) delete[] m_pnIndices;
	
	if (m_pnSubSetIndices) delete[] m_pnSubSetIndices;

	for (int i = 0; i < m_nSubMeshes; i++) if (m_ppnSubSetIndices[i]) delete[] m_ppnSubSetIndices[i];
	if (m_ppnSubSetIndices) delete[] m_ppnSubSetIndices;
}

int ReadIntegerFromFile(FILE* pInFile)
{
	int nValue = 0;
	UINT nReads = (UINT)::fread(&nValue, sizeof(int), 1, pInFile);

	wchar_t buffer[128];
	swprintf_s(buffer, 128, L"  Read Integer: %d\n", nValue);
	OutputDebugString(buffer);

	return(nValue);
}

float ReadFloatFromFile(FILE* pInFile)
{
	float fValue = 0;
	UINT nReads = (UINT)::fread(&fValue, sizeof(float), 1, pInFile);
	return(fValue);
}

BYTE ReadStringFromFile(FILE* pInFile, char* pstrToken, int nBufferSize)
{
	if (!pInFile) return 0;

	// Read 7-bit encoded integer for string length (compatible with C# BinaryWriter)
	int nStrLength = 0;
	int shift = 0;
	BYTE byteRead = 0;
	do {
		if (fread(&byteRead, sizeof(BYTE), 1, pInFile) != 1) return 0;
		nStrLength |= (byteRead & 0x7F) << shift;
		shift += 7;
	} while (byteRead & 0x80);

	if (nStrLength >= nBufferSize)
	{
		// String is too long for buffer, skip it
		fseek(pInFile, nStrLength, SEEK_CUR);
		return 0; // Or return nStrLength but indicate truncation? For now return 0 to indicate error/skip.
	}

	UINT nReads = (UINT)::fread(pstrToken, sizeof(char), nStrLength, pInFile);
	pstrToken[nStrLength] = '\0';

	return (BYTE)nStrLength;
}

void MeshLoader::LoadMaterialsInfoFromFile(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, FILE* pInFile, GameObject* pGameObject, Scene* pScene, const std::string& strMeshDir)
{
	char pstrToken[64] = { '\0' };
	int nMaterials = ReadIntegerFromFile(pInFile);

	MATERIAL xmf4Material;
	ZeroMemory(&xmf4Material, sizeof(MATERIAL));

	for (; ; )
	{
		if (ReadStringFromFile(pInFile, pstrToken, 64) == 0) break;
		if (!strcmp(pstrToken, "</Materials>")) break;

		if (!strcmp(pstrToken, "<Material>:"))
		{
			int nMaterial = ReadIntegerFromFile(pInFile);
		}
		else if (!strcmp(pstrToken, "<AlbedoColor>:"))
		{
			fread(&xmf4Material.m_cDiffuse, sizeof(float), 4, pInFile);
			if (pGameObject) pGameObject->SetMaterial(xmf4Material);
		}
		else if (!strcmp(pstrToken, "<AlbedoMap>:"))
		{
			char pstrTextureName[64] = { 0 };
			ReadStringFromFile(pInFile, pstrTextureName, 64);
			if (pGameObject && pScene)
			{
				// Build full path: {meshDir}/Textures/{filename}
				std::string fullTexPath = strMeshDir + "Textures/" + pstrTextureName;
				pGameObject->SetTextureName(fullTexPath.c_str());

				D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
				D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
				pScene->AllocateDescriptor(&cpuHandle, &gpuHandle);

				pGameObject->LoadTexture(pd3dDevice, pd3dCommandList, cpuHandle);
				pGameObject->SetSrvGpuDescriptorHandle(gpuHandle);
			}
		}
		else if (!strcmp(pstrToken, "<EmissiveColor>:"))
		{
			XMFLOAT4 emissive;
			fread(&emissive, sizeof(float), 4, pInFile);
		}
		else if (!strcmp(pstrToken, "<SpecularColor>:"))
		{
			XMFLOAT4 specular;
			fread(&specular, sizeof(float), 4, pInFile);
		}
		else if (!strcmp(pstrToken, "<Glossiness>:") ||
				 !strcmp(pstrToken, "<Metallic>:") ||
				 !strcmp(pstrToken, "<SpecularHighlight>:") ||
				 !strcmp(pstrToken, "<GlossyReflection>:") ||
				 !strcmp(pstrToken, "<Smoothness>:"))
		{
			float fValue;
			fread(&fValue, sizeof(float), 1, pInFile);
		}
	}
}



// 같은 파일에서 같은 순서로 등장하는 Mesh* 를 공유 — GPU 버텍스/인덱스 버퍼 한 번만 생성.
// 키: filepath, 값: <Mesh> 디렉티브 순서대로의 Mesh* 리스트.
// LoadFrameHierarchyFromFile 에서 frame 순회 시 정수 counter 와 함께 사용.
std::unordered_map<std::string, std::vector<Mesh*>> MeshLoader::s_meshCache;
thread_local std::string                            MeshLoader::t_currentLoadPath;
thread_local int                                    MeshLoader::t_meshCounter = 0;
thread_local bool                                   MeshLoader::t_isCacheHit  = false;

GameObject* MeshLoader::LoadGeometryFromFile(Scene* pScene, ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, const char* pstrFileName)
{
	FILE* pInFile = NULL;
	if (::fopen_s(&pInFile, pstrFileName, "rb") != 0)
	{
		return nullptr;
	}
	::rewind(pInFile);

	// Extract directory from mesh path (e.g. "Assets/Enemies/Elementals/StormElemental_Bl/")
	std::string strMeshDir = pstrFileName;
	size_t lastSlash = strMeshDir.find_last_of("/\\");
	strMeshDir = (lastSlash != std::string::npos) ? strMeshDir.substr(0, lastSlash + 1) : "";

	// 메쉬 공유 캐시 초기화 (이 로드 세션 전용 스레드로컬 상태)
	t_currentLoadPath = pstrFileName ? pstrFileName : "";
	t_meshCounter     = 0;
	t_isCacheHit      = (s_meshCache.find(t_currentLoadPath) != s_meshCache.end());

	GameObject* pGameObject = NULL;
	char pstrToken[64] = { '\0' };

	for ( ; ; )
	{
		if (::ReadStringFromFile(pInFile, pstrToken, 64) == 0) break;

		if (!strcmp(pstrToken, "<Hierarchy>:"))
		{
			pGameObject = MeshLoader::LoadFrameHierarchyFromFile(pScene, pd3dDevice, pd3dCommandList, pd3dGraphicsRootSignature, pInFile, strMeshDir);
		}
		else if (!strcmp(pstrToken, "</Hierarchy>"))
		{
			break;
		}
	}
    fclose(pInFile);

    // 로드 세션 종료 — 스레드로컬 상태 클리어
    t_currentLoadPath.clear();
    t_isCacheHit = false;

	return(pGameObject);
}

GameObject* MeshLoader::LoadFrameHierarchyFromFile(Scene* pScene, ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList, ID3D12RootSignature* pd3dGraphicsRootSignature, FILE* pInFile, const std::string& strMeshDir)
{
    char pstrToken[64] = { '\0' };
    UINT nReads = 0;
    int nFrame = 0;
    GameObject* pGameObject = nullptr;

    for ( ; ; )
    {
        if (::ReadStringFromFile(pInFile, pstrToken, 64) == 0) break;

        wchar_t buffer[128];
        swprintf_s(buffer, 128, L"  Read Token: %hs\n", pstrToken);
        OutputDebugString(buffer);

        if (!strcmp(pstrToken, "<Frame>:"))
        {
            pGameObject = pScene->CreateGameObject(pd3dDevice, pd3dCommandList);
            nFrame = ::ReadIntegerFromFile(pInFile);
            ::ReadStringFromFile(pInFile, pGameObject->m_pstrFrameName, 64);
        }
        else if (!strcmp(pstrToken, "<Transform>:"))
        {
            XMFLOAT3 xmf3Position, xmf3Rotation, xmf3Scale;
            XMFLOAT4 xmf4Rotation;
            nReads = (UINT)::fread(&xmf3Position, sizeof(float), 3, pInFile);
            nReads = (UINT)::fread(&xmf3Rotation, sizeof(float), 3, pInFile);
            nReads = (UINT)::fread(&xmf3Scale, sizeof(float), 3, pInFile);
            nReads = (UINT)::fread(&xmf4Rotation, sizeof(float), 4, pInFile);
        }
        else if (!strcmp(pstrToken, "<TransformMatrix>:"))
        {
            XMFLOAT4X4 xmf4x4Transform;
            nReads = (UINT)::fread(&xmf4x4Transform, sizeof(float), 16, pInFile);
            if (pGameObject) pGameObject->SetTransform(xmf4x4Transform);
        }
        else if (!strcmp(pstrToken, "<Mesh>:"))
        {
            MeshLoadInfo *pMeshInfo = MeshLoader::LoadMeshInfoFromFile(pInFile);
            if (pGameObject && pMeshInfo)
            {
                Mesh *pMesh = nullptr;
                const int meshIdx = t_meshCounter++;

                // 캐시 hit — 같은 파일 같은 순번 Mesh* 재사용 (GPU 리소스 공유)
                if (t_isCacheHit && !t_currentLoadPath.empty())
                {
                    auto cit = s_meshCache.find(t_currentLoadPath);
                    if (cit != s_meshCache.end() && meshIdx < (int)cit->second.size())
                    {
                        pMesh = cit->second[meshIdx];
                        // SetMesh 가 AddRef 하지만 캐시 자체도 외부 참조이므로 정상.
                    }
                }

                if (!pMesh)
                {
                    if (pMeshInfo->m_nType & VERTEXT_BONE_INDEX_WEIGHT)
                    {
                        pMesh = new SkinnedMesh(pd3dDevice, pd3dCommandList, pMeshInfo);
                    }
                    else if (pMeshInfo->m_nType & VERTEXT_NORMAL)
                    {
                        pMesh = new MeshIlluminatedFromFile(pd3dDevice, pd3dCommandList, pMeshInfo);
                    }
                    else
                    {
                        pMesh = new MeshFromFile(pd3dDevice, pd3dCommandList, pMeshInfo);
                    }

                    // AABB 전파 — ColliderComponent 가 collider extents 로 활용
                    if (pMesh)
                    {
                        pMesh->m_xmf3AABBCenter  = pMeshInfo->m_xmf3AABBCenter;
                        pMesh->m_xmf3AABBExtents = pMeshInfo->m_xmf3AABBExtents;
                    }

                    // 캐시 등록 (첫 로드 시) — 캐시 보유 ref 유지 위해 AddRef
                    if (pMesh && !t_currentLoadPath.empty())
                    {
                        pMesh->AddRef();   // 캐시가 소유하는 ref
                        s_meshCache[t_currentLoadPath].push_back(pMesh);
                    }
                }

                if (pMesh) pGameObject->SetMesh(pMesh);
                delete pMeshInfo;
            }
        }
        else if (!strcmp(pstrToken, "<Materials>:"))
        {
            LoadMaterialsInfoFromFile(pd3dDevice, pd3dCommandList, pInFile, pGameObject, pScene, strMeshDir);
        }
        else if (!strcmp(pstrToken, "<Children>:"))
        {
            int nChilds = ::ReadIntegerFromFile(pInFile);
            if (nChilds > 0)
            {
                for (int i = 0; i < nChilds; i++)
                {
                    GameObject *pChild = MeshLoader::LoadFrameHierarchyFromFile(pScene, pd3dDevice, pd3dCommandList, pd3dGraphicsRootSignature, pInFile, strMeshDir);
                    if (pGameObject && pChild)
                    {
                        pGameObject->SetChild(pChild);
                    }
                }
            }
        }
        else if (!strcmp(pstrToken, "</Frame>"))
        {
            break;
        }
    }
    return(pGameObject);
}

MeshLoadInfo* MeshLoader::LoadMeshInfoFromFile(FILE* pInFile)
{
	char pstrToken[64] = { '\0' };
	UINT nReads = 0;
	int nPositions = 0, nColors = 0, nNormals = 0, nIndices = 0, nSubMeshes = 0, nSubIndices = 0;
	MeshLoadInfo *pMeshInfo = new MeshLoadInfo;

	pMeshInfo->m_nVertices = ::ReadIntegerFromFile(pInFile);
	::ReadStringFromFile(pInFile, pMeshInfo->m_pstrMeshName, 64);

	for ( ; ; )
	{
		if (::ReadStringFromFile(pInFile, pstrToken, 64) == 0) break;

		if (!strcmp(pstrToken, "<Bounds>:"))
		{
			nReads = (UINT)::fread(&(pMeshInfo->m_xmf3AABBCenter), sizeof(XMFLOAT3), 1, pInFile);
			nReads = (UINT)::fread(&(pMeshInfo->m_xmf3AABBExtents), sizeof(XMFLOAT3), 1, pInFile);
		}
		else if (!strcmp(pstrToken, "<Positions>:"))
		{
			nPositions = ::ReadIntegerFromFile(pInFile);
			if (nPositions > 0)
			{
				pMeshInfo->m_nType |= VERTEXT_POSITION;
				pMeshInfo->m_pxmf3Positions = new XMFLOAT3[nPositions];
				nReads = (UINT)::fread(pMeshInfo->m_pxmf3Positions, sizeof(XMFLOAT3), nPositions, pInFile);
			}
		}
		else if (!strcmp(pstrToken, "<Colors>:"))
		{
			nColors = ::ReadIntegerFromFile(pInFile);
			if (nColors > 0)
			{
				pMeshInfo->m_nType |= VERTEXT_COLOR;
				pMeshInfo->m_pxmf4Colors = new XMFLOAT4[nColors];
				nReads = (UINT)::fread(pMeshInfo->m_pxmf4Colors, sizeof(XMFLOAT4), nColors, pInFile);
			}
		}
		else if (!strcmp(pstrToken, "<Normals>:"))
		{
			nNormals = ::ReadIntegerFromFile(pInFile);
			if (nNormals > 0)
			{
				pMeshInfo->m_nType |= VERTEXT_NORMAL;
				pMeshInfo->m_pxmf3Normals = new XMFLOAT3[nNormals];
				nReads = (UINT)::fread(pMeshInfo->m_pxmf3Normals, sizeof(XMFLOAT3), nNormals, pInFile);
			}
		}
		else if (!strcmp(pstrToken, "<TexCoords>:"))
		{
			int nTextureCoords = ::ReadIntegerFromFile(pInFile);
			if (nTextureCoords > 0)
			{
				pMeshInfo->m_nType |= VERTEXT_TEXTURE_COORD0;
				pMeshInfo->m_pxmf2TextureCoords0 = new XMFLOAT2[nTextureCoords];
				for (int i = 0; i < nTextureCoords; i++)
				{
					::fread(&pMeshInfo->m_pxmf2TextureCoords0[i], sizeof(XMFLOAT2), 1, pInFile);
					pMeshInfo->m_pxmf2TextureCoords0[i].y = 1.0f - pMeshInfo->m_pxmf2TextureCoords0[i].y;
				}
			}
		}
		else if (!strcmp(pstrToken, "<BoneWeights>:"))
		{
			int nBoneWeights = ::ReadIntegerFromFile(pInFile);
			if (nBoneWeights > 0)
			{
				pMeshInfo->m_nType |= 0x10; // VERTEXT_BONE_INDEX_WEIGHT
				pMeshInfo->m_pxmn4BoneIndices = new XMINT4[nBoneWeights];
				pMeshInfo->m_pxmf4BoneWeights = new XMFLOAT4[nBoneWeights];

				for (int i = 0; i < nBoneWeights; i++)
				{
					int idx; float w;
					fread(&idx, sizeof(int), 1, pInFile); fread(&w, sizeof(float), 1, pInFile);
					pMeshInfo->m_pxmn4BoneIndices[i].x = idx; pMeshInfo->m_pxmf4BoneWeights[i].x = w;

					fread(&idx, sizeof(int), 1, pInFile); fread(&w, sizeof(float), 1, pInFile);
					pMeshInfo->m_pxmn4BoneIndices[i].y = idx; pMeshInfo->m_pxmf4BoneWeights[i].y = w;

					fread(&idx, sizeof(int), 1, pInFile); fread(&w, sizeof(float), 1, pInFile);
					pMeshInfo->m_pxmn4BoneIndices[i].z = idx; pMeshInfo->m_pxmf4BoneWeights[i].z = w;

					fread(&idx, sizeof(int), 1, pInFile); fread(&w, sizeof(float), 1, pInFile);
					pMeshInfo->m_pxmn4BoneIndices[i].w = idx; pMeshInfo->m_pxmf4BoneWeights[i].w = w;
				}
			}
		}
		else if (!strcmp(pstrToken, "<BindPoses>:"))
		{
			int nBindPoses = ::ReadIntegerFromFile(pInFile);
			if (nBindPoses > 0)
			{
				for (int i = 0; i < nBindPoses; i++)
				{
					XMFLOAT4X4 mat;
					fread(&mat, sizeof(float), 16, pInFile);
					pMeshInfo->m_vBindPoses.push_back(mat);
				}
			}
		}
		else if (!strcmp(pstrToken, "<BoneNames>:"))
		{
			int nBoneNames = ::ReadIntegerFromFile(pInFile);
			if (nBoneNames > 0)
			{
				char pstrBoneName[64] = { 0 };
				for (int i = 0; i < nBoneNames; i++)
				{
					::ReadStringFromFile(pInFile, pstrBoneName, 64);
					pMeshInfo->m_vBoneNames.push_back(pstrBoneName);
				}
			}
		}
		else if (!strcmp(pstrToken, "<Indices>:"))
		{
			nIndices = ::ReadIntegerFromFile(pInFile);
			if (nIndices > 0)
			{
				pMeshInfo->m_pnIndices = new UINT[nIndices];
				nReads = (UINT)::fread(pMeshInfo->m_pnIndices, sizeof(int), nIndices, pInFile);
			}
		}
		else if (!strcmp(pstrToken, "<SubMeshes>:"))
		{
			pMeshInfo->m_nSubMeshes = ::ReadIntegerFromFile(pInFile);
			if (pMeshInfo->m_nSubMeshes > 0)
			{
				pMeshInfo->m_pnSubSetIndices = new int[pMeshInfo->m_nSubMeshes];
				pMeshInfo->m_ppnSubSetIndices = new UINT*[pMeshInfo->m_nSubMeshes];
				for (int i = 0; i < pMeshInfo->m_nSubMeshes; i++)
				{
					pMeshInfo->m_ppnSubSetIndices[i] = NULL;
					::ReadStringFromFile(pInFile, pstrToken, 64);
					if (!strcmp(pstrToken, "<SubMesh>:"))
					{
						int nIndex = ::ReadIntegerFromFile(pInFile);
						pMeshInfo->m_pnSubSetIndices[i] = ::ReadIntegerFromFile(pInFile);
						if (pMeshInfo->m_pnSubSetIndices[i] > 0)
						{
							pMeshInfo->m_ppnSubSetIndices[i] = new UINT[pMeshInfo->m_pnSubSetIndices[i]];
							nReads = (UINT)::fread(pMeshInfo->m_ppnSubSetIndices[i], sizeof(UINT), pMeshInfo->m_pnSubSetIndices[i], pInFile);
						}

					}
				}
			}
		}
		else if (!strcmp(pstrToken, "</Mesh>"))
		{
			break;
		}
	}
	return(pMeshInfo);
}