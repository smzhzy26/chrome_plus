#include "pakfile.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#pragma warning(disable : 4334)
#pragma warning(disable : 4267)
#pragma warning(disable : 4838)

extern "C" {
// The deflate/raw-inflate primitives and their constants live in miniz, which
// the project builds as a separate static library; this translation unit needs
// the types (`z_stream`, `MZ_*`) to run its own bounded inflate loop.
#include "..\mini_gzip\miniz.h"
#include "..\mini_gzip\mini_gzip.h"
void* gzip_compress(uint8_t* data, size_t len, size_t* out_len);
}

namespace {
#pragma pack(push)
#pragma pack(1)

constexpr int kPack4FileVersion = 4;
constexpr int kPack5FileVersion = 5;

struct Pak4Header {
  uint32_t num_entries;
  uint8_t encoding;
};

struct Pak5Header {
  uint32_t encoding;
  uint16_t resource_count;
  uint16_t alias_count;
};

struct PakEntry {
  uint16_t resource_id;
  uint32_t file_offset;
};

struct PakAlias {
  uint16_t resource_id;
  uint16_t entry_index;
};
#pragma pack(pop)

// Validates the pak header and the entry array against the size of the mapped
// view.
//
// The original version trusted the file completely: it read `resource_count`
// and the entry offsets straight out of the buffer and formed pointers from
// them, so a truncated or malformed pak made every later read go past the end
// of the mapping. It also fell through from the v4 branch into the v5 branch
// (no `return`), overwriting the pointers it had just set and treating the file
// as v5 -- and with `end_entry` never initialised on that path, the sentinel
// check below dereferenced an indeterminate pointer.
bool CheckHeader(uint8_t* buffer, size_t buffer_size, PakEntry*& pak_entry,
                 PakEntry*& end_entry) {
  // The version alone needs four bytes; every larger read is guarded as it is
  // reached.
  if (buffer_size < sizeof(uint32_t)) {
    return false;
  }

  const uint32_t version = *reinterpret_cast<const uint32_t*>(buffer);
  size_t entry_array_offset = 0;
  size_t entry_count = 0;

  if (version == kPack4FileVersion) {
    if (buffer_size < sizeof(uint32_t) + sizeof(Pak4Header)) {
      return false;
    }
    const auto* pak_header =
        reinterpret_cast<const Pak4Header*>(buffer + sizeof(uint32_t));
    if (pak_header->encoding != 1) {
      return false;
    }
    entry_array_offset = sizeof(uint32_t) + sizeof(Pak4Header);
    entry_count = pak_header->num_entries;
  } else if (version == kPack5FileVersion) {
    if (buffer_size < sizeof(uint32_t) + sizeof(Pak5Header)) {
      return false;
    }
    const auto* pak_header =
        reinterpret_cast<const Pak5Header*>(buffer + sizeof(uint32_t));
    if (pak_header->encoding != 1) {
      return false;
    }
    entry_array_offset = sizeof(uint32_t) + sizeof(Pak5Header);
    entry_count = pak_header->resource_count;
  } else {
    // Explicit, rather than falling through with the pointers unset.
    return false;
  }

  // The array holds `entry_count` entries plus the sentinel, so the whole array
  // has to fit. Compared as `needed <= available` instead of dividing: a
  // division underflows when fewer than one entry fits, and the multiplication
  // is not overflow-safe on a 32-bit build, where a v4 pak's 32-bit
  // `num_entries` can wrap the sentinel offset into a value that passes a naive
  // range check.
  const size_t available = buffer_size - entry_array_offset;
  const size_t needed = (entry_count + 1) * sizeof(PakEntry);
  if (needed / sizeof(PakEntry) != entry_count + 1 || needed > available) {
    return false;
  }

  pak_entry = reinterpret_cast<PakEntry*>(buffer + entry_array_offset);
  end_entry = pak_entry + entry_count;

  // In order to save the "next item" of the last item,
  // the id of this special item must be 0
  if (end_entry->resource_id != 0) {
    return false;
  }

  // The sentinel closes the last real entry's byte range, which therefore
  // includes that entry's 4-byte ISIZE trailer. It only has to lie inside the
  // view.
  if (end_entry->file_offset > buffer_size) {
    return false;
  }

  return true;
}

// --- Bounded decompression -------------------------------------------------
//
// `mini_gz_unpack` could not be used from here. It frees the caller's output
// buffer on every error path (`free(mem_out); return (-1);`, and the same for
// -2 and -4), which is a double free the moment the caller holds that buffer in
// a `unique_ptr` -- as this file did. Its inflate loop also has no terminating
// case for a truncated stream: once the input is exhausted `in_bytes_avail`
// underflows, and the only guard is `assert(ret != MZ_BUF_ERROR)`, which
// compiles away because the release configurations define NDEBUG. Measured
// against the real library, a stream truncated to half its length and one
// truncated to 30 bytes both failed to return within 8 seconds.
//
// The decompression is therefore done here: a loop with a hard iteration cap,
// handling every `mz_inflate` return code, and never touching the caller's
// buffer.

// Offset of the raw deflate stream inside a gzip entry, i.e. past the optional
// header fields. The library's own `mini_gz_start` read the FEXTRA length but
// then advanced only two bytes, never past the payload itself, so it computed
// the wrong offset for an entry that carries an extra field -- the layout this
// file writes when it patches an entry.
std::optional<size_t> DeflateStreamOffset(std::span<const uint8_t> entry) {
  constexpr size_t kGzipHeaderSize = 10;
  constexpr uint8_t kFlagHeaderCrc = 0x02;
  constexpr uint8_t kFlagExtra = 0x04;
  constexpr uint8_t kFlagName = 0x08;
  constexpr uint8_t kFlagComment = 0x10;

  if (entry.size() < kGzipHeaderSize) {
    return std::nullopt;
  }
  const uint8_t flags = entry[3];
  size_t offset = kGzipHeaderSize;

  const auto skip_to_nul = [&]() -> bool {
    while (offset < entry.size() && entry[offset] != 0x00) {
      ++offset;
    }
    if (offset >= entry.size()) {
      return false;  // no terminator
    }
    ++offset;
    return true;
  };

  if ((flags & kFlagExtra) != 0) {
    if (offset + 2 > entry.size()) {
      return std::nullopt;
    }
    const size_t extra_length = static_cast<size_t>(entry[offset]) |
                                (static_cast<size_t>(entry[offset + 1]) << 8);
    offset += 2 + extra_length;
    if (offset > entry.size()) {
      return std::nullopt;
    }
  }
  if ((flags & kFlagName) != 0 && !skip_to_nul()) {
    return std::nullopt;
  }
  if ((flags & kFlagComment) != 0 && !skip_to_nul()) {
    return std::nullopt;
  }
  if ((flags & kFlagHeaderCrc) != 0) {
    offset += 2;
    if (offset > entry.size()) {
      return std::nullopt;
    }
  }
  return offset;
}

// Decompresses `entry` into `out`, requiring exactly `expected_len` bytes.
// Returns the length written, or nullopt when the entry is malformed, does not
// decompress to `expected_len`, or does not finish.
std::optional<size_t> InflateExact(std::span<const uint8_t> entry,
                                   std::span<uint8_t> out,
                                   size_t expected_len) {
  constexpr int kChunkSize = 64 * 1024;
  // A stream that decompresses correctly reaches `avail_out == 0` or
  // MZ_STREAM_END in a handful of iterations. The cap means a stream that keeps
  // returning without progress cannot spin here even if the analysis above is
  // wrong about when that can happen -- which is the point: this is the
  // startup path of the browser and it must not be able to hang.
  constexpr int kMaxIterations = 1 << 16;

  if (entry.size() < 10) {
    return std::nullopt;
  }
  const auto stream_offset = DeflateStreamOffset(entry);
  if (!stream_offset || *stream_offset >= entry.size()) {
    return std::nullopt;
  }

  z_stream stream;
  std::memset(&stream, 0, sizeof(stream));
  if (inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
    return std::nullopt;
  }

  const uint8_t* next_in = entry.data() + *stream_offset;
  size_t in_bytes_avail = entry.size() - *stream_offset;
  stream.next_out = out.data();
  stream.avail_out = static_cast<unsigned int>(out.size());

  std::optional<size_t> result;
  for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
    const int chunk =
        static_cast<int>((std::min)(static_cast<size_t>(kChunkSize),
                                    in_bytes_avail));
    stream.avail_in += static_cast<unsigned int>(chunk);
    stream.next_in = const_cast<uint8_t*>(next_in);
    const int ret = mz_inflate(&stream, MZ_SYNC_FLUSH);
    next_in += chunk;
    in_bytes_avail -= static_cast<size_t>(chunk);

    if (stream.avail_out == 0) {
      // The buffer is exactly `expected_len` bytes, so a full buffer is a
      // successful decode of the whole payload. Mirroring the library here is
      // safe precisely because the caller sizes `out` from the entry's own
      // declared length.
      result = out.size();
      break;
    }
    if (ret == MZ_STREAM_END) {
      result = out.size() - stream.avail_out;
      break;
    }
    if (ret == MZ_DATA_ERROR || ret == MZ_PARAM_ERROR ||
        ret == MZ_MEM_ERROR) {
      break;  // malformed; result stays nullopt
    }
    if (ret == MZ_BUF_ERROR && in_bytes_avail == 0) {
      break;  // truncated: no more input to give, and it wants more
    }
  }

