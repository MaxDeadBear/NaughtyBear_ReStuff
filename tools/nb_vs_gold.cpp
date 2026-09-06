// VS gold reference: runs a vertex shader through the SDK's CPU
// ShaderInterpreter per vertex, with guest memory reconstructed from a
// voldraw dump (RESTUFF_DUMP_VOLDRAW in the live renderer: full register
// shadow + each active vfetch slot's raw big-endian window + manifest).
//
// The interpreter executes the real vfetches against that memory, so its
// per-vertex position export is the hardware-exact answer for the exact
// draw the live renderer saw. Diff against the translated-GLSL CPU replay
// (volharness/positions.csv) to locate translator divergence.
//
// Build:
//   clang++ -std=c++23 -O2 -w -DFMT_HEADER_ONLY=1 \
//     -I tools/shim -I "$SDK/include" \
//     tools/nb_vs_gold.cpp "$SDK/src/graphics/pipeline/shader/interpreter.cpp" \
//     -o nb_vs_gold
// Run:
//   nb_vs_gold voldraw_regs.bin ucode_vs_<hash>.bin voldraw_manifest.txt 696
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
    static int s_trace = -1;
    if (s_trace == -1) s_trace = getenv("NB_EXPORT_TRACE") ? 1 : 0;
    if (s_trace)
      fprintf(stderr, "EXPORT e%u mask=%X (%.9g, %.9g, %.9g, %.9g)\n", i, m, v[0], v[1], v[2],
              v[3]);
    for (int k = 0; k < 4; ++k)
      if ((m >> k) & 1) value[i][k] = v[k];
    mask[i] |= m;
  }
};

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s <regs.bin> <vs_ucode.bin> <manifest.txt> <nverts>\n", argv[0]);
    return 1;
  }
  auto regs_bytes = ReadFile(argv[1]);
  if (regs_bytes.size() < gfx::RegisterFile::kRegisterCount * 4) {
    fprintf(stderr, "bad regs.bin (%zu bytes)\n", regs_bytes.size());
    return 1;
  }
  static gfx::RegisterFile rf;
  std::memcpy(rf.values, regs_bytes.data(), gfx::RegisterFile::kRegisterCount * 4);

  uint8_t* phys = static_cast<uint8_t*>(mmap(nullptr, 0x20000000, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  if (phys == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  // Populate guest memory from the manifest (addresses masked to 512MB space).
  {
    FILE* mf = fopen(argv[3], "r");
    if (!mf) {
      perror("manifest");
      return 1;
    }
    unsigned slot, words;
    unsigned long long base, f0, f1;
    while (fscanf(mf, "%u 0x%llX %u 0x%llX 0x%llX", &slot, &base, &words, &f0, &f1) == 5) {
      char path[64];
      snprintf(path, sizeof(path), "voldraw_slot%02u.bin", slot);
      auto blob = ReadFile(path);
      const uint64_t off = base & 0x1FFFFFFFull;
      if (blob.empty() || off + blob.size() > 0x20000000ull) {
        fprintf(stderr, "slot %u: missing/oversize (%zu bytes @%llx)\n", slot, blob.size(), base);
        continue;
      }
      std::memcpy(phys + off, blob.data(), blob.size());
      fprintf(stderr, "# slot %u -> +0x%llx (%zu bytes)\n", slot, off, blob.size());
    }
    fclose(mf);
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
  interp.SetShader(gfx::xenos::ShaderType::kVertex, ucode.data());

  const int nverts = atoi(argv[4]);
  const char* dbg = getenv("NB_VS_GOLD_TEMPS");
  const int dbg_v = dbg ? atoi(dbg) : -1;
  const char* sv = getenv("NB_VS_START");
  const int v0 = sv ? atoi(sv) : 0;
  float* tr = interp.temp_registers();
  for (int v = v0; v < v0 + nverts; ++v) {
    sink.Reset();
    std::memset(tr, 0, 64 * 4 * sizeof(float));
    tr[0] = float(v);  // GEN_INDEX: vertex index arrives in r0.x
    interp.Execute();
    if (v == dbg_v) {
      for (int r2 = 0; r2 < 16; ++r2)
        fprintf(stderr, "gold r%-2d (%.9g, %.9g, %.9g, %.9g)\n", r2, tr[r2 * 4],
                tr[r2 * 4 + 1], tr[r2 * 4 + 2], tr[r2 * 4 + 3]);
    }
    const int P = int(gfx::ucode::ExportRegister::kVSPosition);
    printf("%d,%.9g,%.9g,%.9g,%.9g,%X\n", v, sink.value[P][0], sink.value[P][1],
           sink.value[P][2], sink.value[P][3], sink.mask[P]);
  }
  return 0;
}
