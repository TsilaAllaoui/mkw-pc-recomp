# Mario Kart World direct build, strict no-repack workflow

This fork integrates the cumulative recompilation/runtime source through M15.26.
Game data is supplied separately by the user. Neither build batch file calls a packer.

## First-time build (no prerequisites or winget)

On 64-bit Windows 10 or 11, drag the folder containing the extracted 1.8.0
`exefs` and `romfs` onto `BUILD-MKW-FROM-SCRATCH.bat`, or double-click the BAT
and paste the folder path. It downloads and installs the required tools, creates
the Python environment, validates the dump, and builds the game. `winget`, a
system Python installation, and the Vulkan SDK are not required.

The downloads are the official MSYS2 self-extracting archive, Microsoft Visual
Studio Build Tools bootstrapper, Microsoft's .NET install script, and Astral's
`uv` install script. MSYS2 goes to `C:\msys64`; .NET, `uv`, managed Python and
download caches stay under ignored `toolchain\`. Microsoft Build Tools supplies
the linker and Windows SDK used by .NET NativeAOT; the game itself is compiled
with MSYS2 UCRT64 Clang. Administrator approval is therefore required for the
MSYS2 extraction and Microsoft Build Tools installation.

The script is restartable. If a download, package install, generation step, or
compile fails, correct the reported problem and run it again; valid tools and
incremental build output are reused. Its safe default is four build jobs. Set
`MKW_JOBS` before running it to choose another positive number.

After the game-data-free self-test passes, launch:

```text
build\mkw180\dkb.exe
```

The executable has the dump's absolute paths embedded as defaults, so keep the
dump where it was during the build. Rerun the BAT after moving it.

## Existing developer setup

`BUILD-MKW.bat` is the shorter incremental entry point for a machine which
already has the prerequisites from [building.md](building.md). Set its paths or
environment variables first, then run it from CMD:

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

`RUN-MKW.bat` uses the same developer-machine default dump root if `MKW_GAME`
is unset. It is not needed for the from-scratch flow; `dkb.exe` can be launched
directly.

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
