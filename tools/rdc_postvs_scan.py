# rdc script: scan a draw's post-VS vertices for degenerate/NaN/huge positions
# and report triangles that would collapse or clip away.
#
#   rdc script tools/rdc_postvs_scan.py --arg eid=<EID> [--arg verbose=1] --json
#
# Motive: the ground-hole trapezoid (missing terrain triangles) looks like the
# triangle fan around ONE dead vertex -- if a vertex position decodes wrong
# (NaN / inf / w~0 / far outside clip), every triangle sharing it vanishes.
# This proves or refutes that in one pass, and if bad verts exist it prints
# their indices so the vertex-fetch decode can be diffed offline.
import struct, math

eid = int(args.get('eid', '0'))
verbose = args.get('verbose')
controller.SetFrameEvent(eid, True)

pvs = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
result = {}
if pvs.vertexResourceId == rd.ResourceId.Null():
    result = {'error': 'no postvs data at eid %d' % eid}
else:
    stride = pvs.vertexByteStride
    vraw = controller.GetBufferData(pvs.vertexResourceId, pvs.vertexByteOffset, 0)
    nverts_buf = len(vraw) // stride if stride else 0

    # index stream (original IB drives postvs verts)
    idxs = None
    if pvs.indexResourceId != rd.ResourceId.Null() and pvs.indexByteStride:
        iraw = controller.GetBufferData(pvs.indexResourceId, pvs.indexByteOffset,
                                        pvs.numIndices * pvs.indexByteStride)
        fmt = {1: 'B', 2: 'H', 4: 'I'}[pvs.indexByteStride]
        idxs = list(struct.unpack('<%d%s' % (len(iraw) // pvs.indexByteStride, fmt), iraw))
        restart = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[pvs.indexByteStride]
    else:
        idxs = list(range(pvs.numIndices))
        restart = None

    def pos_of(i):
        off = i * stride
        if off + 16 > len(vraw):
            return None
        return struct.unpack_from('<4f', vraw, off)

    def bad(p):
        if p is None:
            return 'oob'
        if any(not math.isfinite(c) for c in p):
            return 'nonfinite'
        x, y, z, w = p
        if abs(w) < 1e-12:
            return 'w~0'
        nx, ny, nz = x / w, y / w, z / w
        if abs(nx) > 4 or abs(ny) > 4:  # far outside guard-band-ish clip
            return 'ndc-out(%.1f,%.1f)' % (nx, ny)
        return None

    bad_verts = {}
    seen = set()
    for i in idxs:
        if restart is not None and i == restart:
            continue
        if i in seen:
            continue
        seen.add(i)
        b = bad(pos_of(i))
        if b:
            bad_verts[i] = b

    # triangles touching a bad vert (list vs strip via topology)
    topo = controller.GetPipelineState().GetPrimitiveTopology()
    tris = []
    strip = topo in (rd.Topology.TriangleStrip,)
    if strip:
        run = []
        for i in idxs:
            if restart is not None and i == restart:
                run = []
                continue
            run.append(i)
            if len(run) >= 3:
                tris.append(tuple(run[-3:]))
    else:
        for k in range(0, len(idxs) - 2, 3):
            t = (idxs[k], idxs[k+1], idxs[k+2])
            if restart is None or restart not in t:
                tris.append(t)

    hit_tris = [t for t in tris if any(v in bad_verts for v in t)]
    result = {
        'eid': eid,
        'topology': str(topo),
        'num_indices': pvs.numIndices,
        'unique_verts': len(seen),
        'verts_in_buffer': nverts_buf,
        'bad_vert_count': len(bad_verts),
        'bad_verts': {str(k): v for k, v in list(bad_verts.items())[:40]},
        'tris_total': len(tris),
        'tris_with_bad_vert': len(hit_tris),
        'sample_bad_tris': hit_tris[:20],
    }
    if verbose and bad_verts:
        det = {}
        for i in list(bad_verts)[:10]:
            det[str(i)] = pos_of(i)
        result['bad_vert_positions'] = det
