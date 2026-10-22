"""Create the shared 32-byte manifest for an APP linked at 0x08020000."""

import argparse
from pathlib import Path
import re
import struct
import zlib

# The C protocol header is shared with packaging; do not duplicate addresses.
PROTOCOL = {
    name: int(value, 0)
    for name, value in re.findall(
        r"^#define\s+(BOOT_\w+)\s+UINT32_C\((0x[0-9A-Fa-f]+|[0-9]+)\)",
        (Path(__file__).resolve().parents[1] / "Core/Inc/boot_protocol.h").read_text(),
        re.MULTILINE,
    )
}
APP_ADDRESS = PROTOCOL["BOOT_APP_ADDRESS"]
IMAGE_CAPACITY = PROTOCOL["BOOT_IMAGE_CAPACITY"]
MAGIC = PROTOCOL["BOOT_IMAGE_MAGIC"]
COMMITTED = PROTOCOL["BOOT_IMAGE_COMMITTED"]


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
    header = struct.pack("<IIIII", MAGIC, PROTOCOL["BOOT_IMAGE_FORMAT"],
                         len(image), zlib.crc32(image), version)
    return header + struct.pack("<III", zlib.crc32(header), COMMITTED, 0xFFFFFFFF)


def make_hex(segments: list[tuple[int, bytes]]) -> str:
    """Encode sparse Flash regions without filling the gaps between partitions."""
    def record(address: int, kind: int, data: bytes) -> str:
        body = struct.pack(">BHB", len(data), address, kind) + data
        return ":" + (body + bytes([-sum(body) & 0xFF])).hex().upper()

    lines = []
    upper = None
    previous_end = 0
    for address, data in sorted(segments):
        if address < previous_end or address + len(data) > 0x100000000:
            raise ValueError("HEX regions overlap or exceed the 32-bit address space")
        previous_end = address + len(data)
        offset = 0
        while offset < len(data):
            current = address + offset
            if current >> 16 != upper:
                upper = current >> 16
                lines.append(record(0, 4, struct.pack(">H", upper)))
            count = min(16, len(data) - offset, 0x10000 - (current & 0xFFFF))
            lines.append(record(current & 0xFFFF, 0, data[offset:offset + count]))
            offset += count
    lines.append(record(0, 1, b""))
    return "\n".join(lines) + "\n"


def installed_segments(image: bytes, manifest: bytes) -> list[tuple[int, bytes]]:
    return [
        (PROTOCOL["BOOT_INSTALLED_ADDRESS"], manifest),
        # Cancel any stale pending request so it cannot overwrite this APP.
        (PROTOCOL["BOOT_REQUEST_ADDRESS"] + 28, struct.pack("<I", PROTOCOL["BOOT_REQUEST_CONSUMED"])),
        (APP_ADDRESS, image),
    ]


def validate_bootloader(image: bytes) -> None:
    if not 8 <= len(image) <= PROTOCOL["BOOT_LOADER_SIZE"]:
        raise ValueError("Bootloader must fit its reserved Flash partition")
    stack, reset = struct.unpack_from("<II", image)
    base = PROTOCOL["BOOT_FLASH_BASE"]
    if stack & 7 or not (
        0x20000000 < stack <= 0x20020000 or 0x10000000 < stack <= 0x10010000
    ) or not reset & 1 or not base + 8 <= (reset & ~1) < base + len(image):
        raise ValueError("Bootloader has an invalid initial MSP or Reset_Handler")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="raw APP .bin, without extra headers")
    parser.add_argument("--version", type=lambda text: int(text, 0), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--installed-hex", type=Path,
                        help="APP + installed metadata + cancelled old request, for SWD programming")
    parser.add_argument("--update-hex", type=Path,
                        help="staged APP + pending request, for SWD installation testing")
    parser.add_argument("--bootloader", type=Path, help="raw Bootloader .bin for factory package")
    parser.add_argument("--factory-hex", type=Path, help="Bootloader + installed APP; requires --bootloader")
    args = parser.parse_args()
    if bool(args.bootloader) != bool(args.factory_hex):
        parser.error("--bootloader and --factory-hex must be supplied together")
    try:
        image = args.image.read_bytes()
        manifest = make_manifest(image, args.version)
        packages = []
        if args.installed_hex:
            packages.append((args.installed_hex, make_hex(installed_segments(image, manifest))))
        if args.update_hex:
            packages.append((args.update_hex, make_hex([
                (PROTOCOL["BOOT_REQUEST_ADDRESS"], manifest),
                (PROTOCOL["BOOT_STAGE_ADDRESS"], image),
            ])))
        if args.factory_hex:
            bootloader = args.bootloader.read_bytes()
            validate_bootloader(bootloader)
            packages.append((args.factory_hex, make_hex([
                (PROTOCOL["BOOT_FLASH_BASE"], bootloader),
                *installed_segments(image, manifest),
            ])))
        args.output.write_bytes(manifest)
        for path, content in packages:
            path.write_text(content, encoding="ascii")
    except (OSError, ValueError) as error:
        parser.error(str(error))
    _, _, size, crc, version, _, _, _ = struct.unpack("<8I", manifest)
    print(f"size={size}, crc32=0x{crc:08X}, version={version}")
    print(f"APP: 0x{APP_ADDRESS:08X}; staging: 0x{PROTOCOL['BOOT_STAGE_ADDRESS']:08X}")
    print(f"manifest: installed 0x{PROTOCOL['BOOT_INSTALLED_ADDRESS']:08X} "
          f"OR update request 0x{PROTOCOL['BOOT_REQUEST_ADDRESS']:08X}")
    for path, _ in packages:
        print(f"HEX: {path}")


if __name__ == "__main__":
    main()
