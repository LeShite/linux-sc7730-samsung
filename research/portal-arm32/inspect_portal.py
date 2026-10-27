#!/usr/bin/env python3
"""Read-only inventory of Valve's Portal Linux tarball; extracts no files."""

import argparse
import collections
import os
import re
import struct
import tarfile


ROOT = "Portal_Linux/"
ELF_PATHS = {
    "Portal_Linux/game/hl2_linux",
    "Portal_Linux/game/bin/engine.so",
    "Portal_Linux/game/portal/bin/client.so",
    "Portal_Linux/game/portal/bin/server.so",
}
VPK_DIR = "Portal_Linux/game/portal/portal_pak_dir.vpk"
BSP_SAMPLE = "Portal_Linux/game/portal/maps/testchmb_a_00.bsp"
GAMEINFO = "Portal_Linux/game/portal/gameinfo.txt"
BSP_LUMP_NAMES = {
    1: "planes", 2: "texture data", 3: "vertices", 5: "BSP nodes",
    6: "texture info", 7: "faces", 12: "edges", 13: "surfedges",
    14: "models", 16: "leaf faces", 17: "leaf brushes", 18: "brushes",
    19: "brush sides", 20: "areas", 21: "area portals",
}
BSP_LUMP_STRIDES = {
    1: 20, 2: 32, 3: 12, 5: 32, 6: 72, 7: 56, 12: 4, 13: 4,
    14: 48, 16: 2, 17: 2, 18: 12, 19: 8, 20: 8, 21: 12,
}


def read_member(tf, member, limit=None):
    stream = tf.extractfile(member)
    if stream is None:
        return b""
    with stream:
        return stream.read() if limit is None else stream.read(limit)


def parse_elf(data):
    if len(data) < 20 or data[:4] != b"\x7fELF":
        return "not ELF"
    bits = {1: 32, 2: 64}.get(data[4], "?")
    endian = "<" if data[5] == 1 else ">" if data[5] == 2 else None
    if endian is None:
        return f"ELF{bits}, unknown byte order"
    machine = struct.unpack_from(endian + "H", data, 18)[0]
    names = {3: "i386", 40: "ARM", 62: "x86_64", 183: "AArch64"}
    return f"ELF{bits}, {names.get(machine, f'e_machine={machine}')}"


