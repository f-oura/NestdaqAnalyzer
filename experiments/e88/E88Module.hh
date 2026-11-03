// E88 monitor: detector view through the channel map (SC, MRPC, AC), two-ended coincidences,
// times relative to the SC mean time, the E88 trigger (T1) and the B2TT records of the receiver board.
//
// Times are computed within one time frame (frame number of each heartbeat frame relative to the
// time frame id), so coincidences that straddle two time frames are not formed.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "e88/Params.hh"
#include "monitor/Module.hh"

class TH1D;
class TH2D;

namespace nda::e88 {

struct E88Config {
  DetectorDef detectors;
  ChannelMap channels;
  Settings settings;
  double tdcLsbNs = 1.0 / 1024;            // settings: tdc_lsb_ns (high-resolution TDC)
  int b2ttDataType = -1;                   // settings: b2tt_data_type (-1: no B2TT records)
  double scPairWindowNs = 50;              // settings: sc_pair_window
  double mrpcPairWindowNs = 50;            // settings: mrpc_pair_window
  double t1SearchNs[2] = {0, 400};         // settings: t1_search (T1 - SC mean time)
  double b2ttSearchNs[2] = {-100, 2000};   // settings: b2tt_search (record time - T1)
  double deadAfterSeconds = 2;             // warn about channels without hits after this much data
  double triggerMismatchFraction = 0.01;   // warn when T1 and B2TT counts differ by more than this

  // Fills the numeric fields from `settings`.
  void ApplySettings();
};

class E88Module : public monitor::Module {
 public:
  struct Counts {
    std::uint64_t t1 = 0;
    std::uint64_t b2ttTrigger = 0;
    std::uint64_t b2ttError = 0;
    std::uint64_t b2ttOther = 0;        // status and run marker records
    std::uint64_t b2ttUndecodable = 0;
    std::uint64_t b2ttWithoutTime = 0;  // trigger records without a valid E88 time
    std::uint64_t t1WithRecord = 0;     // T1 with a record in the B2TT search window
    std::uint64_t recordsWithT1 = 0;    // trigger records with a T1 in the window
    std::uint64_t repeatedEventIds = 0;
    std::uint64_t skippedEventIds = 0;  // occurrences of a jump of more than one
    std::uint64_t spills = 0;
    std::uint64_t unmappedHits = 0;
  };

  explicit E88Module(E88Config config);
  std::string Name() const override { return "E88"; }
  void Book(monitor::HistRegistry& registry) override;
  void BeginRun(const monitor::RunInfo& run) override;
  void Process(const format::TimeFrameView& tf, const monitor::FrameContext& ctx) override;
  void Update() override;
  void EndRun() override;
  void Report(monitor::WarningList& out) const override;
  std::string JsonSummary() const override;

  const Counts& GetCounts() const { return fCounts; }
  std::uint64_t Pairs(int plane) const { return fPlanes[plane].pairsTotal; }

 private:
  struct Hit {
    double t = 0;
    double tot = 0;
  };
  struct PlaneState {
    int sideCount = 1;
    double pairWindow = 0;  // 0: no two-ended pairing
    std::vector<std::vector<Hit>> hits;  // per (segment * nSides + side), cleared every time frame
    std::uint64_t pairsTotal = 0;
    TH2D* hitPattern = nullptr;
    TH2D* tot = nullptr;
    TH2D* dtOrigin = nullptr;
    TH1D* pairs = nullptr;
    TH2D* pairDt = nullptr;
    TH2D* grid = nullptr;  // AC 3x3 view
  };
  // T1 or B2TT record waiting for its partner (times in ns from the start of the run).
  struct Pending {
    double t = 0;
    bool matched = false;
  };

  void CollectFrame(const format::TimeFrameView& tf, const monitor::FrameContext& ctx);
  void HandleRecord(const std::uint64_t* words, std::size_t n, std::uint32_t tfFrame, double runSeconds);
  void AddT1(double t);
  void AddRecord(double t);
  void FinalizePending(double before);

  E88Config fConfig;
  std::vector<PlaneState> fPlanes;
  int fT1 = -1;  // index of the channel named "T1" in the channel map
  std::vector<std::uint64_t> fChannelHits;  // per mapped channel
  std::unordered_map<std::uint32_t, std::uint64_t> fFemFrames;
  std::map<std::uint64_t, std::uint64_t> fUnmapped;  // address -> hits
  std::vector<std::string> fDead;
  std::vector<double> fOrigins;      // SC mean times of the current time frame (ns from the time frame start)
  std::vector<double> fT1Times;      // same reference
  std::vector<double> fRecordTimes;  // same reference
  std::deque<Pending> fPendingT1;
  std::deque<Pending> fPendingRecords;
  std::uint64_t fT1Finalized = 0;
  std::uint64_t fRecordsFinalized = 0;
  double fTfStartNs = 0;
  Counts fCounts;
  bool fHaveEvent = false;
  std::uint16_t fLastSpill = 0;
  std::uint32_t fLastEvent = 0;
  bool fHaveFirstFrame = false;
  std::uint32_t fPrevFrame = 0;
  std::uint64_t fFrameSpan = 0;
  std::uint64_t fTimeFrames = 0;

  TH1D* fCounters = nullptr;
  TH1D* fT1VsTime = nullptr;
  TH1D* fB2ttVsTime = nullptr;
  TH1D* fT1MinusSc = nullptr;
  TH1D* fB2ttMinusT1 = nullptr;
  TH1D* fEventIdStep = nullptr;
};

}  // namespace nda::e88
