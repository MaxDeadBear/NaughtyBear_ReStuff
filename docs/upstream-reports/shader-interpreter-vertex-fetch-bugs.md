# ShaderInterpreter: unsigned packed vertex formats decode incorrectly (missing offset shift) + uninitialized `packed_offsets`/`packed_widths` locals

**Component:** `rexgraphics` — CPU shader interpreter
**File:** `src/graphics/pipeline/shader/interpreter.cpp`
**Function:** `rex::graphics::ShaderInterpreter::ExecuteVertexFetchInstruction(ucode::VertexFetchInstruction)` (lines 919–1137)
**Verified at:** v0.7.3, commit `987593c0fc6612850a2e1b247b2b03aa5281e663` (clean tree; line numbers below refer to this revision)

## Summary

`ExecuteVertexFetchInstruction` contains two related defects in the packed-integer vertex-format unpacking path:

1. **Bug 1 — missing right-shift by the component's bit offset in the unsigned extraction.** The unsigned path masks the fetched dword by the component's width but never shifts by `packed_offsets[i]` first, so every component at a non-zero bit offset decodes the low bits of its source dword — the field of the lowest-offset component stored in that dword — instead of its own. All unsigned packed formats (`k_8_8_8_8`, `k_2_10_10_10`, `k_10_11_11`, `k_11_11_10`, `k_16_16`, `k_16_16_16_16`) are affected.
2. **Bug 2 — `packed_widths[4]` / `packed_offsets[4]` declared without an initializer.** The format `switch` only assigns the entries whose value is non-zero (in particular, `packed_offsets[0]` is never assigned in *any* case, and `packed_offsets[2]` is not assigned for `k_16_16_16_16`), so the extraction code reads indeterminate values — undefined behavior, and garbage decodes in practice. The signed extraction path is affected by this today; the unsigned path becomes affected as soon as Bug 1 is fixed, so both fixes need to land together.

With both fixes applied, a downstream project's offline parity harness reached exact 696/696 per-vertex position parity between this interpreter and an independent ucode→GLSL translation of real Xbox 360 title vertex shaders (details in "Impact / how found" below).

## Bug 1: unsigned packed extraction masks without shifting by `packed_offsets[i]`

### The code (lines 1108–1115)

```cpp
      } else {
        for (uint32_t i = 0; i < 4; ++i) {
          if (!(packed_components & (UINT32_C(1) << i))) {
            continue;
          }
          uint32_t packed_width = packed_widths[i];
          result[i] = float(packed_dwords[i >> 1] & ((UINT32_C(1) << packed_widths[i]) - 1));
        }
```

Line 1114 extracts component `i` as `dword & ((1 << width) - 1)` — the low `width` bits of the dword — regardless of where the component actually lives. In a Xenos packed vertex format, component `i` occupies the bit field `[packed_offsets[i], packed_offsets[i] + packed_widths[i])` of the (endian-swapped) dword, so unsigned extraction must be **shift-then-mask**:

```cpp
(packed_dwords[i >> 1] >> packed_offsets[i]) & ((UINT32_C(1) << packed_widths[i]) - 1)
```

The function itself demonstrates the correct semantics a few lines earlier: the **signed** path (lines 1081–1089) honors the offset via the shift-up/arithmetic-shift-down idiom:

```cpp
        for (uint32_t i = 0; i < 4; ++i) {
          if (!(packed_components & (UINT32_C(1) << i))) {
            continue;
          }
          uint32_t packed_width = packed_widths[i];
          result[i] =
              float(int32_t(packed_dwords[i >> 1]) << (32 - (packed_width + packed_offsets[i])) >>
                    (32 - packed_width));
        }
```

The unsigned path simply dropped the offset. (Note the `packed_width` local declared on line 1113 is currently dead in the unsigned branch — line 1114 re-indexes `packed_widths[i]` instead — which is itself a hint the expression was mistranscribed.)

### Effect

For every unsigned packed fetch, all enabled components decode a masked copy of the low bits of their source dword — i.e. of component 0's field (components 2 and 3 read `packed_dwords[1]`, which is also `data[0]` for every format except `k_16_16_16_16`).

Concrete example, `k_8_8_8_8` unsigned (the classic layout for blend indices / blend weights), fetched dword `0x44332211`:

| component | expected | actual (v0.7.3) |
|---|---|---|
| x | `0x11` | `0x11` |
| y | `0x22` | `0x11` |
| z | `0x33` | `0x11` |
| w | `0x44` | `0x11` |

With normalization enabled the same collapse happens before the divide by `(1 << width) - 1` (lines 1116–1123, which are themselves fine). For skinning data this means all four bone indices/weights collapse to the first field's bits — geometry deforms wrong wherever a shader reads unsigned packed vertex data through the interpreter.

## Bug 2: `packed_widths` / `packed_offsets` are uninitialized locals

### The code (lines 973–975)

```cpp
    uint32_t packed_components = 0b0000;
    uint32_t packed_widths[4], packed_offsets[4];
    uint32_t packed_dwords[] = {data[0], data[0]};
```

