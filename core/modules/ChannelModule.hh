// Generic per-channel monitor (no channel map needed): hit pattern, rate, ToT and time within the
// heartbeat frame for every (module, channel), with warnings for hot channels and channels that went silent.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "format/TdcWord.hh"
#include "modules/FemTable.hh"
#include "monitor/Module.hh"

class TH1D;
class TH2D;

namespace nda::monitor {
class HistRegistry;
}

namespace nda::modules {

class ChannelModule : public monitor::Module {
 public:
  struct Options {
    int maxFems = 32;
    double totMaxNsHigh = 500;   // ToT axis for high-resolution TDCs [ns]
    double totMaxNsLow = 2000;   // ToT axis for low-resolution TDCs [ns]
    int totBins = 250;
    int timeBins = 512;          // time within the heartbeat frame
    double hotFactor = 10;       // hot: rate above this many times the median of active channels
    double silentMinRateHz = 1;  // silent: a channel above this rate got no hit since the last update
    double silentMinSeconds = 2; // ... provided at least this much data time passed since the last update
  };

  struct FemData {
    std::uint32_t femId = 0;
    format::TdcKind kind = format::TdcKind::kUnknown;
    int nChannels = 0;
    std::vector<std::uint64_t> leading;   // per channel
    std::vector<std::uint64_t> trailing;  // per channel
    std::vector<std::uint64_t> leadingAtLastUpdate;
    std::uint64_t leadingWithoutTot = 0;
    std::uint64_t frames = 0;
    std::uint64_t framesAtLastUpdate = 0;
    std::vector<std::uint32_t> silentChannels;  // found at the last update
    std::vector<std::uint32_t> hotChannels;
    TH1D* hitPattern = nullptr;
    TH1D* rate = nullptr;
    TH2D* tot = nullptr;
    TH2D* time = nullptr;
    TH1D* kinds = nullptr;
  };

  ChannelModule(Options options);
  std::string Name() const override { return "Channels"; }
  void Book(monitor::HistRegistry& registry) override;
  void BeginRun(const monitor::RunInfo& run) override;
  void Process(const format::TimeFrameView& tf, const monitor::FrameContext& ctx) override;
  void Update() override;
  void Report(monitor::WarningList& out) const override;

  int NumFems() const { return static_cast<int>(fData.size()); }
  const FemData& Fem(int index) const { return *fData[index]; }

 private:
  FemData& Get(std::uint32_t femId, std::uint32_t femType);

  Options fOptions;
  monitor::HistRegistry* fRegistry = nullptr;
  FemTable fFems;
  std::vector<std::unique_ptr<FemData>> fData;
};

}  // namespace nda::modules
