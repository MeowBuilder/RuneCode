// light_emitter.hlsl
// 경량 파티클 이미터: Linear / Cone / Sphere / Ring / Burst
// SPH 없이 컴퓨트 셰이더 기반 단순 물리 (스폰·업데이트·렌더 추출)

// ── 구조체 ──────────────────────────────────────────────────────────────────

struct LightParticle {
    float3 pos;         // 위치                  [0-11]
    float  life;        // 남은 수명 (<0: 비활성) [12-15]
    float3 vel;         // 속도                  [16-27]
    float  maxLife;     // 최대 수명              [28-31]
    float4 startColor;  // 스폰 색상             [32-47]
    float  size;        // 현재 크기             [48-51]
    float  startSize;   // 스폰 시 크기           [52-55]
    uint   seed;        // 파티클 개별 시드        [56-59]
    float  pad;         //                       [60-63]
}; // 64 bytes

struct FluidParticleRenderData {
    float3 position;
    float  size;
    float4 color;
}; // 32 bytes

// ── 상수 버퍼 (256 bytes) ───────────────────────────────────────────────────

cbuffer LightEmitterCB : register(b0)
{
    // [0-15] 공통
    int    particleCount;
    int    emitterType;   // 0=Linear 1=Cone 2=Sphere 3=Ring 4=Burst
    float  dt;
    float  elapsed;

    // [16-95] 방향 벡터 (5 x float4)
    float3 origin;    float pad0;
    float3 direction; float pad1;
    float3 rightAxis; float pad2;
    float3 upAxis;    float pad3;
    float3 gravity;   float pad4;   // 중력 벡터 (gravityScale * (0,-9.8,0))

    // [96-127] 색상
    float4 coreColor;
    float4 edgeColor;

    // [128-143] 공통 스칼라
    float  sizeBase;
    float  sizeScale;
    uint   frameSeed;
    int    spawnCount;   // 이번 프레임 추가 스폰 수 (emitRate용, 미사용 시 0)

    // [144-159] 공용 이미터 파라미터
    float  speedMin;
    float  speedMax;
    float  lifetimeMin;
    float  lifetimeMax;

    // [160-175] Linear
    float  linearLength;
    float  linearWidth;
    float  linearRecycleRate;
    float  linearSwirlSpeed;

    // [176-191] Cone
    float  coneHalfAngleDeg;
    float  coneGravityScale;
    float  coneStartSizeMult;
    float  coneEndSizeMult;

    // [192-207] Sphere
    float  sphereRadius;
    float  sphereShellFraction;
    int    sphereInward;
    float  sphereRotationSpeed;

    // [208-239] Ring (32 bytes)
    float  ringRadius;
    float  ringWidth;
    float  ringExpandSpeed;
    float  ringTiltX;
    float  ringRotateSpeed;
    float  ringNormalSpeedMin;
    float  ringNormalSpeedMax;
    float  _rpad;

    // [240-255] Burst
    float  burstBounceCoeff;
    float  burstGroundY;
    float  _bpad0;
    float  _bpad1;
};

// ── 버퍼 ────────────────────────────────────────────────────────────────────

RWStructuredBuffer<LightParticle>           gParticles : register(u0);
RWStructuredBuffer<FluidParticleRenderData> gRenderBuf : register(u1);

// ── PRNG ────────────────────────────────────────────────────────────────────

uint WangHash(uint s)
{
    s = (s ^ 61u) ^ (s >> 16u);
    s *= 9u;
    s ^= s >> 4u;
    s *= 0x27d4eb2du;
    s ^= s >> 15u;
    return s;
}

