<img width="1280" height="816" alt="Untitled9_20260413012316" src="https://github.com/user-attachments/assets/9e4347d4-cc8e-437f-9da6-a2fafdf8537e" />


## VERSION: NAUGHTY BEAR GOLD (Because All DLC Included)


## HOW TO BUILD

This tree is the **native Vulkan renderer** build. It targets ReXGlue SDK **0.10.0**
and needs a Vulkan SDK, because the renderer compiles the SDK's Vulkan provider
straight into the exe and uses libshaderc to translate guest shaders at runtime.

**1. Prerequisites**

- Visual Studio (Community or Build Tools) with *Desktop development with C++*,
  including **C++ Clang tools for Windows**. Clang finds the MSVC toolchain on its
  own, so no Developer Shell is required.
- **Vulkan SDK** (sets `VULKAN_SDK`). Supplies `glslc`, `shaderc_shared.lib`, and
  the Vulkan headers.
- **CMake** 3.25+ and **Ninja** on PATH.
- The **rexglue-sdk** source tree, built and installed (see its wiki). Both the
  installed package *and* the source tree are needed: the game links the installed
  libraries but compiles the Vulkan provider from source.

**2. Apply the SDK patches — required, not optional**

    git -C <rexglue-sdk> apply <this-repo>/sdk-patches/*.patch
    cmake --build <rexglue-sdk>/out/build/win-amd64 --config Release --target install

Three of the hunks are load-bearing: `textureCompressionBC` on the Vulkan device,
the `vkCmdWriteTimestamp` / `vkGetQueryPoolResults` entry points, and the
`simde/x86/fma.h` include used by fused-FMA recompiled code. Without them the game
does not compile. The game builds against the **installed** headers, so the
`cmake --install` step matters.

**3. Dump the game into `assets/`**

Extract your own Naughty Bear (Xbox 360) disc so that `assets/Default.xex` sits at
that exact path — the files go directly in `assets/`, not in a subfolder.

**4. Run codegen once**

`generated/` is not committed (it is recompiled from your own `Default.xex`), and
the `restuff_recomp` target does not exist until it has been produced. So run
codegen before the first configure:

    rexglue codegen restuff_manifest.toml

With fused-FMA emission (matching the preset's `RESTUFF_RECOMP_FMA=ON`):

    cmake -E env REX_EMIT_FMA=1 rexglue codegen restuff_manifest.toml

Later builds re-run codegen automatically when the manifest or the XEX changes.

**5. Configure**

Copy `CMakeUserPresets.json.example` to `CMakeUserPresets.json` and edit the paths
for your machine, then:

    cmake --preset local-relwithdebinfo
    cmake --build out/build/local-relwithdebinfo

Visual Studio's CMake targets view picks the presets up automatically. The user
preset exists because `CMakeLists.txt` defaults `REXSDK_SOURCE_TREE` and
`RESTUFF_VULKAN_INCLUDE` to the original author's Linux paths; it also sets
`CMAKE_RC_COMPILER=llvm-rc`, since `enable_language(RC)` otherwise wants an
`rc.exe` that is only on PATH inside a Developer Shell.

**Note on FMA:** `RESTUFF_RECOMP_FMA` and `REXGLUE_CODEGEN_ENV=REX_EMIT_FMA=1` are a
matched pair and are both set in the example preset. Setting only the compile flag
silently regenerates *unfused* code while the FMA flag stays on. The tell is the
codegen summary: `0 written, 455 unchanged` means the pairing is right.

Codegen runs as part of the build; `generated/` is not committed.

## HOW TO USE

Alongside `restuff.exe` in the build folder you need:

- **`shaderc_shared.dll`** — copy it from `%VULKAN_SDK%\Bin`. The renderer links
  `shaderc_shared.lib` and compiles translated guest shaders at runtime, so the exe
  will not start without this DLL. CMake does not stage it for you.
- the SDK runtime DLLs (`rexruntime*.dll`, `rexgpu-xenos*.dll`), which CMake does stage.
- an **`assets/`** folder containing your dumped game (with `Default.xex` at its root).
  A directory junction back to the repo's `assets/` works and avoids duplicating it.

First launch is slower while the shader/pipeline caches warm; later launches reuse
`shader_spv.bin` and `pipeline_cache.bin`.


for the time being until a launcher is completed all you must do is go into out/build/win-amd64-relwithdebinfo and either put the assets folder with the dumped assets and the default.xex in it or make a new folder somewhere and place the assets with the default.xex in there
the only folder/files you should have are the game files and the default.xex

---

## CURRENT ISSUES WITH THE GAME

Screen-Tearing
IF YOU FIND ANY CRASHES PLEASE MAKE AN ISSUE EXPLAINING WHERE IT WAS AND WHAT YOU WERE DOING (i.e. game crashed during loading or performing an action on an enemy causing the game to crash)

---

## CREDITS

MadLadMikael - for helping setup and teaching how to use REXGLUESDK and GITHUB

---

## DISCLAIMER

RE-STUFFED AND ITS DEVELOPERS DOES NOT CONDONE PIRACY.
