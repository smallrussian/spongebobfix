# Scripts and dialogue (.sb2)

`scripts\c1..c4\*.sb2` (168 files) and `dialogue\*.sb2` (52 files).

## Container
```
u16 line_count
line_count x { u16 length; u8 text[length] }   // each byte XOR 0x30
```
Lines are tab-separated and normally end in `\n` (some final lines don't). `sb2text.py` decodes and
re-encodes all 220 files byte-for-byte.

    python sb2text.py <file.sb2>
    python sb2text.py --tree <game_dir> <out_dir>   # .txt + .json per file, plus script_verbs.json

## Script interpreter (sb2.exe FUN_0043E110)
Each loaded line is a 0x80-byte record: verb at +0x08, args at +0x28 / +0x48 / +0x68 (32 chars each).
Verbs are matched with `strcmpi`. "Actor" = the character selected by `setchar`.

| Verb | Args | Meaning |
|---|---|---|
| setchar | name | select actor |
| walk / run | x y | move actor to point (blocks until arrival) |
| walkgrid / movegrid | gx gy | walk / teleport to walk-grid cell |
| move / movey | x y / y | teleport actor |
| checkgrid | x | re-route actor if not at position |
| rotate | deg | turn actor |
| climb | y | climb to height |
| anim / startanim | name [n] | play animation (blocking / non-blocking); `kickdrop`/`kickup` special |
| anim2d | name | 2D overlay animation |
| idle | seconds | wait (or actor idle) |
| wait | – | wait for previous `add`/`unlock`/`video`/`comment`/`sound`/`narrate` |
| talk / narrate | id | start dialogue / narration |
| comment | id | play a voiced comment |
| sound | name [left\|right] | play sound from scene/chapter bank, panned |
| show / hide | name [can] | show/hide scene object |
| texture | obj tex | swap object texture |
| attach / detach | item boneA boneB | attach prop to actor bones |
| hideattach / showattach | – | toggle attached prop |
| add | item name | add inventory item (isp1/iem1/ikc1 special) |
| undo | item | revert item state |
| unlock / video / skip / save / goal | – | progression, checkpoint |
| block / unblock | gx gy | toggle walk-grid cell (1-based) |
| switch / movie | scene / movie scene | change scene (optionally via movie) |
| script | name | queue another script |
| skipif / exitif | flag | skip next 2 lines / end script if condition holds |
| loop | – | restart script |
| control | on\|off | player control |
| letterbox | init\|open\|close\|in\|out | cinematic bars |
| transition | open\|close | screen transition |
| blending / shadow / terrain | on\|off | render toggles |
| shake, shock, sparkle x y, swapchar, setdoor id | | effects / actor swap / door |
| slots, maze, darts, phone, kickit | init\|spin\|n | minigames |

## Dialogue rows
`sceneid  convo  node  line  speaker  listener  text  n1 n2 n3 extra` — e.g.
`121  A A A  SB  fish  "Hello, faithful customer!"  0 0 0 0`. Scene id 121 = c1l2s1.
Quoted text is a player choice; the numeric columns are branch/flag fields and the last
column may hold an inline command (`anim sb emoexplain`). Exact column semantics: not yet mapped.
