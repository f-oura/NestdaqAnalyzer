// Interface of a monitor module. Every module receives the same parsed time frames,
// whether they come from a file (apps/) or from a FairMQ channel (devices/).
#pragma once

#include <cstdint>
#include <string>

#include "format/Frames.hh"
#include "monitor/Warning.hh"

namespace nda::monitor {

class HistRegistry;

struct RunInfo {
  std::uint64_t runNumber = 0;
};

struct FrameContext {
  std::uint64_t sequence = 0;  // index of this time frame among all received ones
  // False when time frames before this one were skipped (sampling or drops), so that
  // modules do not count the skipped heartbeat frames as missing.
  bool contiguous = true;
};

class Module {
 public:
  virtual ~Module() = default;
  virtual std::string Name() const = 0;
  // Called once before the first run.
  virtual void Book(HistRegistry& registry) = 0;
  virtual void BeginRun(const RunInfo& /*run*/) {}
  virtual void Process(const format::TimeFrameView& tf, const FrameContext& ctx) = 0;
  // Called periodically and at the end of a run to refresh derived histograms (rates and so on).
  virtual void Update() {}
  virtual void EndRun() {}
  // Appends the current warnings.
  virtual void Report(WarningList& /*out*/) const {}
};

}  // namespace nda::monitor
