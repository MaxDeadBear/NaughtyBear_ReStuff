// M2.3: Xenos microcode -> GLSL translator. See ucode_translator.h.
//
// Front-end (CF walk + vertex/texture fetch decode) is ported from the proven
// tools/nb_ucode_dump.cpp disassembler. Back-end is a register-file ALU
// emitter: it declares `vec4 r[64]`, replays each ALU instruction as GLSL, and
// routes export writes (reg 62 = clip position, 63 = point size, 0..15 =
// interpolators for VS; 0..3 = color, 61 = depth for PS). Opcode semantics and
// the component-relative source-swizzle rule are taken verbatim from
// rex/graphics/format/ucode.h (AluVectorOpcode / AluScalarOpcode).
#include "renderer/ucode_translator.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <rex/graphics/format/ucode.h>

using namespace rex::graphics::ucode;

namespace restuff::renderer::ucode {
namespace {

constexpr char kXYZW[4] = {'x', 'y', 'z', 'w'};

std::string I(uint32_t v) { return std::to_string(v); }

// ---- Control-flow walk -----------------------------------------------------
struct Slot {
  bool is_fetch = false;
  uint32_t words[3] = {};
};

// A program-order stream item: an ALU/fetch slot, or a bool-constant `if` guard
// boundary reconstructed from a kCondJmp. Xenos kCondJmp skips a forward range
// of clauses when a uniform bool constant matches a condition; we lower that to
// a structured `if (bcond(n) == exec_cond) { ... }` around the clauses it would
// otherwise skip, so conditionally-disabled feature blocks (extra lights, etc.)
// no longer corrupt the register file by always executing.
struct CfEvent {
  enum Kind {
    kSlot,
    kGuardOpen,   // bool-constant `if` (kCondJmp)
    kGuardClose,
    kLoopOpen,    // kLoopStart: for-loop over LoopConstant[loop_id]
    kLoopClose,   // kLoopEnd (optionally predicated break)
    kPredOpen,    // kCondExecPred*: clause guarded on p0 == exec_cond
    kPredClose,
  } kind = kSlot;
  Slot slot;
  uint32_t bool_addr = 0;  // kGuardOpen: bool constant index (0..255)
  bool exec_cond = true;   // kGuardOpen/kPredOpen/kLoopClose: required condition
  uint32_t loop_id = 0;    // kLoopOpen/kLoopClose
  bool pred_break = false; // kLoopClose: break if p0 == exec_cond
  // M3.75 predicated-jump guards: the guarded range may itself rewrite p0
  // (the bear VS's if/else does), so the branch decision is LATCHED into a
  // bool local at the IF open and the ELSE tests the latch, exactly like the
  // hardware jump which tested p0 once. latch >= 0 selects the latched form;
  // latch_init emits the local's declaration.
  int latch = -1;
  bool latch_init = false;
};

// Walk the CF program, emitting exec-clause slots in program order and, when
// `allow_guards`, structured bool guards for forward conditional jumps. Any CF
// construct we can't represent structurally (loops, calls, backward/predicated/
// unconditional jumps) forces a fallback to a flat, guard-free slot list --
// no worse than the previous always-flatten behaviour -- and sets a warning.
bool WalkClauses(const uint32_t* w, uint32_t n, bool allow_guards,
                 std::vector<CfEvent>& out, std::string& warn) {
  size_t min_clause_dwords = n;
  bool ended = false, unsupported = false;
  // Open guards' merge CF indices (LIFO). Predicated jump-guards carry their
  // latch id + the jump's condition so an unconditional jump can pair as the
  // ELSE of the if/else idiom.
  struct OpenGuard {
    size_t tgt;
    bool is_pred = false;
    int latch = -1;
    bool jump_cond = false;
  };
  std::vector<OpenGuard> guard_targets;
  int latch_count = 0;
  std::vector<CfEvent> ev;
  for (size_t i = 0; !ended; ++i) {
    const size_t base = (i / 2) * 3;
    if (base + 3 > n || base >= min_clause_dwords) break;
    // Close any guards whose merge target is this CF index (innermost first).
    while (!guard_targets.empty() && guard_targets.back().tgt == i) {
      ev.push_back({CfEvent::kGuardClose, {}, 0, true});
      guard_targets.pop_back();
    }
    uint32_t bits[2];
    if (i & 1) {
      bits[0] = (w[base + 1] >> 16) | (w[base + 2] << 16);
      bits[1] = w[base + 2] >> 16;
    } else {
      bits[0] = w[base];
      bits[1] = w[base + 1] & 0xFFFF;
    }
    const auto op = ControlFlowOpcode((bits[1] >> 12) & 0xF);
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
        min_clause_dwords = std::min<size_t>(min_clause_dwords, size_t(ex.address()) * 3);
        // M3.14: predicated clauses execute only when p0 matches the required
        // condition; bool-conditional clauses when the bool constant matches.
        const bool is_pred = op == ControlFlowOpcode::kCondExecPred ||
                             op == ControlFlowOpcode::kCondExecPredEnd ||
                             op == ControlFlowOpcode::kCondExecPredClean ||
                             op == ControlFlowOpcode::kCondExecPredCleanEnd;
        const bool is_bool = op == ControlFlowOpcode::kCondExec ||
                             op == ControlFlowOpcode::kCondExecEnd;
        if (is_pred) {
          ControlFlowCondExecPredInstruction px;
          std::memcpy(&px, bits, 8);
          CfEvent o;
          o.kind = CfEvent::kPredOpen;
          o.exec_cond = px.condition();
          ev.push_back(o);
        } else if (is_bool && allow_guards) {
          ControlFlowCondExecInstruction bx;
          std::memcpy(&bx, bits, 8);
          CfEvent o;
          o.kind = CfEvent::kGuardOpen;
          o.bool_addr = bx.bool_address();
          o.exec_cond = bx.condition();  // execute when bcond == required value
          ev.push_back(o);
        }
        for (uint32_t j = 0; j < ex.count(); ++j) {
          const size_t slot = (size_t(ex.address()) + j) * 3;
          if (slot + 3 > n) break;
          CfEvent s;
          s.kind = CfEvent::kSlot;
          s.slot.is_fetch = (ex.sequence() >> (j * 2)) & 1;
          std::memcpy(s.slot.words, w + slot, 12);
          ev.push_back(s);
        }
        if (is_pred) {
          ev.push_back({CfEvent::kPredClose, {}, 0, true, 0, false});
        } else if (is_bool && allow_guards) {
          ev.push_back({CfEvent::kGuardClose, {}, 0, true, 0, false});
        }
        if (op == ControlFlowOpcode::kExecEnd || op == ControlFlowOpcode::kCondExecEnd ||
            op == ControlFlowOpcode::kCondExecPredEnd ||
            op == ControlFlowOpcode::kCondExecPredCleanEnd) {
          ended = true;
        }
        break;
      }
      case ControlFlowOpcode::kCondJmp: {
        ControlFlowCondJmpInstruction jmp;
        std::memcpy(&jmp, bits, 8);
        const size_t tgt = jmp.address();
        if (!allow_guards) {
          // Guards disabled (A/B): the conditional range always executes.
          break;
        }
        if (tgt <= i) {
          // Backward jump (unstructured loop): not representable.
          unsupported = true;
          warn += "cfjmp ";
        } else if (jmp.is_predicated()) {
          // M3.75: forward predicated jump -- skip the range when p0 ==
          // condition, so it executes when p0 differs. The range can rewrite
          // p0 (the bear VS's shadow if/else does), so latch the decision.
          CfEvent g;
          g.kind = CfEvent::kPredOpen;
          g.exec_cond = !jmp.condition();
          g.latch = latch_count++;
          g.latch_init = true;
          ev.push_back(g);
          guard_targets.push_back({tgt, true, g.latch, jmp.condition()});
        } else if (jmp.is_unconditional()) {
          // M3.75: unconditional forward jump. Supported as the ELSE of the
          // if/else idiom: the innermost open guard is a predicated jump-guard
          // whose merge target is the next CF index (the else's first clause);
          // this jump hops over the else range to the common merge point.
          if (!guard_targets.empty() && guard_targets.back().is_pred &&
              guard_targets.back().tgt == i + 1) {
            const OpenGuard ifg = guard_targets.back();
            guard_targets.pop_back();
            ev.push_back({CfEvent::kGuardClose, {}, 0, true});
            CfEvent g;
            g.kind = CfEvent::kPredOpen;
            g.exec_cond = ifg.jump_cond;  // else runs when p0 == jump condition
            g.latch = ifg.latch;          // test the latched decision
            ev.push_back(g);
            guard_targets.push_back({tgt, true, ifg.latch, ifg.jump_cond});
          } else {
            unsupported = true;
            warn += "cfjmp ";
          }
        } else {
          CfEvent g;
          g.kind = CfEvent::kGuardOpen;
          g.bool_addr = jmp.bool_address();
          g.exec_cond = !jmp.condition();  // jump when bcond==condition -> execute
                                           // the following range when it differs
          ev.push_back(g);
          guard_targets.push_back({tgt, false, -1, false});
        }
        break;
      }
      case ControlFlowOpcode::kReturn:
        ended = true;
        break;
      case ControlFlowOpcode::kAlloc:
        break;  // export-allocation marker, no code
      // M3.14: real loops -- iterate the body LoopConstant[loop_id].count times
      // (bone loops in the skinned-character VSes; flattening to one iteration
      // made their triangles degenerate = characters entirely absent).
      case ControlFlowOpcode::kLoopStart: {
        ControlFlowLoopStartInstruction ls;
        std::memcpy(&ls, bits, 8);
        CfEvent o;
        o.kind = CfEvent::kLoopOpen;
        o.loop_id = ls.loop_id();
        ev.push_back(o);
        break;
      }
      case ControlFlowOpcode::kLoopEnd: {
        ControlFlowLoopEndInstruction le;
        std::memcpy(&le, bits, 8);
        CfEvent o;
        o.kind = CfEvent::kLoopClose;
        o.loop_id = le.loop_id();
        o.pred_break = le.is_predicated_break();
        o.exec_cond = le.condition();
        ev.push_back(o);
        break;
      }
      case ControlFlowOpcode::kCondCall:
        unsupported = true;
        warn += "cf" + I(uint32_t(op)) + " ";
        break;
      default:
        break;
    }
  }
  while (!guard_targets.empty()) {  // defensive: close any left open
    ev.push_back({CfEvent::kGuardClose, {}, 0, true});
    guard_targets.pop_back();
  }

  if (unsupported) {
    out.clear();
    for (const auto& e : ev)
      if (e.kind == CfEvent::kSlot) out.push_back(e);
    return !out.empty();
  }
  out = std::move(ev);
  return !out.empty();
}

