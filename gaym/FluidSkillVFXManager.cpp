#include "stdafx.h"
#include "FluidSkillVFXManager.h"
#include "EffectRegistry.h"
#include "ScreenSpaceFluid.h"
#include "DescriptorHeap.h"
#include <algorithm>
#include <cmath>

void FluidSkillVFXManager::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList,
                                 CDescriptorHeap* pDescriptorHeap, UINT nStartDescIndex)
{
    m_pDevice    = pDevice;
    m_pDescHeap  = pDescriptorHeap;
    m_nStartDesc = nStartDescIndex;

    // 파이프라인은 최초 1회만 컴파일 (스폰 전 스터터 방지)
    LightEmitterSystem::EnsurePipelines(pDevice);

    for (int i = 0; i < MAX_EFFECTS; ++i)
    {
        // SPH 시스템: 슬롯 [0 .. MAX_EFFECTS-1] 디스크립터 사용
        m_Slots[i].pSystem = std::make_unique<FluidParticleSystem>();
        m_Slots[i].pSystem->Init(pDevice, pCommandList, pDescriptorHeap, nStartDescIndex + i);

        // 경량 이미터: GPU 리소스는 첫 사용 시 지연 초기화 (Init() 호출하지 않음)
        m_Slots[i].pLightEmitter = std::make_unique<LightEmitterSystem>();
    }
    OutputDebugStringA("[FluidSkillVFXManager] Initialized\n");
}

int FluidSkillVFXManager::FindSlotForSpawn(bool isPlayerEffect)
{
    // VFXManager 가 player/enemy 풀을 별도 인스턴스로 운영하므로,
    //   이 매니저 안의 슬롯은 모두 같은 owner (player 또는 enemy 한 종류).
    //   풀 가득 차면 가장 오래된 슬롯 evict — 새 스폰을 우선 보장.

    // 1차: 비활성 슬롯
    for (int i = 0; i < MAX_EFFECTS; ++i)
        if (!m_Slots[i].isActive) return i;

    // 2차: 가장 오래된 active 슬롯 evict (spawnGeneration 작을수록 오래됨).
    //   player 매니저: 가장 오래된 player VFX 슬롯 회수 → 신규 player 스킬 시각 보장
    //   enemy 매니저: 가장 오래된 enemy VFX 슬롯 회수 → 신규 보스 패턴 시각 보장
    (void)isPlayerEffect;  // 현재 분기 안 함 (양 매니저 모두 동일 정책)
    int evictIdx = -1;
    uint32_t oldestGen = UINT32_MAX;
    for (int i = 0; i < MAX_EFFECTS; ++i)
    {
        if (m_Slots[i].isActive && m_Slots[i].spawnGeneration < oldestGen)
        {
            oldestGen = m_Slots[i].spawnGeneration;
            evictIdx = i;
        }
    }
    if (evictIdx >= 0)
    {
        // 슬롯 재사용 시 이전 상태 잔존 방지 — 모든 모드 플래그 초기화
        FluidVFXSlot& s = m_Slots[evictIdx];
        s.isActive          = false;
        s.isFadingOut       = false;
        s.useLightEmitter   = false;
        s.useSequence       = false;
        s.useBlur           = false;
        s.isWaveMode        = false;
        s.waveStopped       = false;
        s.isExplodeMode     = false;
        return evictIdx;
    }
    return -1;
}

int FluidSkillVFXManager::SpawnEffect(const XMFLOAT3& origin, const XMFLOAT3& direction,
                                       const FluidSkillVFXDef& def)
{
    int slotIdx = FindSlotForSpawn(false);  // 적 투사체 trail
    if (slotIdx < 0)
    {
        OutputDebugStringA("[FluidSkillVFXManager] No free slot (and no evictable)!\n");
        return -1;
    }
    {
        int i = slotIdx;
        {
            FluidVFXSlot& slot = m_Slots[i];
            slot.isActive       = true;
            slot.isFadingOut    = false;
            slot.elapsed        = 0.0f;
            slot.origin         = origin;
            slot.prevOrigin     = origin;
            slot.direction      = direction;
            slot.def            = def;
            slot.useSequence    = false; // 기존 모드
            slot.isPlayerEffect    = false; // 적 투사체 — SSF 파이프라인 제외
            slot.useBlur           = false; // 적 투사체는 블러 없음 (이전 슬롯 잔류값 초기화)
            slot.spawnGeneration   = ++m_nextSpawnGeneration;

            FluidParticleConfig cfg;
            cfg.element           = def.element;
            cfg.particleCount     = def.particleCount;
            cfg.spawnRadius       = def.spawnRadius;
            cfg.smoothingRadius   = (def.smoothingRadius > 0.0f) ? def.smoothingRadius : 1.2f;
            cfg.restDensity       = (def.restDensity     > 0.0f) ? def.restDensity     : 7.0f;
            cfg.stiffness         = 50.0f;
            cfg.viscosity         = 0.25f;
            cfg.boundaryStiffness = 150.0f;
            cfg.particleSize      = (def.particleSize    > 0.0f) ? def.particleSize    : 0.35f;

            // 적 투사체: 핵 전용 스폰으로 중앙 공백 방지 (cpGroup=0 → 위성 무시하고 핵만 응답)
            cfg.nucleusFraction   = 0.55f;   // 55% 파티클을 핵에 집중 (더 꽉 뭉치게)
            cfg.nucleusRadius     = 0.5f;    // 중앙 작은 구체 (타이트)

            slot.pSystem->Spawn(origin, cfg);
            PushControlPoints(slot);

            wchar_t buf[128];
            swprintf_s(buf, 128, L"[FluidSkillVFXManager] SpawnEffect slot %d\n", i);
            OutputDebugString(buf);
            return i;
        }
    }
    return -1;  // unreachable — FindSlotForSpawn handled both paths
}

