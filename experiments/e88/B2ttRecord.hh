// Records written by the B2TT bridge of the E88 receiver board: the trigger information of the
// partner experiment (spill id, event id, time stamp) together with the E88 time of the trigger.
//
// Layout (64-bit words; the 6-bit data type of the header is configuration):
//   header  [63:58] data type, [57:54] format version (1 or 2), [53:50] record type
//           (1 trigger, 2 status, 3 run marker, 4 error), [49:44] word count (header included),
//           [43:12] local sequence
//   trigger and error records (5 words):
//     word 1  [63:32] standard trigger tag, [31:28] trigger type, [27:24] event kind, [23] source internal,
//             [22] has id, [21] has time, [20] time stamp valid,
//             [19] v2: time valid (v1: capture quality), [18] v2: synchronized, [17] v2: standalone
//     word 2  [63:48] spill id, [47:16] event id
//     word 3  [63:16] time stamp of the partner experiment, [15:0] v2: heartbeat count (8 ns units)
//     word 4  [63:40] v2: heartbeat frame number, [39:32] v2: resync count, [31:0] stream sequence
// This file depends on neither ROOT nor FairMQ.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nda::e88 {

struct B2ttRecord {
  enum Type { kTrigger = 1, kStatus = 2, kRunMarker = 3, kError = 4 };
  int formatVersion = 0;
  int recordType = 0;
  int wordCount = 0;
  std::uint32_t localSequence = 0;
  // Trigger and error records only:
  std::uint32_t triggerTag = 0;
  int triggerType = 0;
  int eventKind = 0;
  bool hasId = false;
  bool hasTime = false;
  std::uint16_t spillId = 0;
  std::uint32_t eventId = 0;
  std::uint64_t timestamp = 0;  // 48 bits
  bool timeValid = false;       // v2: the E88 time below can be used
  std::uint32_t frameNumber = 0;     // v2, 24 bits
  std::uint16_t heartbeatCount = 0;  // v2, 8 ns units within the frame
  std::uint8_t resyncCount = 0;      // v2
  std::uint32_t streamSequence = 0;
};

constexpr double kHeartbeatCountNs = 8.0;

// Decodes a record from its words (header first). Returns false when the words do not form a record
// of a known version and type with the expected size.
inline bool DecodeB2ttRecord(const std::uint64_t* w, std::size_t n, B2ttRecord& r) {
  if (n == 0) return false;
  r = B2ttRecord{};
  r.formatVersion = static_cast<int>((w[0] >> 54) & 0xF);
  r.recordType = static_cast<int>((w[0] >> 50) & 0xF);
  r.wordCount = static_cast<int>((w[0] >> 44) & 0x3F);
  r.localSequence = static_cast<std::uint32_t>((w[0] >> 12) & 0xFFFFFFFF);
  if (r.formatVersion != 1 && r.formatVersion != 2) return false;
  const int expected = (r.recordType == B2ttRecord::kTrigger || r.recordType == B2ttRecord::kError) ? 5
                       : r.recordType == B2ttRecord::kStatus                                        ? 4
                       : r.recordType == B2ttRecord::kRunMarker                                     ? 3
                                                                                                    : 0;
  if (expected == 0 || r.wordCount != expected || n < static_cast<std::size_t>(expected)) return false;
  if (r.recordType != B2ttRecord::kTrigger && r.recordType != B2ttRecord::kError) return true;
  r.triggerTag = static_cast<std::uint32_t>(w[1] >> 32);
  r.triggerType = static_cast<int>((w[1] >> 28) & 0xF);
  r.eventKind = static_cast<int>((w[1] >> 24) & 0xF);
  r.hasId = (w[1] >> 22) & 1;
  r.hasTime = (w[1] >> 21) & 1;
  r.spillId = static_cast<std::uint16_t>(w[2] >> 48);
  r.eventId = static_cast<std::uint32_t>((w[2] >> 16) & 0xFFFFFFFF);
  r.timestamp = w[3] >> 16;
  r.streamSequence = static_cast<std::uint32_t>(w[4] & 0xFFFFFFFF);
  if (r.formatVersion == 2) {
    r.timeValid = (w[1] >> 19) & 1;
    r.heartbeatCount = static_cast<std::uint16_t>(w[3] & 0xFFFF);
    r.frameNumber = static_cast<std::uint32_t>((w[4] >> 40) & 0xFFFFFF);
    r.resyncCount = static_cast<std::uint8_t>((w[4] >> 32) & 0xFF);
  }
  return true;
}

}  // namespace nda::e88