// ---- ALU emission ----------------------------------------------------------
struct Emit {
  // M3.113: r0 holds the vertex index ONLY until something writes it. A fetch
  // sourced from r0.x after that reads a COMPUTED index (the volume VS's bone
  // cascade overwrites r0, then fetches bone rows via r0.x) -- classifying it
  // as a linear attribute feeds it per-vertex stream data instead.
  bool r0_written = false;
  // M3.128: vfetch_mini reuses the last vfetch_full's address setup.
  VertexFetchInstruction last_full_fetch{};
  bool last_full_fetch_valid = false;
  std::ostringstream body;
  uint32_t max_src_reg = 0;   // highest temp read -> PS interpolator input count
  uint32_t interp_out = 0;    // highest interpolator export (VS)
  uint64_t const_taint = 0;   // GPRs whose value involves ALU constants c[]
  bool uses_dbg_t1 = false;   // M3.83: PS_DEBUG=8 latches the slot-1 sample
  uint32_t alu_seen = 0;      // ALU instruction ordinal, for RESTUFF_PS_LATCH
  bool uses_latch = false;    // RESTUFF_PS_LATCH captured a mid-shader register
  std::string warnings;
};

// M4.30 (Ally/LLPC perf): size the GPR array to what the shader actually
// touches instead of a fixed 64. A compiler that doesn't promote a large
// array spills all 64 vec4s to scratch memory per invocation -- on AMD's
// LLPC driver (Z1 Extreme handheld) that made every pixel hammer off-chip
// memory: gpu_fence_wait 46-317ms/frame, presents at 7-15fps while the
// guest ran fine. Every emitted register index is a compile-time constant
// (Src/Dst print r[<int>]; the one dynamic site, PARAM_GEN, is clamped at
// its emission), so scanning the finished body gives the true maximum.
// Defensive: any unrecognized non-constant index falls back to 64.
static uint32_t CountRegs(const std::string& body, uint32_t floor_regs,
                          bool* dynamic_out = nullptr) {
  uint32_t maxr = floor_regs ? floor_regs - 1 : 0;
  bool dynamic = false;
  for (size_t p = body.find("r["); p != std::string::npos; p = body.find("r[", p + 2)) {
    if (p > 0) {
      const char c = body[p - 1];
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') continue;  // interp/attr names
    }
    size_t q = p + 2;
    uint32_t v = 0;
    bool digits = false;
    while (q < body.size() && body[q] >= '0' && body[q] <= '9') {
      v = v * 10 + uint32_t(body[q] - '0');
      ++q;
      digits = true;
    }
    if (digits && q < body.size() && body[q] == ']')
      maxr = std::max(maxr, v);
    else
      dynamic = true;
  }
  if (dynamic_out) *dynamic_out = dynamic;
  if (dynamic) return 64;
  return std::min<uint32_t>(64, maxr + 1);
}

// M4.31: with every register index a compile-time constant, the array can be
// SCALARIZED into individual vec4 locals -- the form no compiler ever spills.
// M4.30's right-sizing fixed menus on the Z1E's LLPC driver (15->120fps) but
// the big lit gameplay shaders (20-40 regs) still spilled: gpu_fence_wait
// stayed 44-52ms at ~900 draws/frame (~20fps). Emission keeps printing r[k];
// ScalarizeRegs rewrites the finished GLSL (r[12] -> r12), and declarations
// are emitted as "vec4 r[k] = vec4(0.0);" lines that the same rewrite turns
// into scalar declarations. The dynamic fallback keeps the legacy array.
static void EmitRegDecls(std::ostream& g, uint32_t nreg, bool dynamic) {
  if (dynamic) {
    g << "  vec4 r[" << nreg << "];\n  for (int i=0;i<" << nreg << ";++i) r[i]=vec4(0.0);\n";
    return;
  }
  for (uint32_t k = 0; k < nreg; ++k) g << "  vec4 r[" << k << "] = vec4(0.0);\n";
}
static std::string ScalarizeRegs(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in[i] == 'r' && i + 1 < in.size() && in[i + 1] == '[' &&
        (i == 0 || !(std::isalnum(static_cast<unsigned char>(in[i - 1])) || in[i - 1] == '_'))) {
      size_t q = i + 2;
      uint32_t v = 0;
      bool dig = false;
      while (q < in.size() && in[q] >= '0' && in[q] <= '9') {
        v = v * 10 + uint32_t(in[q] - '0');
        ++q;
        dig = true;
      }
      if (dig && q < in.size() && in[q] == ']') {
        out += 'r';
        out += std::to_string(v);
        i = q + 1;
        continue;
      }
    }
    out += in[i++];
  }
  return out;
}

// True when a generated expression's value depends on shader constants: it
// references c[] directly or reads a constant-tainted GPR. Used to classify a
// VS's position as matrix-transformed vs raw-passthrough (window-space).
bool ExprReadsConsts(const std::string& s, const Emit& e) {
  if (s.find("c[") != std::string::npos) return true;
  for (size_t p = s.find("r["); p != std::string::npos; p = s.find("r[", p + 2)) {
    uint32_t reg = 0;
    size_t q = p + 2;
    while (q < s.size() && s[q] >= '0' && s[q] <= '9') reg = reg * 10 + (s[q++] - '0');
    if (q < s.size() && s[q] == ']' && reg < 64 && (e.const_taint >> reg) & 1) return true;
  }
  return false;
}

// Component-relative ALU source swizzle: component c reads
// ((swizzle >> (2c)) + c) & 3.  (ucode.h GetSwizzledComponentIndex)
std::string Swizzle4(uint32_t swz) {
  std::string s = ".";
  for (uint32_t c = 0; c < 4; ++c) s += kXYZW[((swz >> (2 * c)) + c) & 3];
  return s;
}

// A vec4 GLSL expression for ALU source operand `i` (1..3) with its
// swizzle/abs/negate applied. Constants land in the c[] UBO; temps in r[].
std::string Src(const AluInstruction& a, uint32_t i, Emit& e) {
  const uint32_t raw = a.src_reg(i);
  std::string base;
  bool absv = false;
  if (a.src_is_temp(i)) {
    const uint32_t reg = raw & 0x3F;
    base = "r[" + I(reg) + "]";
    absv = (raw & 0x80) != 0;
    e.max_src_reg = std::max(e.max_src_reg, reg);
  } else {
    base = "c[" + I(raw & 0xFF) + "]";
    absv = a.abs_constants();
  }
  std::string ex = base + Swizzle4(a.src_swizzle(i));
  if (absv) ex = "abs(" + ex + ")";
  if (a.src_negate(i)) ex = "(-" + ex + ")";
  return ex;
}

// vec4-valued expression for the vector sub-op. `pre` receives side-effect
// statements (predicate register updates) to emit before the value is used.
std::string VectorExpr(const AluInstruction& a, Emit& e, std::string& pre) {
  const std::string s1 = Src(a, 1, e), s2 = Src(a, 2, e), s3 = Src(a, 3, e);
  std::string r;
  switch (a.vector_opcode()) {
    // M3.14: predicate set+push (HUD digit selection etc.):
    // p0 = src0.w==0 && src1.w OP 0; result = (src0.x==0 && src1.x OP 0) ? 0
    // : src0.x + 1.
    case AluVectorOpcode::kSetpEqPush:
    case AluVectorOpcode::kSetpNePush:
    case AluVectorOpcode::kSetpGtPush:
    case AluVectorOpcode::kSetpGePush: {
      const char* cmp = a.vector_opcode() == AluVectorOpcode::kSetpEqPush   ? "=="
                        : a.vector_opcode() == AluVectorOpcode::kSetpNePush ? "!="
                        : a.vector_opcode() == AluVectorOpcode::kSetpGtPush ? ">"
                                                                            : ">=";
      pre = "p0 = ((" + s1 + ").w == 0.0) && ((" + s2 + ").w " + cmp + " 0.0);";
      r = "vec4((((" + s1 + ").x == 0.0) && ((" + s2 + ").x " + cmp + " 0.0)) ? 0.0 : (" + s1 +
          ").x + 1.0)";
      if (a.vector_clamp()) r = "clamp(" + r + ", 0.0, 1.0)";
      return "(" + r + ")";
    }
    default:
      break;
  }
  switch (a.vector_opcode()) {
    case AluVectorOpcode::kAdd: r = s1 + " + " + s2; break;
    case AluVectorOpcode::kMul: r = "xmul(" + s1 + ", " + s2 + ")"; break;
    case AluVectorOpcode::kMax: r = "max(" + s1 + ", " + s2 + ")"; break;  // MOV when s1==s2
    case AluVectorOpcode::kMin: r = "min(" + s1 + ", " + s2 + ")"; break;
    case AluVectorOpcode::kFrc: r = "fract(" + s1 + ")"; break;
    case AluVectorOpcode::kTrunc: r = "trunc(" + s1 + ")"; break;
    case AluVectorOpcode::kFloor: r = "floor(" + s1 + ")"; break;
    case AluVectorOpcode::kMad: r = "(xmul(" + s1 + ", " + s2 + ") + " + s3 + ")"; break;
    case AluVectorOpcode::kDp4: r = "vec4(xdot4(" + s1 + ", " + s2 + "))"; break;
    case AluVectorOpcode::kDp3:
      r = "vec4(xdot3((" + s1 + ").xyz, (" + s2 + ").xyz))";
      break;
    case AluVectorOpcode::kDp2Add:
      r = "vec4(xmul((" + s1 + ").x, (" + s2 + ").x) + xmul((" + s1 + ").y, (" + s2 + ").y) + (" + s3 + ").x)";
      break;
    case AluVectorOpcode::kSeq: r = "mix(vec4(0.0), vec4(1.0), equal(" + s1 + ", " + s2 + "))"; break;
    case AluVectorOpcode::kSgt:
      r = "mix(vec4(0.0), vec4(1.0), greaterThan(" + s1 + ", " + s2 + "))";
      break;
    case AluVectorOpcode::kSge:
      r = "mix(vec4(0.0), vec4(1.0), greaterThanEqual(" + s1 + ", " + s2 + "))";
      break;
    case AluVectorOpcode::kSne:
      r = "mix(vec4(0.0), vec4(1.0), notEqual(" + s1 + ", " + s2 + "))";
      break;
    case AluVectorOpcode::kCndEq: r = "mix(" + s3 + ", " + s2 + ", equal(" + s1 + ", vec4(0.0)))"; break;
    case AluVectorOpcode::kCndGe:
      r = "mix(" + s3 + ", " + s2 + ", greaterThanEqual(" + s1 + ", vec4(0.0)))";
      break;
    case AluVectorOpcode::kCndGt:
      r = "mix(" + s3 + ", " + s2 + ", greaterThan(" + s1 + ", vec4(0.0)))";
      break;
    case AluVectorOpcode::kMaxA: r = "max(" + s1 + ", " + s2 + ")"; break;  // a0 side effect ignored
    default:
      r = "vec4(0.0)";
      e.warnings += "vop" + I(uint32_t(a.vector_opcode())) + " ";
      break;
  }
  if (a.vector_clamp()) r = "clamp(" + r + ", 0.0, 1.0)";
  return "(" + r + ")";
}

