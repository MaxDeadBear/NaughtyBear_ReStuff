# qrenderdoc script: inventory the decal draws (numIndices==1001), dump their
# state + save color target before/after each. Run: qrenderdoc <rdc> --python this.py
import renderdoc as rd

OUT = r"C:\Users\Tynan\AppData\Local\Temp\claude\C--Users-Tynan-Desktop-ReStuff-release\c6a5b44c-54d8-44a6-9504-ed809b532ca9\scratchpad"
lines = []

def flatten(acts, out):
    for a in acts:
        out.append(a)
        flatten(a.children, out)

def save_tex(controller, res_id, path):
    if res_id == rd.ResourceId.Null():
        return False
    ts = rd.TextureSave()
    ts.resourceId = res_id
    ts.destType = rd.FileType.PNG
    ts.mip = 0
    ts.slice.sliceIndex = 0
    return controller.SaveTexture(ts, path)

def work(controller):
    acts = []
    flatten(controller.GetRootActions(), acts)
    draws = [a for a in acts if a.flags & rd.ActionFlags.Drawcall]
    lines.append("total actions=%d draws=%d" % (len(acts), len(draws)))
    decals = [a for a in draws if a.numIndices == 1001]
    lines.append("decal draws (n=1001): eids=%s" % [a.eventId for a in decals])
    for a in decals:
        controller.SetFrameEvent(a.eventId, True)
        ps = controller.GetPipelineState()
        try:
            vk = controller.GetVulkanPipelineState()
            cb0 = vk.colorBlend.blends[0] if len(vk.colorBlend.blends) else None
            if cb0:
                lines.append(
                    "eid %d: blend enabled=%s src=%s dst=%s writeMask=%d depth: test=%s write=%s func=%s"
                    % (a.eventId, cb0.enabled, cb0.colorBlend.source, cb0.colorBlend.destination,
                       cb0.writeMask, vk.depthStencil.depthTestEnable,
                       vk.depthStencil.depthWriteEnable, vk.depthStencil.depthFunction))
            vbs = vk.vertexInput.vertexBuffers
            for i, vb in enumerate(vbs):
                lines.append("  vb[%d]: size=%d offset=%d" % (i, vb.byteSize, vb.byteOffset))
        except Exception as e:
            lines.append("eid %d: vk state error %s" % (a.eventId, e))
        targets = ps.GetOutputTargets()
        if targets:
            save_tex(controller, targets[0].resource, OUT + r"\rdc_after_%d.png" % a.eventId)
        # state right before this draw
        controller.SetFrameEvent(a.eventId - 1, True)
        ps2 = controller.GetPipelineState()
        t2 = ps2.GetOutputTargets()
        if t2:
            save_tex(controller, t2[0].resource, OUT + r"\rdc_before_%d.png" % a.eventId)
        # post-VS bounds: are the tail decals collapsed?
        try:
            controller.SetFrameEvent(a.eventId, True)
            pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            lines.append("  postvs: numIndices=%d baseVertex=%d" % (pv.numIndices, pv.baseVertex))
        except Exception as e:
            lines.append("  postvs error: %s" % e)
    with open(OUT + r"\rdc_report.txt", "w") as f:
        f.write("\n".join(str(x) for x in lines))

pyrenderdoc.Replay().BlockInvoke(work)