int FluidSkillVFXManager::SpawnSPHLayer(const XMFLOAT3& origin, const XMFLOAT3& direction,
                                         const std::string& effectName,
                                         const EffectLayer& layer, bool isPlayerEffect)
{
    const SPHEmitterParams& sph = layer.sph;

    int slotIdx = FindSlotForSpawn(isPlayerEffect);
    if (slotIdx < 0)
    {
        OutputDebugStringA("[FluidSkillVFXManager] No free slot for SPH layer (and no evictable)!\n");
        return -1;
    }
    {
        int i = slotIdx;
        {
            FluidVFXSlot& slot = m_Slots[i];
            slot.isActive         = true;
            slot.isFadingOut      = false;
            slot.useLightEmitter  = false;  // LightEmitter 슬롯 재사용 시 오염 방지
            slot.elapsed          = 0.0f;
            slot.origin           = origin;
            slot.prevOrigin       = origin;
            slot.direction        = direction;
            slot.useSequence      = true;
            slot.sphLayer         = layer;
            slot.effectName       = effectName;
            slot.isPlayerEffect    = isPlayerEffect;
            slot.useBlur           = isPlayerEffect ? layer.useSSF : false;
            slot.currentPhaseIndex = -1;
            slot.spawnGeneration   = ++m_nextSpawnGeneration;

            // 메테오용 마스터 CP 초기 위치: origin 그대로 사용
            // (MeteorBehavior에서 이미 상공 위치를 origin으로 전달)
            slot.masterCPPos       = origin;
            slot.masterCPFallSpeed = sph.masterCPFallSpeed;

            // FluidParticleConfig 설정
            FluidParticleConfig cfg;
            cfg.element           = layer.element;
            cfg.particleCount     = sph.particleCount;
            cfg.spawnRadius       = sph.spawnRadius;
            cfg.boundaryStiffness = 150.0f;
            // 보스 메가브레스 입자 크기를 7.0으로 최적화 (검은 선 제거 + 시야 확보)
            cfg.particleSize      = (sph.particleSize > 0.f) ? sph.particleSize
                                  : (effectName == "Dragon_MegaBreath") ? 7.0f : 0.35f;
            // sizeScale 적용
            if (layer.sizeScale != 1.f && cfg.particleSize > 0.f)
                cfg.particleSize *= layer.sizeScale;

            if (sph.overridePhysics) {
                cfg.stiffness              = sph.sphStiffness;
                cfg.nearPressureMultiplier = sph.sphNearPressureMult;
                cfg.restDensity            = sph.sphRestDensity;
                cfg.viscosity              = sph.sphViscosity;
                cfg.smoothingRadius        = sph.sphSmoothingRadius;
            } else {
                cfg.smoothingRadius = 1.2f;
                cfg.restDensity     = 7.0f;
                cfg.stiffness       = 50.0f;
                cfg.viscosity       = 0.25f;
            }

            // OrbitalCP 페이즈가 있으면 maxParticleSpeed를 높여 CP 추적 가능하게 함
            bool hasOrbitalCP = false;
            for (const auto& ph : sph.phases)
                if (ph.motionMode == ParticleMotionMode::OrbitalCP) { hasOrbitalCP = true; break; }
            cfg.maxParticleSpeed = sph.maxParticleSpeed > 0.f ? sph.maxParticleSpeed
                                                              : (hasOrbitalCP ? 35.0f : 12.0f);

            // 핵-궤도 색상 오버라이드 및 핵 전용 스폰 설정
            if (layer.overrideColors) {
                cfg.overrideColors  = true;
                cfg.customCoreColor = layer.coreColor;
                cfg.customEdgeColor = layer.edgeColor;
            }
            cfg.nucleusFraction = sph.nucleusSpawnFraction;
            cfg.nucleusRadius   = sph.nucleusSpawnRadius;

            // OrbitalCP: 위성 CP 초기 위치에 파티클 스폰 그룹 추가
            // 파티클이 궤도 위치에서 시작해야 위성 인력권 안에 즉시 진입함
            if (hasOrbitalCP && !sph.satelliteCPs.empty())
            {
                int satCount = (int)sph.satelliteCPs.size();
                // 전체 파티클의 60%를 위성 위치에 배분
                int totalSatParticles = (int)(sph.particleCount * 0.60f);
                int perSat = (std::max)(5, totalSatParticles / satCount);

                for (const auto& sat : sph.satelliteCPs)
                {
                    // t=0 (elapsed=0) 기준 위성 CP 초기 위치 계산
                    float angle = sat.orbitPhase;
                    float bx = cosf(angle) * sat.orbitRadius;
                    float bz = sinf(angle) * sat.orbitRadius;
                    float ct = cosf(sat.orbitTiltX), st = sinf(sat.orbitTiltX);

                    FluidParticleConfig::SpawnGroup g;
                    g.center.x = origin.x + bx;
                    g.center.y = origin.y + sat.verticalOffset - bz * st;
                    g.center.z = origin.z + bz * ct;
                    g.count    = perSat;
                    g.radius   = sat.sphereRadius * 0.8f; // 위성 인력권 내에 스폰
                    cfg.spawnGroups.push_back(g);
                }
            }

            // 사방 집결 스폰: cardinalSpawnRadius > 0이면 ±X/±Y/±Z 6방향에 SpawnGroup 추가
            // 각 그룹의 파티클은 cpGroup=-1(전체 CP 응답) + 내향 초기 속도로 빠르게 수렴
            if (sph.cardinalSpawnRadius > 0.f)
            {
                // 발사 방향 기준 로컬 좌표계 (fwd/right/up)
                XMVECTOR fwdV    = XMVector3Normalize(XMLoadFloat3(&direction));
                XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
                float    dotUp   = XMVectorGetX(XMVector3Dot(fwdV, worldUp));
                XMVECTOR rightV  = (fabsf(dotUp) > 0.99f)
                    ? XMVectorSet(1, 0, 0, 0)
                    : XMVector3Normalize(XMVector3Cross(worldUp, fwdV));
                XMVECTOR upV     = XMVector3Cross(fwdV, rightV);

                // 6방향 오프셋 (로컬 좌표계: ±forward, ±right, ±up)
                XMVECTOR offsets[6] = {
                     fwdV,  XMVectorNegate(fwdV),
                     rightV, XMVectorNegate(rightV),
                     upV,    XMVectorNegate(upV)
                };

                float r = sph.cardinalSpawnRadius;
                int perDir = (std::max)(5, sph.particleCount / 6);
                // 중심 스폰 파티클 수 줄이기 (cardinal이 대부분 담당)
                cfg.spawnRadius = 0.8f;

                for (int di = 0; di < 6; ++di)
                {
                    XMVECTOR posV = XMVectorAdd(
                        XMLoadFloat3(&origin),
                        XMVectorScale(offsets[di], r));

                    FluidParticleConfig::SpawnGroup g;
                    XMStoreFloat3(&g.center, posV);
                    g.count       = perDir;
                    g.radius      = 0.7f;         // 각 방향 그룹 산포 반경
                    g.cpGroup     = -1;            // 전체 CP에 응답
                    g.inwardSpeed = sph.cardinalInwardSpeed;
                    cfg.spawnGroups.push_back(g);
                }
            }

            // 첫 페이즈의 모드 설정
            if (!sph.phases.empty()) {
                slot.pSystem->SetMotionMode(sph.phases[0].motionMode);
            }

            // Wave 모드: 플레이어 앞 spawnRadius 위치에 스폰
            XMFLOAT3 spawnPos = origin;
            if (sph.isWave) {
                XMVECTOR fwdV     = XMVector3Normalize(XMLoadFloat3(&direction));
                XMVECTOR shiftedV = XMVectorAdd(XMLoadFloat3(&origin),
                    XMVectorScale(fwdV, cfg.spawnRadius));
                XMStoreFloat3(&spawnPos, shiftedV);
            }
            slot.pSystem->Spawn(spawnPos, cfg);

            // Wave 모드 초기화
            slot.isWaveMode  = sph.isWave;
            slot.waveDist    = 0.f;
            slot.waveStopped = false;
            if (sph.isWave) {
                slot.useSequence = false;
                slot.origin     = spawnPos;
                slot.prevOrigin = spawnPos;

                // ConfinementBox 설계:
                //   center = origin + halfZBig * fwd  (플레이어 위치 기준, spawnPos 아님)
                //   halfZ  = halfZBig
                //   → 뒤 벽(back wall) = origin (플레이어 발 위치) — 뒤로 못 나감
                //   → 앞 벽(front wall) = origin + 2*halfZBig (160m — 사실상 개방)
                //   X/Y 는 waveHalfW/H 로 좌우/상하 제한
                // SPH 압력이 유일하게 열린 앞 방향으로만 흘러나옴 → ZeroAxisVelocity 불필요
                const float halfZBig = 80.f;
                XMVECTOR wDir    = XMVector3Normalize(XMLoadFloat3(&direction));
                XMVECTOR wWorldUp = XMVectorSet(0, 1, 0, 0);
                float wDotUp = XMVectorGetX(XMVector3Dot(wDir, wWorldUp));
                XMVECTOR wRightV = (fabsf(wDotUp) > 0.99f)
                    ? XMVectorSet(1, 0, 0, 0)
                    : XMVector3Normalize(XMVector3Cross(wWorldUp, wDir));
                XMVECTOR wUpV = XMVector3Cross(wDir, wRightV);

                // center = origin(플레이어 위치) + halfZBig * fwd → back wall이 origin에 위치
                XMFLOAT3 boxCenter;
                XMStoreFloat3(&boxCenter, XMVectorAdd(XMLoadFloat3(&origin),
                    XMVectorScale(wDir, halfZBig)));

                ConfinementBoxDesc wbd;
                wbd.active = true;
                wbd.halfExtents = { sph.waveHalfW, sph.waveHalfH, halfZBig };
                wbd.center = boxCenter;
                XMStoreFloat3(&wbd.axisX, wRightV);
                XMStoreFloat3(&wbd.axisY, wUpV);
                XMStoreFloat3(&wbd.axisZ, wDir);
                slot.pSystem->SetConfinementBox(wbd);
                slot.pSystem->SetGlobalGravity(0.f);
                slot.pSystem->SetMotionMode(ParticleMotionMode::Gravity); // wave는 CP 없음 — Gravity 모드로 SPH+힘만 사용

                // Traveling wave 수직 진동 활성화
                if (sph.waveOscAmplitude > 0.f) {
                    XMFLOAT3 fwdDir3, upDir3;
                    XMStoreFloat3(&fwdDir3, wDir);
                    XMStoreFloat3(&upDir3, wUpV);
                    slot.pSystem->SetWaveOscillation(
                        sph.waveOscAmplitude,
                        sph.waveOscFrequency,
                        sph.waveOscWaveNumber,
                        fwdDir3, upDir3);
                }
            }

            wchar_t buf[128];
            swprintf_s(buf, 128, L"[FluidSkillVFXManager] SpawnSPHLayer slot %d\n", i);
            OutputDebugString(buf);
            return i;
        }
    }
    return -1;  // unreachable — FindSlotForSpawn handled both paths
}

void FluidSkillVFXManager::TrackEffect(int id, const XMFLOAT3& origin, const XMFLOAT3& direction)
{
    if (id < 0 || id >= MAX_EFFECTS || !m_Slots[id].isActive) return;
    m_Slots[id].origin    = origin;
    m_Slots[id].direction = direction;
}

void FluidSkillVFXManager::StopEffect(int id)
{
    if (id < 0 || id >= MAX_EFFECTS) return;
    auto& slot = m_Slots[id];
    slot.isActive    = false;
    slot.useSequence = false;
    if (slot.useLightEmitter)
    {
        slot.useLightEmitter = false;
        slot.pLightEmitter->Clear();
    }
    else
    {
        slot.pSystem->Clear();
    }
}

