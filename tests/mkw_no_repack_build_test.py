"""No-repack selection regression checks. All tests use tiny synthetic files."""
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


selector = load('mkw_select_romfs')
cli = load('mkw_cli')
probe = load('mkw_probe')


class NoRepackTests(unittest.TestCase):
    def test_build_never_invokes_pack(self):
        build = (ROOT / 'BUILD-MKW.bat').read_text()
        self.assertNotIn('mkw_cli.py pack', build)
        self.assertNotIn('dkb_pack_romfs.py', build)
        self.assertIn('mkw_select_romfs.py', build)
        self.assertIn('--auto', build)
        self.assertIn('mkw_cli.py build --repo .', build)
        self.assertIn('mkw_cli.py generate --repo .', build)

    def make_tree(self, root):
        repo = root / 'repo'
        repo.mkdir()
        game = root / 'game' / 'updates' / '917504'
        (game / 'exefs').mkdir(parents=True)
        (game / 'exefs' / 'main').write_bytes(b'NSO0')
        (game / 'exefs' / 'main.npdm').write_bytes(b'META')
        (game / 'romfs').mkdir()
        return repo, game

    def test_prefer_packed_over_loose_and_never_rewrite_image(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            repo, game = self.make_tree(root)
            (game / 'romfs' / 'a').write_bytes(b'different')
            packed = root / 'output' / 'romfs'
            packed.mkdir(parents=True)
            image = packed / 'romfs.bin'
            image.write_bytes(b'1234567')
            manifest = {'status':'pass', 'romfs_size':7, 'romfs_sha256': 'x'*64,
                        'files':[{'path':'a','offset':0,'size':7}]}
            (packed/'index.json').write_text(json.dumps(manifest))
            contents = image.read_bytes()
            chosen, backend = selector.select(repo, image, root / 'game', True)
            self.assertEqual((chosen,backend),(packed.resolve(),'packed'))
            self.assertEqual(image.read_bytes(),contents)
            self.assertFalse((repo/'build'/'extracted'/'mkw180'/'romfs-loose').exists())

    def test_loose_fallback_and_reuse_without_packing(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            repo, game = self.make_tree(root)
            (game / 'romfs' / 'System').mkdir()
            (game / 'romfs' / 'System' / 'A.txt').write_bytes(b'abc')
            (game / 'romfs' / 'z').write_bytes(b'defg')
            chosen, backend = selector.select(repo, root/'missing.bin', root/'game', True)
            self.assertEqual(backend,'dkb-loose-v1')
            index = json.loads((chosen/'index.json').read_text())
            self.assertEqual(index['romfs_size'],7)
            self.assertEqual([entry['path'] for entry in index['files']], ['System/A.txt','z'])
            self.assertEqual((chosen/'loose-root.txt').read_text().strip(), str((game/'romfs').resolve()))
            chosen2, backend2 = selector.select(repo, root/'missing.bin', root/'game', True)
            self.assertEqual((chosen2,backend2),(chosen,backend))
            self.assertEqual(len(list(repo.rglob('romfs.bin'))),0)
            import romfs_index
            count, _, _ = romfs_index.write(chosen, repo/'built')
            self.assertEqual(count, 2)
            self.assertIn('#define ROMFS_LOOSE 1', (repo/'built'/'romfs_config.h').read_text())

    def test_recover_compiled_index_and_preserve_image(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            repo, _ = self.make_tree(root)
            image_dir = root / 'output' / 'romfs'
            image_dir.mkdir(parents=True)
            image = image_dir / 'romfs.bin'
            image.write_bytes(b'ABCDEFGH')
            binary = root / 'output' / 'romfs-index.bin'
            binary.write_bytes(struct.pack('<8sQQ',b'TOTKRFS1',2,72) +
                               struct.pack('<QQII',0,4,0,2) +
                               struct.pack('<QQII',4,4,2,2) + b'a\x00b\x00')
            chosen, backend = selector.select(repo, image, None, False)
            self.assertEqual(backend,'packed')
            self.assertEqual(chosen,image_dir.resolve())
            doc = json.loads((image_dir/'index.json').read_text())
            self.assertEqual(len(doc['files']),2)
            self.assertEqual(doc['romfs_sha256'],'')
            self.assertEqual(image.read_bytes(),b'ABCDEFGH')
            # This is exactly the format consumed by the native DKB build.
            import romfs_index
            count, _, _ = romfs_index.write(image_dir, root/'compiled')
            self.assertEqual(count, 2)
            self.assertIn('ROMFS_IMAGE_SIZE 8ull', (root/'compiled'/'romfs_config.h').read_text())

    def test_reject_invalid_compiled_index_without_repacking(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            repo, game = self.make_tree(root)
            (game/'romfs'/'a').write_bytes(b'abc')
            packed = root/'output'/'romfs'
            packed.mkdir(parents=True)
            (packed/'romfs.bin').write_bytes(b'packed')
            (root/'output'/'romfs-index.bin').write_bytes(b'bad')
            chosen, backend = selector.select(repo, packed/'romfs.bin', root/'game', True)
            self.assertEqual(backend,selector.LOOSE_BACKEND)
            self.assertFalse((packed/'index.json').exists())

    def test_original_file_selection_requires_no_packing(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            repo, game = self.make_tree(root)
            image_dir = root/'packed'
            image_dir.mkdir()
            (image_dir/'romfs.bin').write_bytes(b'ab')
            (image_dir/'index.json').write_text(json.dumps({
                'status':'pass', 'romfs_size':2,
                'files':[{'path':'file','offset':0,'size':2}]}))
            chosen, backend = selector.select(repo, image_dir, root/'game', False)
            self.assertEqual(backend,'packed')
            self.assertEqual(chosen,image_dir.resolve())

    def test_declared_backend_matches_romfs_index_and_runtime(self):
        self.assertIn("loose = manifest.get('backend') == 'dkb-loose-v1'", (ROOT/'tools'/'romfs_index.py').read_text())
        self.assertIn('ROMFS_LOOSE', (ROOT/'runtime'/'romfs.c').read_text())
        self.assertIn("'dkb-loose-v1'", (ROOT/'tools'/'mkw_cli.py').read_text())

    def test_dynamic_single_update_directory(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            update = root/'updates'/'version-180'
            (update/'exefs').mkdir(parents=True)
            (update/'exefs'/'main').write_bytes(b'NSO0')
            (update/'exefs'/'main.npdm').write_bytes(b'META')
            (update/'romfs').mkdir()
            self.assertEqual(probe.locate(root)[0], update/'exefs')

    def test_ambiguous_update_fails(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for name in ('v1','v2'):
                exefs = root/'updates'/name/'exefs'
                exefs.mkdir(parents=True)
                (exefs/'main').write_bytes(b'NSO0')
                (exefs/'main.npdm').write_bytes(b'META')
            with self.assertRaisesRegex(ValueError,'Several update'):
                probe.locate(root)


if __name__ == '__main__':
    unittest.main()
