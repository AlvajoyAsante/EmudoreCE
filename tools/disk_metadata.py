"""Optional ECDINFO file types bound to the complete logical DSK payload."""

from __future__ import annotations

import struct
import zlib

VARIABLE_NAME = "ECDINFO"
VERSION = 1
FILE_TYPE_LOCKED = 0x40
HEADER = struct.Struct("<4sBBHII")
CHECKSUM = struct.Struct("<I")


def variable_name(package_id: str) -> str:
    if not 1 <= len(package_id) <= 4 or not package_id.isalnum() or package_id != package_id.upper():
        raise ValueError("disk metadata requires a 1-4 character uppercase alphanumeric package ID")
    return f"EC{package_id}IN"


def _validate_binding(payload: bytes, file_types: bytes) -> None:
    count = len(file_types)
    if not 1 <= count <= 255:
        raise ValueError("disk metadata requires 1..255 file types")
    if not payload or payload[0] != count:
        raise ValueError("disk metadata file count does not match the DSK payload")
    if not 1 + count * 22 <= len(payload) <= 0xFFFFFFFF:
        raise ValueError("invalid DSK payload length for disk metadata")
    if any((file_type & 0x87) != 0x82 for file_type in file_types):
        raise ValueError("disk metadata file types must be closed PRGs")


def build_metadata(payload: bytes, file_types: bytes) -> bytes:
    """Encode original type bytes in directory order, bound to the final DSK."""
    _validate_binding(payload, file_types)
    data = HEADER.pack(b"ECFI", VERSION, len(file_types), 0, len(payload), zlib.crc32(payload))
    data += file_types
    return data + CHECKSUM.pack(zlib.crc32(data))


def validate_metadata(data: bytes, payload: bytes) -> bytes:
    """Return type bytes after validating the format, checksums and DSK binding."""
    if len(data) < HEADER.size + CHECKSUM.size:
        raise ValueError("truncated disk metadata")
    magic, version, count, reserved, payload_size, payload_crc = HEADER.unpack_from(data)
    if magic != b"ECFI" or version != VERSION or reserved != 0:
        raise ValueError("invalid disk metadata header")
    if count == 0 or len(data) != HEADER.size + count + CHECKSUM.size:
        raise ValueError("invalid disk metadata length or file count")
    if CHECKSUM.unpack_from(data, len(data) - CHECKSUM.size)[0] != zlib.crc32(data[:-CHECKSUM.size]):
        raise ValueError("disk metadata CRC32 mismatch")
    file_types = data[HEADER.size:-CHECKSUM.size]
    _validate_binding(payload, file_types)
    if payload_size != len(payload):
        raise ValueError("DSK payload length mismatch")
    if payload_crc != zlib.crc32(payload):
        raise ValueError("DSK payload CRC32 mismatch")
    return file_types