int FluidSkillVFXManager::SpawnFireTrailEffect(const XMFLOAT3& pos,
                                                const XMFLOAT3& waveRight,
                                                float halfWidth,
                                                float lifetime)
{
    (void)waveRight;

    // 매 호출마다 랜덤 변화 → 같은 패턴 반복 방지
    float r0 = (rand() % 1000) * 0.001f;  // [0, 1)
    float r1 = (rand() % 1000) * 0.001f;
    (void)r1;

    // EffectLayer를 직접 채워 SPH 레이어로 스폰
    EffectLayer layer;
    layer.type      = EmitterType::SPH_Gravity;
    layer.element   = ElementType::Fire;
    layer.overrideColors = true;
    // 짙은 붉은색 불꽃
    layer.coreColor = { 0.75f, 0.08f, 0.01f, 1.0f };
    layer.edgeColor = { 0.35f, 0.02f, 0.0f,  0.85f };

    SPHEmitterParams& s    = layer.sph;
    s.particleCount        = 80 + (rand() % 51);              // 80~130
    s.spawnRadius          = halfWidth * (0.8f + r0 * 0.4f);  // 0.8~1.2 × halfWidth
    s.maxParticleSpeed     = 8.f;

    // SPH: restDensity=0 (인력 없음), 낮은 점성으로 자유롭게 퍼짐
    s.overridePhysics      = true;
    s.sphStiffness         = 10.f;
    s.sphNearPressureMult  = 0.3f;
    s.sphRestDensity       = 0.0f;  // 0이면 항상 반발력만 → 뭉침 방지
    s.sphViscosity         = 0.08f;
    s.sphSmoothingRadius   = 2.0f;

    // Phase 0: 메인 화염 — 중앙 버스트 없이 스폰 구체 그대로 바닥에 퍼짐
    float mainDur = lifetime - 1.0f;
    VFXPhase p0;
    p0.startTime  = 0.f;
    p0.duration   = mainDur;
    p0.motionMode = ParticleMotionMode::Gravity;
    p0.offsetParticlesWithOrigin = false;
    p0.gravityDesc.gravity         = { 0.f, -6.f, 0.f };
    p0.gravityDesc.initialSpeedMin = 0.0f;  // 버스트 없음 — 이미 구체로 퍼져 있음
    p0.gravityDesc.initialSpeedMax = 0.0f;
    p0.phaseMaxSpeed = 5.f;
    p0.boxDesc.active      = true;
    p0.boxDesc.halfExtents = { 30.0f, 30.0f, 2.5f };
    s.phases.push_back(p0);

    // Phase 1: 서서히 소멸 (ExplodeFade)
    VFXPhase p1;
    p1.startTime  = mainDur;
    p1.duration   = 1.0f;
    p1.motionMode = ParticleMotionMode::Gravity;
    p1.offsetParticlesWithOrigin = false;
    p1.gravityDesc.gravity         = { 0.f, -6.f, 0.f };
    p1.gravityDesc.initialSpeedMin = 0.0f;
    p1.gravityDesc.initialSpeedMax = 0.0f;
    p1.phaseMaxSpeed               = 5.f;
    p1.triggerExplodeFadeOnEnter   = true;
    p1.boxDesc.active      = true;
    p1.boxDesc.halfExtents = { 30.0f, 30.0f, 2.5f };
    s.phases.push_back(p1);

    XMFLOAT3 up = { 0.f, 1.f, 0.f };
    return SpawnSPHLayer(pos, up, "Q_FireTrail", layer, /*isPlayerEffect*/true);
}

// ── EffectLayer / EffectDef 기반 스폰 ─────────────────────────────────────────

int FluidSkillVFXManager::SpawnEffectLayer(const XMFLOAT3& origin, const XMFLOAT3& direction,
                                            const std::string& effectName,
                                            const EffectLayer& layer, bool isPlayerEffect)
{
    // SPH 레이어: SpawnSPHLayer로 라우팅 (EffectLayer 직접 사용)
    bool isSPH = (layer.type == EmitterType::SPH_Attract ||
                  layer.type == EmitterType::SPH_Gravity  ||
                  layer.type == EmitterType::SPH_Orbital  ||
                  layer.type == EmitterType::SPH_Beam);
    if (isSPH)
        return SpawnSPHLayer(origin, direction, effectName, layer, isPlayerEffect);

    // 경량 이미터 슬롯 탐색 — Player 우선 eviction 적용
    int slotIdx = FindSlotForSpawn(isPlayerEffect);
    if (slotIdx < 0)
    {
        OutputDebugStringA("[FluidSkillVFXManager] SpawnEffectLayer: no free slot (and no evictable)\n");
        return -1;
    }
    {
        int i = slotIdx;
        FluidVFXSlot& slot      = m_Slots[i];
        slot.isActive           = true;
        slot.isFadingOut        = false;
        slot.isExplodeMode      = false;
        slot.elapsed            = 0.0f;
        slot.origin             = origin;
        slot.prevOrigin         = origin;
        slot.direction          = direction;
        slot.isPlayerEffect     = isPlayerEffect;
        slot.useBlur            = false; // 경량 이미터는 SSF 없음
        slot.useSequence        = false;
        slot.isWaveMode         = false;
        slot.waveStopped        = false;
        slot.useLightEmitter    = true;
        slot.lightLayer         = layer;
        slot.effectName         = effectName;
        slot.spawnGeneration    = ++m_nextSpawnGeneration;

        // 첫 사용 시 지연 초기화
        if (!slot.pLightEmitter->IsInited())
        {
            slot.pLightEmitter->Init(m_pDevice, nullptr, m_pDescHeap,
                                     m_nStartDesc + MAX_EFFECTS + i);
            if (!slot.pLightEmitter->IsInited())
            {
                char msg[128];
                sprintf_s(msg, "[FluidVFXMgr] LightEmitter Init FAILED for slot %d\n", i);
                OutputDebugStringA(msg);
                slot.isActive = false;
                return -1;
            }
        }
        slot.pLightEmitter->Spawn(origin, direction, layer);
        {
            char msg[256];
            sprintf_s(msg,
                "[FluidVFXMgr] SpawnEffectLayer LightEmitter slot=%d name=%s "
                "pos=(%.1f,%.1f,%.1f) dir=(%.1f,%.1f,%.1f) count=%d\n",
                i, effectName.c_str(),
                origin.x, origin.y, origin.z,
                direction.x, direction.y, direction.z,
                layer.particleCount);
            OutputDebugStringA(msg);
        }
        return i;
    }
    return -1;  // unreachable — FindSlotForSpawn handled both paths
}

int FluidSkillVFXManager::SpawnEffectDef(const XMFLOAT3& origin, const XMFLOAT3& direction,
                                          const EffectDef& def, bool isPlayerEffect)
{
    int firstId = -1;
    for (const auto& layer : def.layers)
    {
        // emitDelay 미지원 — 즉시 스폰. element는 EffectDef가 권위를 가짐
        EffectLayer l   = layer;
        l.element       = def.element;

        int id = SpawnEffectLayer(origin, direction, def.name, l, isPlayerEffect);
        if (id >= 0 && firstId < 0) firstId = id;
    }
    return firstId;
}

void FluidSkillVFXManager::ImpactEffect(int id, const XMFLOAT3& impactPos)
{
    if (id < 0 || id >= MAX_EFFECTS || !m_Slots[id].isActive) return;
    auto& slot = m_Slots[id];

    // 단일 제어점을 충돌 위치에 배치 - 파티클이 충돌점으로 수렴
    FluidControlPoint cp;
    cp.position           = impactPos;
    cp.attractionStrength = 35.0f;   // 강한 인력으로 빠르게 집결
    cp.sphereRadius       = 0.25f;   // 매우 좁은 구체 -> 밀집
    slot.pSystem->SetControlPoints({ cp });

    // 시퀀스 모드 해제 (충돌 후에는 기존 fade-out 로직)
    slot.useSequence = false;
    slot.pSystem->SetMotionMode(ParticleMotionMode::ControlPoint);
    ConfinementBoxDesc emptyBox;
    emptyBox.active = false;
    slot.pSystem->SetConfinementBox(emptyBox);

    slot.isFadingOut   = true;
    slot.fadeTimer     = 0.7f;  // 0.7초 후 소멸
    slot.isExplodeMode = false; // Impact는 폭발 축소 없음
}

void FluidSkillVFXManager::ExplodeEffect(int id, const XMFLOAT3& impactPos)
{
    if (id < 0 || id >= MAX_EFFECTS || !m_Slots[id].isActive) return;
    auto& slot = m_Slots[id];

    // CP 완전 제거 — 인력 없이 파티클이 자유롭게 날아가도록
    slot.pSystem->SetControlPoints({});

    // SSF 모드 유지 (isPlayerEffect 변경 없음)
    // RenderDepth에서 smoothingRadius를 fadeMult로 줄여 SSF 구체 자체가 시각적으로 축소됨

    // 폭발 시 속도 상한을 높여 빠른 방사 허용 (수렴용 낮은 maxSpeed 덮어쓰기)
    slot.pSystem->SetMaxParticleSpeed(55.f);

    // Gravity 모드로 전환 (강한 중력으로 빠른 낙하)
    slot.pSystem->SetMotionMode(ParticleMotionMode::Gravity);
    GravityDesc gd;
    gd.gravity        = { 0.f, -20.f, 0.f };  // 강한 하향 중력 (팍 터지고 빠르게 낙하)
    gd.initialSpeedMin = 0.f;  // ApplyRadialBurst로 별도 부여
    gd.initialSpeedMax = 0.f;
    slot.pSystem->SetGravityDesc(gd);
    slot.pSystem->SetGlobalGravity(0.f);

    // 충돌 지점에서 방사형 폭발 속도 부여 (빠르게 팍 퍼지도록 속도 대폭 증가)
    slot.pSystem->ApplyRadialBurst(impactPos, 30.f, 55.f);

    // 시퀀스/박스 모드 해제
    slot.useSequence = false;
    ConfinementBoxDesc emptyBox;
    emptyBox.active = false;
    slot.pSystem->SetConfinementBox(emptyBox);

    // 빠르게 소멸 (1.5s → 0.4s)
    slot.isFadingOut      = true;
    slot.fadeTimer        = 0.4f;
    slot.isExplodeMode    = true;
    slot.explodeTotalTime = 0.4f;
    slot.pSystem->SetExplodeFade(1.0f);  // 즉시 폭발 모드: coreColor 강제 + 크기 축소 준비
}