def parse_vpk(data):
    if len(data) < 12:
        raise ValueError("VPK directory header is truncated")
    magic, version, tree_size = struct.unpack_from("<III", data)
    if magic != 0x55AA1234 or version not in (1, 2):
        raise ValueError(f"unsupported VPK header: {magic:#x}, version {version}")
    pos = 28 if version == 2 else 12
    end = pos + tree_size
    if end > len(data):
        raise ValueError("VPK tree extends beyond the directory file")

    def zstr():
        nonlocal pos
        end_string = data.find(b"\0", pos, end)
        if end_string < 0:
            raise ValueError("unterminated string in VPK tree")
        value = data[pos:end_string].decode("utf-8", "replace")
        pos = end_string + 1
        return value

    extensions = collections.Counter()
    examples = []
    total = 0
    while pos < end:
        extension = zstr()
        if not extension:
            break
        while True:
            path = zstr()
            if not path:
                break
            while True:
                name = zstr()
                if not name:
                    break
                crc, preload, archive, offset, length, terminator = struct.unpack_from(
                    "<IHHIIH", data, pos
                )
                pos += 18 + preload
                total += 1
                extensions[extension] += 1
                if len(examples) < 16:
                    full_path = "/".join(part for part in (path, name + "." + extension) if part and part != ".")
                    examples.append((full_path, archive, offset, length, preload))
    return version, tree_size, total, extensions, examples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", nargs="?", default="~/Descargas/Portal_Linux.tar.gz")
    archive = os.path.expanduser(parser.parse_args().archive)
    counts = collections.Counter()
    roots = collections.Counter()
    total_bytes = 0
    entries = 0
    elf_headers = {}
    vpk_data = None
    bsp_data = None
    gameinfo = None

    with tarfile.open(archive, mode="r|gz") as tf:
        for member in tf:
            entries += 1
            total_bytes += member.size
            name = member.name
            if name.startswith(ROOT):
                parts = name[len(ROOT):].split("/", 1)
                if len(parts) == 2:
                    roots[parts[0]] += 1
            suffix = os.path.splitext(name)[1].lower()
            if suffix in {".bsp", ".vpk", ".so"}:
                counts[suffix] += 1
            if name in ELF_PATHS:
                elf_headers[name] = (member.size, parse_elf(read_member(tf, member, 64)))
            elif name == VPK_DIR:
                vpk_data = read_member(tf, member)
            elif name == BSP_SAMPLE:
                bsp_data = read_member(tf, member)
            elif name == GAMEINFO:
                gameinfo = read_member(tf, member).decode("utf-8", "replace")

    print(f"Archive: {archive}")
    print(f"Entries: {entries}; uncompressed member bytes: {total_bytes}")
    print(f"Top-level areas: {dict(roots)}")
    print(f"File counts: {dict(counts)}")
    print("Key ELF files:")
    for name, (size, description) in sorted(elf_headers.items()):
        print(f"  {description:24} {size:>10}  {name}")

    if gameinfo:
        values = [line.strip() for line in gameinfo.splitlines() if "SteamAppId" in line or "gamebin" in line]
        print("gameinfo settings:", "; ".join(values))
    if bsp_data and len(bsp_data) >= 1036:
        ident, version = struct.unpack_from("<4si", bsp_data)
        revision = struct.unpack_from("<i", bsp_data, 8 + 64 * 16)[0]
        print(f"Sample BSP: {ident.decode('ascii', 'replace')} v{version}, revision {revision}")
        lumps = [struct.unpack_from("<4i", bsp_data, 8 + index * 16) for index in range(64)]
        print("Geometry lump counts:")
        for index, stride in BSP_LUMP_STRIDES.items():
            offset, length, lump_version, fourcc = lumps[index]
            if length:
                count, remainder = divmod(length, stride)
                suffix = f" ({remainder} trailing bytes)" if remainder else ""
                print(f"  {BSP_LUMP_NAMES[index]}: {count}{suffix}")
        vertex_offset, vertex_length, _, _ = lumps[3]
        points = [
            struct.unpack_from("<3f", bsp_data, vertex_offset + pos)
            for pos in range(0, vertex_length, 12)
        ]
        if points:
            bounds = [
                (min(point[axis] for point in points), max(point[axis] for point in points))
                for axis in range(3)
            ]
            print("Vertex bounds:", bounds)
        face_offset, face_length, _, _ = lumps[7]
        face_edges = [
            struct.unpack_from("<h", bsp_data, face_offset + pos + 8)[0]
            for pos in range(0, face_length, 56)
        ]
        print(f"Faces with 3+ edges: {sum(n >= 3 for n in face_edges)}")
        entity_offset, entity_length = struct.unpack_from("<ii", bsp_data, 8)
        entity_lump = bsp_data[entity_offset:entity_offset + entity_length]
        classes = collections.Counter(
            name.decode("ascii", "replace")
            for name in re.findall(rb'"classname"\s*"([^"]+)"', entity_lump)
        )
        portal_classes = {name: count for name, count in classes.items() if "portal" in name}
        print(f"Sample BSP entity lump: {sum(classes.values())} entities")
        print("Portal-related entity classes:", portal_classes)
    if vpk_data:
        version, tree_size, total, extensions, examples = parse_vpk(vpk_data)
        print(f"Portal VPK directory: v{version}, tree {tree_size} bytes, {total} indexed resources")
        print("Resource types:", ", ".join(f"{ext}={count}" for ext, count in extensions.most_common()))
        print("Sample paths:")
        for path, archive_id, offset, length, preload in examples:
            print(f"  {path} (archive {archive_id}, offset {offset}, {length} bytes, {preload} preload)")


if __name__ == "__main__":
    main()
