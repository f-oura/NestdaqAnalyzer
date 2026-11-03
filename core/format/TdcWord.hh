// Decoding of the 64-bit AmQStrTdc words (HR-TDC and LR-TDC streaming firmware).
// Bit positions come from AmQStrTdcData.h (namespace AmQStrTdc::Data::v1) in nestdaq-user-impl.
// This file depends on neither ROOT nor FairMQ.
#pragma once

#include <cstdint>

#include "format/NestdaqHeaders.hh"

namespace nda::format {

// 6-bit word type in bits [63:58].
enum Head : std::uint8_t {
  kLeading = 0x0B,
  kTrailing = 0x0D,
  kHeartbeat1 = 0x1C,  // first heartbeat delimiter: frame number, time offset, flags
  kHeartbeat2 = 0x1E,  // second heartbeat delimiter: generated / transferred sizes
  kThrottlingT1Start = 0x19,
  kThrottlingT1End = 0x11,
  kThrottlingT2Start = 0x1A,
  kThrottlingT2End = 0x12,
};

static_assert(int(kLeading) == int(AmQStrTdc::Data::Data), "head type differs from AmQStrTdcData.h");
static_assert(int(kTrailing) == int(AmQStrTdc::Data::Trailer), "head type differs from AmQStrTdcData.h");
static_assert(int(kHeartbeat1) == int(AmQStrTdc::Data::Heartbeat), "head type differs from AmQStrTdcData.h");
static_assert(int(kHeartbeat2) == int(AmQStrTdc::Data::Heartbeat2nd), "head type differs from AmQStrTdcData.h");
static_assert(int(kThrottlingT1Start) == int(AmQStrTdc::Data::ThrottlingT1Start), "head type differs from AmQStrTdcData.h");
static_assert(int(kThrottlingT1End) == int(AmQStrTdc::Data::ThrottlingT1End), "head type differs from AmQStrTdcData.h");
static_assert(int(kThrottlingT2Start) == int(AmQStrTdc::Data::ThrottlingT2Start), "head type differs from AmQStrTdcData.h");
static_assert(int(kThrottlingT2End) == int(AmQStrTdc::Data::ThrottlingT2End), "head type differs from AmQStrTdcData.h");

inline std::uint8_t HeadOf(std::uint64_t word) { return static_cast<std::uint8_t>(word >> 58); }

// Resolution of the TDC words, decided from the SubTimeFrame femType set in the Sampler.
enum class TdcKind { kHighResolution, kLowResolution, kUnknown };

inline TdcKind KindOfFemType(std::uint32_t femType) {
  switch (femType) {
    case ::SubTimeFrame::TDC64H:  // also TDC64H_V1
    case ::SubTimeFrame::TDC64H_V3:
      return TdcKind::kHighResolution;
    case ::SubTimeFrame::TDC64L:  // also TDC64L_V2
    case ::SubTimeFrame::TDC64L_V1:
    case ::SubTimeFrame::TDC64L_V3:
      return TdcKind::kLowResolution;
    default:
      return TdcKind::kUnknown;
  }
}

inline int NumChannelBins(TdcKind kind) { return kind == TdcKind::kLowResolution ? 256 : 128; }

struct TdcHit {
  std::uint32_t channel = 0;
  std::uint32_t tot = 0;  // raw ToT field; 0 means a leading edge without a pair
  std::uint32_t tdc = 0;  // raw time within the heartbeat frame
  bool leading = true;
};

// Only valid for kLeading and kTrailing words.
inline TdcHit DecodeTdc(std::uint64_t word, TdcKind kind) {
  AmQStrTdc::Data::Bits b;
  b.raw = word;
  TdcHit hit;
  hit.leading = HeadOf(word) == kLeading;
  if (kind == TdcKind::kLowResolution) {
    hit.channel = b.ch;
    hit.tot = b.tot;
    hit.tdc = b.tdc;
  } else {
    hit.channel = b.hrch;
    hit.tot = b.hrtot;
    hit.tdc = b.hrtdc;
  }
  return hit;
}

struct Heartbeat1 {
  std::uint32_t frameNumber = 0;  // 24 bits, wraps around
  std::uint16_t timeOffset = 0;
  std::uint16_t flags = 0;
};

inline Heartbeat1 DecodeHeartbeat1(std::uint64_t word) {
  AmQStrTdc::Data::Bits b;
  b.raw = word;
  return Heartbeat1{static_cast<std::uint32_t>(b.hbframe), static_cast<std::uint16_t>(b.toffset),
                    static_cast<std::uint16_t>(b.hbflag)};
}

struct Heartbeat2 {
  std::uint32_t transferredSize = 0;  // bytes of data words in the frame before the delimiters
  std::uint32_t generatedSize = 0;
  std::uint16_t userRegister = 0;
};

inline Heartbeat2 DecodeHeartbeat2(std::uint64_t word) {
  AmQStrTdc::Data::Bits b;
  b.raw = word;
  return Heartbeat2{static_cast<std::uint32_t>(b.transSize), static_cast<std::uint32_t>(b.geneSize),
                    static_cast<std::uint16_t>(b.userReg)};
}

constexpr std::uint32_t kFrameNumberMask = 0xFFFFFF;

// Number of frames from `previous` to `current` on the 24-bit frame counter (1 for consecutive frames).
inline std::uint32_t FrameDistance(std::uint32_t previous, std::uint32_t current) {
  return (current - previous) & kFrameNumberMask;
}

// Length of one heartbeat frame: 2^16 clock cycles of 8 ns.
constexpr double kHeartbeatFrameSeconds = 65536 * 8e-9;

// Assumed time units of the raw TDC fields, chosen so that the field spans one heartbeat frame
// (HR: 29 bits of 2^-10 ns, LR: 19 bits of 1 ns). Not yet confirmed with real data.
inline double TdcUnitNs(TdcKind kind) { return kind == TdcKind::kLowResolution ? 1.0 : 1.0 / 1024.0; }

}  // namespace nda::format