void FluidSkillVFXManager::Update(float deltaTime)
{
    for (auto& slot : m_Slots)
    {
        if (!slot.isActive) continue;

        // ── 경량 이미터 슬롯: GPU가 물리를 처리하므로 CPU 업데이트 최소화 ──
        if (slot.useLightEmitter)
        {
            if (slot.isFadingOut)
            {
                slot.fadeTimer -= deltaTime;
                if (slot.fadeTimer <= 0.0f)
                {
                    slot.isActive       = false;
                    slot.isFadingOut    = false;
                    slot.useLightEmitter = false;
                    slot.pLightEmitter->Clear();
                }
                continue;
            }
            slot.elapsed += deltaTime;
            // 유한 duration 자동 소멸
            if (slot.lightLayer.duration > 0.f && slot.elapsed >= slot.lightLayer.duration)
            {
                slot.isActive        = false;
                slot.useLightEmitter = false;
                slot.pLightEmitter->Clear();
                continue;
            }
            // 투사체 추적
            if (slot.lightLayer.attachToProjectile)
                slot.pLightEmitter->SetOrigin(slot.origin, slot.direction);
            continue;
        }

        if (slot.isFadingOut) {
            // fade-out 중: SPH 업데이트
            slot.fadeTimer -= deltaTime;

            // 폭발 모드: 매 프레임 fadeRatio → GPU에 전달 (크기 축소 + 선명 고정)
            if (slot.isExplodeMode) {
                float fadeRatio = (slot.explodeTotalTime > 0.001f)
                    ? (slot.fadeTimer / slot.explodeTotalTime)
                    : 0.0f;
                fadeRatio = (std::max)(0.0f, (std::min)(1.0f, fadeRatio));
                slot.pSystem->SetExplodeFade(fadeRatio);
            }

            slot.pSystem->Update(deltaTime);
            if (slot.fadeTimer <= 0.0f) {
                slot.isActive      = false;
                slot.isFadingOut   = false;
                slot.isExplodeMode = false;
                slot.useSequence   = false;
                slot.isWaveMode    = false;
                slot.waveStopped   = false;
                slot.pSystem->SetExplodeFade(2.0f); // 정상 모드 복구
                slot.pSystem->Clear();
            }
            continue;
        }

        // Wave 모드: 일정 폭으로 앞으로 전진
        if (slot.isWaveMode && !slot.waveStopped) {
            float moveDelta = slot.sphLayer.sph.waveSpeed * deltaTime;
            slot.waveDist  += moveDelta;

            if (slot.waveDist >= slot.sphLayer.sph.waveMaxDist) {
                // 최대 거리 도달 → fade-out
                // ConfinementBox 해제: 파티클이 자유롭게 퍼지며 소멸
                slot.waveStopped = true;
                slot.isFadingOut = true;
                slot.fadeTimer   = 1.5f;
                slot.pSystem->SetGlobalGravity(0.f);
                ConfinementBoxDesc emptyBox;
                emptyBox.active = false;
                slot.pSystem->SetConfinementBox(emptyBox);
            } else {
                // Wave 진행 중: SPH + 앞방향 힘으로 파티클이 앞으로 흘러나감
                // ConfinementBox의 뒤 벽이 후방 탈출을 막아주므로
                // OffsetParticles / ZeroAxisVelocity 불필요
                XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&slot.direction));
                XMFLOAT3 fwdDir3;
                XMStoreFloat3(&fwdDir3, dir);
                slot.pSystem->ApplyDirectionalForce(fwdDir3,
                    slot.sphLayer.sph.wavePushForce * deltaTime);
            }

            slot.pSystem->Update(deltaTime);
            continue;
        }

        // 시퀀스 모드: 페이즈 전환 + 박스 lerp 로직
        if (slot.useSequence) {
            slot.elapsed += deltaTime;

            // 시퀀스 자동 종료: 모든 페이즈의 (startTime + duration) 중 최대값을 넘으면 종료
            if (!slot.sphLayer.sph.phases.empty()) {
                float maxEnd = 0.f;
                for (const auto& phase : slot.sphLayer.sph.phases) {
                    float phaseEnd = phase.startTime + phase.duration;
                    if (phaseEnd > maxEnd) maxEnd = phaseEnd;
                }
                if (slot.elapsed >= maxEnd) {
                    slot.isActive    = false;
                    slot.useSequence = false;
                    slot.pSystem->Clear();
                    continue;
                }
            }

            // ControlPoint 모드: origin 변화분만큼 파티클 공동이동
            // (Beam/OrbitalCP는 자체 위치 관리, WaveSlash/Meteor는 origin 고정 → 무해)
            {
                XMFLOAT3 delta = {
                    slot.origin.x - slot.prevOrigin.x,
                    slot.origin.y - slot.prevOrigin.y,
                    slot.origin.z - slot.prevOrigin.z
                };
                slot.prevOrigin = slot.origin;
                bool moved = fabsf(delta.x) + fabsf(delta.y) + fabsf(delta.z) > 0.0001f;
                if (moved && slot.currentPhaseIndex >= 0) {
                    const auto& curP = slot.sphLayer.sph.phases[slot.currentPhaseIndex];
                    if (curP.offsetParticlesWithOrigin &&
                        (curP.motionMode == ParticleMotionMode::ControlPoint || curP.motionMode == ParticleMotionMode::OrbitalCP))
                        slot.pSystem->OffsetParticles(delta);
                }
            }

            UpdatePhase(slot, deltaTime);
            slot.pSystem->Update(deltaTime);
            continue;
        }

        // 기존 모드: 투사체 이동 델타만큼 파티클 전체를 같이 이동 (공동이동 프레임)
        XMFLOAT3 delta = {
            slot.origin.x - slot.prevOrigin.x,
            slot.origin.y - slot.prevOrigin.y,
            slot.origin.z - slot.prevOrigin.z
        };
        slot.prevOrigin = slot.origin;
        if (fabsf(delta.x) + fabsf(delta.y) + fabsf(delta.z) > 0.0001f)
            slot.pSystem->OffsetParticles(delta);

        slot.elapsed += deltaTime;
        PushControlPoints(slot);
        slot.pSystem->Update(deltaTime);
    }
}

void FluidSkillVFXManager::DispatchSPH(ID3D12GraphicsCommandList* pCmdList, float deltaTime)
{
    for (auto& slot : m_Slots)
    {
        if (!slot.isActive) continue;
        if (slot.useLightEmitter)
        {
            slot.pLightEmitter->Dispatch(pCmdList, deltaTime);
            continue;
        }
        if (slot.pSystem) slot.pSystem->DispatchSPH(pCmdList, deltaTime);
    }
}

void FluidSkillVFXManager::Render(ID3D12GraphicsCommandList* pCommandList,
                                   const XMFLOAT4X4& viewProj, const XMFLOAT3& camRight, const XMFLOAT3& camUp)
{
    for (auto& slot : m_Slots)
    {
        if (!slot.isActive) continue;
        if (slot.useLightEmitter)
        {
            slot.pLightEmitter->Render(pCommandList, viewProj, camRight, camUp);
            continue;
        }
        if (!slot.pSystem->IsActive()) continue;
        slot.pSystem->Render(pCommandList, viewProj, camRight, camUp);
    }
}

void FluidSkillVFXManager::RenderEnemyEffects(ID3D12GraphicsCommandList* pCommandList,
                                               const XMFLOAT4X4& viewProj,
                                               const XMFLOAT3& camRight,
                                               const XMFLOAT3& camUp)
{
    for (auto& slot : m_Slots)
    {
        if (!slot.isActive) continue;
        if (slot.isPlayerEffect) continue;  // 플레이어 슬롯 건너뜀
        if (slot.useLightEmitter)
        {
            slot.pLightEmitter->Render(pCommandList, viewProj, camRight, camUp);
            continue;
        }
        if (!slot.pSystem->IsActive()) continue;
        slot.pSystem->Render(pCommandList, viewProj, camRight, camUp);
    }
}

void FluidSkillVFXManager::RenderDepth(ID3D12GraphicsCommandList* pCmdList,
                                        const XMFLOAT4X4& viewProjTransposed,
                                        const XMFLOAT4X4& viewTransposed,
                                        const XMFLOAT3& camRight,
                                        const XMFLOAT3& camUp,
                                        float projA, float projB,
                                        ScreenSpaceFluid* pSSF,
                                        bool blurOnly)
{
    for (auto& slot : m_Slots)
    {
        if (!slot.isActive || !slot.pSystem->IsActive()) continue;
        if (slot.useLightEmitter)    continue; // 경량 이미터는 SSF 제외
        if (!slot.isPlayerEffect)    continue; // 적 투사체는 SSF 제외 → RenderEnemyEffects에서 처리
        if (slot.useBlur != blurOnly) continue;
        slot.pSystem->RenderDepth(pCmdList, viewProjTransposed, viewTransposed,
                                   camRight, camUp, projA, projB, pSSF);
    }
}

void FluidSkillVFXManager::RenderThicknessOnly(
    ID3D12GraphicsCommandList* pCmdList,
    ScreenSpaceFluid* pSSF,
    bool blurOnly)
{
    for (auto& slot : m_Slots)
    {
        if (!slot.isActive || !slot.pSystem->IsActive()) continue;
        if (slot.useLightEmitter)    continue; // 경량 이미터는 SSF 제외
        if (!slot.isPlayerEffect)    continue;
        if (slot.useBlur != blurOnly) continue;
        slot.pSystem->RenderThicknessOnly(pCmdList, pSSF);
    }
}

