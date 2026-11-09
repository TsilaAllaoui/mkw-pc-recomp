# Mario Kart World 1.8.0 direct build target

This branch starts at DKB commit `b7e6fa1f` and integrates the cumulative Windows compatibility and renderer work
through M15.26 directly into DKB's normal `recomp/`, `runtime/` and `tools/` directories. It does not clone or patch
a second engine checkout.

No game data is included. You must supply your own legally dumped 1.8.0 update with `exefs/` and `romfs/` folders.
ExeFS, RomFS, generated C, objects, shader caches, profiles and executables stay under ignored directories.

## Build

Install the prerequisites from [building.md](building.md), including MSYS2 UCRT64 and .NET 10. Then run:

```batch
set "MKW_GAME=D:\Games\MarioKartWorld-1.8.0"
BUILD-MKW.bat
```

The first build creates `.venv`, builds the shader translator, lifts `main`, `subsdk0`, `subsdk1` and `sdk`, applies
the required guest allocator observers to the generated C, packs the supplied RomFS locally and links
`build\mkw180\dkb.exe`.

To reuse an existing packed image without copying, rewriting or hashing it:

```batch
set "MKW_GAME=D:\Games\MarioKartWorld-1.8.0"
set "MKW_PACKED_ROMFS=G:\private\mkw-romfs"
BUILD-MKW.bat
```

The selected directory must contain `romfs.bin` and its `index.json`. The image remains outside Git.

## Run

```batch
set "MKW_GAME=D:\Games\MarioKartWorld-1.8.0"
RUN-MKW.bat
```

The validated defaults use Vulkan compute ASTC decoding, BC3 storage and automatic memory-pool selection. Runtime
logs, profiles and shader caches remain under ignored `build/` paths.

## Validation

```batch
py -3 tools\check_content.py
py -3 -m unittest tests.mkw_direct_build_test
```

The content guard also runs in CI and rejects common game-content, generated-output and binary formats.
