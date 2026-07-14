#!/usr/bin/env python3
"""Generate a stylized seated Buddha statue as glTF 2.0 (.gltf + .bin)."""

from __future__ import annotations

import json
import math
import struct
from pathlib import Path

import numpy as np

OUT_DIR = Path(__file__).resolve().parents[1] / "assets"
OUT_NAME = "tuong_phat"


def uv_sphere(cx, cy, cz, rx, ry, rz, stacks=16, slices=24):
    verts, norms, indices = [], [], []
    for i in range(stacks + 1):
        v = i / stacks
        phi = v * math.pi
        for j in range(slices + 1):
            u = j / slices
            theta = u * 2 * math.pi
            x = math.sin(phi) * math.cos(theta)
            y = math.cos(phi)
            z = math.sin(phi) * math.sin(theta)
            verts.append((cx + rx * x, cy + ry * y, cz + rz * z))
            nlen = math.sqrt((x / rx) ** 2 + (y / ry) ** 2 + (z / rz) ** 2) or 1.0
            norms.append((x / rx / nlen, y / ry / nlen, z / rz / nlen))
    for i in range(stacks):
        for j in range(slices):
            a = i * (slices + 1) + j
            b = a + slices + 1
            indices.extend([a, b, a + 1, a + 1, b, b + 1])
    return verts, norms, indices


def cylinder(cx, cy0, cy1, r0, r1, slices=24, closed=True):
    verts, norms, indices = [], [], []
    h = cy1 - cy0
    for i, (y, r) in enumerate(((cy0, r0), (cy1, r1))):
        for j in range(slices + 1):
            u = j / slices
            theta = u * 2 * math.pi
            x, z = math.cos(theta), math.sin(theta)
            verts.append((cx + r * x, y, r * z))
            nx, nz = x, z
            # approximate side normal for tapered cylinder
            slope = (r0 - r1) / (h if h else 1)
            n = np.array([nx, slope, nz], dtype=float)
            n /= np.linalg.norm(n) or 1.0
            norms.append(tuple(n))
    base = 0
    for j in range(slices):
        a, b = base + j, base + j + 1
        c, d = base + (slices + 1) + j, base + (slices + 1) + j + 1
        indices.extend([a, c, b, b, c, d])
    if closed:
        # bottom + top caps
        bot_center = len(verts)
        verts.append((cx, cy0, 0.0))
        norms.append((0.0, -1.0, 0.0))
        top_center = len(verts)
        verts.append((cx, cy1, 0.0))
        norms.append((0.0, 1.0, 0.0))
        for j in range(slices):
            a, b = j, j + 1
            indices.extend([bot_center, b, a])
            c, d = (slices + 1) + j, (slices + 1) + j + 1
            indices.extend([top_center, c, d])
    return verts, norms, indices


def torus(cx, cy, cz, R, r, seg_u=32, seg_v=12):
    verts, norms, indices = [], [], []
    for i in range(seg_u + 1):
        u = i / seg_u * 2 * math.pi
        for j in range(seg_v + 1):
            v = j / seg_v * 2 * math.pi
            x = (R + r * math.cos(v)) * math.cos(u)
            y = r * math.sin(v)
            z = (R + r * math.cos(v)) * math.sin(u)
            verts.append((cx + x, cy + y, cz + z))
            nx = math.cos(v) * math.cos(u)
            ny = math.sin(v)
            nz = math.cos(v) * math.sin(u)
            norms.append((nx, ny, nz))
    for i in range(seg_u):
        for j in range(seg_v):
            a = i * (seg_v + 1) + j
            b = a + seg_v + 1
            indices.extend([a, b, a + 1, a + 1, b, b + 1])
    return verts, norms, indices


def box(cx, cy, cz, sx, sy, sz):
    hx, hy, hz = sx / 2, sy / 2, sz / 2
    corners = [
        (cx - hx, cy - hy, cz - hz),
        (cx + hx, cy - hy, cz - hz),
        (cx + hx, cy + hy, cz - hz),
        (cx - hx, cy + hy, cz - hz),
        (cx - hx, cy - hy, cz + hz),
        (cx + hx, cy - hy, cz + hz),
        (cx + hx, cy + hy, cz + hz),
        (cx - hx, cy + hy, cz + hz),
    ]
    faces = [
        ([0, 1, 2, 3], (0, 0, -1)),
        ([5, 4, 7, 6], (0, 0, 1)),
        ([4, 0, 3, 7], (-1, 0, 0)),
        ([1, 5, 6, 2], (1, 0, 0)),
        ([3, 2, 6, 7], (0, 1, 0)),
        ([4, 5, 1, 0], (0, -1, 0)),
    ]
    verts, norms, indices = [], [], []
    for idxs, n in faces:
        base = len(verts)
        for i in idxs:
            verts.append(corners[i])
            norms.append(n)
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return verts, norms, indices


