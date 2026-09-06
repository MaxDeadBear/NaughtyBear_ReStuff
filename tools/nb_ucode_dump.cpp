// Standalone Xenos ucode dumper for shader_dump/*.ucode.bin blobs.
// Uses the SDK's header-only rex/graphics/format/ucode.h instruction structs.
// Build: clang++ -std=c++20 -I "<SDK>/include" nb_ucode_dump.cpp -o nb_ucode_dump
// ALU opcodes print as integers — decode against the AluVectorOpcode /
// AluScalarOpcode enums in ucode.h.
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <rex/graphics/format/ucode.h>

using namespace rex::graphics::ucode;
namespace xenos = rex::graphics::xenos;

static std::vector<uint32_t> ReadWords(const char* path) {
  std::vector<uint32_t> words;
  FILE* f = std::fopen(path, "rb");
  if (!f) return words;
  uint32_t w;
  while (std::fread(&w, 4, 1, f) == 1) {
    words.push_back(std::byteswap(w));  // guest memory is big-endian
  }
  std::fclose(f);
  return words;
}

static const char* CfName(ControlFlowOpcode op) {
  switch (op) {
    case ControlFlowOpcode::kNop: return "nop";
    case ControlFlowOpcode::kExec: return "exec";
    case ControlFlowOpcode::kExecEnd: return "exec_end";
    case ControlFlowOpcode::kCondExec: return "cond_exec";
    case ControlFlowOpcode::kCondExecEnd: return "cond_exec_end";
    case ControlFlowOpcode::kCondExecPred: return "cond_exec_pred";
    case ControlFlowOpcode::kCondExecPredEnd: return "cond_exec_pred_end";
    case ControlFlowOpcode::kLoopStart: return "loop_start";
    case ControlFlowOpcode::kLoopEnd: return "loop_end";
    case ControlFlowOpcode::kCondCall: return "cond_call";
    case ControlFlowOpcode::kReturn: return "return";
    case ControlFlowOpcode::kCondJmp: return "cond_jmp";
    case ControlFlowOpcode::kAlloc: return "alloc";
    case ControlFlowOpcode::kCondExecPredClean: return "cond_exec_pred_clean";
    case ControlFlowOpcode::kCondExecPredCleanEnd: return "cond_exec_pred_clean_end";
    case ControlFlowOpcode::kMarkVsFetchDone: return "mark_vs_fetch_done";
    default: return "?";
  }
}

static void PrintSwizzle(uint32_t swiz) {
  // 2 bits per component, offset-from-identity encoding for ALU; print raw.
  std::printf("swiz=%03X", swiz);
}

