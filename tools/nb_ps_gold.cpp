// PS-only gold reference: runs a pixel shader through the SDK's CPU
// ShaderInterpreter with a register file exported by nb_trace_dump's
// XTR_HARNESS mode and interpolators injected DIRECTLY on the command line.
//
// Unlike nb_interp_harness this needs no vertex streams, index buffer, or
// guest memory -- so it works on harness exports whose memread payloads were
// not in the trace's indexed region (ib_ok=0 / got=0). That makes it usable
// for the reference build's world draws, where only regs.bin is recoverable.
//
// The interpreter stores ZEROS for texture fetches, so this is the gold ALU
// reference for the all-samples-black case: run our translated GLSL with its
// texture samples forced to zero and the two must agree component-for-
// component. Any divergence is a translator bug.
//
// Build (tools/shim FIRST on the include path so interpreter.cpp picks up the
// shim Memory class):
//   clang++ -std=c++23 -O2 -w -DFMT_HEADER_ONLY=1 \
//     -I tools/shim -I ~/Git/rexglue-sdk/include \
//     tools/nb_ps_gold.cpp \
//     ~/Git/rexglue-sdk/src/graphics/pipeline/shader/interpreter.cpp \
//     -o nb_ps_gold
// Run:
//   nb_ps_gold <regs.bin> <ps_ucode.bin> [rN=x,y,z,w ...]
//     e.g. nb_ps_gold regs.bin ps_1bab.ucode.bin r0=0.5,0.5,0,1 r1=0,1,0,0
//   env PS_GOLD_REGS=1  -> also dump temp registers r0..r15 after execution
#include <sys/mman.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/register_file.h>

namespace gfx = rex::graphics;

static std::vector<uint8_t> ReadFile(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return out;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(size_t(sz));
  if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
  fclose(f);
  return out;
}

struct CaptureSink : gfx::ShaderInterpreter::ExportSink {
  float value[64][4];
  uint32_t mask[64];
  void Reset() {
    std::memset(value, 0, sizeof(value));
    std::memset(mask, 0, sizeof(mask));
  }
  void Export(gfx::ucode::ExportRegister er, const float* v, uint32_t m) override {
    const uint32_t i = uint32_t(er);
    if (i >= 64) return;
    for (int k = 0; k < 4; ++k) {
      if ((m >> k) & 1) value[i][k] = v[k];
    }
    mask[i] |= m;
  }
};

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <regs.bin> <ps_ucode.bin> [rN=x,y,z,w ...]\n", argv[0]);
    return 1;
  }

  auto regs_bytes = ReadFile(argv[1]);
  if (regs_bytes.size() < gfx::RegisterFile::kRegisterCount * 4) {
    fprintf(stderr, "bad regs.bin (%zu bytes)\n", regs_bytes.size());
    return 1;
  }
  static gfx::RegisterFile rf;
  std::memcpy(rf.values, regs_bytes.data(), gfx::RegisterFile::kRegisterCount * 4);

  // The interpreter dereferences guest memory for fetches even when it stores
  // zeros for the results; give it a sparse, readable address space.
  uint8_t* phys = static_cast<uint8_t*>(mmap(nullptr, 0x20000000, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  if (phys == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  rex::memory::Memory mem;
  mem.physical_membase_shim = phys;

  auto ucode_be = ReadFile(argv[2]);
  if (ucode_be.empty() || (ucode_be.size() & 3)) {
    fprintf(stderr, "bad ucode %s\n", argv[2]);
    return 1;
  }
  std::vector<uint32_t> ucode(ucode_be.size() / 4);
  for (size_t i = 0; i < ucode.size(); ++i) {
    const uint8_t* p = ucode_be.data() + i * 4;
    ucode[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  gfx::ShaderInterpreter interp(rf, mem);
  CaptureSink sink;
  interp.SetExportSink(&sink);
  interp.SetShader(rex::graphics::xenos::ShaderType::kPixel, ucode.data());

  sink.Reset();
  float* tr = interp.temp_registers();
  std::memset(tr, 0, 64 * 4 * sizeof(float));

  // Interpolator injection: rN=x,y,z,w
  for (int a = 3; a < argc; ++a) {
    const char* s = argv[a];
    if (s[0] != 'r') continue;
    char* end = nullptr;
    const long idx = strtol(s + 1, &end, 10);
    if (!end || *end != '=' || idx < 0 || idx >= 64) continue;
    float v[4] = {0, 0, 0, 0};
    const char* p = end + 1;
    for (int k = 0; k < 4 && p && *p; ++k) {
      v[k] = strtof(p, &end);
      p = (end && *end == ',') ? end + 1 : nullptr;
    }
    std::memcpy(tr + idx * 4, v, sizeof(v));
    printf("# in  r%-2ld (%.9g, %.9g, %.9g, %.9g)\n", idx, v[0], v[1], v[2], v[3]);
  }

  const uint32_t ps_base = rf.values[0x2308] & 0x1FF;
  printf("# ps_base=%u  bool[4..7]=%08X %08X %08X %08X\n", ps_base, rf.values[0x4904],
         rf.values[0x4905], rf.values[0x4906], rf.values[0x4907]);

  interp.Execute();

  if (getenv("PS_GOLD_REGS")) {
    for (int r = 0; r < 16; ++r)
      printf("  r%-2d (%.9g, %.9g, %.9g, %.9g)\n", r, tr[r * 4], tr[r * 4 + 1], tr[r * 4 + 2],
             tr[r * 4 + 3]);
  }
  for (int er = 0; er < 64; ++er) {
    if (!sink.mask[er]) continue;
    printf("  PS e%d m=%X (%.9g, %.9g, %.9g, %.9g)\n", er, sink.mask[er], sink.value[er][0],
           sink.value[er][1], sink.value[er][2], sink.value[er][3]);
  }
  return 0;
}