def merge(parts, color):
    verts, norms, indices = [], [], []
    colors = []
    for v, n, idx in parts:
        base = len(verts)
        verts.extend(v)
        norms.extend(n)
        indices.extend([i + base for i in idx])
        colors.extend([color] * len(v))
    return verts, norms, colors, indices


def build_buddha():
    gold = (0.85, 0.70, 0.28, 1.0)
    robe = (0.78, 0.45, 0.18, 1.0)
    base_c = (0.55, 0.42, 0.22, 1.0)
    skin = (0.92, 0.78, 0.55, 1.0)

    meshes = []

    # Lotus / pedestal
    pedestal = [
        cylinder(0, -0.05, 0.08, 1.15, 1.05, 48),
        torus(0, 0.10, 0, 0.95, 0.12, 48, 10),
        cylinder(0, 0.08, 0.22, 0.85, 0.80, 36),
    ]
    meshes.append(merge(pedestal, base_c))

    # Crossed legs (robe)
    legs = [
        uv_sphere(-0.35, 0.38, 0.05, 0.42, 0.22, 0.32, 14, 20),
        uv_sphere(0.35, 0.38, 0.05, 0.42, 0.22, 0.32, 14, 20),
        uv_sphere(0.0, 0.32, 0.28, 0.38, 0.18, 0.25, 12, 18),
    ]
    meshes.append(merge(legs, robe))

    # Torso / robe
    torso = [
        uv_sphere(0, 0.95, 0.0, 0.48, 0.55, 0.32, 18, 24),
        cylinder(0, 0.55, 1.15, 0.42, 0.38, 28),
    ]
    meshes.append(merge(torso, robe))

    # Chest / inner skin
    meshes.append(merge([uv_sphere(0, 1.05, 0.18, 0.28, 0.32, 0.12, 12, 16)], skin))

    # Shoulders
    shoulders = [
        uv_sphere(-0.42, 1.25, 0.0, 0.18, 0.16, 0.16, 12, 16),
        uv_sphere(0.42, 1.25, 0.0, 0.18, 0.16, 0.16, 12, 16),
    ]
    meshes.append(merge(shoulders, robe))

    # Arms resting in lap (dhyana mudra)
    arms = [
        uv_sphere(-0.28, 0.72, 0.28, 0.14, 0.12, 0.28, 12, 16),
        uv_sphere(0.28, 0.72, 0.28, 0.14, 0.12, 0.28, 12, 16),
        uv_sphere(0.0, 0.68, 0.42, 0.22, 0.08, 0.14, 10, 14),  # hands
    ]
    meshes.append(merge(arms, robe))

    # Neck
    meshes.append(merge([cylinder(0, 1.40, 1.55, 0.12, 0.14, 16)], skin))

    # Head
    head = [
        uv_sphere(0, 1.78, 0.0, 0.28, 0.32, 0.28, 18, 24),
        uv_sphere(0, 2.05, 0.0, 0.12, 0.10, 0.12, 12, 16),  # ushnisha
    ]
    meshes.append(merge(head, gold))

    # Ears
    ears = [
        uv_sphere(-0.30, 1.75, 0.0, 0.05, 0.12, 0.04, 10, 12),
        uv_sphere(0.30, 1.75, 0.0, 0.05, 0.12, 0.04, 10, 12),
    ]
    meshes.append(merge(ears, gold))

    # Simple eyes / mouth (dark gold accents)
    face = [
        uv_sphere(-0.09, 1.80, 0.24, 0.035, 0.02, 0.02, 8, 10),
        uv_sphere(0.09, 1.80, 0.24, 0.035, 0.02, 0.02, 8, 10),
        uv_sphere(0.0, 1.68, 0.26, 0.04, 0.015, 0.015, 8, 10),
    ]
    meshes.append(merge(face, (0.25, 0.15, 0.05, 1.0)))

    # Hair curls band
    meshes.append(merge([torus(0, 1.95, 0, 0.20, 0.04, 24, 8)], (0.35, 0.22, 0.08, 1.0)))

    return meshes


def pack_meshes(meshes):
    all_pos, all_nrm, all_col, all_idx = [], [], [], []
    primitives = []
    for verts, norms, colors, indices in meshes:
        v_off = len(all_pos)
        i_off = len(all_idx)
        all_pos.extend(verts)
        all_nrm.extend(norms)
        all_col.extend(colors)
        all_idx.extend([i + v_off for i in indices])
        primitives.append(
            {
                "vertex_count": len(verts),
                "index_count": len(indices),
                "index_byte_offset": i_off * 4,
                "vertex_byte_offset": v_off,
            }
        )
    return all_pos, all_nrm, all_col, all_idx, primitives


