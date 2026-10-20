#!/usr/bin/env python3
"""Purpose: Build/inspect unsigned ECB package v1 with the Python standard library.

Summary: Validate images and manifests against slot limits shared by the legacy
brain and the shared-updater IDU layout (the smaller pkg partition wins).
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

MAGIC = b"ECBPKG\0\0"
VERSION = 1
HEADER = struct.Struct("<8sHHHHIIII")
ENTRY = struct.Struct("<BBBBIIHH32s32s16s")
CHECK_OFFSET = 24
BRAIN, NODE = 1, 2
MAX_NODES, MAX_NAME, MAX_CAN_ID = 8, 32, 0x7FF
DEFAULT_CMD, DEFAULT_REPLY = 0x79E, 0x79F
BRAIN_LIMIT, NODE_LIMIT, PACKAGE_LIMIT = 0x300000, 0x1C0000, 0x780000
IMAGE_HEADER_SIZE, CHIP_ID_OFFSET, S3_CHIP_ID = 24, 12, 9
APP_DESC_OFFSET, APP_DESC_MAGIC = 32, 0xABCD5432
ALIGNMENT = 4


def check_image(data, kind):
    limit = BRAIN_LIMIT if kind == BRAIN else NODE_LIMIT
    if not data or len(data) > limit:
        raise ValueError(f"{'brain' if kind == BRAIN else 'node'} image must be 1..{limit} bytes")
    if (len(data) < APP_DESC_OFFSET + 4 or data[0] != 0xE9 or
            not 1 <= data[1] <= 16 or
            struct.unpack_from('<H', data, CHIP_ID_OFFSET)[0] != S3_CHIP_ID or
            struct.unpack_from('<I', data, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC):
        raise ValueError("expected an ESP32-S3 application .bin (0xE9, chip id 9, app descriptor); not a bootloader or merged image")
    offset = IMAGE_HEADER_SIZE
    for _ in range(data[1]):
        if offset + 8 > len(data):
            raise ValueError('truncated ESP image segment header')
        size = struct.unpack_from('<I', data, offset + 4)[0]
        if size % ALIGNMENT or size > len(data) - offset - 8:
            raise ValueError('invalid ESP image segment length')
        offset += 8 + size
    image_end = (offset + 16) & ~15  # checksum byte, padded to a 16-byte boundary
    if data[23] not in (0, 1) or image_end + 32 * data[23] != len(data):
        raise ValueError('expected exact unsigned ESP app image length; no trailing padding or signatures')


def manifest_check(data):
    raw = bytearray(data)
    raw[CHECK_OFFSET:CHECK_OFFSET + 4] = bytes(4)
    return zlib.crc32(raw)


def can_id(text):
    base = 16 if text.lower().startswith('0x') else 10
    digits = text[2:] if base == 16 else text
    allowed = '0123456789abcdef' if base == 16 else '0123456789'
    if not digits or any(c.lower() not in allowed for c in digits):
        raise ValueError(f"invalid CAN ID {text!r}: use decimal or 0x-prefixed hex")
    value = int(digits, base)
    if value > MAX_CAN_ID:
        raise ValueError("CAN IDs must be 0..0x7FF")
    return value


def node_spec(text):
    path, *options = text.split(',')
    values = {'cmd': DEFAULT_CMD, 'reply': DEFAULT_REPLY, 'name': ''}
    seen = set()
    for option in options:
        key, sep, value = option.partition('=')
        if not sep or key not in values or key in seen:
            raise ValueError(f"invalid or repeated node option {option!r}")
        seen.add(key)
        values[key] = value if key == 'name' else can_id(value)
    try:
        name = values['name'].encode('ascii')
    except UnicodeEncodeError as exc:
        raise ValueError('node name must be ASCII') from exc
    if len(name) > MAX_NAME or b'\0' in name:
        raise ValueError('node name must be at most 32 ASCII bytes, without NUL')
    return Path(path), values['cmd'], values['reply'], name


def build(args):
    sources = []
    if args.brain:
        sources.append((BRAIN, Path(args.brain), 0, 0, b''))
    if len(args.node) > MAX_NODES:
        raise ValueError('at most eight node entries are allowed')
    sources.extend((NODE, *node_spec(spec)) for spec in args.node)
    if not sources:
        raise ValueError('provide --brain and/or at least one --node')
    manifest_size = HEADER.size + ENTRY.size * len(sources)
    offset = manifest_size
    table, payloads = bytearray(), bytearray()
    for kind, path, cmd, reply, name in sources:
        limit = BRAIN_LIMIT if kind == BRAIN else NODE_LIMIT
        if path.stat().st_size > limit:
            raise ValueError(f'{path}: image exceeds {limit}-byte slot')
        data = path.read_bytes()
        check_image(data, kind)
        padding = (-offset) % ALIGNMENT
        payloads.extend(bytes(padding))
        offset += padding
        table.extend(ENTRY.pack(kind, 0, len(name), 0, offset, len(data), cmd, reply,
                                hashlib.sha256(data).digest(), name, bytes(16)))
        payloads.extend(data)
        offset += len(data)
    if offset > PACKAGE_LIMIT:
        raise ValueError(f'package is {offset} bytes; pkg partition limit is {PACKAGE_LIMIT}')
    manifest = bytearray(HEADER.pack(MAGIC, VERSION, HEADER.size, ENTRY.size,
                                     len(sources), manifest_size, offset, 0, 0)) + table
    struct.pack_into('<I', manifest, CHECK_OFFSET, manifest_check(manifest))
    Path(args.output).write_bytes(manifest + payloads)
    print(f'{args.output}: {len(sources)} entries, {offset} bytes')


def inspect(data):
    if len(data) < HEADER.size:
        raise ValueError('truncated header')
    magic, version, hs, es, count, ms, total, check, flags = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION:
        raise ValueError('bad magic or unsupported version')
    if (hs != HEADER.size or es != ENTRY.size or not 1 <= count <= MAX_NODES + 1 or
            ms != hs + count * es or total != len(data) or ms > total or flags):
        raise ValueError('invalid manifest layout/count/flags/total')
    if manifest_check(data[:ms]) != check:
        raise ValueError('manifest CRC32 mismatch')
    results, ranges = [], []
    brains = nodes = 0
    for i in range(count):
        kind, ef, nl, reserved, off, size, cmd, reply, sha, name, tail = ENTRY.unpack_from(data, hs + i * es)
        if kind not in (BRAIN, NODE) or ef or reserved or any(tail):
            raise ValueError(f'entry {i}: invalid type/flags/reserved')
        brains += kind == BRAIN
        nodes += kind == NODE
        if brains > 1 or nodes > MAX_NODES:
            raise ValueError('too many brain or node entries')
        if nl > MAX_NAME or any(name[nl:]) or any(c == 0 or c > 127 for c in name[:nl]):
            raise ValueError(f'entry {i}: invalid name')
        if (kind == BRAIN and (cmd or reply or nl)) or max(cmd, reply) > MAX_CAN_ID:
            raise ValueError(f'entry {i}: invalid CAN IDs or brain-only fields')
        if off % ALIGNMENT or off < ms or size == 0 or off + size > total:
            raise ValueError(f'entry {i}: invalid payload bounds/alignment/size')
        if any(off < end and start < off + size for start, end in ranges):
            raise ValueError(f'entry {i}: overlapping payloads')
        ranges.append((off, off + size))
        payload = data[off:off + size]
        if hashlib.sha256(payload).digest() != sha:
            raise ValueError(f'entry {i}: SHA-256 mismatch')
        check_image(payload, kind)
        results.append(dict(index=i, type='BRAIN' if kind == BRAIN else 'NODE', offset=off,
                            size=size, sha256=sha.hex(), cmd_id=cmd, reply_id=reply,
                            name=name[:nl].decode('ascii')))
    return dict(version=version, size=total, manifest_crc32=f'{check:08x}', entries=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    b = commands.add_parser('build')
    b.add_argument('--brain')
    b.add_argument('--node', action='append', default=[])
    b.add_argument('-o', '--output', required=True)
    info = commands.add_parser('info')
    info.add_argument('package')
    args = parser.parse_args()
    try:
        if args.command == 'build':
            build(args)
        else:
            path = Path(args.package)
            if path.stat().st_size > PACKAGE_LIMIT:
                raise ValueError(f'package exceeds {PACKAGE_LIMIT}-byte pkg partition')
            print(json.dumps(inspect(path.read_bytes()), indent=2))
    except (OSError, ValueError, struct.error) as exc:
        parser.exit(2, f'ecb_pkg: {exc}\n')


if __name__ == '__main__':
    main()