bool FluidSkillVFXManager::HasActiveSlots(bool blurOnly) const
{
    for (const auto& slot : m_Slots)
    {
        if (!slot.isActive) continue;
        if (slot.useLightEmitter)  continue; // 경량 이미터는 SSF 없음
        if (!slot.isPlayerEffect)  continue;
        if (slot.useBlur != blurOnly) continue;
        if (slot.pSystem->IsActive()) return true;
    }
    return false;
}

void FluidSkillVFXManager::PushControlPoints(FluidVFXSlot& slot) const
{
    if (slot.def.cpDescs.empty()) return;

    XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&slot.direction));
    XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    float dot = XMVectorGetX(XMVector3Dot(fwd, worldUp));
    XMVECTOR right = (fabsf(dot) > 0.99f)
        ? XMVectorSet(1, 0, 0, 0)
        : XMVector3Normalize(XMVector3Cross(worldUp, fwd));
    XMVECTOR up = XMVector3Cross(fwd, right);

    XMVECTOR originV = XMLoadFloat3(&slot.origin);

    std::vector<FluidControlPoint> cps;
    for (const auto& cpd : slot.def.cpDescs)
    {
        float angle = slot.elapsed * cpd.orbitSpeed + cpd.orbitPhase;
        float c = cosf(angle), s = sinf(angle);

        // orbitTilt: up 성분을 fwd 방향으로 기울임
        // tilt=0 → right/up 면, tilt=π/2 → right/fwd 면
        float ct = cosf(cpd.orbitTilt), st = sinf(cpd.orbitTilt);
        XMVECTOR tiltedAxis = up * ct + fwd * st;
        XMVECTOR worldPos = originV
            + right      * (c * cpd.orbitRadius)
            + tiltedAxis * (s * cpd.orbitRadius)
            + fwd        * cpd.forwardBias;

        FluidControlPoint cp;
        XMStoreFloat3(&cp.position, worldPos);
        cp.attractionStrength = cpd.attractionStrength;
        cp.sphereRadius       = cpd.sphereRadius;
        cps.push_back(cp);
    }
    slot.pSystem->SetControlPoints(cps);
}

