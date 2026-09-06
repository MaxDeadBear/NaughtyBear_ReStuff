// Offline PM4 trace walker: reads a .xtr GPU trace (trace_gpu_stream=true on
// the EMULATED xenos path) and prints per-draw register context in the same
// shape as the native renderer's frame dump, so the two can be diffed. This is
// the ground-truth instrument for content the native capture never produces
// (title right band / wordmark): whatever the emulated CP executed is in here.
//
// Build:
//   clang++ -std=c++23 -DFMT_HEADER_ONLY=1 -I <SDK>/include \
//     tools/nb_trace_dump.cpp \
//     <SDK>/out/linux-amd64/RelWithDebInfo/librexgraphicsrd.a \
//     <SDK>/out/linux-amd64/RelWithDebInfo/librexcorerd.a \
//     <SDK>/out/linux-amd64/RelWithDebInfo/librexfilesystemrd.a \
//     <SDK>/out/linux-amd64/RelWithDebInfo/libsnappyrd.a \
//     -o nb_trace_dump
// Run:
//   nb_trace_dump <trace.xtr> [frame_first] [frame_last]
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <map>
#include <set>

#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/trace_protocol.h>
#include <rex/graphics/trace_reader.h>

#include <snappy.h>

namespace gfx = rex::graphics;

// kMemoryRead index: guest phys addr -> latest record (compressed payload
// pointer into the mmapped trace). The tracer dedups unchanged memory, so a
// static terrain VB/IB is traced ONCE at level load and never again -- the
// index must span the whole file, not just the printed window. Payloads are
// decompressed lazily at FindMemRead time and cached until a newer record
// for the same base address supersedes them.
struct LazyMem {
  const uint8_t* payload;
  uint32_t enc, dec;
  uint32_t fmt;
};
static std::map<uint32_t, LazyMem> mem_index;
static std::map<uint32_t, std::vector<uint8_t>> mem_decoded;

static const std::vector<uint8_t>* FindMemRead(uint32_t addr, uint32_t* offset) {
  auto it = mem_index.upper_bound(addr);
  if (it == mem_index.begin()) return nullptr;
  --it;
  if (addr < it->first || addr >= it->first + it->second.dec) return nullptr;
  *offset = addr - it->first;
  auto dit = mem_decoded.find(it->first);
  if (dit == mem_decoded.end()) {
    const LazyMem& lm = it->second;
    std::vector<uint8_t> out(lm.dec);
    bool ok = false;
    if (lm.fmt == uint32_t(gfx::MemoryEncodingFormat::kNone)) {
      std::memcpy(out.data(), lm.payload, lm.dec);
      ok = true;
    } else {
      ok = snappy::RawUncompress(reinterpret_cast<const char*>(lm.payload),
                                 lm.enc, reinterpret_cast<char*>(out.data()));
    }
    if (!ok) return nullptr;
    dit = mem_decoded.emplace(it->first, std::move(out)).first;
  }
  return &dit->second;
}

static uint32_t regs[0x5004];  // register shadow incl. shader constants+fetch

static float RegF(uint32_t idx) {
  float f;
  std::memcpy(&f, &regs[idx], 4);
  return f;
}

