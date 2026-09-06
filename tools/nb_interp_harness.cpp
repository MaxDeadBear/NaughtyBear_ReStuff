// Ground-truth interpolant harness: replays a captured draw's vertex shader
// through the SDK's CPU ShaderInterpreter (the gold ALU/vfetch-semantics
// reference) on the REAL inputs exported by nb_trace_dump's XTR_HARNESS mode
// (full register file + raw guest memory spans + index buffer). Prints every
// export register (position + interpolators o_0..o_15) per vertex, so our
// translated-GLSL VS can be evaluated on the same vertices and diffed
// component-by-component. Any half-magnitude component = the world-dim bug.
//
// Build (tools/shim FIRST on the include path; interpreter.cpp compiled from
// SDK source so its Memory is the shim class):
//   clang++ -std=c++23 -O2 -w -DFMT_HEADER_ONLY=1 \
//     -I tools/shim -I ~/Git/rexglue-sdk/include \
//     tools/nb_interp_harness.cpp \
//     ~/Git/rexglue-sdk/src/graphics/pipeline/shader/interpreter.cpp \
//     -o nb_interp_harness
// Run:
//   nb_interp_harness <harness_dir> <vs_ucode.bin> [ib_pos ...]
//     harness_dir: regs.bin + ib.bin + mem_<addr8>.bin from XTR_HARNESS
//     vs_ucode.bin: guest-BE ucode dump (shader_dump/vs_*.ucode.bin)
//     ib_pos: positions in the index buffer to execute (default: a spread)
#include <sys/mman.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
    fprintf(stderr, "usage: %s <harness_dir> <vs_ucode.bin> [ib_pos ...]\n", argv[0]);
    return 1;
  }
  const std::string dir = argv[1];

  // Register file straight from the trace tool's shadow (same index space).
  auto regs_bytes = ReadFile(dir + "/regs.bin");
  if (regs_bytes.size() < gfx::RegisterFile::kRegisterCount * 4) {
    fprintf(stderr, "bad regs.bin (%zu bytes)\n", regs_bytes.size());
    return 1;
  }
  static gfx::RegisterFile rf;
  std::memcpy(rf.values, regs_bytes.data(), gfx::RegisterFile::kRegisterCount * 4);

  // 512MB guest physical space, sparse; raw big-endian guest bytes land at
  // their absolute physical offsets (the interpreter indexes the base pointer
  // by guest dword address, no masking needed below 0x20000000).
  uint8_t* phys = static_cast<uint8_t*>(mmap(nullptr, 0x20000000, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  if (phys == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    if (name.rfind("mem_", 0) != 0) continue;
    const uint32_t addr = uint32_t(strtoull(name.c_str() + 4, nullptr, 16));
    auto bytes = ReadFile(e.path().string());
    if (bytes.empty() || uint64_t(addr) + bytes.size() > 0x20000000ull) continue;
    std::memcpy(phys + addr, bytes.data(), bytes.size());
    printf("# mem %08X +%zu\n", addr, bytes.size());
  }
  rex::memory::Memory mem;
  mem.physical_membase_shim = phys;

  // Guest-BE ucode -> host order for the interpreter.
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

  auto ib = ReadFile(dir + "/ib.bin");
  const size_t nidx = ib.size() / 2;

  // Index post-processing exactly as the draw path does (draw_extent_estimator
  // pattern): swap, 24-bit, +VGT_INDX_OFFSET, clamp to VGT_MIN/MAX_VTX_INDX.
  const uint32_t indx_offset = rf.values[0x2102];
  const uint32_t min_index = rf.values[0x2101];
  const uint32_t max_index = rf.values[0x2100];
  printf("# n=%zu indx_offset=%u min=%u max=%u\n", nidx, indx_offset, min_index, max_index);

  gfx::ShaderInterpreter interp(rf, mem);
  interp.SetShader(rex::graphics::xenos::ShaderType::kVertex, ucode.data());
  CaptureSink sink;
  interp.SetExportSink(&sink);

  // Optional PS chaining: --ps <ps_ucode.bin> runs the pixel shader after the
  // VS per vertex, with interpolators preloaded from the VS exports. The
  // interpreter stores ZEROS for texture fetch results, so this is the gold
  // ALU reference for the all-samples-black case (validates offline executors).
  std::vector<uint32_t> ps_ucode;
  int argi = 3;
  if (argi + 1 < argc && std::string(argv[argi]) == "--ps") {
    auto ps_be = ReadFile(argv[argi + 1]);
    ps_ucode.resize(ps_be.size() / 4);
    for (size_t i = 0; i < ps_ucode.size(); ++i) {
      const uint8_t* p = ps_be.data() + i * 4;
      ps_ucode[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    argi += 2;
  }

  std::vector<size_t> positions;
  for (int a = argi; a < argc; ++a) positions.push_back(strtoull(argv[a], nullptr, 0));
  if (positions.empty()) {
    for (size_t p = 0; p < nidx; p += nidx > 16 ? nidx / 16 : 1) positions.push_back(p);
  }

  for (size_t pos : positions) {
    if (pos >= nidx) continue;
    uint32_t vi = (uint32_t(ib[pos * 2]) << 8) | ib[pos * 2 + 1];  // BE u16
    if (vi == 0xFFFF) {
      printf("ib[%zu] = restart\n", pos);
      continue;
    }
    vi = (vi + indx_offset) & 0xFFFFFF;
    if (vi < min_index) vi = min_index;
    if (vi > max_index) vi = max_index;

    sink.Reset();
    std::memset(interp.temp_registers(), 0, 64 * 4 * sizeof(float));
    interp.temp_registers()[0] = float(vi);
    interp.Execute();

    printf("ib[%zu] vtx=%u\n", pos, vi);
    if (!ps_ucode.empty()) {
      // Preload r0..r9 with the VS interpolator exports, then run the PS.
      float o_in[10][4] = {};
      for (int er = 0; er < 10; ++er)
        if (sink.mask[er]) std::memcpy(o_in[er], sink.value[er], 16);
      sink.Reset();
      std::memset(interp.temp_registers(), 0, 64 * 4 * sizeof(float));
      std::memcpy(interp.temp_registers(), o_in, sizeof(o_in));
      interp.SetShader(rex::graphics::xenos::ShaderType::kPixel, ps_ucode.data());
      interp.Execute();
      interp.SetShader(rex::graphics::xenos::ShaderType::kVertex, ucode.data());
      if (getenv("HARNESS_REGS")) {
        const float* tr = interp.temp_registers();
        for (int r = 0; r < 10; ++r)
          printf("  PS r%-2d (%.9g, %.9g, %.9g, %.9g)\n", r, tr[r * 4], tr[r * 4 + 1],
                 tr[r * 4 + 2], tr[r * 4 + 3]);
      }
      for (int er = 0; er < 64; ++er) {
        if (!sink.mask[er]) continue;
        printf("  PS e%d m=%X (%.9g, %.9g, %.9g, %.9g)\n", er, sink.mask[er], sink.value[er][0],
               sink.value[er][1], sink.value[er][2], sink.value[er][3]);
      }
      continue;
    }
    if (getenv("HARNESS_REGS")) {
      const float* tr = interp.temp_registers();
      for (int r = 0; r < 10; ++r)
        printf("  r%-2d (%.9g, %.9g, %.9g, %.9g)\n", r, tr[r * 4], tr[r * 4 + 1], tr[r * 4 + 2],
               tr[r * 4 + 3]);
    }
    for (int er = 0; er < 64; ++er) {
      if (!sink.mask[er]) continue;
      const char* nm = er == 62 ? "pos" : nullptr;
      if (nm)
        printf("  %s m=%X (%.9g, %.9g, %.9g, %.9g)\n", nm, sink.mask[er], sink.value[er][0],
               sink.value[er][1], sink.value[er][2], sink.value[er][3]);
      else
        printf("  o_%d m=%X (%.9g, %.9g, %.9g, %.9g)\n", er, sink.mask[er], sink.value[er][0],
               sink.value[er][1], sink.value[er][2], sink.value[er][3]);
    }
  }
  return 0;
}