// ============================================================================
// 시퀀스 기반 페이즈 전환 로직
// ============================================================================
void FluidSkillVFXManager::UpdatePhase(FluidVFXSlot& slot, float dt)
{
    if (slot.sphLayer.sph.phases.empty()) return;

    // 현재 페이즈 찾기
    int targetPhase = 0;
    for (int i = 0; i < static_cast<int>(slot.sphLayer.sph.phases.size()); i++) {
        if (slot.elapsed >= slot.sphLayer.sph.phases[i].startTime) {
            targetPhase = i;
        }
    }

    // 페이즈 전환 발생
    if (targetPhase != slot.currentPhaseIndex) {
        slot.currentPhaseIndex = targetPhase;
        const auto& phase = slot.sphLayer.sph.phases[targetPhase];

        // 모드 전환
        slot.pSystem->SetMotionMode(phase.motionMode);
        slot.pSystem->SetConfinementBox(phase.boxDesc);

        if (phase.motionMode == ParticleMotionMode::Beam) {
            // 빔 길이 우선순위: phase.beamDesc.beamLength > swirlFadeEnd(레거시) > 기본값
            bool isBossBreath = (slot.effectName == "Dragon_MegaBreath");
            float beamLen = (phase.beamDesc.beamLength > 0.f)
                          ? phase.beamDesc.beamLength
                          : ((phase.beamDesc.swirlFadeEnd > 0.f)
                                ? phase.beamDesc.swirlFadeEnd
                                : (isBossBreath ? 120.f : 20.f));

            BeamDesc bd = phase.beamDesc;
            bd.startPos      = slot.origin;
            bd.enableFlow    = isBossBreath || phase.beamDesc.enableFlow;
            bd.verticalScale = phase.beamDesc.verticalScale;

            XMVECTOR endV = XMVectorAdd(
                XMLoadFloat3(&slot.origin),
                XMVectorScale(XMLoadFloat3(&slot.direction), beamLen)
            );
            XMStoreFloat3(&bd.endPos, endV);
            // prevDir을 현재 방향으로 초기화 (첫 프레임 점프 방지)
            XMStoreFloat3(&bd.prevDir, XMVector3Normalize(
                XMVectorSubtract(endV, XMLoadFloat3(&slot.origin))));
            slot.pSystem->SetBeamDesc(bd);
            slot.pSystem->InitBeamParticles();
        }

        if (phase.motionMode == ParticleMotionMode::Gravity) {
            // 폭발 페이즈: 속도 상한 오버라이드 (방사 속도보다 낮으면 클램프됨)
            if (phase.phaseMaxSpeed > 0.f)
                slot.pSystem->SetMaxParticleSpeed(phase.phaseMaxSpeed);

            slot.pSystem->SetGravityDesc(phase.gravityDesc);
            if (phase.gravityDesc.initialSpeedMax > 0.f) {
                XMFLOAT3 burstOrigin;
                if (slot.masterCPFallSpeed > 0.f) {
                    // 낙하 메테오: 폭발 중심을 타겟 지면 바로 아래(-1m)로 고정
                    // origin.y = targetPos.y + METEOR_SPAWN_HEIGHT(50), groundY = origin.y - 50
                    float groundY = slot.origin.y - 50.0f;
                    burstOrigin = { slot.masterCPPos.x, groundY - 1.0f, slot.masterCPPos.z };
                } else {
                    burstOrigin = slot.origin;
                }
                slot.pSystem->ApplyRadialBurst(burstOrigin,
                    phase.gravityDesc.initialSpeedMin,
                    phase.gravityDesc.initialSpeedMax);
            }

            // 폭발 페이드 트리거: 파티클이 즉시 작아지며 사라짐
            // SSF 모드 유지 — RenderDepth에서 smoothingRadius*fadeMult로 SSF 구체 자체를 축소
            if (phase.triggerExplodeFadeOnEnter) {
                slot.pSystem->SetExplodeFade(1.0f);
                slot.isFadingOut      = true;
                slot.isExplodeMode    = true;
                slot.fadeTimer        = phase.duration;
                slot.explodeTotalTime = phase.duration;
                slot.useSequence      = false; // isFadingOut 블록이 이후를 처리
            }
        }

        // ControlPoint 모드: 페이즈 cpDescs가 있으면 CP 설정
        if (phase.motionMode == ParticleMotionMode::ControlPoint && !phase.cpDescs.empty()) {
            // cpDescs -> 월드 공간 FluidControlPoint 변환
            XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&slot.direction));
            XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
            float dotV = XMVectorGetX(XMVector3Dot(fwd, worldUp));
            XMVECTOR rightV = (fabsf(dotV) > 0.99f)
                ? XMVectorSet(1, 0, 0, 0)
                : XMVector3Normalize(XMVector3Cross(worldUp, fwd));
            XMVECTOR upV = XMVector3Cross(fwd, rightV);
            XMVECTOR originV = XMLoadFloat3(&slot.origin);

            std::vector<FluidControlPoint> cps;
            for (const auto& cpd : phase.cpDescs) {
                FluidControlPoint cp;
                XMVECTOR worldPos = originV
                    + rightV * cpd.orbitRadius
                    + fwd * cpd.forwardBias;
                XMStoreFloat3(&cp.position, worldPos);
                cp.attractionStrength = cpd.attractionStrength;
                cp.sphereRadius       = cpd.sphereRadius;
                cps.push_back(cp);
            }
            slot.pSystem->SetControlPoints(cps);
        } else {
            // ControlPoint(cpDescs 없음) 또는 Gravity/Beam 등 비-CP 모드:
            // 이전 페이즈에서 남은 CP 잔류를 방지해 명시적으로 초기화
            slot.pSystem->SetControlPoints({});
        }

        // ConfinementBox의 center를 현재 origin 기준으로 설정
        if (phase.boxDesc.active) {
            ConfinementBoxDesc bd = phase.boxDesc;
            XMVECTOR centerV = XMVectorAdd(
                XMLoadFloat3(&slot.origin),
                XMVectorScale(XMLoadFloat3(&slot.direction), 2.5f)
            );
            XMStoreFloat3(&bd.center, centerV);

            // 방향 기준으로 박스 축 설정
            XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&slot.direction));
            XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
            float dotUp = XMVectorGetX(XMVector3Dot(fwd, worldUp));
            XMVECTOR rightV = (fabsf(dotUp) > 0.99f)
                ? XMVectorSet(1, 0, 0, 0)
                : XMVector3Normalize(XMVector3Cross(worldUp, fwd));
            XMVECTOR upV = XMVector3Cross(fwd, rightV);

            XMStoreFloat3(&bd.axisX, rightV);
            XMStoreFloat3(&bd.axisY, upV);
            XMStoreFloat3(&bd.axisZ, fwd);

            slot.pSystem->SetConfinementBox(bd);
        }

        // Phase 진입 시 forward 방향 속도 제거
        if (phase.cancelForwardVelocityOnEnter)
        {
            XMFLOAT3 fwd;
            XMStoreFloat3(&fwd, XMVector3Normalize(XMLoadFloat3(&slot.direction)));
            slot.pSystem->ZeroAxisVelocity(fwd);
        }

        // Phase 진입 시 랜덤 좌우 + 상하 속도 부여 (3D 확산)
        // SebLague 방식: 파티클이 모든 방향으로 고르게 퍼져야 어떤 각도에서도 두껍게 보임
        if (phase.randomSidewaysImpulse > 0.f)
        {
            XMVECTOR fwdV    = XMVector3Normalize(XMLoadFloat3(&slot.direction));
            XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
            float    dotV    = XMVectorGetX(XMVector3Dot(fwdV, worldUp));
            XMVECTOR rightV  = (fabsf(dotV) > 0.99f)
                ? XMVectorSet(1, 0, 0, 0)
                : XMVector3Normalize(XMVector3Cross(worldUp, fwdV));
            XMVECTOR upV     = XMVector3Cross(fwdV, rightV);

            // 좌우 랜덤 산란 (기존)
            XMFLOAT3 rightDir;
            XMStoreFloat3(&rightDir, rightV);
            slot.pSystem->ApplyRandomSidewaysImpulse(rightDir, phase.randomSidewaysImpulse);

            // 상하 양방향 산란 추가: 위 파티클은 위로, 아래 파티클은 아래로 밀려
            // GPU opFlags 0x04(ApplyAxisSpreadForce)를 사용 — 좌우(0x02)와 동시 적용 가능
            XMFLOAT3 upDir;
            XMStoreFloat3(&upDir, upV);
            slot.pSystem->ApplyAxisSpreadForce(upDir, slot.origin,
                phase.randomSidewaysImpulse * 0.45f);
        }

        // 전역 중력 설정
        slot.pSystem->SetGlobalGravity(phase.globalGravityStrength);
    }

    // ─── 매 프레임 업데이트 ───

    const auto& curPhase = slot.sphLayer.sph.phases[slot.currentPhaseIndex];

    // OrbitalCP 매 프레임 위성 CP 갱신
    if (curPhase.motionMode == ParticleMotionMode::OrbitalCP) {
        UpdateOrbitalCPs(slot, dt);
    }

    // ControlPoint/Beam + sphLayer.sph.cpDescs: 매 프레임 궤도 CP 갱신
    if (!slot.sphLayer.sph.cpDescs.empty() &&
        (curPhase.motionMode == ParticleMotionMode::ControlPoint ||
         curPhase.motionMode == ParticleMotionMode::Beam))
    {
        XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&slot.direction));
        XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        float dot = XMVectorGetX(XMVector3Dot(fwd, worldUp));
        XMVECTOR right = (fabsf(dot) > 0.99f)
            ? XMVectorSet(1, 0, 0, 0)
            : XMVector3Normalize(XMVector3Cross(worldUp, fwd));
        XMVECTOR up = XMVector3Cross(fwd, right);
        XMVECTOR originV = XMLoadFloat3(&slot.origin);

        std::vector<FluidControlPoint> cps;
        for (const auto& cpd : slot.sphLayer.sph.cpDescs)
        {
            float angle = slot.elapsed * cpd.orbitSpeed + cpd.orbitPhase;
            float c = cosf(angle), s = sinf(angle);
            float ct = cosf(cpd.orbitTilt), st_val = sinf(cpd.orbitTilt);
            XMVECTOR tiltedAxis = up * ct + fwd * st_val;
            XMVECTOR worldPos = originV
                + right      * (c * cpd.orbitRadius)
                + tiltedAxis * (s * cpd.orbitRadius)
                + fwd        * cpd.forwardBias;
            FluidControlPoint cp;
            XMStoreFloat3(&cp.position, worldPos);
            cp.attractionStrength = cpd.attractionStrength;
            cp.sphereRadius       = cpd.sphereRadius;
            cps.push_back(cp);
        }
        slot.pSystem->SetControlPoints(cps);
    }

    // Beam 모드: 매 프레임 startPos/endPos 갱신 (플레이어 방향 추적)
    // prevDir 보존하면서 startPos/endPos만 업데이트
    if (curPhase.motionMode == ParticleMotionMode::Beam) {
        bool isBossBreath = (slot.effectName == "Dragon_MegaBreath");
        float beamLen = (curPhase.beamDesc.beamLength > 0.f)
                      ? curPhase.beamDesc.beamLength
                      : ((curPhase.beamDesc.swirlFadeEnd > 0.f)
                            ? curPhase.beamDesc.swirlFadeEnd
                            : (isBossBreath ? 120.f : 20.f));

        BeamDesc bd = slot.pSystem->GetBeamDesc();  // 현재 빔 상태 (prevDir 포함)
        bd.speedMin     = curPhase.beamDesc.speedMin;
        bd.speedMax     = curPhase.beamDesc.speedMax;
        bd.spreadRadius = curPhase.beamDesc.spreadRadius;
        bd.enableFlow    = isBossBreath || curPhase.beamDesc.enableFlow;
        bd.verticalScale = curPhase.beamDesc.verticalScale;
        bd.startPos     = slot.origin;

        XMVECTOR endV = XMVectorAdd(
            XMLoadFloat3(&slot.origin),
            XMVectorScale(XMLoadFloat3(&slot.direction), beamLen)
        );
        XMStoreFloat3(&bd.endPos, endV);
        slot.pSystem->SetBeamDesc(bd);
    }

    // Phase 내 진행률 계산 (박스 halfExtents lerp용)
    float phaseProgress = (slot.elapsed - curPhase.startTime) / curPhase.duration;
    phaseProgress = std::clamp(phaseProgress, 0.f, 1.f);

    // 박스 halfExtents lerp (이전 페이즈와 현재 페이즈 간)
    if (curPhase.boxDesc.active) {
        XMFLOAT3 prev = (slot.currentPhaseIndex > 0)
            ? slot.sphLayer.sph.phases[slot.currentPhaseIndex - 1].boxDesc.halfExtents
            : XMFLOAT3{ 0.5f, 0.5f, 0.5f };
        XMFLOAT3 curr = curPhase.boxDesc.halfExtents;

        ConfinementBoxDesc bd = curPhase.boxDesc;
        bd.halfExtents.x = prev.x + (curr.x - prev.x) * phaseProgress;
        bd.halfExtents.y = prev.y + (curr.y - prev.y) * phaseProgress;
        bd.halfExtents.z = prev.z + (curr.z - prev.z) * phaseProgress;

        // center: origin + direction * 2.5f (phase 진입 시와 동일한 고정 공식)
        // halfExtents.z 를 쓰면 웅덩이처럼 박스가 얇은 이펙트에서 center가 잘못 계산됨
        XMVECTOR centerV = XMVectorAdd(
            XMLoadFloat3(&slot.origin),
            XMVectorScale(XMLoadFloat3(&slot.direction), 2.5f)
        );
        XMStoreFloat3(&bd.center, centerV);

        // 방향 기준으로 박스 축 설정
        XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&slot.direction));
        XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        float dotUp = XMVectorGetX(XMVector3Dot(fwd, worldUp));
        XMVECTOR rightV = (fabsf(dotUp) > 0.99f)
            ? XMVectorSet(1, 0, 0, 0)
            : XMVector3Normalize(XMVector3Cross(worldUp, fwd));
        XMVECTOR upV = XMVector3Cross(fwd, rightV);

        XMStoreFloat3(&bd.axisX, rightV);
        XMStoreFloat3(&bd.axisY, upV);
        XMStoreFloat3(&bd.axisZ, fwd);

        slot.pSystem->SetConfinementBox(bd);
    }

    // expansion force: 박스 확장 방향으로 파티클 밀어줌
    if (curPhase.expansionForceStrength > 0.f && curPhase.boxDesc.active) {
        // 로컬 방향 -> 월드 방향 변환
        XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&slot.direction));
        XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        float dotV = XMVectorGetX(XMVector3Dot(fwd, worldUp));
        XMVECTOR rightV = (fabsf(dotV) > 0.99f)
            ? XMVectorSet(1, 0, 0, 0)
            : XMVector3Normalize(XMVector3Cross(worldUp, fwd));

        // 로컬 (x, y, z) -> 월드 벡터
        XMVECTOR localForce = XMLoadFloat3(&curPhase.expansionForce);
        XMVECTOR worldForce = XMVectorGetX(localForce) * rightV
                            + XMVectorGetY(localForce) * XMVectorSet(0,1,0,0)
                            + XMVectorGetZ(localForce) * fwd;
        worldForce = XMVector3Normalize(worldForce);

        XMFLOAT3 forceDir;
        XMStoreFloat3(&forceDir, worldForce);

        if (curPhase.useAxisSpreadForce)
        {
            // 양방향 분산: 박스 center 기준으로 좌우로 밀기
            XMFLOAT3 boxCenter;
            XMStoreFloat3(&boxCenter, XMVectorAdd(
                XMLoadFloat3(&slot.origin),
                XMVectorScale(XMLoadFloat3(&slot.direction),
                              curPhase.boxDesc.halfExtents.z)
            ));
            slot.pSystem->ApplyAxisSpreadForce(forceDir, boxCenter,
                                                curPhase.expansionForceStrength * dt);
        }
        else
        {
            slot.pSystem->ApplyDirectionalForce(forceDir, curPhase.expansionForceStrength * dt);
        }
    }
}

