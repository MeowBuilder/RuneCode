#include "stdafx.h"
#include "MapLoader.h"
#include "Scene.h"
#include "Room.h"
#include "Shader.h"
#include "Mesh.h"
#include "GameObject.h"
#include "RenderComponent.h"
#include "ColliderComponent.h"
#include "CollisionLayer.h"
#include "TransformComponent.h"
#include "EnemySpawnData.h"
#include "EnemySpawner.h"
#include "MeleeAttackBehavior.h"
#include "RangedAttackBehavior.h"
#include "RushAoEAttackBehavior.h"
#include "RushFrontAttackBehavior.h"
#include "ChargedShotAttackBehavior.h"
#include "GrenadeThrowAttackBehavior.h"
#include "QuickJabAttackBehavior.h"
#include "SuicideExplodeAttackBehavior.h"
#include "Dx12App.h"
#include "TorchSystem.h"

#include <fstream>
#include <algorithm>
#include <random>
#include <sstream>
#include <map>
#include <tuple>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
//  JSON parser implementation
// ─────────────────────────────────────────────────────────────────────────────

const JsonVal& JsonVal::operator[](const std::string& k) const
{
    static JsonVal sNull;
    auto it = obj.find(k);
    return it != obj.end() ? it->second : sNull;
}

namespace {

struct JParser
{
    const char* p;
    const char* end;

    JParser(const char* s, size_t n) : p(s), end(s + n) {}

    void skipWS()
    {
        while (p < end && ((unsigned char)*p <= 0x20 ||
               (unsigned char)*p == 0xEF ||   // UTF-8 BOM bytes
               (unsigned char)*p == 0xBB ||
               (unsigned char)*p == 0xBF))
            p++;
    }

    JsonVal parseValue()
    {
        skipWS();
        if (p >= end) return {};
        switch (*p) {
        case '{': return parseObject();
        case '[': return parseArray();
        case '"': return parseString();
        case 't': { p += 4; JsonVal v; v.type = JsonVal::T::Bool; v.b = true;  return v; }
        case 'f': { p += 5; JsonVal v; v.type = JsonVal::T::Bool; v.b = false; return v; }
        case 'n': { p += 4; return {}; }
        default:  return parseNumber();
        }
    }

    JsonVal parseObject()
    {
        JsonVal v; v.type = JsonVal::T::Obj;
        p++; // skip '{'
        while (p < end) {
            skipWS();
            if (*p == '}') { p++; break; }
            if (*p == ',') { p++; continue; }
            if (*p != '"') { p++; continue; } // malformed – skip
            std::string key = parseString().str;
            skipWS();
            if (p < end && *p == ':') p++;
            v.obj[key] = parseValue();
        }
        return v;
    }

    JsonVal parseArray()
    {
        JsonVal v; v.type = JsonVal::T::Arr;
        p++; // skip '['
        while (p < end) {
            skipWS();
            if (*p == ']') { p++; break; }
            if (*p == ',') { p++; continue; }
            v.arr.push_back(parseValue());
        }
        return v;
    }

    JsonVal parseString()
    {
        JsonVal v; v.type = JsonVal::T::Str;
        p++; // skip opening '"'
        while (p < end && *p != '"') {
            if (*p == '\\') {
                p++;
                if (p >= end) break;
                switch (*p) {
                case '"':  v.str += '"';  break;
                case '\\': v.str += '\\'; break;
                case '/':  v.str += '/';  break;
                case 'n':  v.str += '\n'; break;
                case 'r':  v.str += '\r'; break;
                case 't':  v.str += '\t'; break;
                default:   v.str += *p;   break;
                }
                p++;
            } else {
                v.str += *p++;
            }
        }
        if (p < end) p++; // skip closing '"'
        return v;
    }

    JsonVal parseNumber()
    {
        JsonVal v; v.type = JsonVal::T::Num;
        char* endptr = nullptr;
        v.num = strtod(p, &endptr);
        if (endptr > p) p = endptr;
        else p++; // skip unknown char to avoid infinite loop
        return v;
    }
};

} // anonymous namespace

JsonVal JsonVal::parse(const std::string& text)
{
    if (text.empty()) return {};
    JParser jp(text.data(), text.size());
    return jp.parseValue();
}

