# Asset-free native tests

Run from the repository root with Visual Studio 2022 C++ tools available:

```powershell
msbuild tests/filesystem_ownership_tests.vcxproj /m:1 /nr:false /p:Configuration=Release /p:Platform=x64
./build/tests/filesystem_ownership_tests.exe
msbuild tests/sound_bank_subset_tests.vcxproj /m:1 /nr:false /p:Configuration=Release /p:Platform=x64
./build/tests/sound_bank_subset_tests.exe
msbuild tests/iwzmap_smodel_rules_tests.vcxproj /m:1 /nr:false /p:Configuration=Release /p:Platform=x64
./build/tests/iwzmap_smodel_rules_tests.exe
msbuild tests/iwzmap_voxel_lists_tests.vcxproj /m:1 /nr:false /p:Configuration=Release /p:Platform=x64
./build/tests/iwzmap_voxel_lists_tests.exe
msbuild tests/iwzmap_light_ref_tests.vcxproj /m:1 /nr:false /p:Configuration=Release /p:Platform=x64
./build/tests/iwzmap_light_ref_tests.exe
msbuild tests/iwzmap_world_layers_tests.vcxproj /m:1 /nr:false /p:Configuration=Release /p:Platform=x64
./build/tests/iwzmap_world_layers_tests.exe
msbuild tests/iwzmap_draw_classes_tests.vcxproj /m:1 /nr:false /p:Configuration=Release /p:Platform=x64
./build/tests/iwzmap_draw_classes_tests.exe
```

The filesystem target compiles the production `filesystem.cpp` with a minimal
test precompiled-header substitute and directory-helper definitions. It checks
close/exists/destruction, copy and move ownership, successful and failed reopen,
and empty-mode rejection. Exclusive Windows file opens detect leaked streams;
no game assets or game process are used. The corrected `close()` clears its
owned pointer even if `fclose` fails and treats an already closed object as a
successful no-op. `open()` releases the previous stream before replacing it.

The soundbank target requires the JSON submodule. It checks schema ranges,
synthetic alias-index lookups and verified report publication. These tests do
not establish retail dependency closure, package loading or audio playback.

The map compiler rules target (`iwzmap_smodel_rules.hpp`, header only) checks how a static model with no
light grid lit instance in the template map gets its draw instance: the neutral donor (light grid lit, material
skin 0, single light grid sample, no motion flags), the fitted model fields, and the predicate of the earlier crash
(a donor's material skin index past the model's material list), on field values of real cp_zmb instances. It does
not show that the game draws such a model; that is a map run (README, "Static models").

Each executable creates a unique temporary test directory and removes only its
own known files on success. On failure, the directory remains for diagnosis.

The voxel light list target (`iwzmap_voxel_lists.hpp`, header only) checks the compiler's static light voxel lists: which distinct light sets need a list of
their own when a leaf may name a few lights its box is outside of (`absorb`), the smallest slack that keeps the array under the u16 address limit (`choose`), and a
fixture whose entry counts equal `studio/voxellights.py` of the Studio repository. The budget policy (`budget_slack`, `choose_budget`: a set held by n
leaves takes up to budget / n extra lights, the compiler's default) has the same kind of fixture.

The world layer target (`iwzmap_world_layers.hpp`, header only) checks the world.bin version 2 surface record (44 byte version 1 record plus layer offset and stride, read field by field),
the validation of a layered surface's record against the layer blob (inside, 4 aligned, no 32 bit wrap), the zone's layer stream (the blob, padded with zeros up to the zeroed 16 bytes a vertex
of a world without layers, which is what version 1 files still get), the `vertexLayerData` of a layered and of a plain surface, and the one log line of a build.

The draw class target (`iwzmap_draw_classes.hpp`, header only) checks the rules that put generated world surfaces in the dpvs decal, trans and emissive ranges: the sort key bands of the
template world (cp_zmb: opaque 1..6, decal 7..17, trans 18..34, emissive 35..40, ends included, an unset band classifies nothing), the `draw_classes.txt` reader (comments, CRLF, every
malformed line refused with its number, two keys for one material refused), the stable partition by class (the identity for a world with only opaque surfaces, so a world without decals
keeps its surface order), and the range arithmetic (a plain world gets opaque 0..count and three empty ranges at count, exactly as before).