void FluidSkillVFXManager::UpdateOrbitalCPs(FluidVFXSlot& slot, float dt)
{
    // 마스터 CP 위치 갱신
    // fallSpeed == 0: 투사체 추적 (fireball 등), > 0: 낙하 (Meteor)
    if (slot.masterCPFallSpeed > 0.f)
        slot.masterCPPos.y -= slot.masterCPFallSpeed * dt;
    else
        slot.masterCPPos = slot.origin;

    const SPHEmitterParams& seqDef = slot.sphLayer.sph;

    std::vector<FluidControlPoint> cps;
    // 마스터 CP 추가
    FluidControlPoint masterCP;
    masterCP.position           = slot.masterCPPos;
    masterCP.attractionStrength = seqDef.masterCPStrength;
    masterCP.sphereRadius       = seqDef.masterCPSphereRadius;
    cps.push_back(masterCP);

    // 위성 CP들 공전 + 세차운동 (궤도면 자체가 Y축 주위로 회전)
    for (const auto& sat : slot.sphLayer.sph.satelliteCPs) {
        float theta = sat.orbitPhase + slot.elapsed * sat.orbitSpeed;   // 궤도 내 각도
        float omega = slot.elapsed * sat.precessionSpeed;                // 궤도면 세차 각도
        float phi   = sat.orbitTiltX;                                    // 궤도면 기울기

        // 호흡: 삼각파로 수축/팽창 속도 동일 (사인파는 고점/저점 근처에서 느려짐)
        // triangle(t): t=[0,π] → +1→-1 선형, t=[π,2π] → -1→+1 선형
        float tPhase = fmodf(slot.elapsed * sat.breatheSpeed + sat.breathePhase, 2.f * 3.14159265f);
        float triWave = (tPhase < 3.14159265f)
            ? (1.f - 2.f * tPhase / 3.14159265f)        // +1 → -1
            : (-1.f + 2.f * (tPhase - 3.14159265f) / 3.14159265f); // -1 → +1
        float breathe = 1.f + sat.breatheAmplitude * triWave;
        float R = sat.orbitRadius * breathe;

        // 1) 기울어진 궤도면 내 위치 (X축 기준 tilt 적용)
        float lx = R * cosf(theta);                   // 궤도 X 성분
        float ly = R * sinf(theta) * sinf(phi);       // 기울기로 생긴 Y 성분
        float lz = R * sinf(theta) * cosf(phi);       // 궤도 Z 성분

        // 2) Y축 기준 세차운동 (궤도면 자체 회전)
        float cosW = cosf(omega), sinW = sinf(omega);
        float wx = lx * cosW + lz * sinW;
        float wy = ly;
        float wz = -lx * sinW + lz * cosW;

        FluidControlPoint satCP;
        satCP.position.x         = slot.masterCPPos.x + wx;
        satCP.position.y         = slot.masterCPPos.y + sat.verticalOffset + wy;
        satCP.position.z         = slot.masterCPPos.z + wz;
        satCP.attractionStrength = sat.attractionStrength;
        satCP.sphereRadius       = sat.sphereRadius;
        cps.push_back(satCP);
    }

    slot.pSystem->SetControlPoints(cps);
}

FluidSkillVFXDef FluidSkillVFXManager::GetVFXDef(ElementType element, const RuneCombo& combo, float chargeRatio)
{
    FluidSkillVFXDef def;
    def.element = element;
    def.particleCount = 256;
    def.spawnRadius   = 0.8f;

    switch (element)
    {
    case ElementType::Fire:
    {
        // 원자 모형: 핵(nucleus) + 3개 기울어진 궤도 링 (전자 각 2개)
        // 투사체가 nucleus, 전자들이 서로 다른 평면에서 공전
        def.particleCount = 120;
        def.spawnRadius   = 0.3f;

        // 핵: 투사체 중심에 고정 (orbitRadius=0)
        {
            FluidCPDesc nucleus;
            nucleus.orbitRadius        = 0.0f;
            nucleus.orbitSpeed         = 0.0f;
            nucleus.orbitPhase         = 0.0f;
            nucleus.forwardBias        = 0.0f;
            nucleus.orbitTilt          = 0.0f;
            nucleus.attractionStrength = 28.0f;
            nucleus.sphereRadius       = 0.4f;
            def.cpDescs.push_back(nucleus);
        }

        // 3개 궤도 링: tilt 0°(right/up면), 60°(앞으로 기울어짐), -60°(뒤로 기울어짐)
        constexpr float R = 1.8f;
        const float tilts[3]  = { 0.f, XM_PI / 3.f, -XM_PI / 3.f };
        const float speeds[3] = { 9.f, 6.f, 11.f };

        for (int ring = 0; ring < 3; ++ring)
        {
            for (int e = 0; e < 2; ++e)  // 링당 전자 2개: 180° 간격
            {
                FluidCPDesc cp;
                cp.orbitRadius        = R;
                cp.orbitSpeed         = speeds[ring];
                cp.orbitPhase         = e * XM_PI + ring * (XM_2PI / 3.f);
                cp.forwardBias        = 0.0f;
                cp.orbitTilt          = tilts[ring];
                cp.attractionStrength = 45.0f;
                cp.sphereRadius       = 0.45f;
                def.cpDescs.push_back(cp);
            }
        }
        break;
    }
    case ElementType::Water:
    {
        // 핵(nucleus) + 3개 비대칭 궤도 CP — 회전하며 찌그러지는 물 덩어리
        def.particleCount = 200;
        def.spawnRadius   = 1.0f;

        // 핵: 중심에 고정, 밀집된 코어 형성
        {
            FluidCPDesc nuc;
            nuc.orbitRadius        = 0.0f;
            nuc.orbitSpeed         = 0.0f;
            nuc.orbitPhase         = 0.0f;
            nuc.forwardBias        = 0.0f;
            nuc.orbitTilt          = 0.0f;
            nuc.attractionStrength = 28.0f;
            nuc.sphereRadius       = 1.1f;
            def.cpDescs.push_back(nuc);
        }

        // 앞쪽 돌출 CP: forwardBias로 투사체 진행 방향 elongation
        {
            FluidCPDesc cp;
            cp.orbitRadius        = 1.1f;
            cp.orbitSpeed         = 7.0f;
            cp.orbitPhase         = 0.0f;
            cp.forwardBias        = 1.8f;
            cp.orbitTilt          = XM_PI / 4.0f;   // 45° 앞으로 기울어진 궤도
            cp.attractionStrength = 22.0f;
            cp.sphereRadius       = 1.3f;
            def.cpDescs.push_back(cp);
        }

        // 넓은 궤도 CP: 빠른 회전으로 옆면 파동 생성
        {
            FluidCPDesc cp;
            cp.orbitRadius        = 1.7f;
            cp.orbitSpeed         = 12.0f;
            cp.orbitPhase         = XM_2PI / 3.0f;
            cp.forwardBias        = 0.4f;
            cp.orbitTilt          = -XM_PI / 5.0f;  // 뒤로 기울어진 궤도
            cp.attractionStrength = 18.0f;
            cp.sphereRadius       = 1.5f;
            def.cpDescs.push_back(cp);
        }

        // 뒤쪽 느린 CP: 꼬리 효과
        {
            FluidCPDesc cp;
            cp.orbitRadius        = 1.3f;
            cp.orbitSpeed         = 5.5f;
            cp.orbitPhase         = XM_2PI * 2.0f / 3.0f;
            cp.forwardBias        = -1.0f;
            cp.orbitTilt          = XM_PI / 3.0f;   // 60° 앞으로 기울어진 궤도
            cp.attractionStrength = 20.0f;
            cp.sphereRadius       = 1.4f;
            def.cpDescs.push_back(cp);
        }
        break;
    }
    case ElementType::Wind:
    {
        // 2개 CP, 빠른 궤도 - 와류형
        FluidCPDesc cp0;
        cp0.orbitRadius = 0.5f; cp0.orbitSpeed = 22.0f;
        cp0.orbitPhase = 0.0f;  cp0.forwardBias = 0.4f;
        cp0.attractionStrength = 18.0f; cp0.sphereRadius = 1.5f;

        FluidCPDesc cp1 = cp0;
        cp1.orbitPhase = XM_PI; cp1.forwardBias = -0.4f;

        def.cpDescs = { cp0, cp1 };
        break;
    }
    case ElementType::Earth:
    {
        // 1개 CP, 궤도 - 밀집 구체
        FluidCPDesc cp0;
        cp0.orbitRadius = 0.3f; cp0.orbitSpeed = 7.0f;
        cp0.orbitPhase = 0.0f;  cp0.forwardBias = 0.2f;
        cp0.attractionStrength = 25.0f; cp0.sphereRadius = 1.6f;

        def.cpDescs = { cp0 };
        def.particleCount = 100;
        def.spawnRadius   = 0.6f;
        break;
    }
    default:
    {
        FluidCPDesc cp0;
        cp0.orbitRadius = 0.6f; cp0.orbitSpeed = 12.0f;
        cp0.orbitPhase = 0.0f;  cp0.forwardBias = 0.0f;
        cp0.attractionStrength = 20.0f; cp0.sphereRadius = 1.8f;
        def.cpDescs = { cp0 };
        break;
    }
    }

    // Shape override: Place > Channel > Charge (mutually exclusive)
    if (combo.hasPlace) {
        // Mine: single CP at center, no orbit -- dense ball
        def.cpDescs.clear();
        FluidCPDesc cp;
        cp.orbitRadius        = 0.0f;
        cp.orbitSpeed         = 0.0f;
        cp.orbitPhase         = 0.0f;
        cp.forwardBias        = 0.0f;
        cp.attractionStrength = 35.0f;
        cp.sphereRadius       = 0.9f;
        def.cpDescs           = { cp };
        def.particleCount     = 120;
        def.spawnRadius       = 1.2f;
    }
    else if (combo.hasChannel) {
        // Beam: 5 CPs along forward axis, no orbit
        def.cpDescs.clear();
        for (int i = 0; i < 5; ++i)
        {
            FluidCPDesc cp;
            cp.orbitRadius        = 0.05f;
            cp.orbitSpeed         = 0.0f;
            cp.orbitPhase         = 0.0f;
            cp.forwardBias        = -1.5f + i * 0.75f;
            cp.attractionStrength = 22.0f;
            cp.sphereRadius       = 0.8f;
            def.cpDescs.push_back(cp);
        }
        def.particleCount = (int)(def.particleCount * 1.2f);
        def.spawnRadius   = 0.5f;
    }
    else if (combo.hasCharge) {
        // Charge: orbit radius grows with charge ratio
        float cr = chargeRatio;
        for (auto& cpd : def.cpDescs) {
            cpd.orbitRadius        *= 0.5f + 1.5f * cr;
            cpd.orbitSpeed         *= 1.0f + 0.6f * cr;
            cpd.attractionStrength *= 1.2f + 0.4f * cr;
            cpd.sphereRadius       *= 0.8f + 0.5f * cr;
        }
        def.particleCount = (int)(def.particleCount * (1.0f + 0.5f * cr));
        def.spawnRadius   *= 0.7f + 0.5f * cr;
    }

    // Enhance: additional scale regardless of shape
    if (combo.hasEnhance) {
        for (auto& cpd : def.cpDescs) {
            cpd.orbitRadius        *= 1.4f;
            cpd.attractionStrength *= 1.2f;
            cpd.sphereRadius       *= 1.35f;
        }
        def.particleCount = (int)(def.particleCount * 1.5f);
    }

    if (def.particleCount > 4096) def.particleCount = 4096;

    return def;
}

