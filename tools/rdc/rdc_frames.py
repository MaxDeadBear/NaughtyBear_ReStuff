# qrenderdoc script: self-loads the multi-frame capture, saves the swapchain
# image at every Present boundary. Run: qrenderdoc --python rdc_frames.py
import traceback

OUT = r"C:\Users\Tynan\AppData\Local\Temp\claude\C--Users-Tynan-Desktop-ReStuff-release\c6a5b44c-54d8-44a6-9504-ed809b532ca9\scratchpad"
CAP = r"C:\Users\Tynan\AppData\Local\Temp\RenderDoc\restuff_2026.08.26_23.29_capture.rdc"
log = open(OUT + r"\rdc_frames_log.txt", "w")

def L(msg):
    log.write(str(msg) + "\n")
    log.flush()

try:
    import renderdoc as rd

    L("loading capture...")
    res = pyrenderdoc.LoadCapture(CAP, rd.ReplayOptions(), CAP, False, True)
    L("LoadCapture returned %s" % res)

    def work(controller):
        try:
            acts = []
            def flatten(a_list):
                for a in a_list:
                    acts.append(a)
                    flatten(a.children)
            flatten(controller.GetRootActions())
            presents = [a for a in acts if a.flags & rd.ActionFlags.Present]
            L("actions=%d presents=%d" % (len(acts), len(presents)))
            texs = controller.GetTextures()
            swaps = [t for t in texs if t.creationFlags & rd.TextureCategory.SwapBuffer]
            L("swapchain textures: %s" % [(str(t.resourceId), t.width, t.height) for t in swaps])
            for pi, p in enumerate(presents):
                controller.SetFrameEvent(p.eventId, True)
                for t in swaps:
                    ts = rd.TextureSave()
                    ts.resourceId = t.resourceId
                    ts.destType = rd.FileType.PNG
                    ok = controller.SaveTexture(ts, OUT + r"\pf_%02d_%s.png" % (pi, str(t.resourceId).replace("::", "_")))
                    L("present %d eid=%d tex=%s saved=%s" % (pi, p.eventId, t.resourceId, ok))
            L("DONE")
        except Exception:
            L(traceback.format_exc())
    pyrenderdoc.Replay().BlockInvoke(work)
except Exception:
    L(traceback.format_exc())
log.close()