// float-valued expression for the scalar sub-op. Scalar operands come from the
// 3rd source: a = comp (3 + swz[6:7])&3, b = comp (0 + swz[0:1])&3. `pre`
// receives predicate-register side effects to emit before the value is used.
std::string ScalarExpr(const AluInstruction& a, Emit& e, bool& updates_ps, std::string& pre) {
  // Co-issued instructions park the idle scalar slot as retain_prev with a
  // DON'T-CARE src3 register field (often garbage, e.g. r63). Bail before the
  // operand machinery so the phantom register never inflates max_src_reg --
  // that drove one PS to declare 64 interpolant inputs (256 components, past
  // NVIDIA's 128 limit) and wedged the driver's SPIR-V compiler.
  if (a.scalar_opcode() == AluScalarOpcode::kRetainPrev) {
    updates_ps = false;
    return "ps";
  }
  // mulsc/addsc/subsc: two operands from DIFFERENT sources -- a float constant
  // c[src3_reg].a (the W-swizzle component) and a temp register r[idx].b (the
  // X-swizzle component), where idx is reconstructed from opcode[0]/src3_sel/
  // src3_swizzle[2:5]. abs_constants and the src3 negate apply to BOTH operands.
  // (ucode.h: AluScalarOpcode mulsc/addsc/subsc + scalar_const_reg_op_src_temp_reg.)
  switch (a.scalar_opcode()) {
    case AluScalarOpcode::kMulsc0:
    case AluScalarOpcode::kMulsc1:
    case AluScalarOpcode::kAddsc0:
    case AluScalarOpcode::kAddsc1:
    case AluScalarOpcode::kSubsc0:
    case AluScalarOpcode::kSubsc1: {
      updates_ps = true;
      const uint32_t sw = a.src_swizzle(3);
      const uint32_t cidx = a.src_reg(3) & 0xFF;
      const uint32_t ridx = a.scalar_const_reg_op_src_temp_reg() & 0x3F;
      e.max_src_reg = std::max(e.max_src_reg, ridx);
      auto both = [&](std::string s) {
        if (a.abs_constants()) s = "abs(" + s + ")";
        if (a.src_negate(3)) s = "(-" + s + ")";
        return s;
      };
      const std::string A = both(std::string("c[") + I(cidx) + "]." + kXYZW[(3 + ((sw >> 6) & 3)) & 3]);
      const std::string B = both(std::string("r[") + I(ridx) + "]." + kXYZW[sw & 3]);
      switch (a.scalar_opcode()) {
        case AluScalarOpcode::kMulsc0:
        case AluScalarOpcode::kMulsc1: return "xmul(" + A + ", " + B + ")";
        case AluScalarOpcode::kAddsc0:
        case AluScalarOpcode::kAddsc1: return A + " + " + B;
        default: return A + " - " + B;  // subsc: constant - register
      }
    }
    default:
      break;
  }
  const uint32_t raw = a.src_reg(3);
  std::string base;
  bool absv = false;
  if (a.src_is_temp(3)) {
    const uint32_t reg = raw & 0x3F;
    base = "r[" + I(reg) + "]";
    absv = (raw & 0x80) != 0;
    e.max_src_reg = std::max(e.max_src_reg, reg);
  } else {
    base = "c[" + I(raw & 0xFF) + "]";
    absv = a.abs_constants();
  }
  auto comp = [&](uint32_t c) {
    std::string x = base + "." + kXYZW[c];
    if (absv) x = "abs(" + x + ")";
    if (a.src_negate(3)) x = "(-" + x + ")";
    return x;
  };
  const uint32_t sw = a.src_swizzle(3);
  const std::string A = comp((3 + ((sw >> 6) & 3)) & 3);
  const std::string B = comp((0 + (sw & 3)) & 3);
  updates_ps = true;
  switch (a.scalar_opcode()) {
    case AluScalarOpcode::kAdds: return A + " + " + B;
    case AluScalarOpcode::kMuls: return "xmul(" + A + ", " + B + ")";
    case AluScalarOpcode::kMaxs: return "max(" + A + ", " + B + ")";
    case AluScalarOpcode::kMins: return "min(" + A + ", " + B + ")";
    case AluScalarOpcode::kSubs: return A + " - " + B;
    case AluScalarOpcode::kAddsPrev: return A + " + ps";
    case AluScalarOpcode::kMulsPrev: return "xmul(" + A + ", ps)";
    case AluScalarOpcode::kSubsPrev: return A + " - ps";
    case AluScalarOpcode::kFrcs: return "fract(" + A + ")";
    case AluScalarOpcode::kTruncs: return "trunc(" + A + ")";
    case AluScalarOpcode::kFloors: return "floor(" + A + ")";
    case AluScalarOpcode::kExp: return "exp2(" + A + ")";
    // The c/f suffixed variants CLAMP infinities; the plain ones do not. We
    // previously collapsed all three into one unclamped expression, so an inf
    // (e.g. inversesqrt(0) on a zero-length normal) escaped and poisoned the
    // whole lighting accumulator to NaN -- while the SDK stayed finite.
    case AluScalarOpcode::kLog: return "log2(" + A + ")";
    case AluScalarOpcode::kLogc: return "_cinf(log2(" + A + "))";
    case AluScalarOpcode::kRcp: return "(1.0 / " + A + ")";
    case AluScalarOpcode::kRcpf: return "_zinf(1.0 / " + A + ")";
    case AluScalarOpcode::kRcpc: return "_cinf(1.0 / " + A + ")";
    case AluScalarOpcode::kRsq: return "inversesqrt(" + A + ")";
    case AluScalarOpcode::kRsqf: return "_zinf(inversesqrt(" + A + "))";
    case AluScalarOpcode::kRsqc: return "_cinf(inversesqrt(" + A + "))";
    case AluScalarOpcode::kSqrt: return "sqrt(" + A + ")";
    case AluScalarOpcode::kSin: return "sin(" + A + ")";
    case AluScalarOpcode::kCos: return "cos(" + A + ")";
    case AluScalarOpcode::kSeqs: return "(" + A + " == 0.0 ? 1.0 : 0.0)";
    case AluScalarOpcode::kSgts: return "(" + A + " > 0.0 ? 1.0 : 0.0)";
    case AluScalarOpcode::kSges: return "(" + A + " >= 0.0 ? 1.0 : 0.0)";
    case AluScalarOpcode::kSnes: return "(" + A + " != 0.0 ? 1.0 : 0.0)";
    // M3.14: predicate sets (drive kCondExecPred clauses and loop breaks).
    case AluScalarOpcode::kSetpEq:
      pre = "p0 = (" + A + " == 0.0);";
      return "(p0 ? 0.0 : 1.0)";
    case AluScalarOpcode::kSetpNe:
      pre = "p0 = (" + A + " != 0.0);";
      return "(p0 ? 0.0 : 1.0)";
    case AluScalarOpcode::kSetpGt:
      pre = "p0 = (" + A + " > 0.0);";
      return "(p0 ? 0.0 : 1.0)";
    case AluScalarOpcode::kSetpGe:
      pre = "p0 = (" + A + " >= 0.0);";
      return "(p0 ? 0.0 : 1.0)";
    case AluScalarOpcode::kSetpInv:
      pre = "p0 = (" + A + " == 1.0);";
      return "(p0 ? 0.0 : ((" + A + " == 0.0) ? 1.0 : " + A + "))";
    case AluScalarOpcode::kSetpPop:
      pre = "p0 = ((" + A + " - 1.0) <= 0.0);";
      return "max(" + A + " - 1.0, 0.0)";
    case AluScalarOpcode::kSetpClr:
      pre = "p0 = false;";
      return "3.402823466e38";
    case AluScalarOpcode::kSetpRstr:
      pre = "p0 = (" + A + " == 0.0);";
      return A;
    case AluScalarOpcode::kRetainPrev:
      updates_ps = false;
      return "ps";  // dest = ps, ps unchanged
    default:
      updates_ps = false;
      e.warnings += "sop" + I(uint32_t(a.scalar_opcode())) + " ";
      return "ps";
  }
}

// Emit one ALU instruction. `target` receives export writes (VS: writes into
// oPos/oPsz/o_k accumulators; PS: col_k / gl_FragDepth). Non-export writes go
// to the r[] register file.
void EmitAlu(const AluInstruction& a, Emit& e, bool is_vertex, TranslatedShader& out) {
  const uint32_t V = a.vector_write_mask();
  const uint32_t S = a.scalar_write_mask();

  // REGISTER LATCH (RESTUFF_PS_LATCH="<reg>,<alu_index>"): copy r[reg] into a
  // global just BEFORE ALU instruction #alu_index, so PS_DEBUG=24 can display a
  // mid-shader value. Epilogue-only probes cannot do this -- the export writes
  // r0 (dst=r0 mask=F), and light accumulators get overwritten by later blocks,
  // which is why several earlier probes read clobbered registers and lied.
  // Counts ALU instructions in emission order, matching the disassembly.
  {
    static const std::pair<int, int> latch = [] {
      const char* s = getenv("RESTUFF_PS_LATCH");
      if (!s) return std::make_pair(-1, -1);
      int rg = -1, ix = -1;
      if (sscanf(s, "%d,%d", &rg, &ix) != 2) return std::make_pair(-1, -1);
      return std::make_pair(rg, ix);
    }();
    if (!is_vertex && latch.first >= 0 && int(e.alu_seen) == latch.second) {
      e.body << "  _latch = r[" << latch.first << "];  // RESTUFF_PS_LATCH\n";
      e.uses_latch = true;
    }
    ++e.alu_seen;
  }

  std::string vpre, spre;
  const std::string vexpr = VectorExpr(a, e, vpre);
  bool updates_ps = false;
  const std::string sexpr = ScalarExpr(a, e, updates_ps, spre);

  // M3.14: per-instruction predication -- the instruction only executes when
  // p0 matches its required condition.
  const bool predicated = a.is_predicated();
  e.body << "  " << (predicated ? (a.predicate_condition() ? "if (p0) {\n" : "if (!p0) {\n")
                                : "{\n");
  if (!vpre.empty()) e.body << "    " << vpre << "\n";
  e.body << "    vec4 vr = " << vexpr << ";\n";
  if (!spre.empty()) e.body << "    " << spre << "\n";
  if (updates_ps) e.body << "    ps = " << sexpr << ";\n";
  std::string sval = updates_ps ? "ps" : sexpr;  // scalar result value
  // M3.95: the scalar saturate bit. Hardware keeps the previous-scalar (ps)
  // chain UNCLAMPED and saturates only the value written to the destination /
  // export combine (interpreter.cpp: scalar_result = scalar_clamp ?
  // saturate(previous_scalar) : previous_scalar). This was dropped entirely --
  // rcp(0)=inf reached the fog multiply of every world PS unclamped.
  if (a.scalar_clamp()) sval = "clamp(" + sval + ", 0.0, 1.0)";

  if (!a.is_export()) {
    const uint32_t vd = a.vector_dest();
    const uint32_t sd = a.scalar_dest();
    for (uint32_t c = 0; c < 4; ++c)
      if ((V >> c) & 1) e.body << "    r[" << vd << "]." << kXYZW[c] << " = vr." << kXYZW[c] << ";\n";
    for (uint32_t c = 0; c < 4; ++c)
      if ((S >> c) & 1) e.body << "    r[" << sd << "]." << kXYZW[c] << " = " << sval << ";\n";
    if (V && vd == 0) e.r0_written = true;
    if (S && sd == 0) e.r0_written = true;
    if (V && vd < 64 && ExprReadsConsts(vexpr, e)) e.const_taint |= 1ull << vd;
    if (S && sd < 64 && ExprReadsConsts(sexpr, e)) e.const_taint |= 1ull << sd;
    e.body << "  }\n";
    return;
  }

  // Export: combine per component. V&!S -> vector, S&!V -> scalar, V&S -> 1.0,
  // neither (+const0 flag) -> 0.0.  (ucode.h GetConstant0/1WriteMask rules.)
  const bool const0 = a.is_scalar_dest_relative();
  const uint32_t idx = a.vector_dest();
  std::string tgt;
  if (is_vertex) {
    if (idx == 62) tgt = "oPos";
    else if (idx == 63) tgt = "oPsz";
    else if (idx <= 15) { tgt = "o_" + I(idx); e.interp_out = std::max(e.interp_out, idx); out.interpolator_mask |= (1u << idx); out.exports_position = out.exports_position; }
    else { e.body << "  }\n"; return; }
    if (idx == 62) {
      out.exports_position = true;
      if (ExprReadsConsts(vexpr, e) || ExprReadsConsts(sexpr, e))
        out.pos_reads_consts = true;
    }
    if (idx == 63) out.exports_point_size = true;
  } else {
    if (idx <= 3) { tgt = "col_" + I(idx); out.color_export_mask |= (1u << idx); }
    else if (idx == 61) { tgt = "gl_FragDepth"; out.exports_depth = true; }  // depth export
    else { e.body << "  }\n"; return; }
  }

  for (uint32_t c = 0; c < 4; ++c) {
    const bool v = (V >> c) & 1, s = (S >> c) & 1;
    std::string rhs;
    if (v && s) rhs = "1.0";
    else if (v) rhs = std::string("vr.") + kXYZW[c];
    else if (s) rhs = sval;
    else if (const0) rhs = "0.0";
    else continue;
    if (tgt == "gl_FragDepth") { e.body << "    gl_FragDepth = " << rhs << ";\n"; break; }
    e.body << "    " << tgt << "." << kXYZW[c] << " = " << rhs << ";\n";
  }
  e.body << "  }\n";
}