static void DumpSlot(const uint32_t* s, bool is_fetch) {
  if (is_fetch) {
    const auto fop = FetchOpcode(s[0] & 0x1F);
    if (fop == FetchOpcode::kVertexFetch) {
      VertexFetchInstruction vf;
      std::memcpy(&vf, s, 12);
      std::printf(
          "      vfetch%s r%u <- r%u.%u  fetchconst=%u fmt=%u stride=%u offset=%d signed=%d "
          "norm=%d dstswiz=%03X\n",
          vf.is_mini_fetch() ? "_mini" : "_full", vf.dest(), vf.src(), vf.src_swizzle(),
          vf.fetch_constant_index(), unsigned(vf.data_format()), vf.stride(), vf.offset(),
          vf.is_signed(), vf.is_normalized(), vf.dest_swizzle());
    } else {
      TextureFetchInstruction tf;
      std::memcpy(&tf, s, 12);
      std::printf("      tfetch op=%u r%u <- r%u fetchconst=%u dstswiz=%03X srcswiz=%03X unnorm=%u magf=%u minf=%u\n",
                  unsigned(tf.opcode()), tf.dest(), tf.src(), tf.fetch_constant_index(),
                  tf.dest_swizzle(), tf.src_swizzle(), unsigned(tf.unnormalized_coordinates()),
                  unsigned(tf.mag_filter()), unsigned(tf.min_filter()));
    }
    return;
  }
  AluInstruction a;
  std::memcpy(&a, s, 12);
  std::printf("      alu ");
  std::printf("v%u ", unsigned(a.vector_opcode()));
  std::printf("%sdst=r%u mask=%X ", a.is_export() ? "EXPORT " : "", a.vector_dest(),
              a.vector_write_mask());
  if (a.scalar_write_mask() || unsigned(a.scalar_opcode()) != 0) {
    std::printf("| s%u sdst=r%u smask=%X ", unsigned(a.scalar_opcode()), a.scalar_dest(),
                a.scalar_write_mask());
  }
  for (uint32_t i = 1; i <= 3; ++i) {
    std::printf("src%u=%s%u.", i, a.src_is_temp(i) ? "r" : "c", a.src_reg(i));
    PrintSwizzle(a.src_swizzle(i));
    if (a.src_negate(i)) std::printf("(neg)");
    if (!a.src_is_temp(i) && a.src_const_is_addressed(i)) std::printf("(REL)");
    std::printf(" ");
  }
  if (a.abs_constants()) std::printf("ABSC ");
  if (a.is_const_address_register_relative()) std::printf("RELA0 ");
  if (a.is_export()) std::printf("(export reg %u)", a.vector_dest());
  std::printf("\n");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <ucode.bin>...\n", argv[0]);
    return 1;
  }
  for (int arg = 1; arg < argc; ++arg) {
    auto w = ReadWords(argv[arg]);
    std::printf("==== %s (%zu dwords) ====\n", argv[arg], w.size());
    // Control flow: 2 x 48-bit instructions per 3 dwords, from word 0 until
    // the first clause address.
    size_t min_clause_dwords = w.size();
    bool ended = false;
    for (size_t i = 0; !ended; ++i) {
      const size_t base = (i / 2) * 3;
      if (base + 3 > w.size() || base >= min_clause_dwords) break;
      uint32_t bits[2];
      if (i & 1) {
        bits[0] = (w[base + 1] >> 16) | (w[base + 2] << 16);
        bits[1] = w[base + 2] >> 16;
      } else {
        bits[0] = w[base];
        bits[1] = w[base + 1] & 0xFFFF;
      }
      const auto op = ControlFlowOpcode((bits[1] >> 12) & 0xF);
      std::printf("  cf%02zu %-22s", i, CfName(op));
      switch (op) {
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd:
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd: {
          ControlFlowExecInstruction ex;
          std::memcpy(&ex, bits, 8);
          std::printf(" addr=%u count=%u seq=%03X\n", ex.address(), ex.count(), ex.sequence());
          min_clause_dwords = std::min<size_t>(min_clause_dwords, size_t(ex.address()) * 3);
          for (uint32_t j = 0; j < ex.count(); ++j) {
            const size_t slot = (size_t(ex.address()) + j) * 3;
            if (slot + 3 > w.size()) break;
            DumpSlot(w.data() + slot, (ex.sequence() >> (j * 2)) & 1);
          }
          if (op == ControlFlowOpcode::kExecEnd || op == ControlFlowOpcode::kCondExecEnd ||
              op == ControlFlowOpcode::kCondExecPredEnd ||
              op == ControlFlowOpcode::kCondExecPredCleanEnd) {
            ended = true;
          }
          break;
        }
        case ControlFlowOpcode::kAlloc: {
          ControlFlowAllocInstruction al;
          std::memcpy(&al, bits, 8);
          std::printf(" type=%u size=%u\n", unsigned(al.alloc_type()), al.size());
          break;
        }
        default:
          std::printf(" raw=%08X_%04X\n", bits[0], bits[1]);
          if (op == ControlFlowOpcode::kReturn) ended = true;
          break;
      }
    }
  }
  return 0;
}
