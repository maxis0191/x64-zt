# IW7 player animation script (`playeranim`)

The player's third-person animation script (`human_playeranim_cp` for zombies, in `common_cp` and `patch_common_cp`;
`human_playeranim_mp` and three others in `common_core_mp`) is now dumped and built:

- dump: `zonetool -dumpzonetypes <zone> playeranim` writes `dump/<zone>/playeranim/<name>.json`;
- build: a zone source row `playeranim,<name>` reads `zonetool/<zone>/playeranim/<name>.json`.

Layout: read from the game's loader (`Load_PlayerAnimScript` 0x140A63D00 of the shipped executable and its helpers
0x140A63C20, 0x140A641A0, 0x140A643B0, 0x140A642F0); see the comment at the top of `src/zonetool/zonetool/iw7/assets/playeranim.cpp`.

JSON: `animations` (entry, anim, shadow animation names; the 68 bytes the tool does not decode as hex), `scriptAnims`
(121 entries) and `scriptEvents` (55) as `{flags, items, transitions}`, `scriptItems` as `{conditions: [{index,
value[4]}], commands: [{bodyPart, animIndex, animDuration}]}`, transitions, idle turns and twitches, aim and lean
sets, the four pack maps (torso/legs, pack/unpack) and the `xAnims` list.

Measured (12 Nov 2026): the zombies script (2,527 animations, 2,303 items) dumped, changed (6 animations, 6 items) and
built into a mod zone; the zone dumped back with the game's own loader gives the same JSON. What the data says:
condition index 20 is the scripted animation type (a 128-bit set, bit = `SCRIPTEDANIMTYPE_GESTURE000` + n), index 3 the
move type; a gesture's third-person animation is a torso command (body part 2) of an item in the event the weapon
fires (zombies: event 51, the walkie-talkie's; multiplayer: event 49). Every animation of the script must also be in
the animation tree `multiplayer` (`animtrees/multiplayer.atr` of `common_mp`), or the map load stops with
"animation '<name>' not defined in anim tree 'multiplayer'". Used by the server repository's Maxis' Phone
(`ops/maxis/playeranim_phone.py`, `docs/MAXIS-PHONE.md`).
