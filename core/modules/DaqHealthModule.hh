// Generic DAQ health monitor (no channel map needed):
// time frame flags, modules present, words per type, heartbeat delimiter flags, throttling,
// continuity of heartbeat frame numbers, data rate per module.
#pragma once

#include <cstdint>
#include <vector>

#include "modules/FemTable.hh"
#include "monitor/Module.hh"

class TH1D;
class TH2D;

namespace nda::modules {

class DaqHealthModule : public monitor::Module {
 public:
  struct Options {
    int maxFems = 32;
    std::vector<std::uint32_t> expectedFems;  // warn when one of these is missing
    // Warn when a module that was seen has been absent for this many processed time frames.
    std::uint64_t absentFrames = 50;
  };

  // Word kinds counted per module.
  enum Kind {
    kKindLeading,
    kKindTrailing,
    kKindHeartbeat1,
    kKindHeartbeat2,
    kKindThrottlingT1Start,
    kKindThrottlingT1End,
    kKindThrottlingT2Start,
    kKindThrottlingT2End,
    kKindOther,
    kNumKinds
  };

  struct FemCounters {
    std::uint64_t words[kNumKinds] = {};
    std::uint64_t frames = 0;          // heartbeat frames (first delimiters) seen
    std::uint64_t missingFrames = 0;   // gaps in the frame number
    std::uint64_t repeatedFrames = 0;  // same frame number twice in a row
    std::uint64_t flaggedFrames = 0;   // delimiters with a non-zero flag field
    std::uint64_t throttledStfs = 0;   // sub time frames with a non-zero type
    std::uint64_t lastSequence = 0;
    std::uint32_t lastFrame = 0;
    bool haveLastFrame = false;
    std::uint16_t flagBitsSeen = 0;
  };

  struct Totals {
    std::uint64_t timeFrames = 0;
    std::uint64_t subTimeFrames = 0;
    std::uint64_t heartbeatFrames = 0;
    std::uint64_t incomplete = 0;
    std::uint64_t sourceCountMismatch = 0;
    std::uint64_t frameIdMismatch = 0;
  };

  explicit DaqHealthModule(Options options);
  std::string Name() const override { return "DaqHealth"; }
  void Book(monitor::HistRegistry& registry) override;
  void BeginRun(const monitor::RunInfo& run) override;
  void Process(const format::TimeFrameView& tf, const monitor::FrameContext& ctx) override;
  void Update() override;
  void Report(monitor::WarningList& out) const override;

  const Totals& GetTotals() const { return fTotals; }
  const FemTable& Fems() const { return fFems; }
  const FemCounters& Counters(int femIndex) const { return fCounters[femIndex]; }
  double RunSeconds() const;

 private:
  void AddFem(int index, std::uint32_t femId);

  Options fOptions;
  FemTable fFems;
  std::vector<FemCounters> fCounters;
  Totals fTotals;
  std::uint64_t fLastSequence = 0;
  bool fHaveFirstFrame = false;
  std::uint32_t fFirstFrame = 0;
  std::uint32_t fPrevFrame = 0;
  std::uint64_t fFrameSpan = 0;  // frames from the first time frame of the run (extended past the 24-bit wrap)

  TH1D* fTfFlags = nullptr;
  TH1D* fTfVsTime = nullptr;
  TH2D* fWords = nullptr;
  TH2D* fHbdFlags = nullptr;
  TH2D* fFrames = nullptr;
  TH1D* fDataRate = nullptr;
};

}  // namespace nda::modules
