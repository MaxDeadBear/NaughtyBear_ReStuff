# rdc script: full account of what happens at the wedge pixel in one capture.
#
#   rdc --session <s> script tools/rdc_wedge_report.py --arg x=50 --arg y=530 --json
#
# For the pixel: every fragment that touched it, whether it passed, and for
# each contributing draw the identity needed to match it against the other
# capture (index count, near/far via median w, blend/depth state). Run this on
# a WEDGE capture and a CLEAN capture taken at the SAME camera and diff the
# two lists -- that is the comparison that says why water wins in one and
# ground wins in the other (task #24).
import struct, math

X = int(args.get('x', '50')); Y = int(args.get('y', '530'))

acts = {}
last_draw = None
def walk(a):
    global last_draw
    for x in a:
        if x.flags & rd.ActionFlags.Drawcall:
            acts[x.eventId] = x
            last_draw = x
        walk(x.children)
walk(controller.GetRootActions())

# the scene target = the one most draws render into
from collections import Counter
rt_use = Counter()
for eid, a in list(acts.items())[:400]:
    controller.SetFrameEvent(eid, True)
    outs = controller.GetPipelineState().GetOutputTargets()
    for o in outs:
        if o.resource != rd.ResourceId.Null():
            rt_use[str(o.resource)] += 1
            break
scene_rt = rt_use.most_common(1)[0][0] if rt_use else None

# Pixel history must run against the SCENE target at the LAST SCENE draw --
# using the global last draw lands on the post-process/present surface, whose
# history is two fullscreen quads and says nothing about the wedge.
rid = None
target_eid = 0
for eid in sorted(acts):
    controller.SetFrameEvent(eid, True)
    outs = controller.GetPipelineState().GetOutputTargets()
    for o in outs:
        if o.resource != rd.ResourceId.Null():
            if str(o.resource) == scene_rt:
                rid = o.resource; target_eid = eid
            break
if rid is not None:
    controller.SetFrameEvent(target_eid, True)

hist = []
if rid is not None:
    try:
        mods = controller.PixelHistory(rid, X, Y, rd.Subresource(), rd.CompType.Typeless)
    except Exception as ex:
        mods = []
    for m in mods:
        hist.append({'eid': m.eventId, 'passed': bool(m.Passed()),
                     'depth': round(m.shaderOut.depth, 6)})

def describe(eid):
    if eid not in acts: return {'eid': eid, 'missing': True}
    a = acts[eid]
    controller.SetFrameEvent(eid, True)
    vk = controller.GetVulkanPipelineState()
    d = {'eid': eid, 'indices': a.numIndices,
         'depth_write': vk.depthStencil.depthWriteEnable,
         'blend': bool(vk.colorBlend.blends and vk.colorBlend.blends[0].enabled)}
    pvs = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
    if pvs.vertexResourceId != rd.ResourceId.Null() and pvs.vertexByteStride:
        raw = controller.GetBufferData(pvs.vertexResourceId, pvs.vertexByteOffset, 0)
        st = pvs.vertexByteStride
        ws = []
        for i in range(min(len(raw) // st, 2000)):
            _, _, _, w = struct.unpack_from('<4f', raw, i * st)
            if math.isfinite(w) and w > 1e-9: ws.append(w)
        if ws:
            ws.sort(); d['w_med'] = round(ws[len(ws)//2], 1)
    return d

result = {'pixel': [X, Y], 'scene_rt': scene_rt, 'fragments': hist,
          'draws': [describe(h['eid']) for h in hist[:14]]}
