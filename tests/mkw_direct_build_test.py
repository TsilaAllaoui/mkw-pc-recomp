"""Offline tests for the direct MKW target; no game data or GPU required."""
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


overlays = load('mkw_generated_overlays')
selector = load('mkw_select_romfs')


class DirectBuildTests(unittest.TestCase):
    def test_entrypoint_uses_this_dkb_tree(self):
        build = (ROOT / 'BUILD-MKW.bat').read_text(encoding='utf-8')
        self.assertIn('mkw_cli.py generate --repo .', build)
        self.assertIn('mkw_cli.py build --repo .', build)
        self.assertNotIn('workspace\\core', build)
        self.assertNotIn('standalone_builder', build)

    def test_generated_wrappers_use_integrated_dkb_runtime(self):
        source = '#include "cpu.h"\nvoid f_70003e3550(Cpu *c) { c->x[0] = 1; }\n'
        result = overlays.patch_chunk(source, [(0x70003E3550, 'primary_alloc')])
        self.assertIn('dkb_heap_owner_record', result)
        self.assertIn('dkb_original_primary_alloc_70003e3550', result)
        self.assertNotIn('mkw_heap_owner_record', result)

    def test_runtime_exports_generated_overlay_abi(self):
        runtime = (ROOT / 'runtime' / 'dkb_heap_owner.c').read_text(encoding='utf-8')
        self.assertIn('void dkb_heap_owner_record(', runtime)
        self.assertIn('void dkb_heap_owner_discard(', runtime)

    def test_packed_selection_records_path_only(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            packed = root / 'packed'
            packed.mkdir()
            (packed / 'romfs.bin').write_bytes(b'local')
            (packed / 'index.json').write_text(json.dumps({
                'status': 'pass', 'romfs_size': 5, 'files': [{'path': 'x', 'offset': 0, 'size': 5}]
            }), encoding='utf-8')
            output = root / 'repo' / 'build' / 'mkw-local'
            output.mkdir(parents=True)
            old_argv = sys.argv
            try:
                sys.argv = ['mkw_select_romfs.py', '--repo', str(root/'repo'), '--packed', str(packed)]
                self.assertEqual(selector.main(), 0)
            finally:
                sys.argv = old_argv
            record = json.loads((output / 'selected-romfs.json').read_text(encoding='utf-8'))
            self.assertEqual(Path(record['path']), packed.resolve())
            self.assertFalse((root/'repo'/'romfs.bin').exists())


if __name__ == '__main__':
    unittest.main()
