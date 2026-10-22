import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

spec = importlib.util.spec_from_file_location(
    "make_manifest", Path(__file__).resolve().parents[1] / "tools" / "make_manifest.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def decode_hex(text):
    """Independent reader: validate record checksums, ranges and sparse bytes."""
    memory = {}
    upper = 0
    ended = False
    for line in text.splitlines():
        if ended or not line.startswith(":"):
            raise ValueError("invalid HEX record order")
        record = bytes.fromhex(line[1:])
        count, address, kind = struct.unpack_from(">BHB", record)
        if len(record) != count + 5 or sum(record) & 0xFF:
            raise ValueError("invalid HEX length/checksum")
        payload = record[4:-1]
        if kind == 0:
            if address + count > 0x10000:
                raise ValueError("HEX data crosses a 64 KiB boundary")
            for offset, value in enumerate(payload):
                absolute = upper + address + offset
                if absolute in memory:
                    raise ValueError("overlapping HEX records")
                memory[absolute] = value
        elif kind == 4:
            upper = struct.unpack(">H", payload)[0] << 16
        elif kind == 1:
            ended = True
        else:
            raise ValueError("unexpected HEX record type")
    if not ended:
        raise ValueError("missing HEX end record")
    return memory


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

    def test_packages_from_cli(self):
        # Cross a 64 KiB boundary and leave an unaligned final word.
        image = struct.pack("<II", 0x20020000, 0x08020009) + bytes(0x10003)
        bootloader = struct.pack("<II", 0x20020000, 0x08000009) + bytes(43)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "app.bin").write_bytes(image)
            (root / "boot.bin").write_bytes(bootloader)
            subprocess.run([
                sys.executable, "-B", str(Path(module.__file__)), str(root / "app.bin"),
                "--version", "0x2A", "--output", str(root / "manifest.bin"),
                "--installed-hex", str(root / "installed.hex"),
                "--update-hex", str(root / "update.hex"),
                "--bootloader", str(root / "boot.bin"),
                "--factory-hex", str(root / "factory.hex"),
            ], check=True, capture_output=True, text=True)
            manifest = (root / "manifest.bin").read_bytes()
            self.assertEqual(manifest, module.make_manifest(image, 42))
            installed = decode_hex((root / "installed.hex").read_text())
            update = decode_hex((root / "update.hex").read_text())
            factory = decode_hex((root / "factory.hex").read_text())

        def region(address, data):
            return dict(enumerate(data, address))

        # Exact maps also prove reserved sectors and padding are not emitted.
        expected = region(0x08020000, image) | region(0x08008000, manifest)
        expected |= region(0x0800C01C, bytes(4))
        self.assertEqual(installed, expected)
        self.assertEqual(update, region(0x08080000, image) | region(0x0800C000, manifest))
        self.assertEqual(factory, region(0x08000000, bootloader) | expected)

    def test_invalid_version_and_bootloader(self):
        app = struct.pack("<II", 0x20020000, 0x08020009) + bytes(32)
        for version in (-1, 0x100000000):
            with self.subTest(version=version), self.assertRaises(ValueError):
                module.make_manifest(app, version)
        for loader in (b"", bytes(0x4001), app,
                       struct.pack("<II", 0x20020000, 0x08000008) + bytes(32)):
            with self.subTest(size=len(loader)), self.assertRaises(ValueError):
                module.validate_bootloader(loader)

    def test_hex_rejects_overlapping_regions(self):
        with self.assertRaises(ValueError):
            module.make_hex([(0x08000000, bytes(32)), (0x08000010, bytes(32))])


if __name__ == "__main__":
    unittest.main()
