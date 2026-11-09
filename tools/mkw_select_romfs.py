#!/usr/bin/env python3
"""Select MKW RomFS without packing, copying or hashing the large image.

First use an existing packed image with verified index metadata. With --auto,
fall back to an inventory of the extracted loose RomFS if packed metadata is
unavailable. Only small index/selection files are written.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path

INDEX_HEADER = struct.Struct('<8sQQ')
INDEX_ROW = struct.Struct('<QQII')
LOOSE_BACKEND = 'dkb-loose-v1'


def same_bytes_write(path: Path, contents: bytes) -> None:
    if path.is_file() and path.read_bytes() == contents:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    # A temporary sibling avoids a half-written index after interruption.
    temp = path.with_name(path.name + '.mkw-new')
    temp.write_bytes(contents)
    temp.replace(path)


def index_binary(image: Path) -> Path | None:
    # The previously built executable lives one directory above romfs/.
    paths = [image.parent / 'romfs-index.bin', image.parent.parent / 'romfs-index.bin']
    return next((p for p in paths if p.is_file()), None)


def recover_compiled_index(binary: Path, image_size: int) -> tuple[list[dict], str]:
    """Read TOTKRFS1 metadata, *not* the multi-GB image itself."""
    data = binary.read_bytes()
    if len(data) < INDEX_HEADER.size:
        raise ValueError('compiled RomFS index is truncated')
    magic, count, strings_offset = INDEX_HEADER.unpack_from(data)
    if magic != b'TOTKRFS1' or count == 0 or count > 1_000_000:
        raise ValueError('compiled RomFS index magic/count invalid')
    if strings_offset != INDEX_HEADER.size + count * INDEX_ROW.size or strings_offset >= len(data):
        raise ValueError('compiled RomFS index row layout invalid')
    files = []
    previous = b''
    for i in range(count):
        offset, size, name_offset, name_length = INDEX_ROW.unpack_from(data, INDEX_HEADER.size + i * INDEX_ROW.size)
        begin = strings_offset + name_offset
        end = begin + name_length
        if (not name_length or end > len(data) or data[end - 1] != 0 or
                offset > image_size or size > image_size - offset):
            raise ValueError(f'compiled RomFS index row {i} has invalid bounds')
        raw = data[begin:end - 1]
        if (not raw or raw <= previous or b'\\' in raw or b'\0' in raw or
                raw.startswith(b'/') or b'..' in raw.split(b'/')):
            raise ValueError(f'compiled RomFS index row {i} has invalid path')
        previous = raw
        files.append({'path': raw.decode('utf-8'), 'offset': offset, 'size': size})
    return files, hashlib.sha256(data).hexdigest()


def validate_packed_manifest(doc: dict, image_size: int, compiled: Path | None = None) -> None:
    if doc.get('status') != 'pass' or doc.get('romfs_size') != image_size:
        raise ValueError('index.json is incomplete or its image size differs')
    files = doc.get('files')
    if not isinstance(files, list) or not files:
        raise ValueError('index.json has no file entries')
    if compiled:
        # Compare the *entire index* using the canonical index serializer.
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import romfs_index
        wanted = romfs_index.build_index(doc)
        with compiled.open('rb') as f:
            observed = f.read()
        if wanted != observed:
            raise ValueError('index.json does not match the compiled romfs-index.bin')


def find_prior_manifest(repo: Path, image: Path, compiled: Path | None) -> dict | None:
    paths = [image.parent / 'index.json',
             repo / 'build' / 'extracted' / 'mkw180' / 'romfs' / 'index.json',
             repo / 'workspace' / 'core' / 'build' / 'extracted' / 'mkw180' / 'romfs' / 'index.json']
    for path in paths:
        if not path.is_file():
            continue
        try:
            doc = json.loads(path.read_text(encoding='utf-8'))
            validate_packed_manifest(doc, image.stat().st_size, compiled)
            print(f'[INDEX] Reusing small packed RomFS metadata: {path}')
            return doc
        except (ValueError, KeyError, TypeError, OSError) as exc:
            print(f'[INDEX] Skipping incompatible metadata {path}: {exc}')
    return None


def cached_image_hash(repo: Path, compiled_digest: str) -> str:
    """Reuse a prior SHA only when its small compiled-index identity matches."""
    for path in [repo / 'workspace' / 'core' / 'build' / 'mkw180' / 'romfs_config.h',
                 repo / 'build' / 'mkw180' / 'romfs_config.h']:
        if path.is_file():
            source = path.read_text(encoding='utf-8', errors='replace')
            index = re.search(r'^#define ROMFS_INDEX_SHA256 "([0-9a-fA-F]{64})"', source, re.M)
            image = re.search(r'^#define ROMFS_IMAGE_SHA256 "([0-9a-fA-F]{64})"', source, re.M)
            if index and image and index.group(1).lower() == compiled_digest:
                return image.group(1).lower()
    return ''


def choose_packed(repo: Path, input_path: Path) -> Path:
    image = input_path / 'romfs.bin' if input_path.is_dir() else input_path
    if not image.is_file() or image.stat().st_size <= 0:
        raise FileNotFoundError(f'packed image missing: {image}')
    compiled = index_binary(image)
    prior = find_prior_manifest(repo, image, compiled)
    if prior is None:
        if not compiled:
            raise ValueError(f'no index.json or romfs-index.bin accompanying {image}')
        files, digest = recover_compiled_index(compiled, image.stat().st_size)
        sha = cached_image_hash(repo, digest)
        prior = {
            'status': 'pass', 'backend': 'packed', 'romfs_size': image.stat().st_size,
            'romfs_sha256': sha,
            'source': 'Recovered from existing compiled TOTKRFS1 index; image bytes NOT rehashed',
            'files': files,
        }
        print(f'[INDEX] Restored {len(files):,} file entries from {compiled} ({digest[:16]}...)')
        if not sha:
            print('[WARN] No trusted prior image SHA-256 metadata. Full RomFS hash verification is unavailable.')
    # Older inventories may omit this optional hash; an empty string explicitly
    # means no full-image SHA has been verified in this build.
    prior.setdefault('romfs_sha256', '')
    # If output has an image but no accompanying index, write *metadata only* there.
    same_bytes_write(image.parent / 'index.json',
                     (json.dumps(prior, separators=(',', ':'), ensure_ascii=False) + '\n').encode('utf-8'))
    return image.parent.resolve()


def choose_loose(repo: Path, romfs: Path) -> Path:
    if not romfs.is_dir():
        raise FileNotFoundError(f'extracted RomFS folder missing: {romfs}')
    staged = repo / 'build' / 'extracted' / 'mkw180' / 'romfs-loose'
    manifest = staged / 'index.json'
    pointer = staged / 'loose-root.txt'
    root = str(romfs.resolve())
    if manifest.is_file() and pointer.is_file():
        try:
            existing = json.loads(manifest.read_text(encoding='utf-8'))
            if (existing.get('backend') == LOOSE_BACKEND and
                    existing.get('status') == 'inventory-only' and
                    pointer.read_text(encoding='utf-8').strip() == root and existing.get('files')):
                print(f'[REUSE] Existing loose-file inventory: {len(existing["files"]):,} files')
                return staged
        except (OSError, ValueError, TypeError):
            pass
    files = []
    total = 0
    for parent, dirs, names in os.walk(romfs):
        dirs.sort()
        for name in sorted(names):
            if name.startswith('.'):
                continue
            file = Path(parent) / name
            if not file.is_file():
                continue
            rel = file.relative_to(romfs).as_posix()
            if rel.startswith('/') or '..' in rel.split('/') or '\\' in rel:
                raise ValueError(f'invalid loose RomFS path: {rel}')
            size = file.stat().st_size
            files.append({'path': rel, 'offset': total, 'size': size})
            total += size
    if not files:
        raise ValueError(f'no extracted RomFS files in {romfs}')
    files.sort(key=lambda x: x['path'].encode('utf-8'))
    doc = {'status': 'inventory-only', 'backend': LOOSE_BACKEND,
           'source': 'indexed original extracted directory; NO packed image made',
           'romfs_size': total, 'romfs_sha256': '', 'files': files}
    same_bytes_write(manifest, (json.dumps(doc, separators=(',', ':'), ensure_ascii=False) + '\n').encode())
    same_bytes_write(pointer, (root + '\n').encode())
    print(f'[LOOSE] Indexed {len(files):,} extracted files, no packing, no file copies')
    return staged


def select(repo: Path, packed: Path | None, dump: Path | None, auto: bool) -> tuple[Path, str]:
    if packed:
        try:
            chosen = choose_packed(repo, packed)
            return chosen, 'packed'
        except (OSError, ValueError) as exc:
            if not auto:
                raise
            print(f'[FALLBACK] Packed RomFS unusable: {exc}')
    if not auto or dump is None:
        raise ValueError('no packed RomFS selected; pass --auto --dump to allow loose fallback')
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import mkw_probe
    _, romfs = mkw_probe.locate(dump)
    if romfs is None:
        raise ValueError('the extracted update contains no romfs directory')
    return choose_loose(repo, romfs), LOOSE_BACKEND


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--repo', required=True, type=Path)
    parser.add_argument('--packed', type=Path, help='existing romfs.bin path or directory')
    parser.add_argument('--auto', action='store_true', help='use extracted loose files if no usable packed image')
    parser.add_argument('--dump', type=Path, help='game dump root, including updates/917504')
    args = parser.parse_args(argv)
    try:
        repo = args.repo.resolve()
        chosen, backend = select(repo, args.packed, args.dump, args.auto)
        target = repo / 'build' / 'mkw-local' / 'selected-romfs.json'
        same_bytes_write(target, (json.dumps({'path': str(chosen), 'backend': backend}, indent=2) + '\n').encode())
        print(f'[NO-REPACK] RomFS backend={backend}: {chosen}')
        return 0
    except (OSError, ValueError) as exc:
        parser.exit(2, f'[ERROR] RomFS selection: {exc}\nNo packing performed.\n')


if __name__ == '__main__':
    raise SystemExit(main())
