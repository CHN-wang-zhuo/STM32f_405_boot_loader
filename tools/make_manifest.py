"""Create the shared 32-byte manifest for an APP linked at 0x08020000."""

import argparse
from pathlib import Path
import struct
import zlib

APP_ADDRESS = 0x08020000
IMAGE_CAPACITY = 0x60000
MAGIC = 0x424C4657
COMMITTED = 0xB007C0DE


def make_manifest(image: bytes, version: int) -> bytes:
    if not 8 <= len(image) <= IMAGE_CAPACITY:
        raise ValueError("APP size must be 8..393216 bytes")
    if not 0 <= version <= 0xFFFFFFFF:
        raise ValueError("version must fit uint32")
    stack, reset = struct.unpack_from("<II", image)
    if stack & 7 or not (
        0x20000000 < stack <= 0x20020000 or 0x10000000 < stack <= 0x10010000
    ):
        raise ValueError("APP initial MSP must be 8-byte aligned inside SRAM/CCM")
    if not reset & 1 or not APP_ADDRESS + 8 <= (reset & ~1) < APP_ADDRESS + len(image):
        raise ValueError("APP Reset_Handler must be Thumb code linked at 0x08020000")
    header = struct.pack("<IIIII", MAGIC, 1, len(image), zlib.crc32(image), version)
    return header + struct.pack("<III", zlib.crc32(header), COMMITTED, 0xFFFFFFFF)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="raw APP .bin, without extra headers")
    parser.add_argument("--version", type=lambda text: int(text, 0), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = make_manifest(args.image.read_bytes(), args.version)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    args.output.write_bytes(manifest)
    _, _, size, crc, version, _, _, _ = struct.unpack("<8I", manifest)
    print(f"size={size}, crc32=0x{crc:08X}, version={version}")
    print("APP: 0x08020000; staging: 0x08080000")
    print("manifest: installed 0x08008000 OR update request 0x0800C000")


if __name__ == "__main__":
    main()