  inflateEnd(&stream);
  if (!result || *result != expected_len) {
    return std::nullopt;
  }
  return result;
}

}  // namespace

uint16_t TraversalGZIPFile(uint8_t* buffer, size_t buffer_size,
                           std::function<bool(uint8_t*, uint32_t, size_t&)>&& f,
                           uint16_t target_resource_id) {
  PakEntry* pak_entry = nullptr;
  PakEntry* end_entry = nullptr;

  if (!CheckHeader(buffer, buffer_size, pak_entry, end_entry)) {
    return 0;
  }

  // Bounded by the validated entry array, not by the sentinel alone. The
  // original loop trusted `resource_id != 0` to stop it, which is fine only
  // while the array is trustworthy; with the array validated, `end_entry` is the
  // real bound and a malformed file can no longer walk off it.
  for (; pak_entry < end_entry; ++pak_entry) {
    PakEntry* next_entry = pak_entry + 1;
    if (target_resource_id != 0 &&
        pak_entry->resource_id != target_resource_id) {
      continue;
    }

    // Both ends of the range come from the file, so both are checked before the
    // span is formed. A reversed range would otherwise make `old_size` wrap.
    if (pak_entry->file_offset > next_entry->file_offset ||
        next_entry->file_offset > buffer_size) {
      continue;
    }

    const size_t old_size =
        next_entry->file_offset - pak_entry->file_offset;

    if (old_size < 10 * 1024) {
      continue;
    }

    constexpr uint8_t kGzipMagic[] = {0x1F, 0x8B, 0x08};
    std::span<uint8_t> entry_data(buffer + pak_entry->file_offset, old_size);
    if (!std::ranges::equal(entry_data.subspan(0, sizeof(kGzipMagic)),
                            kGzipMagic)) {
      // Not a GZIP file, skipping
      continue;
    }

    // ISIZE lives in the last four bytes of the entry, which the range check
    // above has already shown to be inside the view.
    const uint32_t original_size = *reinterpret_cast<const uint32_t*>(
        buffer + next_entry->file_offset - sizeof(uint32_t));

    // The declared output size is attacker-controlled in a malicious or
    // corrupted pak, and it is about to size an allocation, so it has to be
    // bounded before it is believed. Two limits, both needed:
    //
    //   the ratio  -- deflate cannot expand more than ~1032:1, which is a
    //                property of the format rather than a chosen number, so a
    //                larger claim is not a big file, it is a broken field;
    //   the ceiling -- a ratio alone still permits gigabytes (4 MB of input
    //                times 1032), so an absolute cap does the real limiting.
    //
    // Measured on the real Chrome 119 paks: 1713 gzip entries, largest 2.6 MB
    // after decompression, and the highest ratio any entry reaches is 10.3:1.
    // The limits below are therefore ~100x and ~100x above anything real.
    constexpr size_t kMaxDeflateRatio = 1032;
    constexpr size_t kMaxResourceSize = 256u * 1024 * 1024;
    if (original_size == 0 || original_size > kMaxResourceSize ||
        static_cast<size_t>(original_size) >
            old_size * kMaxDeflateRatio) {
      continue;
    }

    // `malloc`/`free` rather than `make_unique_for_overwrite`: the size comes
    // from the file, so a corrupt value should be able to fail the allocation
    // rather than throw `std::bad_alloc` out of the browser's startup path.
    std::unique_ptr<uint8_t[], decltype(&std::free)> unpack_buffer(
        static_cast<uint8_t*>(std::malloc(original_size)), std::free);
    if (!unpack_buffer) {
      continue;
    }

    const auto unpack_len = InflateExact(entry_data, std::span(unpack_buffer.get(),
                                                               original_size),
                                         original_size);
    if (!unpack_len || *unpack_len != original_size) {
      continue;
    }
    uint32_t unpack_size = static_cast<uint32_t>(*unpack_len);

    {
      size_t new_len = old_size;
      bool changed = f(unpack_buffer.get(), unpack_size, new_len);

      if (changed) {
        size_t compress_size = 0;
        // `gzip_compress` is written in C style, so we free it using
        // `std::free`
        std::unique_ptr<void, decltype(&std::free)> compress_buffer_ptr(
            gzip_compress(unpack_buffer.get(), new_len, &compress_size),
            std::free);

        auto* compress_buffer =
            static_cast<uint8_t*>(compress_buffer_ptr.get());

        if (compress_buffer && compress_size < old_size) {
          std::span<uint8_t> src_span(compress_buffer, compress_size);
          std::ranges::copy(src_span.subspan(0, 10), entry_data.begin());
          entry_data[3] = 0x04;
          uint16_t extra_length =
              static_cast<uint16_t>(old_size - compress_size - 2);
          auto extra_len_dest = reinterpret_cast<uint16_t*>(&entry_data[10]);
          *extra_len_dest = extra_length;
          std::ranges::fill(entry_data.subspan(12, extra_length), 0);
          std::ranges::copy(src_span.subspan(10),
                            entry_data.begin() + 12 + extra_length);
        }
        // Only one resource is the patch target; once the callback has handled
        // it there is nothing left to find, so stop scanning the rest of the
        // pak to avoid decompressing every remaining entry in each renderer
        // process.
        return pak_entry->resource_id;
      }
    }
  }

  return 0;
}

std::optional<PakResourceSlot> FindResourceSlot(uint8_t* buffer,
                                                size_t buffer_size,
                                                uint16_t resource_id) {
  PakEntry* pak_entry = nullptr;
  PakEntry* end_entry = nullptr;

  if (!CheckHeader(buffer, buffer_size, pak_entry, end_entry)) {
    return std::nullopt;
  }

  // Same bound as the traversal: the validated array, not the sentinel.
  for (; pak_entry < end_entry; ++pak_entry) {
    if (pak_entry->resource_id != resource_id) {
      continue;
    }
    const PakEntry* next_entry = pak_entry + 1;
    if (pak_entry->file_offset > next_entry->file_offset ||
        next_entry->file_offset > buffer_size) {
      return std::nullopt;
    }
    return PakResourceSlot{pak_entry->file_offset,
                           next_entry->file_offset - pak_entry->file_offset};
  }

  return std::nullopt;
}
