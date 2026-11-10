# x64 ZoneTool
A fastfile unlinker and linker for various x64 Call of Duty titles. 

- If you are interested in porting maps or assets from IW3/4/5, check <b>[Aurora's Map Porting IW3/4/5 -> H1](https://docs.auroramod.dev/map-porting-iw5)</b>
- If you are interested in porting maps or assets from newer Call of Duty games like H1, S1, H1, H2, or IW7, between each other, check <b>[Aurora's Map Porting (S1 <-> H1 <-> H2)](https://docs.auroramod.dev/map-porting-s1)</b>

## Supported Games
* **IW6** (*Call of Duty: Ghosts*)
* **S1** (*Call of Duty: Advanced Warfare*)
* **T7** (*Call of Duty: Black Ops 3*) ***[dumping only]***
* **H1** (*Call of Duty: Modern Warfare Remastered*)
* **H2** (*Call of Duty: Modern Warfare 2 Campaign Remastered*)
* **IW7** (*Call of Duty: Infinite Warfare*) ***[no custom maps]***

## How to use
Check out the [Aurora Zonetool Basics](https://docs.auroramod.dev/zonetool-basics) for useful guides & information on how to port maps and use zonetool.

## Commands
* `loadzone <zone>`: Loads a zone
* `unloadzones`: Unloads zones
* `verifyzone <zone>`: Lists assets in a zone
* `dumpzone <zone>`: Dumps a zone
* `dumpzone <target game> <zone> <asset filter>`: Dumps a zone converting assets for a specific game
* `dumpasset <type> <name>`: Dumps a single assset

IW7 supports [reviewed soundbank subsets](docs/soundbank-subsets.md) and a
metadata-only `-soundbank-catalog` command for isolating alias dependencies
without exporting or repacking retail audio samples.
* `dumpmap <map>`: Dumps all required assets for a map
* `dumpmap <target game> <map> <asset filter> <skip common>`: Dumps and converts all required assets for a map

### Owner-fork IW7 Zombies enemy catalog extension

The `bigmangeekf/x64-zt` fork adds offline commands for private IW7 Zombies porting work:

* `-enemy-zone-dump <cp_zmb|cp_rave|cp_disco|cp_town|cp_final>`: writes the donor zone's asset inventory.
* `-enemy-table-dump <cp_zmb|cp_rave|cp_disco|cp_town|cp_final>`: writes the map's retail agent-definition stringtable to `enemy-catalog/<map>.csv`.
* `-enemy-extract <map> <definition-table> <agent-type> <zero-based-type-column> <paris_enemy_<map>_<type>>`: writes exactly one matching definition row into that pack's source tree before a following `-buildzone` command.
* `-enemy-script-catalog <map> <safe-output-name>`: lists effective registered `ScriptFile` database keys and header names, status, serialized lengths, and `payload_sha256` after shared and map zones load. The fingerprint covers the three serialized length fields, compressed buffer, and bytecode, but excludes the names so identical compiled payloads can be compared across numeric and path-based keys. It records metadata only; it does not export compiled scripts.
* `-enemy-script-dump <donor-map> <script-list-file> <paris_enemy_<donor-map>_<name>_scripts>`: writes only the explicitly listed original compiled IW7 `ScriptFile` assets to `dump/<pack-name>/<module>.gscbin`, with per-module status, byte count and SHA-256 in `dump/<pack-name>/scriptfile-manifest.csv`. Module paths and numeric ScriptFile IDs are accepted. Before accepting an export, it removes a prior file and compares every written byte against the live donor `ScriptFile` header.
* `-enemy-reference-check cp_zmb <type,name-list.csv> <safe-output-name>`: checks an explicit Spaceland asset list against the loaded IW7 database and writes `enemy-catalog/<safe-output-name>.csv`.
* `-enemy-animclass-audit <map> <animclass-list.csv> <safe-output-name>`: writes the requested map's loaded animation-class state and aim-set array-presence metadata to `enemy-catalog/<safe-output-name>.csv`.
* `-enemy-xmodel-surface-audit <map> <xmodel-list.csv> <safe-output-name>`: lists each requested model's registered XModelSurfs references, physicsasset and physicsfxshape names, and recipient-map DB status.
* `-enemy-physics-dump <donor-map> <physicsasset-list.csv> <paris_enemy_<donor-map>_<name>_physics>`: captures only the named donor PhysicsAssets before retail DB registration, checks the dumped Havok bytes against their pre-registration buffers, and writes a per-asset manifest under `dump/<pack-name>/`.
* `-enemy-weapon-dump <map> <weapon-list.csv> <paris_enemy_<map>_<name>_weapon_source>`: exports exact non-default donor weapon definitions as ZoneTool JSON into a fresh `dump/<pack>/weapons/` source directory and writes a hash manifest.
* `-dump_streamed_image -enemy-image-dump <donor-map> <image-list-file> <paris_enemy_<donor-map>_<name>_images>`: captures requested images during database registration, before the stream cursor is reused. Use a fresh output name and raw pixels (no `-dds`). Each registration version is preserved; the last complete version supplies the portable metadata/pixel files and SHA-256 manifest. The explicit UTF-8 list accepts at most 512 unique names and 64 KiB. A missing registration or incomplete stream fails the command. Images registered before this command begins may need an earlier capture; this command does not certify them from a late database lookup.

Run the reference check from an isolated workspace outside Git with the legitimate IW7 installation available, for example `ZoneTool.exe -headless -enemy-reference-check cp_zmb references.csv spaceland_check_01`. The input path is resolved from the process working directory. The output name must be 1-128 lowercase letters, digits, or underscores; the resulting file is written only to `enemy-catalog/<safe-output-name>.csv`. The command refuses an existing output and rejects an `enemy-catalog` link/reparse point. It permits only `cp_zmb`.

The input is a headerless, strict CSV: one `type,name` pair per LF or CRLF line, with exactly one comma and no quoting, BOM, blank rows, or trailing spaces. `type` must exactly match a registered IW7 asset type. `name` must be printable ASCII, nonempty, at most 512 bytes, without commas, quotes, line breaks, or leading/trailing spaces. Duplicate type/name pairs are rejected. The input is read with a 2 MiB cap and a 100,000-row cap; a final newline is optional. For example:

```csv
xmodel,wpn_zmb_example_view
material,mtl_zmb_example
```

The sample names are placeholders; replace them with exact asset names from the reference list you are checking.

The command loads the same shared/core/map source zones used by the enemy extraction path, including the `cp_zmb` Spaceland zone and available shared zones. It looks up only already-registered database entries, never asks the engine to create a default asset, and calls `DB_IsXAssetDefault` for a non-null header. The output has the header `type,name,status`: `missing` means there is no matching non-null entry, `default` means the entry exists but `DB_IsXAssetDefault` returns true, and `verified` means the entry exists and that function returns false. The file is created exclusively, then checked by exact byte-for-byte readback. It exits successfully only when every row is `verified`; a complete CSV is still written when missing/default rows are found, and that result exits nonzero.

The animclass audit accepts the same strict `type,name` CSV shape, but permits only `animclass` rows and the five Zombies maps. Its report includes state/aim-set counts, aim-set/root names, animation counts and whether each array pointer is present. It is limited to 128 assets, rejects invalid dimensions, and reads only registered asset structures. A non-null array marker describes this local database snapshot; it does not alone prove that a nullable field is semantically optional or that a custom fastfile will load correctly.

The model-surface audit writes `map,model,status,lod_index,xmodelsurfs_name,surface_status,physicsasset_name,physicsfxshape_name`. The two physics columns are the direct names stored on each XModel; they are not an inventory of the referenced asset's transitive event or Havok dependencies.

The PhysicsAsset capture accepts the same strict `type,name` CSV shape, permits only `physicsasset` rows and the five Zombies maps, and is limited to 128 names. It records every matching registration into a unique capture directory, then selects the last registration in donor load order and copies its full serialized source closure into `dump/<pack-name>/`. Each `.hkx` dump must match the live pre-registration buffer byte-for-byte; the manifest records capture count, byte size and SHA-256. A successful capture establishes only source-byte preservation. The enemy pack builder now fails closed if a `paris_enemy_*` pack lacks a portable PhysicsAsset source or its Havok data fails the packfile-header check; `-verifyzone` is still required and may reveal further engine-level incompatibility.

The XModel surface audit accepts the same strict CSV shape but permits only `xmodel` rows and the five Zombies maps. It reads each registered model's six LOD slots, lists the exact referenced XModelSurfs names, and classifies each already-registered surface asset as `missing`, `default`, or `verified`. It caps the request at 128 models and exits nonzero if a model or referenced surface is not verified. This reports database presence only; material, image, shader, animation and runtime dependencies still require separate review.

The weapon dumper accepts the same strict `type,name` CSV shape, limited to `weapon` rows, the five Zombies maps and 64 definitions. Its pack token must start with `paris_enemy_<map>_`, end with `_weapon_source`, and have a new output directory. It reads only exact, non-default registered donor weapons, writes them using the existing IW7 WeaponCompleteDef serializer, parses each generated JSON file before recording its byte count and SHA-256, and refuses an incomplete output. Keep the generated game data outside Git. Add the fresh source folder to the intended pack's `addpath` list before building.

`verified` proves only that this local offline database had a non-null, non-default entry after those zones loaded. It does not prove who supplied the asset, its provenance, the full transitive dependency closure, runtime compatibility, rendering, AI, audio, multiplayer behavior, or owner gameplay acceptance. Review the CSV alongside source and dependency evidence before deciding whether an asset can be treated as recipient-provided.

The script catalog is useful when a retail fastfile exposes opaque numeric ScriptFile names. Run it from an isolated workspace with the legitimate IW7 installation available; its output is written exclusively to `enemy-catalog/<safe-output-name>.csv` and contains no script bytes. The script-dump list is UTF-8 text with one module path or numeric ScriptFile ID per line, without the `.gscbin` extension; blank lines and lines beginning with `#` are ignored. For example, `scripts/mp/agents/lumberjack/lumberjack_agent` selects a named donor module, while a numeric row such as `2606` selects the effective ScriptFile whose header name is `2606`. The command keeps automatic dumping disabled while loading the donor zones and exports only requested scriptfiles. It does not calculate the transitive GSC dependency closure; list every required module explicitly and review the manifest before building a script pack.

For a `paris_enemy_*` `-buildzone`, the fork dumps only selected image, shader, scriptable and technique-set dependencies as the builder traverses that pack's asset closure. It rejects world assets, client-pack ScriptFiles, unsupported dependency types, missing/default database assets, and the generated red missing-image placeholder. Material JSON dependency names remain exact instead of accepting database fallback names. Portable image chunks must match the sizes declared by the donor metadata; process-owned texture/pixel pointers are cleared. This source dump is not a substitute for reviewing the generated inventory and pack provenance.

Enemy extraction captures nonempty vertex, hull, domain and pixel DXBC programs at initial registration. It preloads the shared shader-only `techsets_global_core_mp` and `techsets_common_core_mp` zones where available, without importing their full MP gameplay zones. Live procedural-bone serialization is restricted to enemy packs and bounds/remaps its declared arrays and script strings. A narrow Slasher procedural-bone fixture loaded successfully offline; that does not verify every procedural-bone layout or a complete enemy pack.

Run offline commands with `-headless` from an isolated workspace containing the legitimate IW7 installation links. For a cross-map enemy pack, preload the recipient Spaceland database before verification, for example `ZoneTool.exe -headless -loadzone cp_zmb -verifyzone paris_enemy_cp_rave_slasher`. A standalone `-verifyzone <pack>` may omit that recipient context; the Rave Slasher pack's standalone verification crashed while loading a Havok-backed asset, while the same artifact completed after `cp_zmb` was loaded first. Treat this as a ZoneTool verification-context requirement, not proof that the asset renders or behaves correctly. `-verifyzone` checks native asset loading only; it cannot establish rendering, AI, audio, multiplayer or owner gameplay acceptance. Keep failed candidates and source provenance separate from accepted ports.

These commands use the installed game's local IW7 database. Keep extracted rows, fastfiles, texture packs, shader files, logs, and other game data outside Git. These fork-specific commands are not part of upstream ZoneTool.

  ### Definitions
  * `asset filter`: A filter specifying all the asset types that should be dumped, if not specified or empty it will dump all asset types.
  Asset types are separated by **commas**, **`_`** indicates and empty filter.   
    * Example: `dumpzone h1 mp_clowntown3 sound,material,techset,rawfile`
    * Example: `dumpmap h1 mp_clowntown3 _ true`
  * `skip common`: Skips common zones when dumping a map, can be `true` or `false`.
  * `target game`: The game to convert the assets to.

## IW7 map compiler (`-iwzcompile`)
IW7 has no map compiler of its own. `-iwzcompile` builds a new IW7 map (render geometry, Havok collision, nav graph, entities, lightmap, static models, door brush models, trigger volumes, darkness/named lights) around the world assets of a stock map, which is used as a template for sun, sky, light grid and reflection probes. It writes the regular dump layout, ready for `-buildzone`.

```
zonetool.exe -headless -unbuffered-io -iwzcompile <template zone> <template map> <new map> <source dir>
zonetool.exe -headless -unbuffered-io -iwzcompile cp_zmb_tpl cp_zmb cp_iwz C:\work\iwz_src
```

* Run it from a work folder that holds the game's `zone` files, as for any ZoneTool run. The template zone is a dump of the stock map at `zonetool/<template zone>`.
* `<source dir>` holds `world.bin`, `colmap.hkx`, `navgraph.bin` and `ents.txt`, plus the optional `lighting.txt`, `lights.txt`, `probes.txt`, `smodels.txt`, `triggers.txt`, `scriptables.txt` and a `lightmap` folder.
* The output goes to `dump/<new map>/` (`maps/cp/<map>.d3dbsp.*`, `images`, `lightmaps`, `transient_zones`).
* Switches are environment variables (`IWZ_GEN_TOME=1`, `IWZ_KEEP_ENTSHAPES=1`, `IWZ_KEEP_SUBMODELS=1`, `IWZ_TRIGGER_SHAPES=1`, ...); the full list is in `src/zonetool/zonetool/iw7/iwzmap/`.
* Generated trigger volumes get their own box shapes (`iwzmap_volumes.cpp`) instead of borrowing a template trigger's shape.
* Two runs of the same input differ in a few padding bytes of the output headers (uninitialised pointer residue); the map data itself is identical.

The creator SDK (`maxis0191/iw7-creator-sdk`, `tools-source/iwzmap`) generates the source folder and calls this command.

## Asset Conversion Support

The table below shows which asset conversions are currently supported.

- **Rows** = Source game
- **Columns** = Target game

Legend:
- ✔️ Fully supported
- ⚠️ Partially supported (experimental)
- ❌ Not supported

| **From ↓ / To →** | **IW6** | **S1** | **H1** | **H2** | **T7** | **IW7** |
|-------------------|:-------:|:------:|:------:|:------:|:------:|:-------:|
| **IW6** | ✔️ | ❌ | ✔️ | ✔️ | ❌ | ❌ |
| **S1**  | ❌ | ✔️ | ✔️ | ✔️ | ❌ | ❌ |
| **H1**  | ❌ | ⚠️ | ✔️ | ✔️ | ❌ | ⚠️ |
| **H2**  | ❌ | ❌ | ✔️ | ✔️ | ❌ | ❌ |
| **T7**  | ❌ | ❌ | ⚠️ | ❌ | ✔️ | ⚠️ |
| **IW7** | ❌ | ❌ | ❌ | ❌ | ❌ | ✔️ |