def write_gltf(out_dir: Path, name: str):
    out_dir.mkdir(parents=True, exist_ok=True)
    meshes = build_buddha()
    positions, normals, colors, indices, prim_meta = pack_meshes(meshes)

    pos = np.asarray(positions, dtype=np.float32)
    nrm = np.asarray(normals, dtype=np.float32)
    col = np.asarray(colors, dtype=np.float32)
    idx = np.asarray(indices, dtype=np.uint32)

    # normalize normals
    lens = np.linalg.norm(nrm, axis=1, keepdims=True)
    lens[lens == 0] = 1
    nrm = nrm / lens

    pos_bytes = pos.tobytes()
    nrm_bytes = nrm.tobytes()
    col_bytes = col.tobytes()
    idx_bytes = idx.tobytes()

    # align to 4 bytes
    def pad4(b: bytes) -> bytes:
        return b + (b"\x00" * ((4 - (len(b) % 4)) % 4))

    chunks = [pad4(pos_bytes), pad4(nrm_bytes), pad4(col_bytes), pad4(idx_bytes)]
    offsets = []
    cursor = 0
    for c in chunks:
        offsets.append(cursor)
        cursor += len(c)
    blob = b"".join(chunks)
    bin_name = f"{name}.bin"
    (out_dir / bin_name).write_bytes(blob)

    vmin = pos.min(axis=0).tolist()
    vmax = pos.max(axis=0).tolist()
    n_verts = len(pos)
    n_idx = len(idx)

    accessors = [
        {  # 0 positions
            "bufferView": 0,
            "componentType": 5126,
            "count": n_verts,
            "type": "VEC3",
            "max": vmax,
            "min": vmin,
        },
        {  # 1 normals
            "bufferView": 1,
            "componentType": 5126,
            "count": n_verts,
            "type": "VEC3",
        },
        {  # 2 colors
            "bufferView": 2,
            "componentType": 5126,
            "count": n_verts,
            "type": "VEC4",
        },
        {  # 3 indices
            "bufferView": 3,
            "componentType": 5125,
            "count": n_idx,
            "type": "SCALAR",
        },
    ]

    buffer_views = [
        {"buffer": 0, "byteOffset": offsets[0], "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[1], "byteLength": len(nrm_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[2], "byteLength": len(col_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[3], "byteLength": len(idx_bytes), "target": 34963},
    ]

    gltf = {
        "asset": {"version": "2.0", "generator": "make_buddha_gltf.py"},
        "scene": 0,
        "scenes": [{"nodes": [0], "name": "Scene"}],
        "nodes": [{"mesh": 0, "name": "TuongPhat", "translation": [0, 0, 0]}],
        "meshes": [
            {
                "name": "BuddhaStatue",
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "NORMAL": 1, "COLOR_0": 2},
                        "indices": 3,
                        "mode": 4,
                        "material": 0,
                    }
                ],
            }
        ],
        "materials": [
            {
                "name": "BuddhaMat",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [1, 1, 1, 1],
                    "metallicFactor": 0.35,
                    "roughnessFactor": 0.45,
                },
                "doubleSided": True,
            }
        ],
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(blob), "uri": bin_name}],
    }

    gltf_path = out_dir / f"{name}.gltf"
    gltf_path.write_text(json.dumps(gltf, indent=2), encoding="utf-8")

    # Also write a single-file GLB
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    # embed buffer: replace external uri with no uri (GLB chunk)
    gltf_embedded = json.loads(json_bytes)
    gltf_embedded["buffers"] = [{"byteLength": len(blob)}]
    json_chunk = json.dumps(gltf_embedded, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * ((4 - (len(json_chunk) % 4)) % 4)
    bin_chunk = blob + (b"\x00" * ((4 - (len(blob) % 4)) % 4))

    total = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
    glb = bytearray()
    glb += struct.pack("<4sII", b"glTF", 2, total)
    glb += struct.pack("<II", len(json_chunk), 0x4E4F534A)  # JSON
    glb += json_chunk
    glb += struct.pack("<II", len(bin_chunk), 0x004E4942)  # BIN
    glb += bin_chunk
    glb_path = out_dir / f"{name}.glb"
    glb_path.write_bytes(glb)

    print(f"Wrote {gltf_path}")
    print(f"Wrote {out_dir / bin_name}")
    print(f"Wrote {glb_path}")
    print(f"Vertices: {n_verts}, Triangles: {n_idx // 3}")
    print(f"Bounds Y: {vmin[1]:.2f} .. {vmax[1]:.2f}")


if __name__ == "__main__":
    write_gltf(OUT_DIR, OUT_NAME)
