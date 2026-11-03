// Builds small time frames in memory for the unit tests.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "format/NestdaqHeaders.hh"
#include "format/TdcWord.hh"

namespace ndatest {

inline std::uint64_t Leading(std::uint32_t ch, std::uint32_t tot, std::uint32_t tdc) {
  return (std::uint64_t{nda::format::kLeading} << 58) | (std::uint64_t{ch & 0x7F} << 51) |
         (std::uint64_t{tot & 0x3FFFFF} << 29) | (tdc & 0x1FFFFFFF);
}
inline std::uint64_t Trailing(std::uint32_t ch, std::uint32_t tdc) {
  return (std::uint64_t{nda::format::kTrailing} << 58) | (std::uint64_t{ch & 0x7F} << 51) | (tdc & 0x1FFFFFFF);
}
inline std::uint64_t Delimiter1(std::uint32_t frame, std::uint16_t flags = 0, std::uint16_t offset = 0) {
  return (std::uint64_t{nda::format::kHeartbeat1} << 58) | (std::uint64_t{flags} << 40) |
         (std::uint64_t{offset} << 24) | (frame & 0xFFFFFF);
}
inline std::uint64_t Delimiter2(std::uint32_t transferred, std::uint32_t generated) {
  return (std::uint64_t{nda::format::kHeartbeat2} << 58) | (std::uint64_t{generated & 0xFFFFF} << 20) |
         (transferred & 0xFFFFF);
}

struct Stf {
  std::uint32_t femId = 0;
  std::uint32_t femType = ::SubTimeFrame::TDC64H;
  std::uint16_t type = 0;
  std::int64_t frameId = -1;  // sub time frame id; -1: same as the time frame
  std::vector<std::vector<std::uint64_t>> frames;  // words of each heartbeat frame (delimiters included)
};

// Returns the time frame as 64-bit words (so the storage is 8-byte aligned).
inline std::vector<std::uint64_t> BuildTimeFrame(std::uint32_t frameId, const std::vector<Stf>& stfs,
                                                  std::uint32_t numSource = 0xFFFFFFFF, std::uint16_t tfType = 0) {
  std::vector<std::uint8_t> body;
  auto append = [&body](const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    body.insert(body.end(), b, b + n);
  };
  for (const auto& s : stfs) {
    std::vector<std::uint8_t> payload;
    for (const auto& words : s.frames) {
      ::HeartbeatFrame::Header h;
      h.hLength = sizeof(h);
      h.length = static_cast<std::uint32_t>(sizeof(h) + 8 * words.size());
      const auto* hb = reinterpret_cast<const std::uint8_t*>(&h);
      payload.insert(payload.end(), hb, hb + sizeof(h));
      const auto* wb = reinterpret_cast<const std::uint8_t*>(words.data());
      payload.insert(payload.end(), wb, wb + 8 * words.size());
    }
    ::SubTimeFrame::Header sh;
    sh.hLength = sizeof(sh);
    sh.length = static_cast<std::uint32_t>(sizeof(sh) + payload.size());
    sh.type = s.type;
    sh.timeFrameId = s.frameId >= 0 ? static_cast<std::uint32_t>(s.frameId) : frameId;
    sh.femType = s.femType;
    sh.femId = s.femId;
    sh.numMessages = static_cast<std::uint32_t>(s.frames.size());
    append(&sh, sizeof(sh));
    append(payload.data(), payload.size());
  }
  ::TimeFrame::Header th;
  th.hLength = sizeof(th);
  th.length = static_cast<std::uint32_t>(sizeof(th) + body.size());
  th.type = tfType;
  th.timeFrameId = frameId;
  th.numSource = numSource == 0xFFFFFFFF ? static_cast<std::uint32_t>(stfs.size()) : numSource;
  std::vector<std::uint64_t> out((th.length + 7) / 8, 0);
  auto* o = reinterpret_cast<std::uint8_t*>(out.data());
  std::memcpy(o, &th, sizeof(th));
  std::memcpy(o + sizeof(th), body.data(), body.size());
  return out;
}

inline std::size_t Bytes(const std::vector<std::uint64_t>& tf) {
  return reinterpret_cast<const ::TimeFrame::Header*>(tf.data())->length;
}

}  // namespace ndatest

// Minimal test helpers.
#include <iostream>
inline int gFailures = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond << "\n"; \
      ++gFailures;                                                                    \
    }                                                                                 \
  } while (0)
#define CHECK_EQ(a, b)                                                                                      \
  do {                                                                                                      \
    const auto va = (a);                                                                                    \
    const auto vb = (b);                                                                                    \
    if (!(va == vb)) {                                                                                      \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a " = " << va << ", " #b " = " << vb \
                << "\n";                                                                                    \
      ++gFailures;                                                                                          \
    }                                                                                                       \
  } while (0)