float Rand01(inout uint s)
{
    s = WangHash(s);
    return float(s & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

// ── 원뿔 내 균등 랜덤 방향 ──────────────────────────────────────────────────

float3 RandomInCone(float halfDeg, float3 fwd, float3 right, float3 up, inout uint seed)
{
    float halfRad = halfDeg * 0.01745329f;
    float cosMax  = cos(halfRad);
    float r1 = Rand01(seed);
    float r2 = Rand01(seed);
    float cosA  = 1.0f - r1 * (1.0f - cosMax);
    float sinA  = sqrt(max(0.0f, 1.0f - cosA * cosA));
    float phi   = r2 * 6.28318f;
    return normalize(fwd * cosA + right * sinA * cos(phi) + up * sinA * sin(phi));
}

// ── 구면 균등 랜덤 방향 ─────────────────────────────────────────────────────

float3 RandomOnSphere(inout uint seed)
{
    float r1  = Rand01(seed);
    float r2  = Rand01(seed);
    float phi = acos(clamp(1.0f - 2.0f * r1, -1.0f, 1.0f));
    float th  = r2 * 6.28318f;
    return float3(sin(phi) * cos(th), cos(phi), sin(phi) * sin(th));
}

// ── CS_LightEmit: 파티클 초기화 (Spawn 시 한 번 디스패치) ───────────────────

[numthreads(64, 1, 1)]
void CS_LightEmit(uint3 DTid : SV_DispatchThreadID)
{
    uint idx = DTid.x;
    if ((int)idx >= particleCount) return;

    uint seed = WangHash(frameSeed ^ (idx * 2654435761u));

    float r0 = Rand01(seed);
    float r1 = Rand01(seed);
    float r2 = Rand01(seed);
    float r3 = Rand01(seed);
    float r4 = Rand01(seed);

    float speed    = lerp(speedMin, speedMax, r2);
    float lifetime = lerp(lifetimeMin, lifetimeMax, r3);

    LightParticle p;
    p.seed      = seed;
    p.pad       = 0;
    p.maxLife   = lifetime;
    p.life      = lifetime;
    p.startColor = lerp(edgeColor, coreColor, r0);

    // ── Linear ──────────────────────────────────────────────
    if (emitterType == 0)
    {
        float t   = r0;
        float ang = r1 * 6.28318f;
        float rad = sqrt(r2) * linearWidth;   // sqrt: 면적 균등 분포
        p.pos = origin + direction * t * linearLength
              + rightAxis * rad * cos(ang)
              + upAxis    * rad * sin(ang);
        p.vel         = direction * speed;
        p.life        = 99.f;    // 재활용 방식 — 수명 무한
        p.maxLife     = 99.f;
        p.size        = sizeBase * sizeScale;
        p.startSize   = p.size;
    }
    // ── Cone ────────────────────────────────────────────────
    else if (emitterType == 1)
    {
        float3 d = RandomInCone(coneHalfAngleDeg, direction, rightAxis, upAxis, seed);
        p.pos       = origin;
        p.vel       = d * speed;
        p.size      = sizeBase * sizeScale * coneStartSizeMult;
        p.startSize = p.size;
    }
    // ── Sphere ──────────────────────────────────────────────
    else if (emitterType == 2)
    {
        float3 sdir = RandomOnSphere(seed);
        float  rMin = sphereRadius * (1.0f - sphereShellFraction);
        float  rad  = lerp(rMin, sphereRadius, sqrt(r2));
        p.pos = origin + sdir * rad;
        float3 vdir = (sphereInward != 0) ? -sdir : sdir;
        p.vel       = vdir * speed;
        p.size      = sizeBase * sizeScale;
        p.startSize = p.size;
    }
    // ── Ring ────────────────────────────────────────────────
    else if (emitterType == 3)
    {
        float  ang    = r0 * 6.28318f;
        float  roff   = (r1 - 0.5f) * ringWidth;
        float  cosT   = cos(ringTiltX), sinT = sin(ringTiltX);
        // 링 평면 반지름 방향 (tilt 적용: X축 회전)
        float3 radDir = rightAxis * cos(ang)
                      + (upAxis * cosT - direction * sinT) * sin(ang);
        p.pos = origin + radDir * (ringRadius + roff);
        // 법선 방향 (링 평면에 수직)
        float3 ndir = direction * cosT + upAxis * sinT;
        float  ns   = lerp(ringNormalSpeedMin, ringNormalSpeedMax, r2);
        p.vel       = ndir * ns;
        p.size      = sizeBase * sizeScale;
        p.startSize = p.size;
    }
    // ── Burst ───────────────────────────────────────────────
    else
    {
        float3 bdir = RandomOnSphere(seed);
        p.pos       = origin;
        p.vel       = bdir * speed;
        p.size      = sizeBase * sizeScale;
        p.startSize = p.size;
    }

    gParticles[idx] = p;
}

// ── CS_LightUpdate: 물리 업데이트 (매 프레임) ──────────────────────────────

[numthreads(64, 1, 1)]
void CS_LightUpdate(uint3 DTid : SV_DispatchThreadID)
{
    uint idx = DTid.x;
    if ((int)idx >= particleCount) return;

    LightParticle p = gParticles[idx];
    if (p.life < 0.f) { gParticles[idx] = p; return; }

    // 중력
    p.vel += gravity * dt;

    // ── 이미터별 특수 처리 ──────────────────────────────────
    if (emitterType == 0) // Linear: 범위 이탈 시 재스폰 + swirl 회전
    {
        float proj = dot(p.pos - origin, direction);
        if (proj < 0.f || proj > linearLength)
        {
            uint seed2 = WangHash(p.seed ^ asuint(elapsed));
            float t    = Rand01(seed2);
            float ang  = Rand01(seed2) * 6.28318f;
            float rad  = sqrt(Rand01(seed2)) * linearWidth;
            p.pos = origin + direction * t * linearLength
                  + rightAxis * rad * cos(ang)
                  + upAxis    * rad * sin(ang);
        }

        // Swirl: 라인 축 주위 회전. p.pos 의 line-방사 성분을 swirlSpeed*dt 만큼 회전
        if (linearSwirlSpeed != 0.f)
        {
            float3 rel       = p.pos - origin;
            float  fwdComp   = dot(rel, direction);              // 라인 따라 전진 거리
            float3 radialVec = rel - direction * fwdComp;        // 라인 축 직각 성분
            float  curR      = length(radialVec);
            if (curR > 0.001f)
            {
                // 현재 cos/sin 추출
                float curCos = dot(radialVec, rightAxis) / curR;
                float curSin = dot(radialVec, upAxis)    / curR;
                // 회전각만큼 더하기
                float swirlAng = linearSwirlSpeed * dt;
                float ca = cos(swirlAng);
                float sa = sin(swirlAng);
                float newCos = curCos * ca - curSin * sa;
                float newSin = curSin * ca + curCos * sa;
                // 회전된 radial 로 재구성
                float3 newRadial = (rightAxis * newCos + upAxis * newSin) * curR;
                p.pos = origin + direction * fwdComp + newRadial;
            }
        }
    }
    else if (emitterType == 3 && ringExpandSpeed > 0.f) // Ring: 반경 확장
    {
        float3 toParticle = p.pos - origin;
        // 링 법선 성분 제거 (링 평면 투영)
        float3 ndir = direction * cos(ringTiltX) + upAxis * sin(ringTiltX);
        toParticle -= ndir * dot(toParticle, ndir);
        float  dist = length(toParticle);
        if (dist > 0.001f)
            p.pos += normalize(toParticle) * ringExpandSpeed * dt;
    }
    else if (emitterType == 4 && // Burst: 바닥 반사
             p.pos.y <= burstGroundY && p.vel.y < 0.f)
    {
        p.vel.y *= -burstBounceCoeff;
    }

    p.pos  += p.vel * dt;
    p.life -= dt;

    gParticles[idx] = p;
}

// ── CS_LightToRender: 렌더 데이터 추출 (매 프레임, DispatchSPH 후) ─────────

[numthreads(64, 1, 1)]
void CS_LightToRender(uint3 DTid : SV_DispatchThreadID)
{
    uint idx = DTid.x;
    if ((int)idx >= particleCount) return;

    LightParticle          p  = gParticles[idx];
    FluidParticleRenderData rd;
    rd.position = p.pos;

    if (p.life < 0.f)
    {
        rd.size  = 0.f;
        rd.color = float4(0, 0, 0, 0);
    }
    else
    {
        // t: 수명 진행률 (0=스폰, 1=소멸). Linear는 재활용 방식이라 항상 0
        float t = (p.maxLife > 1.f) ? 0.f : saturate(1.f - p.life / max(p.maxLife, 0.0001f));

        rd.color = lerp(p.startColor, edgeColor, t);

        float szMult = 1.f;

        if (emitterType == 1) // Cone: 알파+크기 페이드
        {
            rd.color.a *= (1.f - t);
            szMult = lerp(coneStartSizeMult, coneEndSizeMult, t);
        }
        else if (emitterType == 4) // Burst: 알파+크기 페이드
        {
            rd.color.a *= (1.f - t);
            szMult = lerp(1.f, 0.2f, t);
        }

        rd.size = p.startSize * szMult;
    }

    gRenderBuf[idx] = rd;
}
