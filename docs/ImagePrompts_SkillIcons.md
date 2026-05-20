# Image Generation Prompts — Skill Icons (v2 Stylized Cartoon)

GPT(또는 DALL·E / gpt-image-1)에 그대로 붙여 넣어 쓰는 프롬프트 모음.
**사용 순서**: STEP 1을 먼저 한 번 보내서 스타일을 고정 → STEP 2의 스킬 프롬프트를 하나씩.

---

## STEP 1 — 마스터 스타일 시트 v2 (가장 먼저 1회 전송)

```
[STYLE BIBLE — gaym v2 — STYLIZED CARTOON ILLUSTRATION]

This is a 2D illustration, NOT a 3D render. Treat every asset as a hand-drawn cartoon icon.

Art direction:
- Stylized cartoon fantasy illustration.
- Bold chunky shapes, exaggerated cartoon proportions, strong readable silhouette.
- 2-to-3-tone cel-shading: one base color, one shadow, one highlight. Avoid soft gradients.
- Optional thin dark outline accent on the main silhouette is welcome.
- Surfaces look painted, NOT photoreal. No PBR. No realistic texture detail.
- Slight imperfections allowed — hand-illustrated feel, not vector-flat.

References (lean hard on these):
- Hearthstone card art icon-crops
- Clash Royale / Brawl Stars chunky cartoon shapes
- Riot Arcane key-art level of stylization (NOT a 3D model)
- Supergiant Hades 2D ability icons
- Don't Starve / Cuphead level of distinct silhouette
DO NOT reference: Diablo realistic textures, Unreal Engine renders, photoreal CGI, ArtStation hyperreal.

Color language (saturated cartoon palette, push vivid):
- Fire   = punchy orange-red, embers yellow-white core
- Water  = bright cyan + ice-blue shadow, near-white foam pops
- Wind   = electric mint/teal with white motion streaks
- Earth  = warm ochre + chunky brown shadow + bright crack glow

Shading rules:
- Light from upper-left, hard-edged shadow shapes (not blurred).
- One crisp highlight shape per form. Keep shadow in 1–2 flat tones, not gradients.
- Glow is a stylized halo shape, not photoreal volumetric light.

Silhouette & readability:
- Must read instantly at 64×64. Big, simple, chunky.
- Avoid fine filigree, avoid photoreal scratches, avoid tiny detail clutter.

Frame (used for every skill icon):
- A chunky cartoon bronze ring with stylized rune notches — drawn, not modeled.
- Inner area = flat dark stone tablet, slight cartoony inner shadow.
- The icon art sits FLATLY inside, illustration style, not 3D-rendered.

Mood: heroic, arcane, mildly dark, but always cartoon-illustrative — NEVER photoreal, NEVER 3D-render-y, NEVER anime-moe, NEVER chibi-cute.

For all following requests, use this v2 STYLE BIBLE as the ground truth.
If a prompt accidentally implies realism, override it toward this cartoon style.
```

---

## 최종 스킬 목록 (실제 구현 기준, 18개)

| 속성 | 스킬 |
|---|---|
| 🔥 Fire (3) | Fireball, FireBeam, Meteor |
| 💧 Water (5) | WaterPuddle, WaterWave, WaterVortex, TidalWave, WaterOrb |
| 🌊🔥 Hybrid (1) | WaveSlash |
| 💨 Wind (4) | WindCutter, GaleRush, Tornado, WindShot |
| 🪨 Earth (5) | StoneSpikes, RockThrow, Earthquake, EarthShard, EarthArmor |

---

## STEP 2 — 스킬 아이콘 프롬프트

### 🔥 Fire

**1) Fireball**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a chunky cartoon ball of fire centered on the stone tablet,
exaggerated round flame silhouette, yellow-white hot core, punchy orange mid-tone,
deep red shadow side, a few simple ember dots floating around.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**2) FireBeam** (E 슬롯 / 채널링 빔)
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a thick chunky cartoon beam of fire blasting diagonally across the icon from lower-left to upper-right,
exaggerated cylindrical beam silhouette with a bright yellow-white core stripe down the middle,
punchy orange mid-tone wrapping the core, deep red flame licks curling off the edges,
stylized burst of ember sparks at the origin point where the beam starts.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**3) Meteor** (Ultimate)
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a single cartoon meteor streaking diagonally from upper-left to lower-right,
chunky blackened rock core with simple cracks, big bold flame mantle wrapping it,
exaggerated ember tail behind in cel-shaded ribbons. Reads as the ultimate — more intense glow.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

---

### 💧 Water

