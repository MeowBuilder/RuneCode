# Image Generation Prompts — Skill Icons (v3 — 실제 VFX 매칭)

GPT(또는 DALL·E / gpt-image-1)에 그대로 붙여 넣어 쓰는 프롬프트 모음.
**사용 순서**: STEP 1을 먼저 한 번 보내서 스타일을 고정 → STEP 2의 스킬 프롬프트를 하나씩.

> v3 변경점: 실제 게임 VFX 캡처를 기준으로 프롬프트를 재작성. 코드 미장착 스킬(`WaterWave`, `RockThrow`)은 제외. 원소별 헥스 컬러 코드 추가.

---

## 실제 슬롯 매핑 (Scene.cpp 기준, 16개)

| 원소 | RC (좌클릭 기본) | Q | E | R (Ult) |
|---|---|---|---|---|
| 🔥 Fire  | Fireball     | WaveSlash    | FireBeam     | Meteor     |
| 💧 Water | WaterOrb     | WaterPuddle  | WaterVortex  | TidalWave  |
| 💨 Wind  | WindShot     | WindCutter   | GaleRush     | Tornado    |
| 🪨 Earth | EarthShard   | StoneSpikes  | EarthArmor   | Earthquake |

> 파일명 규칙: `<SkillData.name>.png` (예: `Fireball.png`). 자세한 건 `gaym/Assets/Textures/SkillIcons/README.txt`.

---

## 원소 컬러 팔레트 (모든 프롬프트가 공유)

