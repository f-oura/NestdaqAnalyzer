// Views over one NestDAQ time frame stored contiguously in memory.
//
// Layout (all little endian, see nestdaq-user-impl):
//   TimeFrame header (24 B)
//     SubTimeFrame header (48 B)            one per front-end module (FEM)
//       HeartbeatFrame header (16 B) + 64-bit words, repeated
//
// The `length` field of every header counts the header itself.
// This file depends on neither ROOT nor FairMQ.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "format/NestdaqHeaders.hh"

namespace nda::format {

struct HeartbeatFrameView {
  const std::uint64_t* words = nullptr;
  std::size_t nWords = 0;
};

struct SubTimeFrameView {
  const ::SubTimeFrame::Header* header = nullptr;
  std::vector<HeartbeatFrameView> frames;
};

struct TimeFrameView {
  const ::TimeFrame::Header* header = nullptr;
  std::vector<SubTimeFrameView> subFrames;
};

// Parses one time frame. `data` must be 8-byte aligned and must outlive `out`.
// Returns false and fills `error` when the frame is malformed; `out` is then incomplete.
bool ParseTimeFrame(const std::uint8_t* data, std::size_t size, TimeFrameView& out, std::string& error);

// Time frame type bits (TimeFrame::Header::type).
bool IsIncomplete(const ::TimeFrame::Header& h);
bool IsTimeout(const ::TimeFrame::Header& h);
bool IsDepthLimit(const ::TimeFrame::Header& h);

}  // namespace nda::format