// ALU constant row n (vec4) for the VS, honoring SQ_VS_CONST base.
static void VsConst(uint32_t n, float out[4]) {
  const uint32_t base = regs[0x2307] & 0x1FF;
  for (int k = 0; k < 4; ++k) out[k] = RegF(0x4000 + (base + n) * 4 + k);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <trace.xtr> [first] [last]\n", argv[0]);
    return 1;
  }
  gfx::TraceReader reader;
  if (!reader.Open(argv[1])) {
    fprintf(stderr, "failed to open %s\n", argv[1]);
    return 1;
  }
  // XTR_SAVEMEM=<hexaddr>,<bytes>,<file>: after the printed window, write the
  // latest traced memory at addr (reads OR writes -- resolves land as writes)
  // to a file. For pulling resolved images (shadow-blob minis) out of traces.
  struct SaveMemReq { uint32_t addr = 0, bytes = 0; const char* file = nullptr; };
  SaveMemReq savemem;
  if (const char* sm = getenv("XTR_SAVEMEM")) {
    static char smbuf[512];
    snprintf(smbuf, sizeof(smbuf), "%s", sm);
    char* p1 = strchr(smbuf, ',');
    char* p2 = p1 ? strchr(p1 + 1, ',') : nullptr;
    if (p1 && p2) {
      *p1 = *p2 = 0;
      savemem.addr = uint32_t(strtoull(smbuf, nullptr, 16));
      savemem.bytes = uint32_t(strtoull(p1 + 1, nullptr, 0));
      savemem.file = p2 + 1;
    }
  }
  const int nframes = reader.frame_count();
  int first = argc > 2 ? atoi(argv[2]) : 0;
  int last = argc > 3 ? atoi(argv[3]) : nframes - 1;
  printf("trace: %d frames\n", nframes);

  uint64_t bin_select = 0xFFFFFFFFull, bin_mask = 0xFFFFFFFFull;

  for (int fi = 0; fi < nframes; ++fi) {
    const auto* frame = reader.frame(fi);
    const bool print = fi >= first && fi <= last;
    int draw_i = 0;
    int ib_depth = 0;
    const uint8_t* trace_ptr = frame->start_ptr;
    const gfx::PacketStartCommand* pending = nullptr;
    if (print) printf("==== FRAME %d ====\n", fi);
    // Cheap content fingerprint in fast-forward mode: frame byte size
    // (gameplay frames carry MBs of memreads; menus are tiny).
    if (!print) printf("frame %d bytes=%zu\n", fi, size_t(frame->end_ptr - frame->start_ptr));
    while (trace_ptr < frame->end_ptr) {
      const uint32_t type_raw = *reinterpret_cast<const uint32_t*>(trace_ptr);
      const auto type = static_cast<gfx::TraceCommandType>(type_raw);
      switch (type) {
        case gfx::TraceCommandType::kPrimaryBufferStart: {
          auto* cmd = reinterpret_cast<const gfx::PrimaryBufferStartCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->count * 4;
          break;
        }
        case gfx::TraceCommandType::kPrimaryBufferEnd:
          trace_ptr += sizeof(gfx::PrimaryBufferEndCommand);
          break;
        case gfx::TraceCommandType::kIndirectBufferStart: {
          auto* cmd = reinterpret_cast<const gfx::IndirectBufferStartCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->count * 4;
          ++ib_depth;
          break;
        }
        case gfx::TraceCommandType::kIndirectBufferEnd:
          trace_ptr += sizeof(gfx::IndirectBufferEndCommand);
          if (ib_depth) --ib_depth;
          break;
        case gfx::TraceCommandType::kPacketStart: {
          auto* cmd = reinterpret_cast<const gfx::PacketStartCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->count * 4;
          pending = cmd;
          break;
        }
        case gfx::TraceCommandType::kPacketEnd: {
          trace_ptr += sizeof(gfx::PacketEndCommand);
          if (!pending) break;
          // Fast-forward: packet disassembly (register replay) only matters
          // near the printed window -- games re-upload constants per frame, so
          // a ~30-frame warm-up rebuilds all draw-relevant state. Raw record
          // walking is ~100x faster than DisasmPacket on 8k+ frames.
          static const int warmup = 30;
          // XTR_WATCH_REG=<hex>[,<hex2>]: log every write in [hex, hex2] from
          // frame 0 (forces full-trace disasm -- one-time boot uploads are
          // otherwise skipped by the fast-forward).
          static uint32_t watch_lo = 0, watch_hi = 0;
          static bool watch_init = false;
          if (!watch_init) {
            watch_init = true;
            if (const char* wr = getenv("XTR_WATCH_REG")) {
              watch_lo = uint32_t(strtoull(wr, nullptr, 16));
              const char* c2 = strchr(wr, ',');
              watch_hi = c2 ? uint32_t(strtoull(c2 + 1, nullptr, 16)) : watch_lo;
            }
          }
          if (!watch_lo && fi + warmup < first) {
            pending = nullptr;
            break;
          }
          const uint8_t* pkt = reinterpret_cast<const uint8_t*>(pending) +
                               sizeof(gfx::PacketStartCommand);
          gfx::PacketInfo info = {};
          if (gfx::PacketDisassembler::DisasmPacket(pkt, &info)) {
            // Optional packet-level trace (NB_TRACE_PACKETS=1): every packet
            // name + header, to find spans the native walker misparses.
            static const bool trace_pkts = getenv("NB_TRACE_PACKETS") != nullptr;
            if (print && trace_pkts) {
              const uint32_t hdr = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(pkt));
              printf("  pkt %-28s hdr=%08X count=%u%s\n",
                     info.type_info ? info.type_info->name : "?", hdr, info.count,
                     info.predicated ? " PRED" : "");
            }
            const bool skipped = info.predicated && (bin_select & bin_mask) == 0;
            for (const auto& a : info.actions) {
              switch (a.type) {
                case gfx::PacketAction::Type::kRegisterWrite:
                  if (!skipped && a.register_write.index < 0x5004)
                    regs[a.register_write.index] = a.register_write.value;
                  if (watch_lo && a.register_write.index >= watch_lo &&
                      a.register_write.index <= watch_hi) {
                    static uint32_t last[16] = {};
                    const uint32_t li = a.register_write.index & 15;
                    if (last[li] != a.register_write.value) {
                      last[li] = a.register_write.value;
                      printf("[WATCH] frame=%d reg=%04X value=%08X%s\n", fi,
                             a.register_write.index, a.register_write.value,
                             skipped ? " (SKIPPED-BIN)" : "");
                    }
                  }
                  break;
                case gfx::PacketAction::Type::kSetBinMask:
                  bin_mask = a.set_bin_mask.value;
                  if (print && getenv("NB_TRACE_PACKETS"))
                    printf("      bin_mask <- %016" PRIX64 " (select=%016" PRIX64 ")\n", bin_mask,
                           bin_select);
                  break;
                case gfx::PacketAction::Type::kSetBinSelect:
                  bin_select = a.set_bin_select.value;
                  if (print && getenv("NB_TRACE_PACKETS"))
                    printf("      bin_select <- %016" PRIX64 "\n", bin_select);
                  break;
              }
            }
            if (!skipped && info.type_info &&
                info.type_info->category == gfx::PacketCategory::kDraw) {
              // Raw packet words are guest big-endian in the trace.
              const auto word = [&](int i) {
                return __builtin_bswap32(reinterpret_cast<const uint32_t*>(pkt)[i]);
              };
              const uint32_t header = word(0);
              const uint32_t opcode = (header >> 8) & 0x7F;
              // DRAW_INDX: word1 viz, word2 initiator; DRAW_INDX_2: word1 initiator.
              const uint32_t initiator = (opcode == 0x22) ? word(2) : word(1);
              const uint32_t prim = initiator & 0x3F;
              const uint32_t nidx = (initiator >> 16) & 0xFFFF;
              const uint32_t srcsel = (initiator >> 6) & 3;
              const uint32_t w0 = regs[0x4800], w1 = regs[0x4801], w2 = regs[0x4802];
              const uint32_t ba = (w1 >> 12) & 0xFFFFF;
              float c0[4], c1[4], c4[4], c5[4];
              VsConst(0, c0); VsConst(1, c1); VsConst(4, c4); VsConst(5, c5);
              if (print) {
                printf("#%-3d ib%d op=%02X prim=%u n=%u ss=%u tex0[%08X %ux%u f%u cl%u%u t%u] "
                       "surf=%u/m%u cmask=%X mode=%X VP=(%g,%g,%g,%g)\n",
                       draw_i, ib_depth, opcode, prim, nidx, srcsel, ba << 12, (w2 & 0x1FFF) + 1,
                       ((w2 >> 13) & 0x1FFF) + 1, w1 & 0x3F, (w0 >> 10) & 7, (w0 >> 13) & 7,
                       (w0 >> 31) & 1, regs[0x2000] & 0x3FFF, (regs[0x2000] >> 16) & 3,
                       regs[0x2104] & 0xF, regs[0x2208] & 7, RegF(0x210F), RegF(0x2110),
                       RegF(0x2111), RegF(0x2112));
                // M3.74 shadow-mask hunt: resolve destination + control on copy
                // draws (RB_COPY_CONTROL 0x2318, RB_COPY_DEST_BASE 0x2319,
                // _PITCH 0x231A, _INFO 0x231B) -- where the reference lands the
                // 680-surface shadow mask that world PSes sample via t1.
                if ((regs[0x2208] & 7) == 6)
                  printf("     COPY ctl=%08X dest=%08X pitch=%08X info=%08X\n",
                         regs[0x2318], regs[0x2319], regs[0x231A], regs[0x231B]);
                // M3.76 light-state rows: c10/c11 (sun position/direction in
                // the terrain family) + SQ bool constants 0x4900-07 (bcond
                // bits gate the PS light blocks) -- the native-vs-reference
                // dim discriminator.
                {
                  float c10[4], c11[4];
                  VsConst(10, c10);
                  VsConst(11, c11);
                  // lch: FNV over VS constant rows 8..31 (the light region),
                  // same recipe as the native [DRAWLIST] lch.
                  const uint32_t vsbase = regs[0x2307] & 0x1FF;
                  uint64_t lch = 1469598103934665603ull;
                  for (uint32_t ci = 32; ci < 128; ++ci)
                    lch = (lch ^ regs[0x4000 + (vsbase + 8) * 4 + (ci - 32)]) * 1099511628211ull;
                  printf("     c10=(%.5f,%.5f,%.5f,%.5f) c11=(%.5f,%.5f,%.5f,%.5f) lch=%016llX "
                         "bool=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                         c10[0], c10[1], c10[2], c10[3], c11[0], c11[1], c11[2], c11[3],
                         (unsigned long long)lch, regs[0x4900], regs[0x4901], regs[0x4902],
                         regs[0x4903], regs[0x4904], regs[0x4905], regs[0x4906], regs[0x4907]);
                  // XTR_CROWS=1: full c8..c31 rows (the light region) for
                  // row-level diffing against the native [DRAWCONST] dump.
                  static const bool crows = getenv("XTR_CROWS") != nullptr;
                  if (crows) {
                    for (int rr = 8; rr < 32; ++rr) {
                      float cr[4];
                      VsConst(rr, cr);
                      printf("     CROW c%d=(%.6f,%.6f,%.6f,%.6f)\n", rr, cr[0], cr[1], cr[2],
                             cr[3]);
                    }
                    // PS bank (SQ_PS_CONST 0x2308 base into the same ALU file):
                    // the PS light blocks read THESE c8/c9/c10 -- never yet
                    // compared against native.
                    const uint32_t psb = regs[0x2308] & 0x1FF;
                    for (int rr = 0; rr < 253; ++rr) {
                      const uint32_t* pw = &regs[(0x4000 + (psb + rr) * 4) & 0x7FFF];
                      float pf[4];
                      std::memcpy(pf, pw, 16);
                      printf("     PROW c%d=(%.6f,%.6f,%.6f,%.6f)\n", rr, pf[0], pf[1], pf[2],
                             pf[3]);
                    }
                    for (int rr = 253; rr < 256; ++rr) {
                      const uint32_t* pw = &regs[(0x4000 + (psb + rr) * 4) & 0x7FFF];
                      float pf[4];
                      std::memcpy(pf, pw, 16);
                      printf("     PROW c%d=(%.6f,%.6f,%.6f,%.6f)\n", rr, pf[0], pf[1], pf[2],
                             pf[3]);
                    }
                    {
                      uint64_t ph = 1469598103934665603ull;
                      for (uint32_t rr = 40; rr < 253; ++rr)
                        for (int k = 0; k < 4; ++k)
                          ph = (ph ^ regs[(0x4000 + (psb + rr) * 4 + k) & 0x7FFF]) * 1099511628211ull;
                      printf("     PHASH=%016llX\n", (unsigned long long)ph);
                    }
                    printf("     PBASE=%u\n", psb);
                  }
                }
                // t1/t2 texture fetch slots (6 dwords each at 0x4800+slot*6):
                // the shadow-projection mask rides a secondary sampler.
                {
                  const uint32_t t1w1 = regs[0x4806 + 1], t1w2 = regs[0x4806 + 2];
                  const uint32_t t2w1 = regs[0x480C + 1], t2w2 = regs[0x480C + 2];
                  printf("     t1[%08X %ux%u f%u w0=%08X w3=%08X] t2[%08X %ux%u f%u]\n",
                         ((t1w1 >> 12) & 0xFFFFF) << 12, (t1w2 & 0x1FFF) + 1,
                         ((t1w2 >> 13) & 0x1FFF) + 1, t1w1 & 0x3F, regs[0x4806],
                         regs[0x4806 + 3],
                         ((t2w1 >> 12) & 0xFFFFF) << 12, (t2w2 & 0x1FFF) + 1,
                         ((t2w2 >> 13) & 0x1FFF) + 1, t2w1 & 0x3F);
                }
                printf("     c0=(%.8f,%.8f,%.8f,%.8f) c1=(%.8f,%.8f,%.8f,%.8f)\n",
                       c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3]);
                printf("     c4=(%.8f,%.8f,%.8f,%.8f) c5=(%.8f,%.8f,%.8f,%.8f)\n",
                       c4[0], c4[1], c4[2], c4[3], c5[0], c5[1], c5[2], c5[3]);
                // M3.66 winding diff: the native-vs-emulated comparison fields.
                // su (PA_SU_SC_MODE_CNTL 0x2205): cull/front-face; dc
                // (RB_DEPTHCONTROL 0x2200); vte (PA_CL_VTE_CNTL 0x2206);
                // zsc/zoff (PA_CL_VPORT_ZSCALE/ZOFFSET); c2/c3 complete the WVP.
                float c2r[4], c3r[4];
                VsConst(2, c2r); VsConst(3, c3r);
                printf("     c2=(%.8f,%.8f,%.8f,%.8f) c3=(%.8f,%.8f,%.8f,%.8f)\n",
                       c2r[0], c2r[1], c2r[2], c2r[3], c3r[0], c3r[1], c3r[2], c3r[3]);
                // Vertex-fetch constants are 2 dwords per slot (word0 low bits
                // = base address; word1 = size/format), 0x4800 + slot*2.
                printf("     su=%X dc=%X vte=%X rsti=%X dmasz=%X zsc=%g zoff=%g vf95=[%08X %08X]\n",
                       regs[0x2205], regs[0x2200], regs[0x2206], regs[0x2103], regs[0x21FB], RegF(0x2113), RegF(0x2114),
                       regs[0x4800 + 95 * 2], regs[0x4800 + 95 * 2 + 1]);
                // M3.72 winding-regression census: blend + color control + stencil
                // masks (RB_BLENDCONTROL0 0x2201, RB_COLORCONTROL 0x2202,
                // RB_STENCILREFMASK 0x210C) -- which draw class changed sides.
                printf("     bc=%X cc=%X srm=%X cinfo=%08X sq=[%08X %08X]\n", regs[0x2201],
                       regs[0x2202], regs[0x210C], regs[0x2001], regs[0x2180], regs[0x2181]);
                // XTR_HARNESS=<outdir> + XTR_HARNESS_N=<n>: full interpreter-
                // harness export at the first indexed draw with that index
                // count and a nonzero colormask (the lit pass, not the
                // prepass). Dumps the complete register shadow, the raw
                // (guest-BE) index bytes, and every vertex-fetch slot's memory
                // span, so an offline SDK ShaderInterpreter can replay the
                // draw's VS on the real inputs. Also prints the native
                // [DRAWLIST]-recipe content hash for cross-checking.
                static const char* hz_dir = getenv("XTR_HARNESS");
                static const uint32_t hz_n =
                    getenv("XTR_HARNESS_N") ? strtoul(getenv("XTR_HARNESS_N"), nullptr, 0) : 0;
                // XTR_HARNESS_DRAW=<draw#>: exact draw-index trigger instead
                // of the (n, colormask) match -- for fullscreen quads whose
                // n=4 is ambiguous.
                static const int hz_draw =
                    getenv("XTR_HARNESS_DRAW") ? atoi(getenv("XTR_HARNESS_DRAW")) : -1;
                static bool hz_done = false;
                if (hz_dir && !hz_done && opcode == 0x22 &&
                    (hz_draw >= 0 ? draw_i == hz_draw
                                  : (nidx == hz_n && (regs[0x2104] & 0xF) != 0))) {
                  const uint32_t ib_addr = word(3) & ~0x3u;
                  const uint32_t idx32 = (initiator >> 11) & 1;
                  auto read_span = [&](uint32_t addr, size_t want, std::vector<uint8_t>& out) {
                    out.clear();
                    while (out.size() < want) {
                      uint32_t off = 0;
                      const auto* blk = FindMemRead(addr + uint32_t(out.size()), &off);
                      if (!blk) break;
                      size_t take = blk->size() - off;
                      if (take > want - out.size()) take = want - out.size();
                      out.insert(out.end(), blk->data() + off, blk->data() + off + take);
                    }
                    return out.size() == want;
                  };
                  std::vector<uint8_t> ib;
                  const size_t ib_bytes = size_t(nidx) * (idx32 ? 4 : 2);
                  const bool ib_ok = read_span(ib_addr, ib_bytes, ib);
                  // Native drawlist ch: FNV-1a over host-order widened indices
                  // (u16 0xFFFF restart widens to 0xFFFFFFFF).
                  uint64_t hch = 1469598103934665603ull;
                  for (size_t i = 0; i + (idx32 ? 4 : 2) <= ib.size(); i += (idx32 ? 4 : 2)) {
                    uint32_t v;
                    if (idx32) {
                      v = (uint32_t(ib[i]) << 24) | (uint32_t(ib[i + 1]) << 16) |
                          (uint32_t(ib[i + 2]) << 8) | ib[i + 3];
                    } else {
                      v = (uint32_t(ib[i]) << 8) | ib[i + 1];
                      if (v == 0xFFFFu) v = 0xFFFFFFFFu;
                    }
                    hch = (hch ^ v) * 1099511628211ull;
                  }
                  char nm[512];
                  snprintf(nm, sizeof(nm), "%s/regs.bin", hz_dir);
                  if (FILE* f = fopen(nm, "wb")) {
                    fwrite(regs, 4, 0x5003, f);
                    fclose(f);
                  }
                  snprintf(nm, sizeof(nm), "%s/ib.bin", hz_dir);
                  if (FILE* f = fopen(nm, "wb")) {
                    fwrite(ib.data(), 1, ib.size(), f);
                    fclose(f);
                  }
                  snprintf(nm, sizeof(nm), "%s/meta.txt", hz_dir);
                  FILE* mf = fopen(nm, "w");
                  if (mf)
                    fprintf(mf,
                            "frame=%d draw=%d op=%02X prim=%u n=%u idx32=%u ib=%08X ib_ok=%d "
                            "ch=%016llX cm=%X vsbase=%u psbase=%u pgm_cntl=%08X ctx_misc=%08X\n",
                            fi, draw_i, opcode, prim, nidx, idx32, ib_addr, ib_ok ? 1 : 0,
                            (unsigned long long)hch, regs[0x2104] & 0xF, regs[0x2307] & 0x1FF,
                            regs[0x2308] & 0x1FF, regs[0x2180], regs[0x2181]);
                  std::set<uint32_t> hz_seen;
                  for (uint32_t slot = 0; slot < 96; ++slot) {
                    const uint32_t w0s = regs[0x4800 + slot * 2];
                    const uint32_t w1s = regs[0x4800 + slot * 2 + 1];
                    if ((w0s & 3) != 3) continue;  // FetchConstantType: 3 = kVertex
                    const uint32_t vaddr = (w0s >> 2) << 2;
                    const size_t vbytes = size_t((w1s >> 2) & 0xFFFFFF) * 4;
                    if (!vaddr || !vbytes || vbytes > (64u << 20)) continue;
                    if (!hz_seen.insert(vaddr).second) continue;
                    std::vector<uint8_t> vb;
                    const bool vb_ok = read_span(vaddr, vbytes, vb);
                    snprintf(nm, sizeof(nm), "%s/mem_%08X.bin", hz_dir, vaddr);
                    if (FILE* f = fopen(nm, "wb")) {
                      fwrite(vb.data(), 1, vb.size(), f);
                      fclose(f);
                    }
                    if (mf)
                      fprintf(mf, "vf slot=%u addr=%08X bytes=%zu got=%zu endian=%u\n", slot,
                              vaddr, vbytes, vb.size(), w1s & 3);
                  }
                  // Texture fetch constants (type kTexture=2, 6 dwords/slot):
                  // export each bound texture's guest span for the offline PS
                  // executor. Size is a generous over-estimate (linear extent
                  // x4 covers tiling padding and the mip tail; read_span stops
                  // at whatever the trace actually holds).
                  for (uint32_t ts = 0; ts < 16; ++ts) {
                    const uint32_t tw0 = regs[0x4800 + ts * 6];
                    if ((tw0 & 3) != 2) continue;
                    const uint32_t tw1 = regs[0x4800 + ts * 6 + 1];
                    const uint32_t tw2 = regs[0x4800 + ts * 6 + 2];
                    const uint32_t tbase = ((tw1 >> 12) & 0xFFFFF) << 12;
                    const uint32_t tw = (tw2 & 0x1FFF) + 1, th = ((tw2 >> 13) & 0x1FFF) + 1;
                    const uint32_t tfmt = tw1 & 0x3F;
                    if (!tbase) continue;
                    if (!hz_seen.insert(tbase).second) continue;
                    const size_t bpp4 = (tfmt == 18 || tfmt == 19 || tfmt == 20) ? 2 : 16;
                    size_t tbytes = size_t(tw) * th * bpp4 / 2;
                    if (tbytes > (16u << 20)) tbytes = 16u << 20;
                    std::vector<uint8_t> tb;
                    read_span(tbase, tbytes, tb);
                    snprintf(nm, sizeof(nm), "%s/tex_%08X.bin", hz_dir, tbase);
                    if (FILE* f = fopen(nm, "wb")) {
                      fwrite(tb.data(), 1, tb.size(), f);
                      fclose(f);
                    }
                    if (mf)
                      fprintf(mf, "tex slot=%u addr=%08X fmt=%u %ux%u want=%zu got=%zu w0=%08X "
                                  "w1=%08X w2=%08X w3=%08X w4=%08X w5=%08X\n",
                              ts, tbase, tfmt, tw, th, tbytes, tb.size(),
                              regs[0x4800 + ts * 6], regs[0x4800 + ts * 6 + 1],
                              regs[0x4800 + ts * 6 + 2], regs[0x4800 + ts * 6 + 3],
                              regs[0x4800 + ts * 6 + 4], regs[0x4800 + ts * 6 + 5]);
                  }
                  if (mf) fclose(mf);
                  printf("     [HARNESS] frame=%d draw=%d n=%u ch=%016llX ib_ok=%d -> %s\n", fi,
                         draw_i, nidx, (unsigned long long)hch, ib_ok ? 1 : 0, hz_dir);
                  hz_done = true;
                }
                // XTR_EXPORT=<dir>: dump each printed cull-back strip draw's
                // index + vertex bytes (from the trace's memreads) for the
                // offline winding discriminator. Once per (vb, n, ib).
                static const char* exp_dir = getenv("XTR_EXPORT");
                // All color strips (cull state in the filename would collide;
                // the census wants the full draw set either way).
                if (exp_dir && prim == 6 && opcode == 0x22) {
                  const uint32_t ib_addr = word(3) & ~0x3u;
                  const uint32_t idx32 = (initiator >> 11) & 1;
                  const uint32_t vb = regs[0x4800 + 95 * 2] & ~0x3u;
                  static std::set<uint64_t> exported;
                  if (exported.insert((uint64_t(vb) << 24) ^ (uint64_t(nidx) << 4) ^ ib_addr)
                          .second) {
                    // Buffers regularly SPAN several traced memread blocks --
                    // stitch contiguous blocks until `want` bytes or a gap.
                    auto write_span = [&](const char* nm, uint32_t addr, size_t want) {
                      FILE* f = fopen(nm, "wb");
                      if (!f) return;
                      size_t got = 0;
                      while (got < want) {
                        uint32_t off = 0;
                        const auto* blk = FindMemRead(addr + uint32_t(got), &off);
                        if (!blk) break;
                        size_t take = blk->size() - off;
                        if (take > want - got) take = want - got;
                        fwrite(blk->data() + off, 1, take, f);
                        got += take;
                      }
                      fclose(f);
                    };
                    char nm[512];
                    snprintf(nm, sizeof(nm), "%s/ib%u_%08X_%u_%08X.bin", exp_dir, idx32 ? 32 : 16,
                             vb, nidx, ib_addr);
                    write_span(nm, ib_addr, size_t(nidx) * (idx32 ? 4 : 2));
                    snprintf(nm, sizeof(nm), "%s/vb_%08X_%u_%08X.bin", exp_dir, vb, nidx, ib_addr);
                    write_span(nm, vb, 8u << 20);
                  }
                }
                if (nidx == 750) {
                  // Hills mesh (C53C, stride 8 = pos u32 + colour u32): print
                  // the traced VB's first vertices to get ground-truth colours.
                  const uint32_t vb = regs[0x4800 + 0xBE] & ~0x3u;
                  uint32_t off = 0;
                  const auto* blk = FindMemRead(vb, &off);
                  printf("     MESH vb=%08X %s", vb, blk ? "" : "(no memread)");
                  if (blk) {
                    for (int v = 0; v < 6 && off + v * 8 + 8 <= blk->size(); ++v) {
                      const uint8_t* p = blk->data() + off + v * 8;
                      const int16_t x = int16_t((p[0] << 8) | p[1]);
                      const int16_t y = int16_t((p[2] << 8) | p[3]);
                      printf(" v%d(%d,%d c:%02X%02X%02X%02X)", v, x, y, p[4], p[5], p[6], p[7]);
                    }
                  }
                  printf("\n");
                }
                if ((ba << 12) == 0x06004000) {
                  // Felt draw: stream-0 vfetch const = slot 0x5F (dword 0xBE).
                  // Decode the 12 vertices' s16 positions from the traced
                  // memory (stride 12: pos s16x2 + 2 packed colours).
                  const uint32_t vb = regs[0x4800 + 0xBE] & ~0x3u;
                  uint32_t off = 0;
                  const auto* blk = FindMemRead(vb, &off);
                  printf("     vb=%08X %s pos:", vb, blk ? "" : "(no memread!)");
                  if (blk) {
                    for (int v = 0; v < 12 && off + v * 12 + 4 <= blk->size(); ++v) {
                      const uint8_t* p = blk->data() + off + v * 12;
                      const int16_t x = int16_t((p[0] << 8) | p[1]);
                      const int16_t y = int16_t((p[2] << 8) | p[3]);
                      printf(" (%d,%d)", x, y);
                    }
                    printf("\n     raw bytes v0..v2:");
                    for (int b = 0; b < 36 && off + b < blk->size(); ++b) {
                      if (b % 12 == 0) printf(" |");
                      printf(" %02X", (*blk)[off + b]);
                    }
                  }
                  printf("\n");
                }
              }
              ++draw_i;
            }
          }
          pending = nullptr;
          break;
        }
        case gfx::TraceCommandType::kMemoryRead: {
          auto* cmd = reinterpret_cast<const gfx::MemoryCommand*>(trace_ptr);
          const uint8_t* payload = trace_ptr + sizeof(*cmd);
          trace_ptr += sizeof(*cmd) + cmd->encoded_length;
          if (cmd->decoded_length && cmd->decoded_length < (1u << 23)) {
            mem_index[cmd->base_ptr] =
                LazyMem{payload, cmd->encoded_length, cmd->decoded_length,
                        uint32_t(cmd->encoding_format)};
            mem_decoded.erase(cmd->base_ptr);
          }
          break;
        }
        case gfx::TraceCommandType::kMemoryWrite: {
          // Dynamic ring buffers (0x05/0x06-range IB/VB heaps) are traced as
          // WRITES, not reads -- index them the same way.
          auto* cmd = reinterpret_cast<const gfx::MemoryCommand*>(trace_ptr);
          const uint8_t* payload = trace_ptr + sizeof(*cmd);
          trace_ptr += sizeof(*cmd) + cmd->encoded_length;
          if (cmd->decoded_length && cmd->decoded_length < (1u << 23)) {
            mem_index[cmd->base_ptr] =
                LazyMem{payload, cmd->encoded_length, cmd->decoded_length,
                        uint32_t(cmd->encoding_format)};
            mem_decoded.erase(cmd->base_ptr);
          }
          break;
        }
        case gfx::TraceCommandType::kEdramSnapshot: {
          auto* cmd = reinterpret_cast<const gfx::EdramSnapshotCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->encoded_length;
          break;
        }
        case gfx::TraceCommandType::kEvent:
          trace_ptr += sizeof(gfx::EventCommand);
          break;
        case gfx::TraceCommandType::kRegisters: {
          // M3.82: apply register snapshots (the trace opens with a FULL
          // register-file dump -- shader literal constants uploaded before
          // the trace began are ONLY visible here).
          auto* cmd = reinterpret_cast<const gfx::RegistersCommand*>(trace_ptr);
          const uint8_t* payload = trace_ptr + sizeof(*cmd);
          trace_ptr += sizeof(*cmd) + cmd->encoded_length;
          if (cmd->first_register + cmd->register_count <= 0x5004) {
            std::vector<uint8_t> out(size_t(cmd->register_count) * 4);
            bool ok = false;
            if (cmd->encoding_format == gfx::MemoryEncodingFormat::kNone) {
              std::memcpy(out.data(), payload, out.size());
              ok = true;
            } else {
              ok = snappy::RawUncompress(reinterpret_cast<const char*>(payload),
                                         cmd->encoded_length,
                                         reinterpret_cast<char*>(out.data()));
            }
            if (ok) std::memcpy(&regs[cmd->first_register], out.data(), out.size());
          }
          break;
        }
        case gfx::TraceCommandType::kGammaRamp: {
          auto* cmd = reinterpret_cast<const gfx::GammaRampCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->encoded_length;
          break;
        }
        default:
          fprintf(stderr, "frame %d: unknown trace command %u; aborting frame\n", fi, type_raw);
          trace_ptr = frame->end_ptr;
          break;
      }
    }
    if (print || draw_i > 60)
      printf("---- frame %d: %d draws ----\n", fi, draw_i);
    if (savemem.file && fi == last) {
      FILE* f = fopen(savemem.file, "wb");
      size_t got = 0;
      while (f && got < savemem.bytes) {
        uint32_t off = 0;
        const auto* blk = FindMemRead(savemem.addr + uint32_t(got), &off);
        if (!blk) break;
        size_t take = blk->size() - off;
        if (take > savemem.bytes - got) take = savemem.bytes - got;
        fwrite(blk->data() + off, 1, take, f);
        got += take;
      }
      if (f) fclose(f);
      fprintf(stderr, "savemem %08X: %zu of %u bytes -> %s\n", savemem.addr, got, savemem.bytes,
              savemem.file);
      break;
    }
  }
  return 0;
}