// 슬롯의 실제 렌더 색상 반환 헬퍼 (overrideColors 우선)
static FluidElementColor GetSlotColors(const FluidVFXSlot& slot)
{
    if (slot.useSequence && slot.sphLayer.overrideColors)
        return { slot.sphLayer.coreColor, slot.sphLayer.edgeColor };
    ElementType elem = slot.useSequence ? slot.sphLayer.element : slot.def.element;
    return FluidElementColors::Get(elem);
}

// 가장 최근에 스폰된 non-fading 슬롯 우선, 없으면 fading 중 최신 슬롯 반환
static const FluidVFXSlot* PickDominantSlot(
    const std::array<FluidVFXSlot, FluidSkillVFXManager::MAX_EFFECTS>& slots,
    bool playerOnly, bool useBlurFilter, bool blurOnly)
{
    const FluidVFXSlot* best = nullptr;
    bool bestIsAlive = false; // true = non-fading

    for (const auto& slot : slots)
    {
        if (!slot.isActive) continue;
        if (playerOnly && !slot.isPlayerEffect) continue;
        if (useBlurFilter && slot.useBlur != blurOnly) continue;

        bool alive = !slot.isFadingOut && !slot.isExplodeMode;

        // 우선순위: alive > fading, 같은 우선순위면 더 최근(spawnGeneration 큰 것)
        if (!best
            || (alive && !bestIsAlive)
            || (alive == bestIsAlive && slot.spawnGeneration > best->spawnGeneration))
        {
            best = &slot;
            bestIsAlive = alive;
        }
    }
    return best;
}

XMFLOAT4 FluidSkillVFXManager::GetDominantFluidColor() const
{
    const auto* s = PickDominantSlot(m_Slots, true, false, false);
    return s ? GetSlotColors(*s).coreColor : XMFLOAT4{ 0.15f, 0.55f, 1.0f, 0.85f };
}

FluidElementColor FluidSkillVFXManager::GetDominantFluidColors() const
{
    const auto* s = PickDominantSlot(m_Slots, true, false, false);
    return s ? GetSlotColors(*s) : FluidElementColors::Get(ElementType::None);
}

FluidElementColor FluidSkillVFXManager::GetDominantFluidColors(bool blurOnly) const
{
    const auto* s = PickDominantSlot(m_Slots, true, true, blurOnly);
    return s ? GetSlotColors(*s) : FluidElementColors::Get(ElementType::None);
}

void FluidSkillVFXManager::StopWave(int id)
{
    if (id < 0 || id >= MAX_EFFECTS || !m_Slots[id].isActive) return;
    auto& slot = m_Slots[id];
    if (!slot.isWaveMode || slot.waveStopped) return;

    slot.waveStopped = true;
    // ConfinementBox 해제 → 파티클이 자유롭게 흩어지며 소멸
    ConfinementBoxDesc emptyBox;
    emptyBox.active = false;
    slot.pSystem->SetConfinementBox(emptyBox);
    slot.isFadingOut = true;
    slot.fadeTimer   = 0.5f;
}

XMFLOAT3 FluidSkillVFXManager::GetWaveFrontPos(int id) const
{
    if (id < 0 || id >= MAX_EFFECTS || !m_Slots[id].isActive)
        return {};
    const auto& slot = m_Slots[id];
    // 선두 위치 = 스폰 원점 + 진행방향 * 이동거리
    XMVECTOR frontV = XMVectorAdd(
        XMLoadFloat3(&slot.origin),
        XMVectorScale(XMVector3Normalize(XMLoadFloat3(&slot.direction)), slot.waveDist));
    XMFLOAT3 result;
    XMStoreFloat3(&result, frontV);
    return result;
}

float FluidSkillVFXManager::GetWaveDist(int id) const
{
    if (id < 0 || id >= MAX_EFFECTS || !m_Slots[id].isActive) return 0.f;
    return m_Slots[id].waveDist;
}

XMFLOAT3 FluidSkillVFXManager::GetWaveOrigin(int id) const
{
    if (id < 0 || id >= MAX_EFFECTS || !m_Slots[id].isActive) return {};
    return m_Slots[id].origin;
}

XMFLOAT3 FluidSkillVFXManager::GetWaveDir(int id) const
{
    if (id < 0 || id >= MAX_EFFECTS || !m_Slots[id].isActive) return {0,0,1};
    return m_Slots[id].direction;
}

bool FluidSkillVFXManager::IsWaveActive(int id) const
{
    if (id < 0 || id >= MAX_EFFECTS || !m_Slots[id].isActive) return false;
    return m_Slots[id].isWaveMode && !m_Slots[id].waveStopped;
}

bool FluidSkillVFXManager::IsPointInWave(int id, const XMFLOAT3& point) const
{
    if (id < 0 || id >= MAX_EFFECTS || !m_Slots[id].isActive) return false;
    const FluidVFXSlot& slot = m_Slots[id];
    if (!slot.isWaveMode) return false;

    // 파도 로컬 좌표계 (fwd/right/up)
    XMVECTOR fwdV    = XMVector3Normalize(XMLoadFloat3(&slot.direction));
    XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    float    dotUp   = XMVectorGetX(XMVector3Dot(fwdV, worldUp));
    XMVECTOR rightV  = (fabsf(dotUp) > 0.99f)
        ? XMVectorSet(1, 0, 0, 0)
        : XMVector3Normalize(XMVector3Cross(worldUp, fwdV));
    XMVECTOR upV     = XMVector3Cross(fwdV, rightV);

    // 파도 스폰 위치 (back wall) → 점까지 벡터
    XMVECTOR toPoint = XMVectorSubtract(XMLoadFloat3(&point), XMLoadFloat3(&slot.origin));

    float fwdDist   = XMVectorGetX(XMVector3Dot(toPoint, fwdV));
    float rightDist = fabsf(XMVectorGetX(XMVector3Dot(toPoint, rightV)));
    float upDist    = fabsf(XMVectorGetX(XMVector3Dot(toPoint, upV)));

    // 파도 선두(waveDist)와 back wall(0) 사이, 폭/높이 범위 내
    if (fwdDist < 0.f || fwdDist > slot.waveDist) return false;
    if (rightDist > slot.sphLayer.sph.waveHalfW)   return false;
    if (upDist    > slot.sphLayer.sph.waveHalfH * 2.0f) return false; // 높이는 여유 있게

    return true;
}

void FluidSkillVFXManager::RenderPlayerLightEmitters(ID3D12GraphicsCommandList* pCommandList,
                                                      const XMFLOAT4X4& viewProj,
                                                      const XMFLOAT3& camRight, const XMFLOAT3& camUp)
{
    for (auto& slot : m_Slots)
    {
        if (!slot.isActive)       continue;
        if (!slot.isPlayerEffect) continue;
        if (!slot.useLightEmitter) continue;
        slot.pLightEmitter->Render(pCommandList, viewProj, camRight, camUp);
    }
}

// ─── 외부 동기화: 슬롯의 현재 페이즈 인덱스 조회 ─────────────────────────────
int FluidSkillVFXManager::GetSlotPhase(int id) const
{
    if (id < 0 || id >= MAX_EFFECTS) return -1;
    const FluidVFXSlot& slot = m_Slots[id];
    if (!slot.isActive) return -1;
    return slot.currentPhaseIndex;
}
