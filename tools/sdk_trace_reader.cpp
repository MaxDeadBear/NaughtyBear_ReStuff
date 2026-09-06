/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cinttypes>

#include <rex/filesystem.h>
#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/trace_protocol.h>
#include <rex/graphics/trace_reader.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/mapped_memory.h>
#include <rex/platform.h>

#include <snappy.h>

namespace rex::graphics {

bool TraceReader::Open(const std::string_view path) {
  Close();

  mmap_.reset();
#if REX_PLATFORM_ANDROID
  if (rex::filesystem::IsAndroidContentUri(path)) {
    mmap_ = memory::MappedMemory::OpenForAndroidContentUri(path, memory::MappedMemory::Mode::kRead);
  }
#endif  // REX_PLATFORM_ANDROID
  if (!mmap_) {
    mmap_ = memory::MappedMemory::Open(rex::to_path(path), memory::MappedMemory::Mode::kRead);
  }
  if (!mmap_) {
    return false;
  }

  trace_data_ = reinterpret_cast<const uint8_t*>(mmap_->data());
  trace_size_ = mmap_->size();

  // Verify version.
  auto header = reinterpret_cast<const TraceHeader*>(trace_data_);
  if (header->version != kTraceFormatVersion) {
    REXGPU_ERROR("Trace format version mismatch, code has {}, file has {}", kTraceFormatVersion,
                 header->version);
    if (header->version < kTraceFormatVersion) {
      REXGPU_ERROR("You need to regenerate your trace for the latest version");
    }
    return false;
  }

  REXGPU_INFO("Mapped {}b trace from {}", trace_size_, rex::path_to_utf8(path));
  REXGPU_INFO("   Version: {}", header->version);
  auto commit_str = std::string(header->build_commit_sha, rex::countof(header->build_commit_sha));
  REXGPU_INFO("    Commit: {}", commit_str);
  REXGPU_INFO("  Title ID: {}", header->title_id);

  ParseTrace();

  return true;
}

void TraceReader::Close() {
  mmap_.reset();
  trace_data_ = nullptr;
  trace_size_ = 0;
}

void TraceReader::ParseTrace() {
  // Skip file header.
  auto trace_ptr = trace_data_;
  trace_ptr += sizeof(TraceHeader);

  Frame current_frame;
  current_frame.start_ptr = trace_ptr;
  const PacketStartCommand* packet_start = nullptr;
  const uint8_t* packet_start_ptr = nullptr;
  const uint8_t* last_ptr = trace_ptr;
  bool pending_break = false;
  auto current_command_buffer = new CommandBuffer();
  current_frame.command_tree = std::unique_ptr<CommandBuffer>(current_command_buffer);

  // Diagnostic ring (env XTR_TRACE_RECS): remember the last 32 records so a
  // desync (unknown type) reports what preceded it.
  struct RecLog { size_t off; uint32_t type; };
  static RecLog s_recs[32];
  static int s_rec_n = 0;
  const bool s_reclog = getenv("XTR_TRACE_RECS") != nullptr;
  // Resync support: dev-build trace writers pad/overrun variable-length blobs
  // (observed: EdramSnapshot encoded_length short by ~32KB in a Proton-side
  // "rexglue-dev" trace). On landing at an invalid type, scan forward for the
  // first offset where a 6-record chain validates against the real struct
  // sizes, and continue there. Blob CONTENT before the slip is unaffected.
  const uint8_t* const t_end = trace_data_ + trace_size_;
  auto rec_size_at = [&](const uint8_t* p) -> size_t {
    if (p + 4 > t_end) return 0;
    const uint32_t t = memory::load<uint32_t>(p);
    auto enc_at = [&](size_t off) -> uint64_t {
      return p + off + 4 <= t_end ? memory::load<uint32_t>(p + off) : ~0ull;
    };
    switch (static_cast<TraceCommandType>(t)) {
      case TraceCommandType::kPrimaryBufferStart: {
        const uint64_t base = enc_at(4);
        if (base >= 0x20000000ull) return 0;
        const uint64_t c = enc_at(8);
        return c < 4u * 1024 * 1024 ? sizeof(PrimaryBufferStartCommand) + c * 4 : 0;
      }
      case TraceCommandType::kPrimaryBufferEnd: return sizeof(PrimaryBufferEndCommand);
      case TraceCommandType::kIndirectBufferStart: {
        const uint64_t c = enc_at(8);
        return c < 4u * 1024 * 1024 ? sizeof(IndirectBufferStartCommand) + c * 4 : 0;
      }
      case TraceCommandType::kIndirectBufferEnd: return sizeof(IndirectBufferEndCommand);
      case TraceCommandType::kPacketStart: {
        const uint64_t c = enc_at(8);
        return c < 64u * 1024 ? sizeof(PacketStartCommand) + c * 4 : 0;
      }
      case TraceCommandType::kPacketEnd: return sizeof(PacketEndCommand);
      case TraceCommandType::kMemoryRead:
      case TraceCommandType::kMemoryWrite: {
        // base_ptr must be guest-physical: rejects the "false chain" lock-on
        // where garbage floats parse as plausible jumbo memread records.
        const uint64_t base = enc_at(offsetof(MemoryCommand, base_ptr));
        if (base >= 0x20000000ull || (base & 3)) return 0;
        const uint64_t fmt = enc_at(offsetof(MemoryCommand, encoding_format));
        if (fmt > 1) return 0;
        const uint64_t e = enc_at(offsetof(MemoryCommand, encoded_length));
        return e < 64u * 1024 * 1024 ? sizeof(MemoryCommand) + e : 0;
      }
      case TraceCommandType::kEdramSnapshot: {
        const uint64_t e = enc_at(offsetof(EdramSnapshotCommand, encoded_length));
        return e < 64u * 1024 * 1024 ? sizeof(EdramSnapshotCommand) + e : 0;
      }
      case TraceCommandType::kEvent: return sizeof(EventCommand);
      case TraceCommandType::kRegisters: {
        const uint64_t e = enc_at(offsetof(RegistersCommand, encoded_length));
        return e < 16u * 1024 * 1024 ? sizeof(RegistersCommand) + e : 0;
      }
      case TraceCommandType::kGammaRamp: {
        const uint64_t e = enc_at(offsetof(GammaRampCommand, encoded_length));
        return e < 1u * 1024 * 1024 ? sizeof(GammaRampCommand) + e : 0;
      }
      default: return 0;
    }
  };
  auto chain_valid = [&](const uint8_t* p, int depth) {
    // Require at least one small CONTROL record (packet/buffer start/end,
    // event) in the chain -- a pure run of jumbo memread-shaped strides is the
    // classic false lock inside corrupted spans.
    bool control_seen = false;
    for (int i = 0; i < depth; ++i) {
      const size_t s = rec_size_at(p);
      if (!s) return false;
      const uint32_t t = memory::load<uint32_t>(p);
      if (t <= 5 || t == 9) control_seen = true;
      p += s;
      if (p > t_end) return false;
      if (p == t_end) return control_seen;  // clean EOF counts as valid
    }
    return control_seen;
  };
  while (trace_ptr < trace_data_ + trace_size_) {
    ++current_frame.command_count;
    auto type = static_cast<TraceCommandType>(memory::load<uint32_t>(trace_ptr));
    if (s_reclog) s_recs[(s_rec_n++) & 31] = {size_t(trace_ptr - trace_data_), uint32_t(type)};
    switch (type) {
      case TraceCommandType::kPrimaryBufferStart: {
        auto cmd = reinterpret_cast<const PrimaryBufferStartCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->count * 4;
        break;
      }
      case TraceCommandType::kPrimaryBufferEnd: {
        auto cmd = reinterpret_cast<const PrimaryBufferEndCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd);
        break;
      }
      case TraceCommandType::kIndirectBufferStart: {
        auto cmd = reinterpret_cast<const IndirectBufferStartCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->count * 4;

        // Traverse down a level.
        auto sub_command_buffer = new CommandBuffer();
        sub_command_buffer->parent = current_command_buffer;
        current_command_buffer->commands.push_back(CommandBuffer::Command(sub_command_buffer));
        current_command_buffer = sub_command_buffer;
        break;
      }
      case TraceCommandType::kIndirectBufferEnd: {
        auto cmd = reinterpret_cast<const IndirectBufferEndCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd);

        // IB packet is wrapped in a kPacketStart/kPacketEnd. Skip the end.
        auto end_cmd = reinterpret_cast<const PacketEndCommand*>(trace_ptr);
        assert_true(end_cmd->type == TraceCommandType::kPacketEnd);
        trace_ptr += sizeof(*cmd);

        // Go back up a level. If parent is null, this frame started in an
        // indirect buffer.
        if (current_command_buffer->parent) {
          current_command_buffer = current_command_buffer->parent;
        }
        break;
      }
      case TraceCommandType::kPacketStart: {
        auto cmd = reinterpret_cast<const PacketStartCommand*>(trace_ptr);
        packet_start_ptr = trace_ptr;
        packet_start = cmd;
        trace_ptr += sizeof(*cmd) + cmd->count * 4;
        break;
      }
      case TraceCommandType::kPacketEnd: {
        auto cmd = reinterpret_cast<const PacketEndCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd);
        if (!packet_start_ptr) {
          continue;
        }
        auto packet_category =
            PacketDisassembler::GetPacketCategory(packet_start_ptr + sizeof(*packet_start));
        switch (packet_category) {
          case PacketCategory::kDraw: {
            Frame::Command command;
            command.type = Frame::Command::Type::kDraw;
            command.head_ptr = packet_start_ptr;
            command.start_ptr = last_ptr;
            command.end_ptr = trace_ptr;
            current_frame.commands.push_back(std::move(command));
            last_ptr = trace_ptr;
            current_command_buffer->commands.push_back(
                CommandBuffer::Command(uint32_t(current_frame.commands.size() - 1)));
            break;
          }
          case PacketCategory::kSwap: {
            Frame::Command command;
            command.type = Frame::Command::Type::kSwap;
            command.head_ptr = packet_start_ptr;
            command.start_ptr = last_ptr;
            command.end_ptr = trace_ptr;
            current_frame.commands.push_back(std::move(command));
            last_ptr = trace_ptr;
            current_command_buffer->commands.push_back(
                CommandBuffer::Command(uint32_t(current_frame.commands.size() - 1)));
          } break;
          case PacketCategory::kGeneric: {
            // Ignored.
            break;
          }
        }
        if (pending_break) {
          current_frame.end_ptr = trace_ptr;
          frames_.push_back(std::move(current_frame));
          current_command_buffer = new CommandBuffer();
          current_frame.command_tree = std::unique_ptr<CommandBuffer>(current_command_buffer);
          current_frame.start_ptr = trace_ptr;
          current_frame.end_ptr = nullptr;
          current_frame.command_count = 0;
          pending_break = false;
        }
        break;
      }
      case TraceCommandType::kMemoryRead: {
        auto cmd = reinterpret_cast<const MemoryCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->encoded_length;
        break;
      }
      case TraceCommandType::kMemoryWrite: {
        auto cmd = reinterpret_cast<const MemoryCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->encoded_length;
        break;
      }
      case TraceCommandType::kEdramSnapshot: {
        auto cmd = reinterpret_cast<const EdramSnapshotCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->encoded_length;
        break;
      }
      case TraceCommandType::kEvent: {
        auto cmd = reinterpret_cast<const EventCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd);
        switch (cmd->event_type) {
          case EventCommand::Type::kSwap: {
            pending_break = true;
            break;
          }
        }
        break;
      }
      case TraceCommandType::kRegisters: {
        auto cmd = reinterpret_cast<const RegistersCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->encoded_length;
        break;
      }
      case TraceCommandType::kGammaRamp: {
        auto cmd = reinterpret_cast<const GammaRampCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->encoded_length;
        break;
      }
      default:
        // Unknown command type: almost always the TRUNCATED TAIL of a trace
        // whose writer was hard-killed mid-record (the game exits via
        // "hard-exiting process"). Everything before this point is intact --
        // stop parsing and keep the completed frames instead of asserting.
        {
          // Unknown type: resync (see note above the loop).
          const size_t bad_off = size_t(trace_ptr - trace_data_);
          const uint8_t* p = trace_ptr + 1;
          const uint8_t* found = nullptr;
          const uint8_t* scan_end =
              t_end - p > 16 * 1024 * 1024 ? p + 16 * 1024 * 1024 : t_end;
          for (; p < scan_end; ++p) {
            if (chain_valid(p, 6)) {
              found = p;
              break;
            }
          }
          if (found) {
            static int s_resyncs = 0;
            ++s_resyncs;
            if (s_resyncs <= 8 || (s_resyncs % 500) == 0)
              fprintf(stderr, "trace: resync #%d +%zu bytes after unknown type at +%zu\n",
                      s_resyncs, size_t(found - trace_ptr), bad_off);
            trace_ptr = found;
          } else {
            fprintf(stderr, "trace: unresyncable at +%zu -- stopping\n", bad_off);
            if (s_reclog) {
              const int n = s_rec_n < 32 ? s_rec_n : 32;
              for (int i = 0; i < n; ++i) {
                const auto& r = s_recs[(s_rec_n - n + i) & 31];
                fprintf(stderr, "  rec[-%02d] off=+%zu type=%u\n", n - i, r.off, r.type);
              }
            }
            trace_ptr = trace_data_ + trace_size_;
          }
        }
        break;
    }
  }
  if (pending_break || current_frame.command_count) {
    current_frame.end_ptr = trace_ptr;
    frames_.push_back(std::move(current_frame));
  }
}

bool TraceReader::DecompressMemory(MemoryEncodingFormat encoding_format, const void* src,
                                   size_t src_size, void* dest, size_t dest_size) {
  switch (encoding_format) {
    case MemoryEncodingFormat::kNone:
      assert_true(src_size == dest_size);
      std::memcpy(dest, src, src_size);
      return true;
    case MemoryEncodingFormat::kSnappy:
      return snappy::RawUncompress(reinterpret_cast<const char*>(src), src_size,
                                   reinterpret_cast<char*>(dest));
    default:
      assert_unhandled_case(encoding_format);
      return false;
  }
}

}  // namespace rex::graphics