// Map a fetch destination swizzle component (3 bits, absolute) to a source
// expression drawn from the fetched vec4 `in`.  0..3=xyzw, 4=0, 5=1, 7=keep.
std::string FetchDstComp(uint32_t dst_swz, uint32_t c, const std::string& in, uint32_t dst_reg) {
  const uint32_t s = (dst_swz >> (3 * c)) & 7;
  switch (s) {
    case 0: return in + ".x";
    case 1: return in + ".y";
    case 2: return in + ".z";
    case 3: return in + ".w";
    case 4: return "0.0";
    case 5: return "1.0";
    default: return "r[" + I(dst_reg) + "]." + kXYZW[c];  // keep
  }
}

// M3.14: emit structured control-flow events (bool guards, predicate clauses,
// loops). Returns false for kSlot so the caller handles fetch/ALU slots.
bool EmitCfEvent(const CfEvent& it, Emit& e) {
  switch (it.kind) {
    case CfEvent::kGuardOpen:
      e.body << "  if (bcond(" << it.bool_addr << ") == " << (it.exec_cond ? "true" : "false")
             << ") {\n";
      return true;
    case CfEvent::kGuardClose:
    case CfEvent::kPredClose:
      e.body << "  }\n";
      return true;
    case CfEvent::kPredOpen:
      if (it.latch >= 0) {
        // M3.75 latched jump-guard: the branch decision is p0 AT THE JUMP;
        // the body may rewrite p0, and the else leg tests the same latch.
        if (it.latch_init) e.body << "  bool _br" << it.latch << " = p0;\n";
        e.body << "  if (_br" << it.latch << " == " << (it.exec_cond ? "true" : "false")
               << ") {\n";
      } else {
        e.body << "  if (p0 == " << (it.exec_cond ? "true" : "false") << ") {\n";
      }
      return true;
    case CfEvent::kLoopOpen:
      // LoopConstant[loop_id]: count[0:8], aL start[8:16], aL step[16:24] (s8).
      e.body << "  { uint _lc = lc[" << (it.loop_id >> 2) << "][" << (it.loop_id & 3) << "];\n"
             << "    int _al0 = int((_lc >> 8) & 255u); int _stp = (int(_lc) << 8) >> 24;\n"
             << "    for (int _it = 0; _it < int(_lc & 255u); ++_it) { aL = _al0 + _it * _stp;\n";
      return true;
    case CfEvent::kLoopClose:
      if (it.pred_break)
        e.body << "    if (p0 == " << (it.exec_cond ? "true" : "false") << ") break;\n";
      e.body << "  } }\n";
      return true;
    default:
      return false;
  }
}

}  // namespace

