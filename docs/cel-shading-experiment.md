# Cel shading experiment

This prototype applies brightness bands and optional dark image edges just
before the final front-buffer resolve. It affects the HUD and menus as well
as the world, and works with replacement textures. It is a
post-process, not a replacement for the game's material lighting equations;
edges can follow texture detail as well as object boundaries.

Open **F4 > Modding** during gameplay:

| Setting | Default | Purpose |
| --- | --- | --- |
| `cel_shading` | `false` | Enable/disable live. |
| `cel_bands` | `5` | 2–12 brightness bands; fewer looks flatter. |
| `cel_strength` | `0.75` | Band intensity, 0–1. |
| `cel_edges` | `0.35` | Dark edge intensity, 0–1; 0 disables edge samples. |

The local test configuration has `cel_shading = true`. Changing controls does
not compile shaders or rebuild pipelines. Turning the checkbox off skips the
new pass (unless the existing `RESTUFF_SCENE_TONE` experiment is enabled).
Use **Save to config** to retain changes across launches.

Both `out/build/local-relwithdebinfo/restuff.exe` and `restuff-cel-fix.exe` now
contain the corrected cel boundary and the display controls below. The second
name was originally needed while the old game locked `restuff.exe`; both were
updated once it closed. They use the same config, assets, DLLs, and texture
pack in the build directory.

## Brightness and gamma

Open **F4 > Display** in the native translated renderer:

| Setting | Default | Range | Effect |
| --- | --- | --- | --- |
| `display_brightness` | `1.0` | 0.25–2.0 | Overall brightness multiplier. |
| `display_gamma` | `1.0` | 0.5–3.0 | Above 1 lifts midtones; below 1 darkens them. |

Both controls change live and work with cel shading on or off. Set both to
`1.0` for the original display output. Use **Save to config** to persist them.
They affect the game image, HUD and menus. These controls apply to the native
translated path, not the SDK's separate Xenos renderer.

`display_adjust.frag` applies `pow(clamp(colour * brightness, 0, 1), 1/gamma)`
after the game's display LUT, in the existing final presentation draw. Neutral
values bypass the adjustment math entirely. No additional frame copy or draw
is added. The fragment push constants occupy bytes 16–31; the existing vertex
transform remains at bytes 0–15. If the guest LUT is disabled diagnostically,
the shader skips its colour transform while the image is still initialized
for a valid descriptor. The controls become active once the guest's first
display-ramp upload has completed.

The initial scene/UI boundary heuristic failed to fire in gameplay, leaving
the checkbox enabled without changing the image. The corrected implementation
selects the last main-surface colour resolve into the current front buffer.
It runs once immediately before that resolve, after initializing the source
surface if needed. It does not rely on UI flags or a minimum draw count, and
does not target offscreen or depth resolves. Frames lacking a matching colour
resolve skip the effect. The legacy tone experiment keeps its existing
boundary and does not apply the tone curve a second time in the cel pass.
The effect costs one
scene copy and one fullscreen draw; gameplay performance needs measurement,
especially at higher internal resolutions. Scratch resources are initialized
once so the controls can toggle live.

Sources: `src/shaders/scene_cel.vert`, `src/shaders/scene_cel.frag`, and the scene
post-process setup/recording in `src/native_vk.cpp`. CMake embeds the compiled
SPIR-V. The translated guest shader code is unchanged.

## Backup and rollback

The pre-experiment backup is `backups/pre-cel-20260906-185621/`. It contains the
shader sources/translator, pipeline code, native renderer, CMakeLists.txt, and
the pre-experiment executable, PDB, config, and shader/pipeline caches under
`runtime/`. `SHA256.json` records hashes of all 17 copied files, verified after
copying. This backup includes the earlier texture-loading fixes.

For a quick visual rollback, disable `cel_shading`. For a binary rollback,
close the game and copy `runtime/restuff.exe` from the backup into
`out/build/local-relwithdebinfo/`. Restore the backed-up config and caches too
if you want the complete saved runtime state. Keep the DLLs and assets already
beside the executable. For source rollback, restore the backed-up
`CMakeLists.txt` and `src/native_vk.cpp`; the new shader files will then be
unreferenced.

Validation: the RelWithDebInfo build passed, both new shader modules passed
`spirv-val --target-env vulkan1.0`, and the game created the scene tone/cel
pipeline successfully. `tools/test_scene_effect_boundary.cpp` covers absent UI
markers, repeated front-buffer resolves, offscreen/depth resolves, unknown
front buffers, and small/menu frames. `[cel] pass recorded before front-buffer
resolve` confirms an actual cel draw was recorded, rather than only pipeline
initialization. Visual A/B testing is still pending.
