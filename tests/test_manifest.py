import importlib.util
from pathlib import Path
import struct
import unittest
import zlib

spec = importlib.util.spec_from_file_location(
    "make_manifest", Path(__file__).resolve().parents[1] / "tools" / "make_manifest.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ManifestTest(unittest.TestCase):
    def test_wire_format(self):
        image = struct.pack("<II", 0x20020000, 0x08020009) + bytes(range(59))
        wire = module.make_manifest(image, 42)
        magic, fmt, size, crc, version, header_crc, commit, consumed = struct.unpack("<8I", wire)
        self.assertEqual((magic, fmt, size, version, commit, consumed),
                         (0x424C4657, 1, len(image), 42, 0xB007C0DE, 0xFFFFFFFF))
        self.assertEqual(crc, zlib.crc32(image))
        self.assertEqual(header_crc, zlib.crc32(wire[:20]))

    def test_reject_invalid_image(self):
        images = [b"", b"\xff" * 32, b"\xff" * (module.IMAGE_CAPACITY + 1),
                  struct.pack("<II", 0x20020000, 0x08000009) + bytes(32),
                  struct.pack("<II", 0x20020000, 0x08020008) + bytes(32)]
        for image in images:
            with self.subTest(size=len(image)), self.assertRaises(ValueError):
                module.make_manifest(image, 1)


if __name__ == "__main__":
    unittest.main()