TranslatedShader TranslateVertexShader(const uint32_t* be_words, uint32_t num_dwords) {
  TranslatedShader r;
  std::vector<uint32_t> w(num_dwords);
  for (uint32_t i = 0; i < num_dwords; ++i) w[i] = std::byteswap(be_words[i]);

  Emit e;
  // M3.14: VS bool guards read bc[] appended to the VS constant UBO block.
  // RESTUFF_NO_VS_GUARDS=1: flatten VS conditionals instead (A/B lever for
  // suspected guard-driven regressions, e.g. overbright vertex lighting).
  static const bool no_vs_guards = getenv("RESTUFF_NO_VS_GUARDS") != nullptr;
  std::vector<CfEvent> ev;
  if (!WalkClauses(w.data(), num_dwords, /*allow_guards=*/!no_vs_guards, ev, e.warnings)) {
    r.error = "empty/malformed CF";
    return r;
  }

  uint32_t loc = 0;
  // Fetches emit INLINE in program order (relative fetches read index registers
  // computed/loaded earlier). Linear fetches (src r0.x = the vertex index) stay
  // Vulkan vertex attributes; REGISTER-relative fetches (index from a GPR --
  // bone-data lookups for skinning) become storage-buffer reads: the attribute
  // path would feed them linear per-vertex garbage.
  for (const auto& it : ev) {
    if (EmitCfEvent(it, e)) continue;
    if (it.kind != CfEvent::kSlot) continue;
    const Slot& s = it.slot;
    if (s.is_fetch) {
      if ((s.words[0] & 0x1F) != uint32_t(FetchOpcode::kVertexFetch)) continue;
      ++r.vfetch_count;  // M3.138: counted before any attr-resolution decision
      VertexFetchInstruction vf;
      std::memcpy(&vf, s.words, 12);
      // M3.128: vfetch_mini REUSES the preceding vfetch_full's address setup.
      // Per the ucode definition (format/ucode.h:698-730): fetch_constant_index,
      // src register/swizzle, stride and index-rounding are "applicable only to
      // vfetch_full ... reused in vfetch_mini"; a mini's own bits for those are
      // meaningless. We were reading them from every fetch, so a mini yielded
      // stride 0 -- and the walker drops any draw with a zero-stride attribute
      // (native_backend_vk.cpp, CAPCENSUS "stride0"). That was discarding
      // 65-85% of ALL captured draws (census: in=170000 out=58016
      // stride0=109704), which is why whole effect classes never appeared.
      // Inherit the full fetch's fields for minis; format/offset/dest/swizzle/
      // signedness stay per-instruction.
      const bool vf_mini = vf.is_mini_fetch();
      // M3.128c RESULT (corrects the M3.128c commit message, which claimed the
      // fix was load-bearing on this title): the A/B is NEGATIVE. With
      // RESTUFF_NO_MINI_INHERIT=1 the gameplay census still reports stride0=0
      // (in=5.4M), identical to the fixed decode -- so the one mini observed
      // carrying stride=0 (full said 7) belongs to a shader whose draws never
      // reach the zero-stride gate here. M3.128 is a SPEC-CORRECTNESS fix with
      // no measured effect on this title's draw counts or visuals; keep it
      // (the hazard is real and cheap to avoid) but do not credit it with
      // fixing anything the user can see.
      // (RESTUFF_MINI_LOG=1): does this title use vfetch_mini at all,
      // and does inheriting change the decoded stride/slot? Answers whether the
      // M3.128 fix is load-bearing here or merely spec-correct.
      if (getenv("RESTUFF_MINI_LOG") && vf_mini) {
        static std::atomic<int> s_ml{20};
        if (s_ml.fetch_sub(1, std::memory_order_relaxed) > 0)
          fprintf(stderr, "[MINIFETCH] mini seen: own(slot=%u stride=%u src=%u) "
                          "full(slot=%u stride=%u src=%u) valid=%d\n",
                  vf.fetch_constant_index(), vf.stride(), vf.src(),
                  e.last_full_fetch_valid ? e.last_full_fetch.fetch_constant_index() : 999u,
                  e.last_full_fetch_valid ? e.last_full_fetch.stride() : 999u,
                  e.last_full_fetch_valid ? e.last_full_fetch.src() : 999u,
                  int(e.last_full_fetch_valid));
      }
      if (!vf_mini) {
        e.last_full_fetch = vf;
        e.last_full_fetch_valid = true;
      }
      // Address-setup fields come from the owning full fetch for minis.
      // RESTUFF_NO_MINI_INHERIT=1: A/B lever -- decode minis the old (wrong)
      // way so the census can measure how many draws the inheritance actually
      // saves on this title.
      static const bool s_no_mini_inherit = getenv("RESTUFF_NO_MINI_INHERIT") != nullptr;
      const VertexFetchInstruction& vfa =
          (vf_mini && e.last_full_fetch_valid && !s_no_mini_inherit) ? e.last_full_fetch : vf;
      struct R0Mark {  // runs after this fetch's classification+emit
        Emit& e; uint32_t dst;
        ~R0Mark() { if (dst == 0) e.r0_written = true; }
      } r0mark{e, vf.dest()};
      const bool relative = !(vfa.src() == 0 && vfa.src_swizzle() == 0 && !e.r0_written);
      if (relative) {
        // M3.14: up to two distinct relative-fetch slots (bone data + mesh
        // neighbor/morph reads), each a storage buffer.
        const uint32_t slot = vfa.fetch_constant_index();
        const char* buf = nullptr;
        if (r.rel_fetch_slot == ~0u || r.rel_fetch_slot == slot) {
          r.rel_fetch_slot = slot;
          buf = "vfd";
        } else if (r.rel_fetch_slot2 == ~0u || r.rel_fetch_slot2 == slot) {
          r.rel_fetch_slot2 = slot;
          buf = "vfd2";
        } else {
          e.warnings += "relslot" + I(slot) + " ";
          continue;
        }
        const uint32_t fmt = uint32_t(vf.data_format());
        // Dword footprint per format (payload LE-normalized at capture).
        // M3.112: xenos::VertexFormat truth (SDK xenos.h): 57 = 32_32_32_FLOAT
        // (THREE f32 -- the old "two f32" mapping zeroed every neighbor
        // normal's Z in the shadow-volume adjacency fetches), 34 = 32_32
        // (two s32/u32 integers -- NOT 16_16_16_16, which is 26). The volume
        // VS's per-vertex extrusion decision consumes both (offset 6 normals,
        // offset 4 adjacency links), so both mistakes flipped face
        // classifications and everted shell patches (the phantom shadow).
        const uint32_t ndw =
            fmt == 38 ? 4
            : (fmt == 37 || fmt == 57) ? 3
            : (fmt == 36 || fmt == 34 || fmt == 26 || fmt == 32) ? 2
                                                                 : 1;  // 31/25/6/33: one dword
        const uint32_t dst = vf.dest();
        e.max_src_reg = std::max(e.max_src_reg, vfa.src());
        e.body << "  {\n    int _w = int(r[" << vfa.src() << "]." << kXYZW[vfa.src_swizzle() & 3]
               << ") * " << vfa.stride() << " + " << uint32_t(vf.offset()) << ";\n"
               << "    bool _oob = _w < 0 || _w + " << ndw << " > int(" << buf
               << ".w.length());\n"
               << "    _w = _oob ? 0 : _w;\n";
        switch (fmt) {
          case 36:
          case 37:
          case 38:
          case 57:  // 1/2/3/4 x f32 (57 = 32_32_32_FLOAT)
            e.body << "    vec4 _v = vec4(";
            for (uint32_t c = 0; c < 4; ++c) {
              if (c) e.body << ", ";
              if (c < ndw) e.body << "uintBitsToFloat(" << buf << ".w[_w + " << c << "])";
              else e.body << (c == 3 ? "1.0" : "0.0");
            }
            e.body << ");\n";
            break;
          case 33:    // 32 (one integer)
          case 34: {  // 32_32 (two integers)
            e.body << "    uint _a = " << buf << ".w[_w], _b2 = "
                   << (ndw > 1 ? std::string(buf) + ".w[_w + 1]" : std::string("0u")) << ";\n";
            if (vf.is_signed())
              e.body << "    vec4 _v = vec4(float(int(_a)), float(int(_b2)), 0.0, 1.0);\n";
            else
              e.body << "    vec4 _v = vec4(float(_a), float(_b2), 0.0, 1.0);\n";
            if (vf.is_normalized())
              e.body << "    _v.xy /= " << (vf.is_signed() ? "2147483647.0" : "4294967295.0")
                     << ";\n";
            break;
          }
          case 6:  // 8_8_8_8
            if (vf.is_normalized())
              e.body << "    vec4 _v = " << (vf.is_signed() ? "unpackSnorm4x8" : "unpackUnorm4x8")
                     << "(" << buf << ".w[_w]);\n";
            else if (vf.is_signed())
              e.body << "    uint _b = " << buf
                     << ".w[_w];\n    vec4 _v = vec4(ivec4(int(_b) << 24 >> 24, int(_b) << 16 >> "
                        "24, int(_b) << 8 >> 24, int(_b) >> 24));\n";
            else
              e.body << "    uint _b = " << buf
                     << ".w[_w];\n    vec4 _v = vec4(float(_b & 255u), float((_b >> 8) & 255u), "
                        "float((_b >> 16) & 255u), float(_b >> 24));\n";
            break;
          case 26: {  // 16_16_16_16 integer
            e.body << "    uint _a = " << buf << ".w[_w], _b2 = " << buf << ".w[_w + 1];\n";
            if (vf.is_signed())
              e.body << "    vec4 _v = vec4(ivec4(int(_a) << 16 >> 16, int(_a) >> 16, int(_b2) "
                        "<< 16 >> 16, int(_b2) >> 16));\n";
            else
              e.body << "    vec4 _v = vec4(float(_a & 0xFFFFu), float(_a >> 16), float(_b2 & "
                        "0xFFFFu), float(_b2 >> 16));\n";
            if (vf.is_normalized())
              e.body << "    _v /= " << (vf.is_signed() ? "32767.0" : "65535.0") << ";\n";
            break;
          }
          case 31:  // 16_16_FLOAT
            e.body << "    vec2 _h = unpackHalf2x16(" << buf << ".w[_w]);\n"
                   << "    vec4 _v = vec4(_h, 0.0, 1.0);\n";
            break;
          case 32:  // 16_16_16_16_FLOAT
            e.body << "    vec2 _h0 = unpackHalf2x16(" << buf << ".w[_w]);\n"
                   << "    vec2 _h1 = unpackHalf2x16(" << buf << ".w[_w + 1]);\n"
                   << "    vec4 _v = vec4(_h0, _h1);\n";
            break;
          case 25: {  // 16_16 integer
            e.body << "    uint _a = " << buf << ".w[_w];\n";
            if (vf.is_signed())
              e.body << "    vec4 _v = vec4(float(int(_a) << 16 >> 16), float(int(_a) >> 16), "
                        "0.0, 1.0);\n";
            else
              e.body << "    vec4 _v = vec4(float(_a & 0xFFFFu), float(_a >> 16), 0.0, 1.0);\n";
            if (vf.is_normalized())
              e.body << "    _v.xy /= " << (vf.is_signed() ? "32767.0" : "65535.0") << ";\n";
            break;
          }
          default:
            e.warnings += "relfmt" + I(fmt) + " ";
            e.body << "    vec4 _v = vec4(0.0);\n";
            break;
        }
        // Xenos bounds-checks computed fetches against the fetch constant's
        // SIZE and returns ZERO out of range (D3D robustness matches). We
        // previously CLAMPED to the last in-range record -- nonzero garbage
        // that varies with the animation. The volume shaders' neighbor
        // fetches (indices ~1e9 from float-bits data) rely on the zero.
        e.body << "    if (_oob) _v = vec4(0.0);\n";
        for (uint32_t c = 0; c < 4; ++c)
          e.body << "    r[" << dst << "]." << kXYZW[c] << " = "
                 << FetchDstComp(vf.dest_swizzle(), c, "_v", dst) << ";\n";
        e.body << "  }\n";
        continue;
      }
      VertexAttr a;
      a.fetch_slot = vfa.fetch_constant_index();
      a.dest_reg = vf.dest();
      a.byte_offset = uint32_t(vf.offset()) * 4;
      a.stride_bytes = vfa.stride() * 4;
      a.format = uint32_t(vf.data_format());
      a.dst_swizzle = vf.dest_swizzle();
      a.is_signed = vf.is_signed();
      a.is_normalized = vf.is_normalized();
      a.location = loc++;
      r.attrs.push_back(a);
      const std::string in = "in_" + I(a.location);
      e.body << "  r[" << a.dest_reg << "] = vec4(";
      for (uint32_t c = 0; c < 4; ++c)
        e.body << (c ? ", " : "") << FetchDstComp(a.dst_swizzle, c, in, a.dest_reg);
      e.body << ");\n";
    } else {
      AluInstruction a;
      std::memcpy(&a, s.words, 12);
      EmitAlu(a, e, /*is_vertex=*/true, r);
    }
  }

  std::ostringstream g;
  g << "#version 450\n";
  // M3.22: this title draws its world TWICE -- a z-prepass (colour mask 0) and
  // then an opaque colour pass of the same geometry, both ztest+zwrite with
  // zfunc GEQUAL under inverted-Z. The colour pass therefore only survives on
  // the equality edge against the depth the prepass just wrote. Without an
  // invariance guarantee the compiler is free to schedule the position math
  // differently per pipeline (the two passes are separate pipelines), so the
  // colour z can land an ULP below the prepass z, fail GEQUAL, and leave depth
  // with no colour = a black patch that moves with the camera.
  g << "invariant gl_Position;\n";
  // M3.14: lc = 32 loop constants (0x4908..), bc = 256 bool bits (0x4900..),
  // appended to the per-draw constant block.
  g << "layout(std140, set=0, binding=0) uniform VsConsts { vec4 c[256]; uvec4 lc[8]; "
       "uvec4 bc[2]; };\n";
  g << "float _cinf(float x){ return isinf(x) ? (x > 0.0 ? 3.402823466e38 : -3.402823466e38) : x; }\n";
  g << "float _zinf(float x){ return isinf(x) ? (x > 0.0 ? 0.0 : -0.0) : x; }\n";
  g << "vec4 xmul(vec4 a, vec4 b){ return mix(a*b, vec4(0.0), equal(min(abs(a),abs(b)), vec4(0.0))); }\n";
  g << "float xmul(float a, float b){ return min(abs(a),abs(b)) == 0.0 ? 0.0 : a*b; }\n";
  g << "float xdot4(vec4 a, vec4 b){ vec4 p = xmul(a,b); return ((p.x + p.y) + p.z) + p.w; }\n";
  g << "float xdot3(vec3 a, vec3 b){ vec3 p = mix(a*b, vec3(0.0), equal(min(abs(a),abs(b)), vec3(0.0))); return (p.x + p.y) + p.z; }\n";
  g << "bool bcond(int i){ return ((bc[i>>7][(i>>5)&3] >> uint(i&31)) & 1u) != 0u; }\n";
  // M3.11: raw dwords of the register-relative fetch stream (skinning bone
  // data), bound with a dynamic offset to the draw's captured stream payload.
  if (r.rel_fetch_slot != ~0u)
    g << "layout(std430, set=0, binding=2) readonly buffer VfData { uint w[]; } vfd;\n";
  if (r.rel_fetch_slot2 != ~0u)
    g << "layout(std430, set=0, binding=3) readonly buffer VfData2 { uint w[]; } vfd2;\n";
  // M3.19: rot.x > 0.5 counter-rotates clip space 90 degrees -- the game
  // renders gameplay ROTATED into EDRAM (viewport wider than surface pitch);
  // hardware un-rotates during the tiled resolve walk, we un-rotate here.
  // rot.y selects direction.
  g << "layout(push_constant) uniform PC { vec4 ndc; layout(offset=96) vec4 rot; } pc;"
       "  // ndc: xy=scale zw=offset\n";
  for (const auto& a : r.attrs) g << "layout(location=" << a.location << ") in vec4 in_" << a.location << ";\n";
  // Declare ALL 16 interpolator outputs, not just the ones this VS writes. A
  // paired PS may read interpolators this VS doesn't compute (e.g. 8859F7 has
  // no colour attribute but its PS samples a colour interpolator); on hardware
  // an unwritten export reads as 0 (transparent), so we must zero-init them all
  // -- otherwise they're undefined and the quad renders opaque garbage.
  constexpr uint32_t kInterp = 16;
  for (uint32_t i = 0; i < kInterp; ++i)
    g << "layout(location=" << i << ") out vec4 o_" << i << ";\n";
  g << "void main() {\n";
  bool rdyn = false;
  const uint32_t nreg = CountRegs(e.body.str(), 1, &rdyn);  // M4.30: r[0].x below
  EmitRegDecls(g, nreg, rdyn);
  // M3.109: Xenos injects the vertex index into r0.x at VS entry (the SDK
  // SpirvShaderTranslator does the same -- see its vertex_index_* system
  // constants). The sun-shadow volume VS gates its whole bone-component
  // select cascade on r0.x (abs(r0.x) < 2 for the first iteration); with the
  // old zero-init every vertex took the index<2 branch, mis-picked its bone
  // palette component, and patches of the extruded shell everted (the
  // net -1 phantom shadow). gl_VertexIndex carries the guest IB value 1:1
  // (vertexOffset 0, auto-index draws match GEN_INDEX).
  g << "  r[0].x = float(gl_VertexIndex);\n";
  g << "  float ps = 0.0;\n  bool p0 = false;\n  int aL = 0;\n";
  g << "  vec4 oPos = vec4(0.0,0.0,0.0,1.0);\n  vec4 oPsz = vec4(1.0);\n";
  for (uint32_t i = 0; i < kInterp; ++i) g << "  o_" << i << " = vec4(0.0);\n";
  if (e.uses_dbg_t1) g << "  vec4 _dbg_t1 = vec4(0.0);\n";
  if (e.uses_latch) g << "  vec4 _latch = vec4(0.0);\n";
  g << e.body.str();
  // RESTUFF_VS_DEBUG=1 (+RESTUFF_VS_DEBUG_HASH=<hex>): overwrite o_0 with the
  // POSITION-derived UV field. Pairs with PS_DEBUG=3: an identity gradient on
  // screen means positions rasterize upright (so a transposed o_0 came from
  // attribute delivery); a transposed gradient means gl_Position itself gets
  // transposed after the VS.
  {
    static const int vs_dbg = [] {
      const char* e2 = getenv("RESTUFF_VS_DEBUG");
      return e2 ? atoi(e2) : 0;
    }();
    static const uint64_t vs_dbg_hash = [] {
      const char* e2 = getenv("RESTUFF_VS_DEBUG_HASH");
      return e2 ? strtoull(e2, nullptr, 16) : 0ull;
    }();
    bool apply = vs_dbg == 1;
    if (apply && vs_dbg_hash) {
      uint64_t h = 1469598103934665603ull;
      for (uint32_t i = 0; i < num_dwords; ++i) h = (h ^ be_words[i]) * 1099511628211ull;
      apply = (h == vs_dbg_hash);
    }
    if (apply) g << "  o_0 = vec4(oPos.x * 0.5 + 0.5, oPos.y * 0.5 + 0.5, 0.0, 1.0);\n";
  }
  // Assign outputs. Guest clip -> Vulkan clip via push-constant ndc transform.
  g << "  vec2 _gp = oPos.xy * pc.ndc.xy + pc.ndc.zw * oPos.w;\n";
  // M3.30: emit the rot branch ONLY when a rotation lever is actually on --
  // an always-present branch on push-constant data was one more way for bad
  // state to transpose the frame, and the levers are diagnostic-only.
  {
    static const bool rot_lever = getenv("RESTUFF_ROT") || getenv("RESTUFF_ROT_ALL") ||
                                  getenv("RESTUFF_PREROT");
    if (rot_lever)
      g << "  if (pc.rot.x > 0.5) _gp = (pc.rot.y > 0.5) ? vec2(-_gp.y, _gp.x) : vec2(_gp.y, -_gp.x);\n";
  }
  // M3.119 REVERTED: this title sets PA_CL_CLIP_CNTL.DX_CLIP_SPACE_DEF
  // (clip z already 0..w), so the SDK's GL->D3D z remap branch does NOT
  // apply; applying it anyway degenerated depth to a constant 0.5. The
  // "global depth-scale divergence" that motivated it was a measurement
  // artifact (floor-heavy ROI median compared against a full-frame median).
  g << "  gl_Position = vec4(_gp, oPos.z, oPos.w);\n";
  g << "  gl_PointSize = oPsz.x;\n";
  g << "}\n";

  r.glsl = rdyn ? g.str() : ScalarizeRegs(g.str());

  // M3.60: emit the compute winding-probe variant for non-skinned matrix VSes.
  // It runs the SAME fetch+transform (e.body) on 3 vertices whose attributes the
  // renderer pre-decodes into an SSBO, and writes their clip positions so the
  // renderer can compute the true screen-space winding and flip inverted draws.
  // Skinned VSes read rel-fetch storage the probe doesn't bind, and M3.41
  // already handles their winding, so skip them.
  uint32_t kmax = 1;
  for (const auto& a : r.attrs) kmax = std::max(kmax, a.location + 1);
  // Vertex stride in the probe's attrs SSBO is FIXED at 8 vec4s -- must match
  // the renderer's kWpMaxLoc (which also skips shaders with more attributes).
  if (r.exports_position && r.rel_fetch_slot == ~0u && r.rel_fetch_slot2 == ~0u && kmax <= 8) {
    std::ostringstream p;
    p << "#version 450\n";
    p << "layout(local_size_x=3) in;\n";
    p << "layout(std140, set=0, binding=0) uniform VsConsts { vec4 c[256]; uvec4 lc[8]; "
         "uvec4 bc[2]; };\n";
    p << "float _cinf(float x){ return isinf(x) ? (x > 0.0 ? 3.402823466e38 : -3.402823466e38) : x; }\n";
    p << "float _zinf(float x){ return isinf(x) ? (x > 0.0 ? 0.0 : -0.0) : x; }\n";
  p << "vec4 xmul(vec4 a, vec4 b){ return mix(a*b, vec4(0.0), equal(min(abs(a),abs(b)), vec4(0.0))); }\n";
  p << "float xmul(float a, float b){ return min(abs(a),abs(b)) == 0.0 ? 0.0 : a*b; }\n";
  p << "float xdot4(vec4 a, vec4 b){ vec4 p = xmul(a,b); return ((p.x + p.y) + p.z) + p.w; }\n";
  p << "float xdot3(vec3 a, vec3 b){ vec3 p = mix(a*b, vec3(0.0), equal(min(abs(a),abs(b)), vec3(0.0))); return (p.x + p.y) + p.z; }\n";
    p << "bool bcond(int i){ return ((bc[i>>7][(i>>5)&3] >> uint(i&31)) & 1u) != 0u; }\n";
    p << "layout(std430, set=0, binding=4) readonly buffer PAttrs { vec4 pa[]; };\n";
    p << "layout(std430, set=0, binding=5) buffer PClip { vec4 pclip[]; };\n";
    p << "layout(push_constant) uniform PC { vec4 ndc; } pc;\n";
    p << "void main() {\n";
    // One workgroup = one triangle (3 invocations); the renderer dispatches N
    // groups to sample N triangles of a draw and majority-votes the winding.
    p << "  uint _vi = gl_GlobalInvocationID.x;\n";
    bool pdyn = false;
    const uint32_t pnreg = CountRegs(e.body.str(), 1, &pdyn);  // M4.30
    EmitRegDecls(p, pnreg, pdyn);
    p << "  float ps = 0.0;\n  bool p0 = false;\n  int aL = 0;\n";
    p << "  vec4 oPos = vec4(0.0,0.0,0.0,1.0);\n  vec4 oPsz = vec4(1.0);\n";
    for (uint32_t i = 0; i < kInterp; ++i) p << "  vec4 o_" << i << " = vec4(0.0);\n";
    for (const auto& a : r.attrs)
      p << "  vec4 in_" << a.location << " = pa[_vi*8u + " << a.location << "u];\n";
    p << e.body.str();
    p << "  vec2 _gp = oPos.xy * pc.ndc.xy + pc.ndc.zw * oPos.w;\n";
    p << "  pclip[_vi] = vec4(_gp, oPos.z, oPos.w);\n";
    p << "}\n";
    r.probe_glsl = pdyn ? p.str() : ScalarizeRegs(p.str());
  }
  r.ok = true;
  if (!e.warnings.empty()) r.error = "unhandled ops: " + e.warnings;
  return r;
}

