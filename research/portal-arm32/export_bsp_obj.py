#!/usr/bin/env python3
"""Export Source BSP face geometry as an untextured Wavefront OBJ."""

import argparse
import os
import struct


HEADER_SIZE = 8 + 64 * 16 + 4
VERTEX_LUMP = 3
FACE_LUMP = 7
EDGE_LUMP = 12
SURFEDGE_LUMP = 13
FACE_STRIDE = 56


def lump(data, number):
    offset, length, version, fourcc = struct.unpack_from("<4i", data, 8 + number * 16)
    if offset < HEADER_SIZE or length < 0 or offset + length > len(data):
        raise ValueError(f"invalid BSP lump {number}: offset={offset}, length={length}")
    return data[offset:offset + length]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bsp", help="Path to a Source VBSP v20 map")
    parser.add_argument("obj", help="Output OBJ path")
    args = parser.parse_args()

    with open(args.bsp, "rb") as source:
        data = source.read()
    ident, version = struct.unpack_from("<4si", data)
    if ident != b"VBSP" or version != 20:
        raise ValueError(f"expected VBSP v20; got {ident!r} v{version}")

    vertices_data = lump(data, VERTEX_LUMP)
    edges_data = lump(data, EDGE_LUMP)
    surfedges_data = lump(data, SURFEDGE_LUMP)
    faces_data = lump(data, FACE_LUMP)
    if len(vertices_data) % 12 or len(edges_data) % 4 or len(surfedges_data) % 4 or len(faces_data) % FACE_STRIDE:
        raise ValueError("geometry lump length is not aligned to its record size")

    vertices = [
        struct.unpack_from("<3f", vertices_data, offset)
        for offset in range(0, len(vertices_data), 12)
    ]
    edges = [
        struct.unpack_from("<HH", edges_data, offset)
        for offset in range(0, len(edges_data), 4)
    ]
    surfedges = [
        struct.unpack_from("<i", surfedges_data, offset)[0]
        for offset in range(0, len(surfedges_data), 4)
    ]

    polygons = []
    skipped = 0
    for offset in range(0, len(faces_data), FACE_STRIDE):
        first_edge = struct.unpack_from("<i", faces_data, offset + 4)[0]
        edge_count = struct.unpack_from("<h", faces_data, offset + 8)[0]
        if edge_count < 3 or first_edge < 0 or first_edge + edge_count > len(surfedges):
            skipped += 1
            continue
        polygon = []
        for surfedge in surfedges[first_edge:first_edge + edge_count]:
            edge_id = abs(surfedge)
            if edge_id >= len(edges):
                polygon = []
                break
            v0, v1 = edges[edge_id]
            vertex_id = v1 if surfedge < 0 else v0
            if vertex_id >= len(vertices):
                polygon = []
                break
            polygon.append(vertex_id + 1)
        if len(polygon) >= 3:
            polygons.append(polygon)
        else:
            skipped += 1

    os.makedirs(os.path.dirname(os.path.abspath(args.obj)), exist_ok=True)
    with open(args.obj, "w", encoding="ascii") as output:
        output.write("# Untextured Source BSP face export\n")
        for x, y, z in vertices:
            output.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
        for index, polygon in enumerate(polygons, 1):
            output.write(f"g face_{index}\nf " + " ".join(map(str, polygon)) + "\n")

    print(f"VBSP v{version}: {len(vertices)} vertices, {len(polygons)} faces, {skipped} skipped")
    print(f"Wrote {args.obj} ({os.path.getsize(args.obj)} bytes)")


if __name__ == "__main__":
    main()