JsonVal JsonVal::parseFile(const char* path)
{
    std::ifstream fs(path, std::ios::binary);
    if (!fs.is_open()) {
        char buf[256];
        sprintf_s(buf, "[MapLoader] Cannot open: %s\n", path);
        OutputDebugStringA(buf);
        return {};
    }
    std::ostringstream ss;
    ss << fs.rdbuf();
    return parse(ss.str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  OBJ mesh builder helper
//  Parses an OBJ file and returns GPU mesh buffers (position, normal, uv, index).
//  Uses the same buffer layout as CubeMesh / RingMesh.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// OBJ load result: mesh + local-space AABB
struct ObjResult {
    Mesh*    pMesh = nullptr;           // primary (merged or first group)
    XMFLOAT3 aabbMin{0,0,0};
    XMFLOAT3 aabbMax{0,0,0};
    bool     valid = false;
    // Per-group submeshes (populated when OBJ has "g" groups)
    std::vector<Mesh*>       subMeshes;
    std::vector<std::string> subGroups;
};
std::map<std::string, ObjResult> s_meshCache;
static std::map<std::string, JsonVal>  s_jsonCache;  // 파싱된 JSON 재사용

// Deduplication key: (posIdx, uvIdx, nrmIdx)
struct FaceKey {
    int p, t, n;
    bool operator==(const FaceKey& o) const { return p==o.p && t==o.t && n==o.n; }
};
struct FaceKeyHash {
    size_t operator()(const FaceKey& k) const {
        size_t h = (size_t)(k.p * 73856093) ^ (size_t)(k.t * 19349663) ^ (size_t)(k.n * 83492791);
        return h;
    }
};

struct ObjRawData {
    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> uvs;

    // Merged output (used when OBJ has no "g" groups)
    std::vector<XMFLOAT3> outPos;
    std::vector<XMFLOAT3> outNrm;
    std::vector<XMFLOAT2> outUV;
    std::vector<UINT>     outIdx;

    // Per-group output (used when OBJ has "g" groups)
    struct GroupData {
        std::string name;
        std::vector<XMFLOAT3> outPos;
        std::vector<XMFLOAT3> outNrm;
        std::vector<XMFLOAT2> outUV;
        std::vector<UINT>     outIdx;
        std::unordered_map<FaceKey, UINT, FaceKeyHash> vertexMap;
    };
    std::vector<GroupData> groups;
    int currentGroup = -1;  // index into groups, -1 = no groups yet
};

// Parse one "v/t/n" token (1-based OBJ indices, 0 means absent)
FaceKey parseFaceToken(const char* tok)
{
    FaceKey k{0,0,0};
    // Try v/t/n, v//n, v/t, v
    if (sscanf_s(tok, "%d/%d/%d", &k.p, &k.t, &k.n) == 3) {}
    else if (sscanf_s(tok, "%d//%d", &k.p, &k.n) == 2) {}
    else if (sscanf_s(tok, "%d/%d",  &k.p, &k.t) == 2) {}
    else sscanf_s(tok, "%d", &k.p);
    return k;
}

ObjResult LoadObjMesh(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                      const std::string& path)
{
    // Cache lookup
    auto it = s_meshCache.find(path);
    if (it != s_meshCache.end()) return it->second;

    std::ifstream fs(path, std::ios::in);
    if (!fs.is_open()) {
        char buf[512];
        sprintf_s(buf, "[MapLoader] OBJ not found: %s\n", path.c_str());
        OutputDebugStringA(buf);
        s_meshCache[path] = {};
        return {};
    }

    ObjRawData raw;
    std::unordered_map<FaceKey, UINT, FaceKeyHash> vertexMap;

    std::string line;
    while (std::getline(fs, line)) {
        if (line.empty() || line[0] == '#') continue;

        char type[8] = {};
        if (sscanf_s(line.c_str(), "%7s", type, (unsigned)sizeof(type)) != 1) continue;
        const char* rest = line.c_str() + strlen(type);
        while (*rest == ' ') rest++;

        if (strcmp(type, "v") == 0) {
            XMFLOAT3 v;
            sscanf_s(rest, "%f %f %f", &v.x, &v.y, &v.z);
            v.z = -v.z;  // exporter negates Z (Unity LH->OBJ RH); restore for DX12 (also LH)
            raw.positions.push_back(v);
        } else if (strcmp(type, "vn") == 0) {
            XMFLOAT3 n;
            sscanf_s(rest, "%f %f %f", &n.x, &n.y, &n.z);
            n.z = -n.z;  // same for normals
            raw.normals.push_back(n);
        } else if (strcmp(type, "vt") == 0) {
            XMFLOAT2 uv;
            sscanf_s(rest, "%f %f", &uv.x, &uv.y);
            raw.uvs.push_back(uv);
        } else if (strcmp(type, "g") == 0) {
            std::string grpName(rest);
            while (!grpName.empty() && (grpName.back() == '\r' || grpName.back() == '\n' || grpName.back() == ' '))
                grpName.pop_back();
            raw.groups.push_back({grpName, {}, {}, {}, {}, {}});
            raw.currentGroup = (int)raw.groups.size() - 1;
        } else if (strcmp(type, "f") == 0) {
            // Triangulate: fan from first vertex
            char tok[4][64] = {};
            int n = sscanf_s(rest, "%63s %63s %63s %63s",
                             tok[0], (unsigned)64, tok[1], (unsigned)64,
                             tok[2], (unsigned)64, tok[3], (unsigned)64);
            if (n < 3) continue;

            FaceKey keys[4];
            for (int i = 0; i < n; i++) keys[i] = parseFaceToken(tok[i]);

            // Fan triangulate: (0,1,2) and (0,2,3) if quad
            int triCount = n - 2;
            for (int t = 0; t < triCount; t++) {
                FaceKey tri[3] = { keys[0], keys[t+1], keys[t+2] };
                for (auto& k : tri) {
                    if (raw.currentGroup >= 0) {
                        // Per-group path
                        auto& grp = raw.groups[raw.currentGroup];
                        auto vit = grp.vertexMap.find(k);
                        if (vit == grp.vertexMap.end()) {
                            UINT idx = (UINT)grp.outPos.size();
                            grp.vertexMap[k] = idx;
                            grp.outPos.push_back(k.p > 0 && k.p <= (int)raw.positions.size() ? raw.positions[k.p-1] : XMFLOAT3(0,0,0));
                            grp.outNrm.push_back(k.n > 0 && k.n <= (int)raw.normals.size() ? raw.normals[k.n-1] : XMFLOAT3(0,1,0));
                            grp.outUV.push_back(k.t > 0 && k.t <= (int)raw.uvs.size() ? raw.uvs[k.t-1] : XMFLOAT2(0,0));
                            grp.outIdx.push_back(idx);
                        } else {
                            grp.outIdx.push_back(vit->second);
                        }
                    } else {
                        // Merged path
                        auto vit = vertexMap.find(k);
                        if (vit == vertexMap.end()) {
                            UINT idx = (UINT)raw.outPos.size();
                            vertexMap[k] = idx;

                            if (k.p > 0 && k.p <= (int)raw.positions.size())
                                raw.outPos.push_back(raw.positions[k.p - 1]);
                            else
                                raw.outPos.push_back(XMFLOAT3(0,0,0));

                            if (k.n > 0 && k.n <= (int)raw.normals.size())
                                raw.outNrm.push_back(raw.normals[k.n - 1]);
                            else
                                raw.outNrm.push_back(XMFLOAT3(0,1,0));

                            if (k.t > 0 && k.t <= (int)raw.uvs.size())
                                raw.outUV.push_back(raw.uvs[k.t - 1]);
                            else
                                raw.outUV.push_back(XMFLOAT2(0,0));

                            raw.outIdx.push_back(idx);
                        } else {
                            raw.outIdx.push_back(vit->second);
                        }
                    }
                }
            }
        }
    }

    // Compute local-space AABB from raw position data
    if (raw.positions.empty()) {
        char buf[512];
        sprintf_s(buf, "[MapLoader] OBJ empty geometry: %s\n", path.c_str());
        OutputDebugStringA(buf);
        s_meshCache[path] = {};
        return {};
    }

    ObjResult result;
    result.aabbMin = raw.positions[0];
    result.aabbMax = raw.positions[0];
    for (const auto& v : raw.positions) {
        result.aabbMin.x = (std::min)(result.aabbMin.x, v.x);
        result.aabbMin.y = (std::min)(result.aabbMin.y, v.y);
        result.aabbMin.z = (std::min)(result.aabbMin.z, v.z);
        result.aabbMax.x = (std::max)(result.aabbMax.x, v.x);
        result.aabbMax.y = (std::max)(result.aabbMax.y, v.y);
        result.aabbMax.z = (std::max)(result.aabbMax.z, v.z);
    }

    if (!raw.groups.empty()) {
        // Build one mesh per group
        for (auto& grp : raw.groups) {
            if (grp.outPos.empty() || grp.outIdx.empty()) continue;
            ObjMesh* pSubMesh = new ObjMesh();
            pSubMesh->Build(pDevice, pCommandList, grp.outPos, grp.outNrm, grp.outUV, grp.outIdx);
            result.subMeshes.push_back(pSubMesh);
            result.subGroups.push_back(grp.name);
        }
        if (result.subMeshes.empty()) {
            s_meshCache[path] = {};
            return {};
        }
        result.pMesh = result.subMeshes[0];  // backward compat: primary = first group
    } else {
        // Build single merged mesh (no groups)
        if (raw.outPos.empty() || raw.outIdx.empty()) {
            char buf[512];
            sprintf_s(buf, "[MapLoader] OBJ empty geometry: %s\n", path.c_str());
            OutputDebugStringA(buf);
            s_meshCache[path] = {};
            return {};
        }
        ObjMesh* pMesh = new ObjMesh();
        pMesh->Build(pDevice, pCommandList, raw.outPos, raw.outNrm, raw.outUV, raw.outIdx);
        result.pMesh = pMesh;
    }

    result.valid = true;
    s_meshCache[path] = result;
    return result;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  MapLoader::LoadIntoScene
// ─────────────────────────────────────────────────────────────────────────────

// ─── Global map scale ────────────────────────────────────────────────────────
// Increase to make the entire map larger relative to the character.
// All positions, object scales, room bounds, and obstacle sizes are multiplied.
static constexpr float MAP_SCALE = 5.0f;
// ─────────────────────────────────────────────────────────────────────────────

bool MapLoader::LoadIntoScene(
    const char*                 jsonPath,
    Scene*                      pScene,
    ID3D12Device*               pDevice,
    ID3D12GraphicsCommandList*  pCommandList,
    Shader*                     pShader,
    DirectX::XMFLOAT3           positionOffset,
    bool                        skipRoomAndSpawn)
{
    // s_meshCache / s_jsonCache / s_textureCache — cleared 하지 않고 재사용

    auto jsonIt = s_jsonCache.find(jsonPath);
    if (jsonIt == s_jsonCache.end())
    {
        JsonVal parsed = JsonVal::parseFile(jsonPath);
        if (parsed.isNull()) {
            OutputDebugStringA("[MapLoader] Failed to parse map.json\n");
            return false;
        }
        s_jsonCache[jsonPath] = std::move(parsed);
    }
    const JsonVal& root = s_jsonCache[jsonPath];

    // ── 1. Rooms ─────────────────────────────────────────────────────────────
    if (!skipRoomAndSpawn)
    {
        const JsonVal& rooms = root["rooms"];
        for (size_t i = 0; i < rooms.size(); i++) {
            const JsonVal& r = rooms[i];
            const JsonVal& bMin = r["boundsMin"];
            const JsonVal& bMax = r["boundsMax"];
            XMFLOAT3 mn(bMin[0].f()*MAP_SCALE, bMin[1].f()*MAP_SCALE, -bMin[2].f()*MAP_SCALE);
            XMFLOAT3 mx(bMax[0].f()*MAP_SCALE, bMax[1].f()*MAP_SCALE, -bMax[2].f()*MAP_SCALE);
            XMFLOAT3 center((mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f, (mn.z+mx.z)*0.5f);
            XMFLOAT3 extents(fabsf(mx.x-mn.x)*0.5f, fabsf(mx.y-mn.y)*0.5f, fabsf(mx.z-mn.z)*0.5f);

            pScene->CreateRoomFromBounds(center, extents);
        }

        // Default to first room for objects below
        if (!pScene->GetCurrentRoom() && !pScene->GetRooms().empty())
            pScene->SetCurrentRoom(pScene->GetRooms()[0].get());
    }

    // ── 2. Player spawn ───────────────────────────────────────────────────────
    if (!skipRoomAndSpawn && root.has("playerSpawn")) {
        const JsonVal& ps = root["playerSpawn"];
        const JsonVal& pos = ps["position"];
        if (pScene->GetPlayer()) {
            pScene->GetPlayer()->GetTransform()->SetPosition(
                pos[0].f()*MAP_SCALE, pos[1].f()*MAP_SCALE, -pos[2].f()*MAP_SCALE);
        }
    }

    // ── 3. Map objects (renderable geometry) ─────────────────────────────────
    // Get the directory of the JSON file to resolve relative mesh paths
    std::string jsonDir = jsonPath;
    size_t lastSlash = jsonDir.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        jsonDir = jsonDir.substr(0, lastSlash + 1);
    else
        jsonDir = "";

    const JsonVal& mapObjs = root["mapObjects"];
    TorchSystem* pTorchSystemRef = pScene->GetTorchSystem();  // Brazier 감지 시 불 추가용
    for (size_t i = 0; i < mapObjs.size(); i++) {
        const JsonVal& mo = mapObjs[i];
        std::string meshRelPath = mo["meshFile"].str;
        std::string meshPath = jsonDir + meshRelPath;

        ObjResult objRes = LoadObjMesh(pDevice, pCommandList, meshPath);
        if (!objRes.valid || !objRes.pMesh) continue;

        GameObject* pGO = pScene->CreateGameObject(pDevice, pCommandList);

        // Check if this is a lava object (by mesh name)
        std::string meshNameLower = meshRelPath;
        std::transform(meshNameLower.begin(), meshNameLower.end(), meshNameLower.begin(), ::tolower);
        if (meshNameLower.find("lava") != std::string::npos) {
            pGO->SetLava(true);
        }

        // Transform (+ positionOffset 적용 — 복제 시 오프셋 위치에 배치)
        const JsonVal& pos = mo["position"];
        const JsonVal& rot = mo["rotation"];
        const JsonVal& scl = mo["scale"];
        float sx = scl[0].f()*MAP_SCALE, sy = scl[1].f()*MAP_SCALE, sz = scl[2].f()*MAP_SCALE;
        pGO->GetTransform()->SetPosition(
            pos[0].f()*MAP_SCALE + positionOffset.x,
            pos[1].f()*MAP_SCALE + positionOffset.y,
            -pos[2].f()*MAP_SCALE + positionOffset.z);
        pGO->GetTransform()->SetRotation(XMFLOAT4(rot[0].f(), rot[1].f(), -rot[2].f(), rot[3].f()));
        pGO->GetTransform()->SetScale(sx, sy, sz);

        // Brazier_002: 맵에 배치된 화로 오브젝트 위에 횃불 flame 스폰.
        // TorchSystem 의 flame billboard + 조명 공용 재활용. mesh 중복 스폰 X.
        // flameScale 3.5 로 꽤 크게, heightOffset 은 Brazier 스케일 비례 받침 상단.
        {
            std::string moName = mo.has("name") ? mo["name"].str : "";
            if (pTorchSystemRef && moName.find("Brazier_002") != std::string::npos)
            {
                XMFLOAT3 brazierPos = pGO->GetTransform()->GetPosition();
                float heightOff = sy * 1.7f;  // 받침 위쪽에 살짝 낮게 — 화로 속에 꽂힌 느낌
                pTorchSystemRef->AddTorch(brazierPos, pDevice, pCommandList,
                                          3.5f /*flameScale*/, false /*spawnMesh*/, heightOff);
            }
        }

        // Render
        pGO->SetMesh(objRes.pMesh);
        auto* pRC = pGO->AddComponent<RenderComponent>();
        pRC->SetMesh(objRes.pMesh);
        pRC->SetCastsShadow(true);
        pShader->AddRenderComponent(pRC);

        // Helper: apply material properties from a JSON value (top-level or per-material entry)
        auto applyMat = [&](GameObject* pTarget, const JsonVal& src) {
            float r = 1.f, g = 1.f, b = 1.f;
            if (src.has("color")) {
                const JsonVal& col = src["color"];
                r = col[0].f() / 255.f;
                g = col[1].f() / 255.f;
                b = col[2].f() / 255.f;
            }
            float smooth   = src.has("smoothness") ? src["smoothness"].f() : 0.5f;
            float metallic = src.has("metallic")   ? src["metallic"].f()   : 0.0f;
            float specPow  = 2.0f + smooth * smooth * 254.0f;
            float specStr  = (1.0f - metallic) * 0.1f + metallic * 0.9f;

            float er = 0.f, eg = 0.f, eb = 0.f;
            if (src.has("emissive")) {
                const JsonVal& em = src["emissive"];
                er = em[0].f() / 255.f;
                eg = em[1].f() / 255.f;
                eb = em[2].f() / 255.f;
            }

            MATERIAL mat;
            mat.m_cAmbient  = XMFLOAT4(r * 0.25f, g * 0.25f, b * 0.25f, 1.0f);
            mat.m_cDiffuse  = XMFLOAT4(r, g, b, 1.0f);
            mat.m_cSpecular = XMFLOAT4(specStr, specStr, specStr, specPow);
            mat.m_cEmissive = XMFLOAT4(er, eg, eb, 1.0f);
            pTarget->SetMaterial(mat);
        };

        // Helper: load texture from a JSON value
        auto applyTex = [&](GameObject* pTarget, const JsonVal& src) {
            if (src.has("texture") && !src["texture"].str.empty()) {
                std::string texFullPath = jsonDir + src["texture"].str;
                pTarget->SetTextureName(texFullPath);
                D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
                D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
                pScene->AllocateDescriptor(&cpuHandle, &gpuHandle);
                pTarget->LoadTexture(pDevice, pCommandList, cpuHandle);
                pTarget->SetSrvGpuDescriptorHandle(gpuHandle);
            }
            if (src.has("emissiveTexture") && !src["emissiveTexture"].str.empty()) {
                std::string emTexFullPath = jsonDir + src["emissiveTexture"].str;
                pTarget->SetEmissiveTextureName(emTexFullPath);
                D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
                D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
                pScene->AllocateDescriptor(&cpuHandle, &gpuHandle);
                pTarget->LoadEmissiveTexture(pDevice, pCommandList, cpuHandle);
                pTarget->SetEmissiveSrvGpuDescriptorHandle(gpuHandle);
            }
        };

        if (mo.has("materials") && !objRes.subMeshes.empty()) {
            // Multi-material: override primary GO with sub_0 mesh + materials[0],
            // then create additional GOs for each remaining submesh.
            const JsonVal& mats = mo["materials"];
            size_t count = (std::min)(objRes.subMeshes.size(), mats.size());
            for (size_t mi = 0; mi < count; mi++) {
                GameObject* pSubGO = (mi == 0) ? pGO : pScene->CreateGameObject(pDevice, pCommandList);
                if (mi > 0) {
                    pSubGO->GetTransform()->SetPosition(
                        pos[0].f()*MAP_SCALE, pos[1].f()*MAP_SCALE, -pos[2].f()*MAP_SCALE);
                    pSubGO->GetTransform()->SetRotation(XMFLOAT4(rot[0].f(), rot[1].f(), -rot[2].f(), rot[3].f()));
                    pSubGO->GetTransform()->SetScale(sx, sy, sz);
                    pSubGO->SetMesh(objRes.subMeshes[mi]);  // FIX: GameObject에도 mesh 지정 (누락 버그)
                    auto* pRC2 = pSubGO->AddComponent<RenderComponent>();
                    pRC2->SetMesh(objRes.subMeshes[mi]);
                    pRC2->SetCastsShadow(true);
                    pShader->AddRenderComponent(pRC2);
                } else {
                    // Patch the primary GO's RenderComponent to use sub_0 mesh
                    pGO->SetMesh(objRes.subMeshes[0]);
                    if (auto* pRC0 = pGO->GetComponent<RenderComponent>())
                        pRC0->SetMesh(objRes.subMeshes[0]);
                }
                applyMat(pSubGO, mats[mi]);
                applyTex(pSubGO, mats[mi]);
            }
        } else {
            // Single-material path
            applyMat(pGO, mo);

            // Floor tile variety: ~20% of grid floor tiles get a lava decoration variant
            bool usedLavaVariant = false;
            if (meshRelPath.find("LavaMaze_GridTile_01") != std::string::npos) {
                const JsonVal& tpos = mo["position"];
                // Convert to grid indices (tile spacing = 2 units) to avoid alignment bias
                int gx = (int)roundf(tpos[0].f() * 0.5f);
                int gz = (int)roundf(tpos[2].f() * 0.5f);
                unsigned int h = (unsigned int)(gx * 2246822519u) ^ (unsigned int)(gz * 3266489917u);
                if (h % 5 == 0) {
                    int variant = (int)((h >> 8u) % 4u) + 1;  // upper bits for variant; low bits correlated with %5
                    char varTex[128];
                    sprintf_s(varTex, "meshes/textures/grid_tile_lava/grid_tile_lava (%d).png", variant);
                    std::string texFullPath = jsonDir + varTex;
                    pGO->SetTextureName(texFullPath);
                    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
                    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
                    pScene->AllocateDescriptor(&cpuHandle, &gpuHandle);
                    pGO->LoadTexture(pDevice, pCommandList, cpuHandle);
                    pGO->SetSrvGpuDescriptorHandle(gpuHandle);
                    pGO->SetLava(false);  // pool texture has a fixed design; UV animation would cause it to drift
                    usedLavaVariant = true;
                }
            }
            if (!usedLavaVariant)
                applyTex(pGO, mo);
        }

        // Collider: SetExtents uses LOCAL-space extents; ColliderComponent::Update()
        // transforms them by the full world matrix (which already includes sx/sy/sz),
        // so do NOT pre-multiply by scale here (that would square the scale).
        XMFLOAT3 localExt(
            (objRes.aabbMax.x - objRes.aabbMin.x) * 0.5f,
            (objRes.aabbMax.y - objRes.aabbMin.y) * 0.5f,
            (objRes.aabbMax.z - objRes.aabbMin.z) * 0.5f);
        XMFLOAT3 localCenter(
            (objRes.aabbMin.x + objRes.aabbMax.x) * 0.5f,
            (objRes.aabbMin.y + objRes.aabbMax.y) * 0.5f,
            (objRes.aabbMin.z + objRes.aabbMax.z) * 0.5f);

        // World-space approximate extents (for skip checks only)
        float worldExtX = localExt.x * sx;
        float worldExtY = localExt.y * sy;
        float worldExtZ = localExt.z * sz;
        float maxWorldExt = worldExtX;
        if (worldExtY > maxWorldExt) maxWorldExt = worldExtY;
        if (worldExtZ > maxWorldExt) maxWorldExt = worldExtZ;

        // Props ("prop": true) are render-only – no collision
        bool isProp = mo.has("prop") && mo["prop"].b;

        // Skip tiny objects (decorations, grass blades, pebbles)
        if (!isProp && maxWorldExt > 0.3f) {
            // Skip horizontal surfaces (floors, ceilings) – they cause the player to
            // be flung sideways when the push-back MTV picks the smaller XZ axis.
            // A mesh is "horizontal" if its world Y-extent is much smaller than XZ.
            float maxWorldXZ = worldExtX > worldExtZ ? worldExtX : worldExtZ;
            bool isHorizontal = (worldExtY * 4.0f < maxWorldXZ);
            if (!isHorizontal) {
            auto* pCol = pGO->AddComponent<ColliderComponent>();
            pCol->SetExtents(localExt.x, localExt.y, localExt.z);
            pCol->SetCenter(localCenter.x, localCenter.y, localCenter.z);
            pCol->SetLayer(CollisionLayer::Wall);
            pCol->SetCollisionMask(CollisionMask::Wall);
            } // !isHorizontal
        } // !isProp && maxWorldExt > 0.3f
    }

    // ── 3b. 플레이어 스폰 근처 테스트 횃불 하드코딩 제거 (사용자 요청)
    //        Brazier_002 위의 flame 이 맵 횃불 역할을 대신함.

    // ── 4. Obstacles (collision only) ────────────────────────────────────────
    const JsonVal& obstacles = root["obstacles"];
    for (size_t i = 0; i < obstacles.size(); i++) {
        const JsonVal& obs = obstacles[i];
        const JsonVal& center = obs["center"];
        const JsonVal& size   = obs["size"];

        GameObject* pGO = pScene->CreateGameObject(pDevice, pCommandList);
        pGO->GetTransform()->SetPosition(
            center[0].f()*MAP_SCALE + positionOffset.x,
            center[1].f()*MAP_SCALE + positionOffset.y,
            -center[2].f()*MAP_SCALE + positionOffset.z);

        auto* pCol = pGO->AddComponent<ColliderComponent>();
        pCol->SetExtents(size[0].f()*MAP_SCALE*0.5f, size[1].f()*MAP_SCALE*0.5f, size[2].f()*MAP_SCALE*0.5f);
        pCol->SetCenter(0.0f, 0.0f, 0.0f);
        pCol->SetLayer(CollisionLayer::Wall);
        pCol->SetCollisionMask(CollisionMask::Wall);
    }

    // ── 5. Enemy spawns → RoomSpawnConfig ────────────────────────────────────
    // 복제 로딩 시 적 스폰 건너뜀 (중복 등록 방지)
    if (skipRoomAndSpawn) return true;

    const JsonVal& enemySpawns = root["enemySpawns"];
    RoomSpawnConfig spawnConfig;

    for (size_t i = 0; i < enemySpawns.size(); i++) {
        const JsonVal& es = enemySpawns[i];
        std::string presetName = es["presetName"].str;
        int count = es.has("count") ? es["count"].i() : 1;
        const JsonVal& pos = es["position"];
        XMFLOAT3 spawnPos(pos[0].f()*MAP_SCALE, pos[1].f()*MAP_SCALE, -pos[2].f()*MAP_SCALE);

        // Register preset if not already registered
        if (!pScene->GetEnemySpawner()->HasPreset(presetName)) {
            EnemySpawnData data;

            // Stats
            if (es.has("stats")) {
                const JsonVal& stats = es["stats"];
                data.m_Stats.m_fMaxHP          = stats["maxHP"].f();
                data.m_Stats.m_fCurrentHP      = stats["maxHP"].f();
                data.m_Stats.m_fMoveSpeed      = stats["moveSpeed"].f();
                data.m_Stats.m_fAttackRange    = stats["attackRange"].f();
                data.m_Stats.m_fAttackCooldown = stats["attackCooldown"].f();
                data.m_Stats.m_fDetectionRange = stats["detectionRange"].f();
            }

            // Attack behavior factory (use default parameters; stats are applied via EnemyComponent)
            std::string attackType = es.has("attackType") ? es["attackType"].str : "Melee";
            ProjectileManager* pProjMgr = pScene->GetProjectileManager();
            if (attackType == "RushFront") {
                data.m_fnCreateAttack = []() -> std::unique_ptr<IAttackBehavior> {
                    return std::make_unique<RushFrontAttackBehavior>();
                };
            } else if (attackType == "RushAoE") {
                data.m_fnCreateAttack = []() -> std::unique_ptr<IAttackBehavior> {
                    return std::make_unique<RushAoEAttackBehavior>();
                };
            } else if (attackType == "Ranged") {
                data.m_fnCreateAttack = [pProjMgr]() -> std::unique_ptr<IAttackBehavior> {
                    return std::make_unique<RangedAttackBehavior>(pProjMgr);
                };
            } else if (attackType == "ChargedShot") {
                data.m_fnCreateAttack = [pProjMgr]() -> std::unique_ptr<IAttackBehavior> {
                    return std::make_unique<ChargedShotAttackBehavior>(pProjMgr);
                };
            } else if (attackType == "GrenadeThrow") {
                data.m_fnCreateAttack = [pProjMgr]() -> std::unique_ptr<IAttackBehavior> {
                    return std::make_unique<GrenadeThrowAttackBehavior>(pProjMgr);
                };
            } else if (attackType == "QuickJab") {
                data.m_fnCreateAttack = []() -> std::unique_ptr<IAttackBehavior> {
                    return std::make_unique<QuickJabAttackBehavior>();
                };
            } else if (attackType == "SuicideExplode") {
                data.m_fnCreateAttack = [pProjMgr]() -> std::unique_ptr<IAttackBehavior> {
                    return std::make_unique<SuicideExplodeAttackBehavior>(pProjMgr);
                };
            } else { // Melee (default)
                data.m_fnCreateAttack = []() -> std::unique_ptr<IAttackBehavior> {
                    return std::make_unique<MeleeAttackBehavior>();
                };
            }

            // Attack indicator
            if (es.has("indicator")) {
                const JsonVal& ind = es["indicator"];
                std::string indType = ind["type"].str;
                if      (indType == "Circle")     data.m_IndicatorConfig.m_eType = IndicatorType::Circle;
                else if (indType == "RushCircle") data.m_IndicatorConfig.m_eType = IndicatorType::RushCircle;
                else if (indType == "RushCone")   data.m_IndicatorConfig.m_eType = IndicatorType::RushCone;
                else if (indType == "ForwardBox") data.m_IndicatorConfig.m_eType = IndicatorType::ForwardBox;
                else                              data.m_IndicatorConfig.m_eType = IndicatorType::None;

                data.m_IndicatorConfig.m_fRushDistance = ind["rushDistance"].f();
                data.m_IndicatorConfig.m_fHitRadius    = ind["hitRadius"].f();
                data.m_IndicatorConfig.m_fConeAngle    = ind["coneAngle"].f();
                // ForwardBox 전방 길이 — ChargedShot 등 라인형 텔레그래프에서 사용
                if (ind.has("hitLength")) {
                    data.m_IndicatorConfig.m_fHitLength = ind["hitLength"].f();
                }
            }

            // Animation clips
            if (es.has("animClips")) {
                const JsonVal& clips = es["animClips"];
                if (clips.has("idle"))    data.m_AnimConfig.m_strIdleClip    = clips["idle"].str;
                if (clips.has("chase"))   data.m_AnimConfig.m_strChaseClip   = clips["chase"].str;
                if (clips.has("attack"))  data.m_AnimConfig.m_strAttackClip  = clips["attack"].str;
                if (clips.has("stagger")) data.m_AnimConfig.m_strStaggerClip = clips["stagger"].str;
                if (clips.has("death"))   data.m_AnimConfig.m_strDeathClip   = clips["death"].str;
            }

            // Visual
            if (es.has("visual")) {
                const JsonVal& vis = es["visual"];
                data.m_strMeshPath      = vis["meshPath"].str;
                data.m_strAnimationPath = vis["animationPath"].str;
                const JsonVal& scl = vis["scale"];
                data.m_xmf3Scale = XMFLOAT3(scl[0].f(), scl[1].f(), scl[2].f());
                const JsonVal& col = vis["color"];
                data.m_xmf4Color = XMFLOAT4(col[0].f()/255.f, col[1].f()/255.f, col[2].f()/255.f, col[3].f()/255.f);
            }

            pScene->GetEnemySpawner()->RegisterEnemyPreset(presetName, data);
        }

        // 2D grid 배치 — 기존 X축 일렬(2*MAP_SCALE=10m) 은 count 늘면 방 경계 넘음.
        //   ceil(sqrt(count)) x 같은 크기 격자, 셀 간격 5m.
        int cols = static_cast<int>(ceilf(sqrtf(static_cast<float>(count))));
        if (cols < 1) cols = 1;
        const float cell = 5.0f;
        for (int c = 0; c < count; c++) {
            XMFLOAT3 p = spawnPos;
            int row = c / cols;
            int col = c % cols;
            p.x += col * cell;
            p.z += row * cell;
            spawnConfig.AddSpawn(presetName, p);
        }
    }

    // [임시 — 웨이브 시스템 검증용]
    //   웨이브당 최대 kPerWave 마리로 자동 분할 (프레임 압박 완화 + 페이즈 체감).
    //   첫 웨이브 Immediate, 이후는 AfterPrevCleared (직전 웨이브 전멸 시).
    //   JSON 직접 지정 (waves 필드) 추가 전까지의 임시 처리.

    // 변종/일반을 분리해 각각 셔플 후 비율 유지하며 인터리브 — 모든 wave 에 변종 비율 보장.
    //   단순 전체 셔플은 wave 0 에 변종이 적게 떨어지는 경우 발생 (운). 비율 인터리브로 해소.
    //   결정적 시드 → 같은 룸 입장 시 같은 배치 (재현성).
    auto IsVariant = [](const std::pair<std::string, XMFLOAT3>& e) {
        const std::string& n = e.first;
        return n.find("_Jabber")    != std::string::npos
            || n.find("_Grenadier") != std::string::npos
            || n.find("_Sniper")    != std::string::npos
            || n.find("_Bomber")    != std::string::npos;
    };
    {
        std::vector<std::pair<std::string, XMFLOAT3>> variants, bases;
        variants.reserve(spawnConfig.m_vEnemySpawns.size());
        bases.reserve(spawnConfig.m_vEnemySpawns.size());
        for (auto& s : spawnConfig.m_vEnemySpawns)
            (IsVariant(s) ? variants : bases).push_back(s);

        std::mt19937 rng(static_cast<unsigned>(spawnConfig.m_vEnemySpawns.size() * 2654435761u));
        std::shuffle(variants.begin(), variants.end(), rng);
        std::shuffle(bases.begin(),    bases.end(),    rng);

        // 위치도 따로 셔플 — preset 슬롯이 새 위치로 흩어짐 (공간 클러스터링 해소)
        std::vector<XMFLOAT3> positions;
        positions.reserve(spawnConfig.m_vEnemySpawns.size());
        for (auto& s : spawnConfig.m_vEnemySpawns) positions.push_back(s.second);
        std::shuffle(positions.begin(), positions.end(), rng);

        // 비율 인터리브 (Bresenham 스타일): v_assigned/i ≈ v_count/total 유지
        spawnConfig.m_vEnemySpawns.clear();
        size_t total = variants.size() + bases.size();
        size_t vAssigned = 0;
        for (size_t i = 0; i < total; ++i)
        {
            bool takeVariant = (vAssigned < variants.size())
                            && (vAssigned * total < (i + 1) * variants.size());
            auto& src = takeVariant ? variants[vAssigned] : bases[i - vAssigned];
            spawnConfig.m_vEnemySpawns.push_back({ src.first, positions[i] });
            if (takeVariant) ++vAssigned;
        }
    }

    // 웨이브당 최대 kPerWave 마리로 자동 분할 — 페이즈 체감 + 포탈/낙하 연출 활성화.
    constexpr size_t kPerWave = 10;
    if (spawnConfig.m_vEnemySpawns.size() > kPerWave && !spawnConfig.HasMultiWave())
    {
        const auto& src = spawnConfig.m_vEnemySpawns;
        for (size_t i = 0; i < src.size(); i += kPerWave)
        {
            EnemyWave w;
            w.trigger = (i == 0) ? EnemyWave::TriggerType::Immediate
                                 : EnemyWave::TriggerType::AfterPrevCleared;
            size_t end = (std::min)(i + kPerWave, src.size());
            for (size_t k = i; k < end; ++k) w.spawns.push_back(src[k]);
            spawnConfig.m_vWaves.push_back(std::move(w));
        }
        // m_vEnemySpawns 는 그대로 두지만 HasMultiWave()==true 라 무시됨
    }

    // Assign spawn config to current room
    CRoom* pRoom = pScene->GetCurrentRoom();
    if (pRoom) {
        pRoom->SetSpawnConfig(spawnConfig);
        pRoom->SetEnemySpawner(pScene->GetEnemySpawner());
        pRoom->SetPlayerTarget(pScene->GetPlayer());
        pRoom->SetScene(pScene);
    }

    char buf[128];
    sprintf_s(buf, "[MapLoader] Loaded: %zu rooms, %zu mapObjects, %zu obstacles, %zu enemySpawns\n",
        root["rooms"].size(), mapObjs.size(), obstacles.size(), enemySpawns.size());
    OutputDebugStringA(buf);
    return true;
}

Mesh* MapLoader::LoadMesh(const char* path, ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList)
{
    ObjResult res = LoadObjMesh(pDevice, pCommandList, path);
    return res.valid ? res.pMesh : nullptr;
}