TranslatedShader TranslatePixelShader(const uint32_t* be_words, uint32_t num_dwords) {
  TranslatedShader r;
  std::vector<uint32_t> w(num_dwords);
  for (uint32_t i = 0; i < num_dwords; ++i) w[i] = std::byteswap(be_words[i]);

  Emit e;
  std::vector<CfEvent> ev;
  if (!WalkClauses(w.data(), num_dwords, /*allow_guards=*/true, ev, e.warnings)) {
    r.error = "empty/malformed CF";
    return r;
  }

  // Emit fetches and ALU inline in program order (a dependent texture read needs
  // its coordinate ALU to have run first -- hoisting all fetches to the top, as
  // the old code did, sampled with the register's pre-ALU value), wrapping each
  // bool-guarded range in an `if (bcond(n)==cond)` so disabled feature blocks
  // don't run and corrupt shared accumulators.
  for (const auto& it : ev) {
    if (EmitCfEvent(it, e)) continue;
    if (it.kind != CfEvent::kSlot) continue;
    const Slot& s = it.slot;
    if (s.is_fetch) {
      if ((s.words[0] & 0x1F) == uint32_t(FetchOpcode::kVertexFetch)) continue;
      TextureFetchInstruction tf;
      std::memcpy(&tf, s.words, 12);
      TextureBinding t;
      t.fetch_slot = tf.fetch_constant_index();
      t.dest_reg = tf.dest();
      t.src_reg = tf.src();
      t.src_swizzle = tf.src_swizzle();
      t.dst_swizzle = tf.dest_swizzle();
      t.dimension = uint32_t(tf.dimension());
      r.textures.push_back(t);
      e.max_src_reg = std::max(e.max_src_reg, t.src_reg);
      // sample = texture(tex_slot, coords); then apply dst swizzle.
      // M3.31: honor the tfetch SOURCE swizzle (2 bits per coordinate,
      // absolute component select). The game's post-FX shaders build (v,u)
      // in the source register and swizzle .yx at the fetch -- assuming .xy
      // here sampled the DOF/post chain TRANSPOSED (the presented-world tilt).
      const uint32_t ssw = t.src_swizzle;
      // M3.93 (RESTUFF_NUDGE_UV="dx,dy", texels at 1280x720): shift slot-1
      // sample coordinates -- the half-texel alignment experiment for the
      // depth-term floor flicker. Applies to ALL translated PSes' tex_1.
      static const std::pair<float, float> s_nudge = [] {
        std::pair<float, float> n{0.f, 0.f};
        if (const char* e = getenv("RESTUFF_NUDGE_UV")) sscanf(e, "%f,%f", &n.first, &n.second);
        return n;
      }();
      // M3.83 PS_DEBUG=8: latch the slot-1 (cookie) sample for epilogue output.
      static const int dbg8 = [] { const char* e2 = getenv("RESTUFF_PS_DEBUG"); return e2 ? atoi(e2) : 0; }();
      if ((dbg8 == 8 || dbg8 == 9) && t.fetch_slot == 1) e.uses_dbg_t1 = true;
      e.body << "  {\n    vec4 smp = texture(tex_" << t.fetch_slot << ", vec2(r[" << t.src_reg
             << "]." << kXYZW[ssw & 3] << ", r[" << t.src_reg << "]." << kXYZW[(ssw >> 2) & 3]
             << ")";
      if (t.fetch_slot == 1 && (s_nudge.first != 0.f || s_nudge.second != 0.f))
        e.body << " + vec2(" << (s_nudge.first / 1280.f) << ", " << (s_nudge.second / 720.f) << ")";
      e.body << ");\n";
      // Apply the fetch constant's EXP_ADJUST (2^e; e==0 => exact no-op).
      e.body << "    smp *= texexp(" << t.fetch_slot << ");\n";
      if ((dbg8 == 8 || dbg8 == 9) && t.fetch_slot == 1) e.body << "    _dbg_t1 = smp;\n";
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t ds = (t.dst_swizzle >> (3 * c)) & 7;
        if (ds == 7) continue;  // keep
        std::string v = ds <= 3 ? (std::string("smp.") + kXYZW[ds]) : (ds == 4 ? "0.0" : "1.0");
        e.body << "    r[" << t.dest_reg << "]." << kXYZW[c] << " = " << v << ";\n";
      }
      e.body << "  }\n";
    } else {
      AluInstruction a;
      std::memcpy(&a, s.words, 12);
      EmitAlu(a, e, /*is_vertex=*/false, r);
    }
  }

  std::ostringstream g;
  g << "#version 450\n";
  // PS constants at binding=1 so VS (binding=0) and PS constant UBOs don't
  // collide in the shared pipeline layout. Textures live in set 1.
  g << "layout(std140, set=0, binding=1) uniform PsConsts { vec4 c[256]; uvec4 lc[8]; "
       "uvec4 bc[2]; };\n";
  // M3.2: fixed-function alpha test (RB_COLORCONTROL/RB_ALPHA_REF), passed as
  // fragment push constants: apc.x = ref, apc.y = compare func (7 = always).
  // M3.7: boolc = the 256 shader bool constants (0x4900..) as two uvec4 at
  // offset 32; bcond(n) tests bit n, used by the kCondJmp feature guards.
  // M3.288: misc.x bit0 = gamma-encode color outputs. The console's gamma RTs
  // ENCODE ON WRITE (Xenos PWL); we sample gamma textures through _SRGB views
  // (linearize on read) but were writing shader output RAW into UNORM RTs, so
  // the display received linear values as if encoded -- lifting shadows by
  // ~x^(1/2.2) (measured vs real-360 footage: ours = real^(1/2.13)). Encoding
  // here, with the RT kept UNORM, also makes Vulkan blending mix ENCODED
  // values exactly as Xenos does (a Vulkan _SRGB RT would blend in linear --
  // the known emulation trap). sRGB curve, not the console's PWL, so the
  // encode is the exact inverse of our existing _SRGB-view decode and
  // RT->resolve->sample round-trips are stable; the sRGB-vs-PWL delta is <=3%.
  g << "layout(push_constant) uniform PC { layout(offset=16) vec4 apc; "
       "layout(offset=32) uvec4 boolc[2]; layout(offset=64) vec4 pgen; "
       "layout(offset=80) uvec4 texexp; layout(offset=112) uvec4 misc; } pc;\n";
  g << "bool bcond(int i){ return ((pc.boolc[i>>7][(i>>5)&3] >> uint(i&31)) & 1u) != 0u; }\n";
  g << "vec3 l2g(vec3 v){ v = clamp(v, vec3(0.0), vec3(1.0)); "
       "return mix(v*12.92, 1.055*pow(v, vec3(1.0/2.4)) - 0.055, "
       "greaterThanEqual(v, vec3(0.0031308))); }\n";
  // Xenos clamped-scalar helpers (kRcpc/kRsqc/kLogc -> +/-FLT_MAX, kRcpf/kRsqf
  // -> +/-0). This is the PIXEL preamble: without these, any PS using a clamped
  // scalar references undefined functions and its pipeline fails to build.
  g << "float _cinf(float x){ return isinf(x) ? (x > 0.0 ? 3.402823466e38 : -3.402823466e38) : x; }\n";
  g << "float _zinf(float x){ return isinf(x) ? (x > 0.0 ? 0.0 : -0.0) : x; }\n";
  g << "vec4 xmul(vec4 a, vec4 b){ return mix(a*b, vec4(0.0), equal(min(abs(a),abs(b)), vec4(0.0))); }\n";
  g << "float xmul(float a, float b){ return min(abs(a),abs(b)) == 0.0 ? 0.0 : a*b; }\n";
  g << "float xdot4(vec4 a, vec4 b){ vec4 p = xmul(a,b); return ((p.x + p.y) + p.z) + p.w; }\n";
  g << "float xdot3(vec3 a, vec3 b){ vec3 p = mix(a*b, vec3(0.0), equal(min(abs(a),abs(b)), vec3(0.0))); return (p.x + p.y) + p.z; }\n";
  // M3.9x: Xenos fetch-constant EXP_ADJUST (dword_3 bits 13..18, signed 6-bit)
  // scales every fetched value by 2^exp_adjust. The host packs it as one signed
  // byte per texture slot. Ignoring it left the sun-shaft's depth fetch
  // unscaled, so its world reconstruction sat inside the sun falloff at EVERY
  // pixel -> a uniform screen-wide darkening = the global world dim.
  g << "float texexp(int s){ uint p = pc.texexp[s>>2]; int e = int((p >> uint(8*(s&3))) & 0xFFu);"
       " if (e > 127) e -= 256; return exp2(float(e)); }\n";
  // Texture slots (unique).
  uint32_t declared = 0;
  for (const auto& t : r.textures) {
    if (declared & (1u << t.fetch_slot)) continue;
    declared |= (1u << t.fetch_slot);
    g << "layout(set=1, binding=" << t.fetch_slot << ") uniform sampler2D tex_" << t.fetch_slot
      << ";\n";
  }
  // Interpolator inputs (registers r0..max read as source arrive as interps).
  // Xenos hardware interpolates at most 16 registers -- higher GPRs read
  // before a write are plain temps (zero-initialized below), never inputs.
  // The clamp also keeps every pipeline under maxFragmentInputComponents.
  const uint32_t interp_in = std::min<uint32_t>(e.max_src_reg, 15);
  for (uint32_t i = 0; i <= interp_in; ++i)
    g << "layout(location=" << i << ") in vec4 o_" << i << ";\n";
  for (uint32_t i = 0; i < 4; ++i)
    if (r.color_export_mask & (1u << i)) g << "layout(location=" << i << ") out vec4 col_" << i << ";\n";
  g << "void main() {\n";
  bool rdyn = false;
  const uint32_t nreg = CountRegs(e.body.str(), interp_in + 1, &rdyn);  // M4.30
  EmitRegDecls(g, nreg, rdyn);
  g << "  float ps = 0.0;\n  bool p0 = false;\n  int aL = 0;\n";
  // Initialize the color OUTPUTS directly (no same-name local, which would
  // shadow the `out` and leave the real fragment output unwritten -> black).
  for (uint32_t i = 0; i < 4; ++i)
    if (r.color_export_mask & (1u << i)) g << "  col_" << i << " = vec4(0.0);\n";
  // Load interpolators into registers.
  for (uint32_t i = 0; i <= interp_in; ++i) g << "  r[" << i << "] = o_" << i << ";\n";
  // PARAM_GEN (SQ_PROGRAM_CNTL bit 18): the hardware injects the pixel position
  // into GPR pgen.y INSTEAD of an interpolator -- x = pixel X (sign flipped on
  // back faces), y = pixel Y, zw = point sprite ST (0 here). Fullscreen post
  // passes (e.g. the DOF) build their UVs from it; without this they read the
  // zero-initialized interpolator and collapse to a constant UV. pgen.z is the
  // PIX_CENTER=0 half-pixel correction (guest sees integer window coords).
  // M4.38: pgen.w is the reciprocal internal render scale -- gl_FragCoord is in
  // HOST pixels, and the guest expects its own 1280x720 window coords, so the
  // fragment position is divided back down before the half-pixel correction.
  // pgen.w == 1.0 at the default scale, leaving this expression unchanged.
  g << "  if (pc.pgen.x > 0.5) {\n"
       "    float px = gl_FragCoord.x * pc.pgen.w - pc.pgen.z, "
       "py = gl_FragCoord.y * pc.pgen.w - pc.pgen.z;\n"
       "    int _pgi = int(pc.pgen.y + 0.5);\n"
       "    vec4 _pgv = vec4(gl_FrontFacing ? px : -px, py, 0.0, 0.0);\n";
  if (rdyn) {
    g << "    r[min(_pgi, " << (nreg - 1) << ")] = _pgv;\n";
  } else {
    // M4.31: constant-index chain keeps the registers scalarizable. PARAM_GEN's
    // target is a register the shader reads, so it is always within nreg.
    for (uint32_t k = 0; k < nreg; ++k)
      g << "    if (_pgi == " << k << ") r[" << k << "] = _pgv;\n";
  }
  g << "  }\n";
  if (e.uses_dbg_t1) g << "  vec4 _dbg_t1 = vec4(0.0);\n";
  if (e.uses_latch) g << "  vec4 _latch = vec4(0.0);\n";
  g << e.body.str();
  // M3.88 (RESTUFF_DEPTH_F24=1): write fixed-function depth through the Xenos
  // float20e4 round-trip (SDK depth_float24_convert_in_pixel_shader
  // equivalent). Hardware depth is 24-bit float; our D32 carries extra
  // precision with different rounding, so the guest's screen-space
  // shadow-term comparisons (bias tuned for 24-bit ULP) flip on the equality
  // edge = per-pixel flicker. Denormal branch omitted: scene depths
  // (>=0.001) are all normalized in 20e4. Costs early-Z; experiment lever.
  {
    static const bool depth_f24 = [] { return getenv("RESTUFF_DEPTH_F24") != nullptr; }();
    if (depth_f24 && !r.exports_depth)
      g << "  { uint _u = floatBitsToUint(gl_FragCoord.z); uint _q;\n"
           "    if (!(gl_FragCoord.z > 0.0)) _q = 0u;\n"
           "    else if (_u >= 0x3FFFFFF8u) _q = 0xFFFFFFu;\n"
           "    else { _u += 0xC8000000u; _u += 3u + ((_u >> 3) & 1u); _q = (_u >> 3) & 0xFFFFFFu; }\n"
           "    gl_FragDepth = _q == 0u ? 0.0 : uintBitsToFloat((((_q >> 20) + 112u) << 23) | ((_q & 0xFFFFFu) << 3)); }\n";
  }
  // RESTUFF_PS_DEBUG (compile-time via translator): 1 = force solid magenta
  // colour (proves the mesh DRAWS where the scene is black -> shading is the
  // bug, not missing geometry); 2 = output the alpha the shading computed as
  // greyscale (is the fog/alpha term collapsing to black?). Read once.
  {
    static const int ps_dbg = [] {
      const char* e = getenv("RESTUFF_PS_DEBUG");
      return e ? atoi(e) : 0;
    }();
    if (ps_dbg == 1 && (r.color_export_mask & 1u))
      g << "  col_0 = vec4(1.0, 0.0, 1.0, 1.0);\n";
    else if (ps_dbg == 2 && (r.color_export_mask & 1u))
      g << "  col_0 = vec4(col_0.aaa, 1.0);\n";
    else if (ps_dbg >= 3 && (r.color_export_mask & 1u)) {
      // Interpolant-field visuals (all hash-gateable):
      //   3 = o_0 UV gradient (fract) -- the tilt-arc transpose discriminator
      //   4 = o_0.xyz signed (0.5+0.5v) -- normal-class interpolants
      //   5 = o_2.xyz signed            -- secondary normal/tangent
      //   6 = o_3.xyz signed            -- shadow/light-cookie UVs
      //   7 = gl_FrontFacing (green front / red back) -- faceness truth
      // RESTUFF_PS_DEBUG_HASH=<hex>: apply ONLY to that PS (same FNV as
      // GuestShaderHash) -- rewriting every PS invalidates the whole pipeline
      // cache and the mass shaderc recompile wrecks the boot/menu nav timing.
      // M3.90: comma-separated hash list (family-wide experiments).
      static const std::vector<uint64_t> dbg_hashes = [] {
        std::vector<uint64_t> v;
        if (const char* e = getenv("RESTUFF_PS_DEBUG_HASH")) {
          char* dup = strdup(e);
          for (char* tok = strtok(dup, ","); tok; tok = strtok(nullptr, ","))
            v.push_back(strtoull(tok, nullptr, 16));
          free(dup);
        }
        return v;
      }();
      bool apply = true;
      if (!dbg_hashes.empty()) {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < num_dwords; ++i) h = (h ^ be_words[i]) * 1099511628211ull;
        apply = std::find(dbg_hashes.begin(), dbg_hashes.end(), h) != dbg_hashes.end();
        // The gate hash is FNV-1a over be_words, which is NOT the same value as
        // GuestShaderHash / the shader_dump filename (those come out of the raw
        // guest bytes in the other byte order) -- the old comment claiming they
        // matched cost a whole run to disprove. Print the real gate value for
        // every translated PS so it can be looked up instead of guessed.
        fprintf(stderr, "[PSGATE] fnv=%016llX dwords=%u %s\n", (unsigned long long)h,
                unsigned(num_dwords), apply ? "<-- MATCH" : "");
      }
      if (apply) {
        if (ps_dbg == 3)
          g << "  col_0 = vec4(fract(o_0.x), fract(o_0.y), 0.0, 1.0);\n";
        else if (ps_dbg == 4)
          g << "  col_0 = vec4(o_0.xyz * 0.5 + 0.5, 1.0);\n";
        else if (ps_dbg == 5)
          g << "  col_0 = vec4(o_2.xyz * 0.5 + 0.5, 1.0);\n";
        else if (ps_dbg == 6)
          g << "  col_0 = vec4(o_3.xyz * 0.5 + 0.5, 1.0);\n";
        else if (ps_dbg == 7)
          g << "  col_0 = gl_FrontFacing ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);\n";
        else if (ps_dbg == 8 && e.uses_dbg_t1)
          g << "  col_0 = vec4(_dbg_t1.rgb, 1.0);\n";  // M3.83: raw cookie sample
        else if (ps_dbg == 9 && e.uses_dbg_t1)
          g << "  col_0 = vec4(_dbg_t1.xxx * 64.0, 1.0);\n";  // M3.83: t1.x scaled -- reads tiny depth values through the 8-bit aux dump (0.015 -> 245)
        else if (ps_dbg == 10 && (r.color_export_mask & 1u))
          g << "  col_0 = vec4(col_0.rgb * 8.0, 1.0);\n";  // M3.85: amplify the real output -- distinguishes computed~0 from lost-downstream
        else if (ps_dbg == 11 && r.exports_depth)
          g << "  gl_FragDepth = 0.5;\n";  // M3.86: known-depth probe -- proves the depth write/resolve path independent of the shader's sampling
        else if (ps_dbg == 12 && (r.color_export_mask & 1u))
          g << "  col_0 = vec4(col_0.rgb * 2.0, col_0.a);\n";  // M3.90: x2 discriminator for the world-at-half deficit
        else if (ps_dbg == 14 && (r.color_export_mask & 1u))
          g << "  col_0 = vec4(o_1.rgb, 1.0);\n";  // M3.94: raw o_1 (suspected baked vertex light)
        else if (ps_dbg == 15 && (r.color_export_mask & 1u))
          // M3.9x world-dim probe: dump the PARAM_GEN screen position the shader
          // actually received, normalized by the 640x360 post target. A correct
          // param-gen shows a smooth x/y gradient; a flat/constant field means
          // r0 never varied per pixel -- which is exactly what would make the
          // sun-shaft's reconstruction position-insensitive (uniform occ = dim).
          g << "  col_0 = vec4(abs(r[0].x) / 640.0, abs(r[0].y) / 360.0, 0.0, 1.0);\n";
        else if (ps_dbg == 16 && (r.color_export_mask & 1u))
          // World-dim probe (world PS 1BAB95FEECAC8E97): r3 is the LIGHT
          // ACCUMULATOR -- the four bool-gated directional blocks each do
          // r3 = max(dot(N,dir),0) * colour + r3, seeded from c8 = (0,0,0).
          // With the game's own constants the four colours sum to at most
          // (2.095, 1.886, 1.572), so a correct accumulation reaches ~2 and
          // this probe (x0.5) approaches white. If it tops out near half
          // that, a light is not contributing -- which is exactly the linear
          // ~1.8x scene deficit. Scaled, not clamped, so over-bright stays
          // visible rather than saturating silently.
          g << "  col_0 = vec4(r[3].xyz * 0.5, 1.0);\n";
        else if (ps_dbg == 17 && (r.color_export_mask & 1u))
          // Companion probe: r8 is the UNPACKED NORMAL (tex*c255.w + c254.w
          // = tex*2 - 1). A correct normal field is a smooth pastel shading;
          // a flat or degenerate one means the normal-map decode is wrong,
          // which would rescale every dot(N,L) uniformly.
          g << "  col_0 = vec4(r[8].xyz * 0.5 + 0.5, 1.0);\n";
        else if (ps_dbg == 18 && (r.color_export_mask & 1u))
          // COVERAGE probe: solid magenta, but hash-gated (modes 1/2 are not,
          // so they repaint every shader and prove nothing about one PS).
          // Answers "which pixels does THIS shader actually own?" -- needed
          // before trusting any region-sampled measurement of its output.
          g << "  col_0 = vec4(1.0, 0.0, 1.0, 1.0);\n";
        else if (ps_dbg == 20 && (r.color_export_mask & 1u))
          // Post pass CABCC0E0 ends in EXPORT = r0.yyyy * scene + blur, so
          // r0.y is the SCENE SCALE FACTOR. With little DOF/fog in view it
          // should sit near 1.0 (near-white here). If it reads ~0.55 mid-grey,
          // the factor itself is the world-dim -- no reference build needed to
          // tell, because the expected value is structural, not a comparison.
          g << "  col_0 = vec4(r[0].yyy, 1.0);\n";
        else if (ps_dbg == 24 && (r.color_export_mask & 1u) && e.uses_latch)
          // Display the RESTUFF_PS_LATCH capture: a register's value at a chosen
          // ALU instruction, before later blocks overwrite it.
          g << "  col_0 = vec4(_latch.xyz, 1.0);\n";
        else if (ps_dbg == 25 && (r.color_export_mask & 1u) && e.uses_latch)
          // Same, mapped signed (0.5+0.5v) for normal-class values.
          g << "  col_0 = vec4(_latch.xyz * 0.5 + 0.5, 1.0);\n";
        else if (ps_dbg == 23 && (r.color_export_mask & 1u))
          // Floor shader 02876DE9B27B1B35: r1 is the LIGHT ACCUMULATOR
          // (r1 = max(dot(N,L), c254.y=0.4) * colour + r1, four times, seeded
          // from c8=0). The four light colours sum to (2.095,1.886,1.572), so
          // with a 0.4 ambient floor r1 must land in luminance [0.73, 1.83].
          // Pinned near the 0.73-0.84 bottom => every dot(N,L) is clamped to
          // the floor (normals/lighting broken); mid-range => lighting is fine
          // and the deficit is elsewhere. Scaled x0.5 to fit 0..1.
          g << "  col_0 = vec4(r[1].xyz * 0.5, 1.0);\n";
        else if (ps_dbg == 22 && (r.color_export_mask & 1u))
          // Post pass CABCC0E0: EXPORT = r0.yyyy * scene + r1, so r1 is the
          // GLOW/BLUR additive term. The frame dump shows every blur-chain
          // surface at luminance 0.0000, which would mean the glow contributes
          // nothing -- but that CONTRADICTS the in-shader r2 probe (0.1635) of
          // the same buffer, so one of the two is a timing artifact. r1 is not
          // written by the export, so reading it here settles which.
          g << "  col_0 = vec4(r[1].xyz, 1.0);\n";
        else if (ps_dbg == 21 && (r.color_export_mask & 1u))
          // Companion: the pass's SCENE INPUT (r2) before the scale is applied.
          // Distinguishes "factor too small" from "scene arrived already dark".
          g << "  col_0 = vec4(r[2].xyz, 1.0);\n";
        else if (ps_dbg == 19 && (r.color_export_mask & 1u)) {
          // SHADER-IDENTITY MAP, now HASH-GATED (it must be): un-gated, the
          // fullscreen post pass repaints itself over the whole frame and hides
          // every world shader feeding the scene buffer. Gate it to the world
          // set and the post pass instead carries their identity colours
          // through, so one drive shows which shader owns which visible pixel.
          uint64_t hh = 1469598103934665603ull;
          for (uint32_t i = 0; i < num_dwords; ++i) hh = (hh ^ be_words[i]) * 1099511628211ull;
          fprintf(stderr, "[PSMAP] fnv=%016llX rgb=%u,%u,%u\n", (unsigned long long)hh,
                  unsigned((hh >> 40) & 0xFF), unsigned((hh >> 24) & 0xFF),
                  unsigned((hh >> 8) & 0xFF));
          char buf[128];
          snprintf(buf, sizeof(buf), "  col_0 = vec4(%.6f, %.6f, %.6f, 1.0);\n",
                   double((hh >> 40) & 0xFF) / 255.0, double((hh >> 24) & 0xFF) / 255.0,
                   double((hh >> 8) & 0xFF) / 255.0);
          g << buf;
        }
      }
    }
  }
  // Alpha test on the RT0 alpha (Xenos CompareFunction order; 0=never..7=always).
  if (r.color_export_mask & 1u) {
    g << "  int af = int(pc.apc.y + 0.5);\n"
         "  if (af != 7) {\n"
         "    float aa = col_0.a, ar = pc.apc.x; bool ap = false;\n"
         "    if (af==1) ap = aa <  ar; else if (af==2) ap = aa == ar;\n"
         "    else if (af==3) ap = aa <= ar; else if (af==4) ap = aa >  ar;\n"
         "    else if (af==5) ap = aa != ar; else if (af==6) ap = aa >= ar;\n"
         "    if (!ap) discard;\n"
         "  }\n";
  }
  // M3.288: encode-on-write for gamma RTs (see the l2g note in the preamble).
  // After the alpha test (which reads the shader-space alpha) and applied to
  // RGB only -- gamma never touches alpha on Xenos.
  {
    bool any = false;
    for (int i = 0; i < 4; ++i) any = any || (r.color_export_mask & (1u << i));
    if (any) {
      g << "  if ((pc.misc.x & 1u) != 0u) {\n";
      for (int i = 0; i < 4; ++i)
        if (r.color_export_mask & (1u << i))
          g << "    col_" << i << ".rgb = l2g(col_" << i << ".rgb);\n";
      g << "  }\n";
    }
  }
  g << "}\n";

  r.glsl = rdyn ? g.str() : ScalarizeRegs(g.str());
  r.ok = true;
  if (!e.warnings.empty()) r.error = "unhandled ops: " + e.warnings;
  return r;
}

const char* TranslatorBuildStamp() { return __DATE__ " " __TIME__; }

}  // namespace restuff::renderer::ucode
