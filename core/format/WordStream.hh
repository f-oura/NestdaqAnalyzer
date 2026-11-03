// Walks the words of a heartbeat frame and recognises multi-word records.
//
// Besides single TDC and delimiter words, a stream can carry records that consist of a header word
// with its own 6-bit type followed by payload words whose upper bits are arbitrary (for example the
// trigger records of a receiver board). Those payload words must not be read as TDC or delimiter words.
// A record type is registered with the bit field of its header that gives the number of words of the
// record, header included. Which types are records is configuration, not part of the generic format.
// This file depends on neither ROOT nor FairMQ.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "format/Frames.hh"
#include "format/TdcWord.hh"

namespace nda::format {

enum class WordRole {
  kWord,           // a single word (TDC, delimiter, throttling, unknown)
  kRecordHeader,   // first word of a record
  kRecordPayload,  // following words of a record
};

class WordStream {
 public:
  // Registers `head` as a record type whose word count (header included) is in bits
  // [countLsb + countBits - 1 : countLsb] of the header word.
  void AddRecordType(std::uint8_t head, int countLsb = 44, int countBits = 6) {
    fRules[head & 0x3F] = Rule{true, countLsb, countBits};
    fAny = true;
  }
  bool HasRecordTypes() const { return fAny; }
  bool IsRecordType(std::uint8_t head) const { return fRules[head & 0x3F].active; }

  // Calls f(word, role, recordSize) for every word of the frame; `recordSize` is the number of words of the
  // current record (0 for single words). A record cannot extend into the delimiter pair that closes the frame;
  // records cut short are counted in the return value (their remaining words are still reported as payload).
  template <class F>
  std::size_t ForEach(const HeartbeatFrameView& hbf, F&& f) const {
    if (!fAny) {
      for (std::size_t i = 0; i < hbf.nWords; ++i) f(hbf.words[i], WordRole::kWord, std::size_t{0});
      return 0;
    }
    std::size_t dataEnd = hbf.nWords;
    if (dataEnd >= 2 && HeadOf(hbf.words[dataEnd - 2]) == kHeartbeat1 && HeadOf(hbf.words[dataEnd - 1]) == kHeartbeat2) {
      dataEnd -= 2;
    }
    std::size_t truncated = 0;
    std::size_t i = 0;
    while (i < hbf.nWords) {
      const std::uint64_t w = hbf.words[i];
      const Rule& r = fRules[HeadOf(w)];
      if (i >= dataEnd || !r.active) {
        f(w, WordRole::kWord, std::size_t{0});
        ++i;
        continue;
      }
      std::size_t n = static_cast<std::size_t>((w >> r.countLsb) & ((std::uint64_t{1} << r.countBits) - 1));
      if (n == 0) n = 1;
      std::size_t end = i + n;
      if (end > dataEnd) {
        end = dataEnd;
        ++truncated;
      }
      f(w, WordRole::kRecordHeader, n);
      for (std::size_t j = i + 1; j < end; ++j) f(hbf.words[j], WordRole::kRecordPayload, n);
      i = end;
    }
    return truncated;
  }

 private:
  struct Rule {
    bool active = false;
    int countLsb = 44;
    int countBits = 6;
  };
  std::array<Rule, 64> fRules{};
  bool fAny = false;
};

}  // namespace nda::format
