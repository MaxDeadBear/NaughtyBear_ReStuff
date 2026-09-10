# qrenderdoc script: FULL history (passed AND failed) for the decal draw at
# the caught pixel, with failure flags. Run: qrenderdoc --python rdc_pixhist3.py
import traceback

OUT = r"C:\Users\Tynan\AppData\Local\Temp\claude\C--Users-Tynan-Desktop-ReStuff-release\c6a5b44c-54d8-44a6-9504-ed809b532ca9\scratchpad"
CAP = r"C:\Users\Tynan\AppData\Local\Temp\RenderDoc\restuff_2026.08.26_23.29_capture.rdc"
log = open(OUT + r"\rdc_pixhist3_log.txt", "w")

def L(m):
    log.write(str(m) + "\n")
    log.flush()

try:
    import renderdoc as rd
    res = pyrenderdoc.LoadCapture(CAP, rd.ReplayOptions(), CAP, False, True)
    L("LoadCapture %s" % res)

    def work(controller):
        try:
            acts = []
            def flatten(al):
                for a in al:
                    acts.append(a)
                    flatten(a.children)
            flatten(controller.GetRootActions())
            by_eid = {a.eventId: a for a in acts}
            controller.SetFrameEvent(acts[-1].eventId, True)
            tex = None
            for t in controller.GetTextures():
                if str(t.resourceId) == "ResourceId::268":
                    tex = t
            L("using tex %s" % tex.resourceId)
            hist = controller.PixelHistory(tex.resourceId, 445, 504, rd.Subresource(0, 0, 0),
                                           rd.CompType.Typeless)
            L("mods=%d" % len(hist))
            for h in hist:
                a = by_eid.get(h.eventId)
                ndx = a.numIndices if a else -1
                if ndx != 1001:
                    continue
                p = h.preMod.col.floatValue
                flags = []
                for fn in ("sampleMasked", "backfaceCulled", "depthClipped", "viewClipped",
                           "scissorClipped", "shaderDiscarded", "depthTestFailed",
                           "stencilTestFailed", "predicationSkipped", "depthBoundsFailed"):
                    if getattr(h, fn, False):
                        flags.append(fn)
                so = h.shaderOut.col.floatValue
                L("eid=%d prim=%d passed=%s flags=%s shaderOut=(%.3f,%.3f,%.3f,%.3f) "
                  "pre=(%.3f,%.3f,%.3f) preZ=%.6f fragZ=%.6f" %
                  (h.eventId, h.primitiveID, h.Passed(), ",".join(flags) if flags else "none",
                   so[0], so[1], so[2], so[3], p[0], p[1], p[2], h.preMod.depth,
                   h.shaderOut.depth))
            L("DONE")
        except Exception:
            L(traceback.format_exc())
    pyrenderdoc.Replay().BlockInvoke(work)
except Exception:
    L(traceback.format_exc())
log.close()