The `switch (instr.data_format())` that follows (lines 976–1076) was clearly written assuming zero-initialization: each case assigns only the entries whose correct value is non-zero. For example `k_8_8_8_8` (lines 977–983):

```cpp
      case xenos::VertexFormat::k_8_8_8_8: {
        packed_components = 0b1111;
        packed_widths[0] = packed_widths[1] = packed_widths[2] = packed_widths[3] = 8;
        packed_offsets[1] = 8;
        packed_offsets[2] = 16;
        packed_offsets[3] = 24;
      } break;
```

and `k_16_16_16_16` (lines 1011–1016):

```cpp
      case xenos::VertexFormat::k_16_16_16_16: {
        packed_components = 0b1111;
        packed_widths[0] = packed_widths[1] = packed_widths[2] = packed_widths[3] = 16;
        packed_offsets[1] = packed_offsets[3] = 16;
        packed_dwords[1] = data[1];
      } break;
```

But the declaration on line 974 has no initializer, so:

- **`packed_offsets[0]` is never assigned by any case** (easily confirmed: the only assignments in the function target indices 1, 2, 3 — lines 980–982, 988–990, 996–997, 1003–1004, 1009, 1014), yet it is read on line 1087 for every signed packed fetch that uses component x, inside `32 - (packed_width + packed_offsets[i])`.
- **`packed_offsets[2]` is never assigned for `k_16_16_16_16`**, yet is read the same way when component z is used.
- The `packed_widths` entries for components enabled in `packed_components` *are* always assigned, but giving both arrays an initializer is the robust fix and costs nothing.

Reading an indeterminate `uint32_t` is undefined behavior ([basic.indet]); even under a benign compiler it yields stack garbage, and here a garbage offset additionally feeds a shift count (`32 - (packed_width + packed_offsets[i])`), which can leave `[0, 31]` and make the shift itself UB. Whether this misbehaves in practice depends entirely on what the optimizer left in that stack slot/register — i.e. it is build- and inlining-dependent, which makes it nasty to bisect downstream.

Note the interaction with Bug 1: fixing Bug 1 makes the *unsigned* path start reading `packed_offsets[i]` too (including the never-assigned `[0]`, and `[2]` for `k_16_16_16_16`), so **fixing Bug 1 without Bug 2 trades wrong-but-deterministic decodes for UB**. The two fixes should land together.

## Suggested patch

```diff
--- a/src/graphics/pipeline/shader/interpreter.cpp
+++ b/src/graphics/pipeline/shader/interpreter.cpp
@@ -971,7 +971,7 @@
     }
 
     uint32_t packed_components = 0b0000;
-    uint32_t packed_widths[4], packed_offsets[4];
+    uint32_t packed_widths[4] = {}, packed_offsets[4] = {};
     uint32_t packed_dwords[] = {data[0], data[0]};
     switch (instr.data_format()) {
       case xenos::VertexFormat::k_8_8_8_8: {
@@ -1111,7 +1111,8 @@
             continue;
           }
           uint32_t packed_width = packed_widths[i];
-          result[i] = float(packed_dwords[i >> 1] & ((UINT32_C(1) << packed_widths[i]) - 1));
+          result[i] = float((packed_dwords[i >> 1] >> packed_offsets[i]) &
+                            ((UINT32_C(1) << packed_width) - 1));
         }
         if (instr.is_normalized()) {
           for (uint32_t i = 0; i < 4; ++i) {
```

(The second hunk also puts the previously dead `packed_width` local to use, matching the style of the signed branch. The normalization loops below it are correct as-is once extraction is fixed.)

## Impact / how found

Found by the NaughtyBear ReStuff project (an Xbox 360 static-recompilation target built on this SDK) while using `ShaderInterpreter` as a CPU reference to debug shadow-volume rendering:

- The project built an offline parity harness that replays the title's real Xenos vertex shaders through two independent implementations — the SDK's `ShaderInterpreter` and the project's own ucode→GLSL translator executing on the GPU — and diffs per-vertex outputs.
- Skinned shadow-volume meshes decoded their bone indices/weights wrong through the interpreter (the collapse-to-component-0 signature of Bug 1, with additional build-dependent garbage attributable to Bug 2) until **both** fixes above were applied to a patched copy.
- With both fixes, the interpreter achieved **exact 696/696 per-vertex position parity** against the independent translator on the title's real shaders — i.e. the remaining vertex-fetch math in this function checks out precisely once these two defects are corrected.

Scope: CPU `ShaderInterpreter` only (anything using it as a reference/fallback executor — parity tooling, trace tooling, software paths). The SPIR-V translation pipeline is unaffected.

## Notes

- The `FIXME(Triang3l)` comment at the top of the function suggests this code was ported from Xenia's shader interpreter; the unsigned-path expression and the missing `= {}` look like transcription regressions relative to the signed path in the same function, which is internally consistent with correct Xenos packed-field semantics.
- Verified against the v0.7.3 source tree (commit `987593c` above). The 0.8.1 nightly packages (`rexglue-sdk-0.8.1.68-dev.g8dadea6`) ship binaries and headers only, so we could not re-confirm line numbers there, but we are not aware of any change to this function since.
