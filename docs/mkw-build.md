# Mario Kart World direct build, strict no-repack workflow

This fork integrates the cumulative recompilation/runtime source through M15.26.
Game data is supplied separately by the user. **BUILD-MKW.bat never calls a packer.**

## One-command build

From CMD in `G:\mkw-recomp`, run:

```cmd
BUILD-MKW.bat
```

The defaults are:

* Game dump root: `H:\Switch2\Mario_Kart_World_Update_1.8.0`. It accepts
  `updates\917504\exefs` and `updates\917504\romfs` below that root. If
  the numbered directory is not present, a single unambiguous version-named
  directory under `updates` may be used instead.
* Existing image: `G:\mkw-recomp\output\MarioKartWorld\romfs\romfs.bin`.
* Jobs: `%NUMBER_OF_PROCESSORS%`. Existing generated C and Ninja objects are reused.

The selector does one of the following, in order:

1. Reuses the existing packed image. If `index.json` is missing beside it, it
   searches known prior manifest locations, or reconstructs **only** the file
   list/offset metadata from `output\MarioKartWorld\romfs-index.bin`.
   No game data is copied and the image contents are not scanned or hashed.
   The rebuilt executable points directly to this image.
2. If the packed image or its required index metadata is missing/unusable,
   creates a **small, inventory-only index** from the extracted `romfs` folder.
   The runtime then opens the original loose files directly using its
   `DKB_LOOSE_ROMFS_V1` backend. This may walk 200k filenames once but never
   concatenates files into `romfs.bin`.
3. If neither is usable, stops with an error. It **never** packs.

If a packed manifest must be reconstructed from an existing compiled index, its
image SHA-256 might be unknown. The builder explicitly records that fact and
never pretends the image was fully hash-verified. An optional full-verify mode
would require the original SHA-256 metadata.

## Optional overrides

```cmd
set "MKW_GAME=H:\Switch2\Mario_Kart_World_Update_1.8.0\updates\917504"
set "MKW_PACKED_ROMFS=G:\mkw-recomp\output\MarioKartWorld\romfs\romfs.bin"
set "MKW_JOBS=16"
BUILD-MKW.bat
```

If you want to force the loose-file mode without deleting the existing image,
set `MKW_PACKED_ROMFS` to a nonexistent path before the build. Neither
source file nor any large image is modified.

`RUN-MKW.bat` uses the same default dump root if `MKW_GAME` is unset.

## Artifacts and preservation

Only the generated small index/selection records are written under
`build\mkw-local`, `build\extracted\mkw180\romfs-loose`, or beside an
existing output image if its `index.json` was missing. The original extracted
files and the existing multi-gigabyte packed image are not rewritten, copied,
repacked or deleted. `tools/mkw_cli.py pack` remains an optional legacy tool,
but the new build script never invokes it.

## Validation

```cmd
py -3 -m unittest tests.mkw_direct_build_test tests.mkw_no_repack_build_test
```

Offline tests can't prove the game runs on your Windows GPU. A successful
build is not proof of a correct rendered frame.