**4) WaterPuddle**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a top-down chunky cartoon puddle of glowing blue water on dark stone,
bold concentric ripple rings (2–3 clean rings, not realistic),
near-white foam highlight on one side, cyan glow halo.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**5) WaterWave**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a curling cartoon wave cresting forward toward the viewer,
bold chunky body of cyan water, exaggerated white foam crest with hard cel-shaded edges,
a few simple droplet shapes flicking off the top.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**6) WaterVortex**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a downward whirlpool funnel seen from a 3/4 high angle,
bold concentric cyan spiral rings (3–4 clean rings) tapering into a dark navy center,
chunky white foam highlights on the outer rim, simple cel-shaded mist puff around the top.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**7) TidalWave** (Ultimate)
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a towering wall of dark blue water curling at the top, dramatic low-angle view,
huge bold silhouette, exaggerated chunky foam crown with hard-edged white highlights,
a couple of stylized mist puffs at the upper rim. Reads as the ultimate — bigger, more imposing.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**8) WaterOrb**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a chunky cartoon sphere of liquid water suspended at center,
bold round silhouette, one crisp white highlight shape on the upper-left,
one dark cyan shadow shape on the lower-right, a single simple droplet hanging beneath about to fall.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

---

### 🌊🔥 Hybrid

**9) WaveSlash** (Q 슬롯 / 물 파도 + 불꽃 자국)
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a chunky cartoon water wave slashing diagonally forward (lower-left to upper-right),
bold cyan wave body with hard-edged white foam crest, exaggerated curl shape like a sword-slash arc,
behind the wave a stylized burning trail of orange-red flame patches with ember dots,
clear visual story: a water blade leaves fire in its wake.
Dual palette: cyan wave + orange-red trail, hard cel-shaded contrast between the two.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

---

### 💨 Wind

**10) WindCutter**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a thin razor-crescent blade of compressed teal air, diagonal slash orientation,
bold cartoon crescent silhouette with hard cel-shaded inner highlight,
2–3 simple white motion-line streaks trailing behind it.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**11) GaleRush**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a forward-charging dash trail shaped like a chunky arrowhead made of mint-cyan wind streaks,
bold layered ribbons of air giving a strong sense of forward motion,
a few simple debris dots flicking sideways.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**12) Tornado** (Ultimate)
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a tall chunky cartoon cyclone of pale-teal swirling air filling the icon,
exaggerated stacked curl shapes (cone-tapered, wide top to narrow base),
a couple of stylized debris chunks orbiting at different heights, simple dust puffs at the base.
Reads as the ultimate — bigger, more imposing.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**13) WindShot**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a sharpened bolt of compressed air shaped like a chunky cartoon feathered arrow,
mid-flight pose pointing diagonally up-right, translucent teal body with hard-edged highlight,
2–3 simple wind ribbon streaks trailing behind the fletching.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

---

### 🪨 Earth

**14) StoneSpikes**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: three chunky cartoon stone spikes erupting upward through cracked ground,
the middle spike tallest, sharp angular silhouettes,
simple dust puffs at the base, bright ochre crack glow seams visible inside each spike.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**15) RockThrow**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a chunky stone-gauntleted cartoon fist hurling a jagged rock forward,
exaggerated boxy fist silhouette, the rock mid-air with 2–3 simple motion lines,
a small dust puff trailing behind, a few small pebble dots.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**16) Earthquake** (Ultimate)
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a top-down view of a cartoon ground crater radiating jagged cracked fissures outward in a star pattern,
a bright glowing ochre core inside the crater, chunky stylized dust plume puffs rising at the center.
Reads as the ultimate — bigger, more intense glow.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**17) EarthShard**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a single chunky razor-sharp obsidian shard standing diagonally at the center,
bold angular silhouette with hard cel-shaded facets,
a bright ochre crack-seam running through it like cracked lava-stone,
2–3 small shard chips floating in the air around it.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**18) EarthArmor**
```
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: chunky cartoon interlocking stone plates forming a protective shoulder bracer / pauldron,
bold blocky silhouette, simple rune marks etched into the plate seams glowing softly,
a subtle stylized shield outline behind hinting at the defensive nature.
Cooler-tinted ochre to read as a defensive icon.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

---

## 사용 팁

1. STEP 1 (스타일 시트)을 한 번 보내고 모델이 "OK" 응답할 때까지 대기.
2. Fireball 한 장 먼저 만들어서 톤 확인. 만족하면 reference 이미지로 첨부하면서 나머지 진행.
3. 같은 속성 묶음(불 3 → 물 5 → 바람 4 → 대지 5) 단위로 진행하면 톤 통일이 잘됨.
4. 각 속성 묶음 끝나면 콘택트 시트로 모아 보고 튀는 놈만 재생성.
5. 글자/숫자/로고는 절대 이미지 모델에 맡기지 말 것 — 후처리(폰트)로 얹기.
