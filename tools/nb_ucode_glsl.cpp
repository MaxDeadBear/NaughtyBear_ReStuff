// Offline validation harness for the M2.3 ucode->GLSL translator.
// Translates each captured shader_dump/*.ucode.bin to GLSL, writes the GLSL to
// /tmp, and shells out to glslc to confirm it compiles to SPIR-V. Lets us
// iterate the translator without launching the game.
//
// Build:
//   clang++-20 -std=c++23 -I "<SDK>/include" -I src \
//     tools/nb_ucode_glsl.cpp src/renderer/ucode_translator.cpp -o /tmp/nb_ucode_glsl
// Run:
//   /tmp/nb_ucode_glsl <builddir>/shader_dump/*.ucode.bin
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "renderer/ucode_translator.h"

namespace uc = restuff::renderer::ucode;

static std::vector<uint32_t> ReadWords(const char* path) {
  std::vector<uint32_t> words;
  FILE* f = std::fopen(path, "rb");
  if (!f) return words;
  uint32_t w;
  while (std::fread(&w, 4, 1, f) == 1) words.push_back(w);  // keep big-endian; translator swaps
  std::fclose(f);
  return words;
}

int main(int argc, char** argv) {
  int ok = 0, fail = 0;
  for (int a = 1; a < argc; ++a) {
    const char* path = argv[a];
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    const bool is_vs = std::strncmp(base, "vs_", 3) == 0;
    auto w = ReadWords(path);
    if (w.empty()) {
      std::printf("[skip] %s (empty)\n", base);
      continue;
    }
    auto r = is_vs ? uc::TranslateVertexShader(w.data(), uint32_t(w.size()))
                   : uc::TranslatePixelShader(w.data(), uint32_t(w.size()));
    std::printf("\n==== %s (%zu dwords, %s) ====\n", base, w.size(), is_vs ? "VS" : "PS");
    if (!r.ok) {
      std::printf("  TRANSLATE FAIL: %s\n", r.error.c_str());
      // Still print any GLSL produced for debugging.
      if (!r.glsl.empty()) std::printf("%s\n", r.glsl.c_str());
      ++fail;
      continue;
    }
    // Write GLSL and compile with glslc.
    std::string glslpath = std::string("/tmp/") + base + (is_vs ? ".vert" : ".frag");
    std::string spvpath = glslpath + ".spv";
    FILE* g = std::fopen(glslpath.c_str(), "wb");
    std::fwrite(r.glsl.data(), 1, r.glsl.size(), g);
    std::fclose(g);
    std::string cmd = "glslc -o " + spvpath + " " + glslpath + " 2>&1";
    std::printf("%s\n", r.glsl.c_str());
    std::printf("  -- glslc --\n");
    int rc = std::system(cmd.c_str());
    if (rc == 0) {
      std::printf("  GLSLC OK -> %s\n", spvpath.c_str());
      ++ok;
    } else {
      std::printf("  GLSLC FAILED (rc=%d)\n", rc);
      ++fail;
    }
  }
  std::printf("\n==== %d ok, %d fail ====\n", ok, fail);
  return fail ? 1 : 0;
}