```
Fire  : core #FFE066 → mid #FFB42E → edge #FF1E00 → shadow #6E0E00
Water : core #B7F0FF → mid #2DB3FF → edge #002EDA → shadow #001658
Wind  : core #ECFFB3 → mid #BFFF00 → edge #59D959 → shadow #1F4D1F
Earth : core #FFD27A → mid #C8884A → edge #99614A → shadow #2E1709 (밝은 균열 #FFBE3A)
```

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
- Fire   = punchy orange-red, embers yellow-white core      (#FFE066 / #FFB42E / #FF1E00)
- Water  = bright cyan + ice-blue shadow, near-white foam    (#B7F0FF / #2DB3FF / #002EDA)
- Wind   = electric lime/mint, white motion streaks          (#ECFFB3 / #BFFF00 / #59D959)
- Earth  = warm ochre + chunky brown shadow + bright crack   (#FFD27A / #C8884A / #99614A, crack #FFBE3A)

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

## STEP 2 — 스킬 아이콘 프롬프트 (실제 VFX 매칭)

각 프롬프트 상단에 `// 게임 내 동작 메모` 한 줄을 둠 — 모델이 의도를 잡기 쉽도록 의역해 둠.

---

### 🔥 Fire

**Fireball** (RC / 좌클릭 기본 — 일렁이는 불 구체 투사체)
```
// In-game: a wavy, shimmering fireball projectile fired forward.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a chunky cartoon ball of fire suspended at center mid-flight,
exaggerated round flame silhouette that bulges and ripples on one side (wavy,
unstable orb feel — not a perfect circle), yellow-white hot core (#FFE066),
punchy orange mid-tone (#FFB42E), deep crimson outer flame (#FF1E00),
shadow side dropping into dark maroon (#6E0E00), a few simple ember dots trailing behind.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**WaveSlash** (Q — 캐릭터 앞으로 지나가는 불 파도 + 바닥 불 장판)
```
// In-game: a forward-traveling fire wave that scorches a burning patch into the ground.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a chunky cartoon wall of FIRE rolling forward (lower-left → upper-right),
bold curling flame crest like a flame-wave breaking forward, yellow-white core (#FFE066)
along the leading edge, orange body (#FFB42E), deep red base (#FF1E00),
beneath the wave a stylized burning ground PATCH — cracked dark stone with bright lava-orange
crack seams (#FF1E00) and small ember dots glowing on the surface. Clear visual story:
a fire wave passing through, leaving a burning floor patch behind it.
Single fire palette only — NOT a hybrid water/fire, NOT cyan.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**FireBeam** (E — 캐릭터가 정면으로 불 빔 발사)
```
// In-game: the player channels a thick continuous beam of fire straight forward.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a thick chunky cartoon beam of fire blasting diagonally from lower-left to upper-right,
exaggerated cylindrical beam silhouette with a bright yellow-white core stripe (#FFE066)
down the middle, punchy orange mid-tone (#FFB42E) wrapping the core, deep crimson flame licks
(#FF1E00) curling off the edges, a stylized burst of ember sparks at the origin point.
Reads as continuous channeled beam, not a single projectile.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**Meteor** (R / Ultimate — 하늘에서 떨어지는 메테오)
```
// In-game: a single large meteor crashes down at the targeted location.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a single cartoon meteor streaking diagonally from upper-left to lower-right,
chunky blackened rock core with simple cracks revealing bright lava (#FFBE3A) inside,
big bold flame mantle (#FFB42E → #FF1E00) wrapping it,
exaggerated ember tail of cel-shaded ribbons (#FFE066 / #FFB42E) trailing behind.
Reads as the ultimate — more intense glow, larger silhouette than other fire skills.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

---

### 💧 Water

**WaterOrb** (RC — 물 구체 투사체)
```
// In-game: a shimmering water sphere fired forward like the basic projectile.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a chunky cartoon sphere of liquid water suspended at center,
bold round silhouette with a slight rippled bulge on one side (feels alive, not glass-still),
near-white foam highlight (#B7F0FF) on the upper-left, bright cyan mid (#2DB3FF),
deep ink-blue shadow shape (#002EDA) on the lower-right,
a single simple droplet hanging beneath about to fall.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**WaterPuddle** (Q — 마우스 위치에 원형 물 장판 생성)
```
// In-game: a circular water puddle is placed at the cursor on the ground.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a top-down chunky cartoon puddle of glowing blue water on dark stone,
bold concentric ripple rings (2–3 clean rings, hard-edged, not realistic),
near-white foam highlight (#B7F0FF) on the upper rim, bright cyan body (#2DB3FF),
deep ink-blue (#002EDA) rim shadow, cyan glow halo around the edge.
Reads as a placed AOE puddle, viewed from above.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**WaterVortex** (E — 바닥에 깔리는 물 오브 / 룬 서클)
```
// In-game: a glowing water orb settles on the ground at the player's feet,
//          with a rune-ring of water rippling outward around the character.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a 3/4 top-down view of a chunky cartoon WATER ORB resting on dark stone,
the orb is a rounded dome of cyan water (#2DB3FF) with a bright near-white highlight (#B7F0FF)
on top, deep ink-blue (#002EDA) shadow underneath where it meets the ground;
encircling the orb on the ground is a stylized RUNE-RING of cyan water — a flat ring with
2–3 simple angular rune marks etched in it, glowing faintly. Small droplet pops at the rim.
Reads as a placed ground orb with a magical water ring around it,
NOT a downward whirlpool funnel and NOT a tornado.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**TidalWave** (R / Ultimate — 거대한 파도가 지나가며 데미지)
```
// In-game: a massive wave sweeps forward across the battlefield, damaging everything it crosses.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a towering wall of dark cyan water curling forward, dramatic low-angle 3/4 view,
huge bold silhouette filling the frame, exaggerated chunky foam crown with hard-edged
white highlights (#B7F0FF), body in vivid cyan (#2DB3FF) with deep ink-blue base (#002EDA),
a couple of stylized mist puffs at the upper rim, a few droplet flecks ahead of the wave
implying forward motion. Reads as the ultimate — bigger, more imposing than WaterPuddle.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

---

### 💨 Wind

**WindShot** (RC — 바람 구체 투사체, 다른 RC들과 같은 느낌)
```
// In-game: a fast wind orb fired forward — visually the wind-element basic projectile.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a chunky cartoon sphere of compressed swirling air suspended at center,
bold round silhouette built from 2–3 cel-shaded layered swirl shapes spiraling inward,
electric lime core (#ECFFB3), bright green-yellow mid (#BFFF00), deeper green edge (#59D959),
a couple of simple white motion-streak ribbons trailing behind the orb implying forward flight.
Reads as the wind-element ORB projectile, matching the other elements' RC orbs in silhouette.
NOT a feathered arrow, NOT a crescent blade.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**WindCutter** (Q — 빠르게 던지는 별모양/버스트 투사체)
```
// In-game: a fast lime-green burst-shaped projectile is hurled forward.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a chunky cartoon STARBURST / SHURIKEN-like shard of compressed wind,
bold 4-to-6-pointed jagged star silhouette (sharp pointed rays radiating from a tight center),
electric lime core (#ECFFB3), bright green-yellow body (#BFFF00), deep green shadow rim (#59D959),
2–3 simple white motion-line streaks trailing behind it implying it was just thrown forward.
NOT a thin crescent blade — clearly a multi-pointed burst projectile.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**GaleRush** (E — 이동기 / 돌진)
```
// In-game: the player dashes forward riding a streak of wind.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a forward-charging dash trail shaped like a chunky arrowhead made of
mint-cyan / lime wind streaks, bold layered ribbons of air (#ECFFB3 highlight, #BFFF00 body,
#59D959 shadow) giving a strong sense of forward motion,
a few simple debris dots flicking sideways. Reads as a movement/dash ability, not a damage spell.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**Tornado** (R / Ultimate — 지정 위치에 토네이도 소환)
```
// In-game: a tall persistent tornado is summoned at the targeted location.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a tall chunky cartoon cyclone of pale lime-green swirling air filling the icon vertically,
exaggerated stacked curl shapes (cone-tapered, wide top → narrow base),
core highlight (#ECFFB3), body (#BFFF00), shadow rim (#59D959),
a couple of stylized debris chunks orbiting at different heights, simple dust puffs at the base.
Reads as the ultimate — bigger and more imposing than WindCutter/WindShot.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

---

### 🪨 Earth

**EarthShard** (RC — 돌 파편 투사체)
```
// In-game: a sharp rock shard fired forward — the earth-element basic projectile.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a single chunky razor-sharp obsidian-stone shard standing diagonally at center,
bold angular faceted silhouette with hard cel-shaded planes,
warm ochre highlight (#FFD27A), mid brown (#C8884A), deep umber shadow (#99614A),
a bright lava-glow crack-seam (#FFBE3A) running through the middle of the shard like cracked
magma-stone, 2–3 small chip fragments floating in the air around it.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**StoneSpikes** (Q — 지정 방향으로 바닥 폭발이 순차적으로 일어남)
```
// In-game: a line of ground bursts erupts forward in sequence from the player along an aimed direction.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a 3/4 angled view of a CRACKED GROUND LANE running diagonally from lower-left to upper-right,
along the lane three chunky cartoon ground BURSTS at staggered scale —
the nearest burst small, the middle one medium, the farthest one tallest —
each burst is a jagged stone spike erupting through a dust cloud puff, with bright lava-orange
crack seams (#FFBE3A) glowing inside the spike and along fissures radiating across the ground.
Stone in warm ochre (#FFD27A → #C8884A → #99614A). Clearly reads as a SEQUENTIAL DIRECTIONAL
attack, not a single static cluster of spikes.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**EarthArmor** (E — 캐릭터 주위에 보호 효과 생성)
```
// In-game: a defensive stone effect forms around the player.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: chunky cartoon interlocking stone plates forming a protective shoulder bracer / pauldron
at center, bold blocky silhouette, simple angular rune marks etched into the plate seams
glowing softly with warm ochre light (#FFBE3A),
plates in warm ochre (#FFD27A → #C8884A → #99614A),
a subtle stylized shield outline behind hinting at the defensive nature.
Cooler-tinted ochre to read as a DEFENSIVE icon, not an offensive one.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

**Earthquake** (R / Ultimate — 캐릭터 주위로 더 크고 넓은 공격 AOE)
```
// In-game: a large radial earth AOE erupts around the player — bigger and wider than other earth skills.
Use the v2 STYLE BIBLE. Square skill icon, 1024×1024, transparent background.
Subject: a top-down view of a CARTOON GROUND CRATER radiating jagged cracked fissures outward
in a star/asterisk pattern that fills the whole icon, a bright glowing ochre core (#FFBE3A)
inside the crater pulsing with lava light, chunky stylized dust plume puffs rising at the center,
broken stone chunks (#C8884A / #99614A) flipped up around the rim.
Reads as the ULTIMATE — bigger, brighter, more intense than StoneSpikes.
RENDERING: flat 2D cartoon illustration, cel-shaded with 2–3 hard-edged tones,
chunky stylized shapes, NOT a 3D render, NOT photoreal.
Think Hearthstone / Hades / Clash Royale icon style.
No text. No border outside the frame.
```

---

## 사용 팁

1. STEP 1 (스타일 시트) 을 한 번 보내고 모델이 "OK" 응답할 때까지 대기.
2. **Fireball / WaterOrb / WindShot / EarthShard** 4개 RC 부터 만들어서 4원소 톤 통일을 먼저 잡기. 4장 다 만족스러우면 reference 이미지로 첨부하면서 나머지 진행.
3. 같은 원소 묶음(4장)을 한 세션에서 끝내야 색 톤이 일관됨.
4. 각 원소 4장 끝나면 콘택트 시트로 모아 보고 튀는 놈만 재생성.
5. 글자/숫자/로고는 절대 이미지 모델에 맡기지 말 것 — 후처리(폰트)로 얹기.
6. 출력 파일명을 **반드시 위 표의 스킬 이름과 정확히 일치**시켜 `gaym/Assets/Textures/SkillIcons/` 에 저장 (예: `Fireball.png`, `WaveSlash.png`).
