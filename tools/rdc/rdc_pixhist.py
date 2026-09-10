# qrenderdoc script: pixel history at the captured flicker region.
# Run: qrenderdoc --python rdc_pixhist.py
import traceback

OUT = r"C:\Users\Tynan\AppData\Local\Temp\claude\C--Users-Tynan-Desktop-ReStuff-release\c6a5b44c-54d8-44a6-9504-ed809b532ca9\scratchpad"
CAP = r"C:\Users\Tynan\AppData\Local\Temp\RenderDoc\restuff_2026.08.26_23.29_capture.rdc"
POINTS = [(450, 480), (470, 500), (430, 460), (576, 435)]
log = open(OUT + r"\rdc_pixhist_log.txt", "w")

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
            by_eid = {}
            for a in acts:
                by_eid[a.eventId] = a
            # candidate scene RTs: 1280x720 color render targets, not swapchain
            texs = controller.GetTextures()
            cands = [t for t in texs
                     if t.width == 1280 and t.height == 720
                     and (t.creationFlags & rd.TextureCategory.ColorTarget)
                     and not (t.creationFlags & rd.TextureCategory.SwapBuffer)]
            L("scene RT candidates: %s" % [(str(t.resourceId), str(t.format.Name())) for t in cands])
            # last event for full history
            last_eid = acts[-1].eventId
            controller.SetFrameEvent(last_eid, True)
            for t in cands[:3]:
                for (px, py) in POINTS:
                    try:
                        hist = controller.PixelHistory(t.resourceId, px, py, rd.Subresource(0, 0, 0),
                                                       rd.CompType.Typeless)
                        writes = [h for h in hist if h.Passed()]
                        L("== tex %s pixel (%d,%d): %d mods, %d passed" %
                          (t.resourceId, px, py, len(hist), len(writes)))
                        for h in writes:
                            a = by_eid.get(h.eventId)
                            ndx = a.numIndices if a else -1
                            c = h.postMod.col.floatValue
                            L("  eid=%d nidx=%d post=(%.3f,%.3f,%.3f,%.3f) prim=%d" %
                              (h.eventId, ndx, c[0], c[1], c[2], c[3], h.primitiveID))
                    except Exception:
                        L(traceback.format_exc())
            L("DONE")
        except Exception:
            L(traceback.format_exc())
    pyrenderdoc.Replay().BlockInvoke(work)
except Exception:
    L(traceback.format_exc())
log.close()
