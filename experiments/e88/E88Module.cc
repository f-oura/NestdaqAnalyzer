#include "e88/E88Module.hh"

#include <TH1D.h>
#include <TH2D.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include "e88/B2ttRecord.hh"
#include "format/TdcWord.hh"
#include "modules/FemTable.hh"
#include "monitor/HistRegistry.hh"

namespace nda::e88 {

namespace {

constexpr double kFrameNs = format::kHeartbeatFrameSeconds * 1e9;

enum CounterBin {
  kBinT1 = 1,
  kBinB2ttTrigger,
  kBinB2ttError,
  kBinB2ttOther,
  kBinUndecodable,
  kBinWithoutTime,
  kBinT1WithRecord,
  kBinRecordsWithT1,
  kBinRepeated,
  kBinSkipped,
  kBinUnmapped,
  kNumCounterBins = kBinUnmapped
};

// Frame distance in the range [-2^23, 2^23).
long SignedFrameDiff(std::uint32_t frame, std::uint32_t reference) {
  long d = static_cast<long>(format::FrameDistance(reference, frame));
  if (d >= (1L << 23)) d -= (1L << 24);
  return d;
}

std::string List(const std::vector<std::string>& items, std::size_t max = 12) {
  std::ostringstream os;
  for (std::size_t i = 0; i < items.size() && i < max; ++i) os << (i ? " " : "") << items[i];
  if (items.size() > max) os << " ... (" << items.size() << ")";
  return os.str();
}

}  // namespace

void E88Config::ApplySettings() {
  tdcLsbNs = settings.Number("tdc_lsb_ns", 0, tdcLsbNs);
  b2ttDataType = static_cast<int>(settings.Integer("b2tt_data_type", 0, b2ttDataType));
  scPairWindowNs = settings.Number("sc_pair_window", 0, scPairWindowNs);
  mrpcPairWindowNs = settings.Number("mrpc_pair_window", 0, mrpcPairWindowNs);
  t1SearchNs[0] = settings.Number("t1_search", 0, t1SearchNs[0]);
  t1SearchNs[1] = settings.Number("t1_search", 1, t1SearchNs[1]);
  b2ttSearchNs[0] = settings.Number("b2tt_search", 0, b2ttSearchNs[0]);
  b2ttSearchNs[1] = settings.Number("b2tt_search", 1, b2ttSearchNs[1]);
}

E88Module::E88Module(E88Config config) : fConfig(std::move(config)) {
  const auto& planes = fConfig.detectors.planes;
  fPlanes.resize(planes.size());
  for (std::size_t i = 0; i < planes.size(); ++i) {
    auto& s = fPlanes[i];
    s.sideCount = planes[i].nSides;
    s.hits.resize(static_cast<std::size_t>(planes[i].nSegments * planes[i].nSides));
    if (planes[i].nSides == 2 && planes[i].detector == "SC") s.pairWindow = fConfig.scPairWindowNs;
    if (planes[i].nSides == 2 && planes[i].detector == "MRPC") s.pairWindow = fConfig.mrpcPairWindowNs;
  }
  for (std::size_t i = 0; i < fConfig.channels.channels.size(); ++i) {
    if (fConfig.channels.channels[i].name == "T1") fT1 = static_cast<int>(i);
  }
  fChannelHits.assign(fConfig.channels.channels.size(), 0);
}

void E88Module::Book(monitor::HistRegistry& r) {
  const auto& planes = fConfig.detectors.planes;
  for (std::size_t i = 0; i < planes.size(); ++i) {
    const auto& p = planes[i];
    auto& s = fPlanes[i];
    const std::string folder = "E88/" + p.Label();
    const std::string L = p.Label();
    const int nch = p.nSegments * p.nSides;
    s.hitPattern = r.Book2D(folder, "hit_pattern", L + " leading edges;segment;side", p.nSegments, 0, p.nSegments,
                            p.nSides, 0, p.nSides);
    s.tot = r.Book2D(folder, "tot", L + " ToT;segment #times n_{side} + side;ToT [ns]", nch, 0, nch, 250, 0, 500);
    const double lo = fConfig.settings.Number("window." + p.detector, 0, -200);
    const double hi = fConfig.settings.Number("window." + p.detector, 1, 200);
    const int bins = std::max(50, std::min(2000, static_cast<int>(std::lround(hi - lo))));
    s.dtOrigin = r.Book2D(folder, "dt_sc_meantime",
                          L + " time - nearest SC mean time;segment #times n_{side} + side;#Deltat [ns]", nch, 0, nch,
                          bins, lo, hi);
    if (s.pairWindow > 0) {
      s.pairs = r.Book1D(folder, "pairs", L + " two-ended coincidences;segment;pairs", p.nSegments, 0, p.nSegments);
      s.pairDt = r.Book2D(folder, "pair_dt", L + " t(side 0) - t(side 1);segment;#Deltat [ns]", p.nSegments, 0,
                          p.nSegments, 200, -s.pairWindow, s.pairWindow);
    }
    if (p.detector == "AC" && p.nSegments == 9 && p.nSides == 1) {
      s.grid = r.Book2D(folder, "grid", L + " leading edges;column;row", 3, 0, 3, 3, 0, 3);
    }
  }
  const std::string t = "E88/Trigger";
  fCounters = r.Book1D(t, "counts", "Trigger and B2TT counts;;count", kNumCounterBins, 0.5,
                       static_cast<double>(kNumCounterBins) + 0.5);
  const char* labels[kNumCounterBins] = {"T1",
                                         "B2TT trigger",
                                         "B2TT error",
                                         "B2TT status/run",
                                         "B2TT undecodable",
                                         "B2TT no E88 time",
                                         "T1 with record",
                                         "record with T1",
                                         "event id repeated",
                                         "event id skipped",
                                         "unmapped hits"};
  for (int i = 0; i < kNumCounterBins; ++i) fCounters->GetXaxis()->SetBinLabel(i + 1, labels[i]);
  fT1VsTime = r.Book1D(t, "t1_vs_time", "T1;run time [s];T1 / s", 600, 0, 600);
  fT1VsTime->SetCanExtend(TH1::kXaxis);
  fB2ttVsTime = r.Book1D(t, "b2tt_vs_time", "B2TT trigger records;run time [s];records / s", 600, 0, 600);
  fB2ttVsTime->SetCanExtend(TH1::kXaxis);
  const double* ts = fConfig.t1SearchNs;
  fT1MinusSc = r.Book1D(t, "t1_minus_sc_meantime", "T1 - SC mean time;#Deltat [ns];candidates",
                        std::max(50, std::min(4000, static_cast<int>(std::lround(4 * (ts[1] - ts[0]))))), ts[0], ts[1]);
  const double* bs = fConfig.b2ttSearchNs;
  fB2ttMinusT1 = r.Book1D(t, "b2tt_minus_t1", "B2TT record time - T1;#Deltat [ns];pairs",
                          std::max(50, std::min(4000, static_cast<int>(std::lround(bs[1] - bs[0])))), bs[0], bs[1]);
  fEventIdStep = r.Book1D(t, "event_id_step", "Event id step between B2TT records (same spill);step;records", 13,
                          -2.5, 10.5);
}

void E88Module::BeginRun(const monitor::RunInfo&) {
  fCounts = Counts{};
  std::fill(fChannelHits.begin(), fChannelHits.end(), 0);
  fFemFrames.clear();
  fUnmapped.clear();
  fDead.clear();
  for (auto& s : fPlanes) s.pairsTotal = 0;
  fPendingT1.clear();
  fPendingRecords.clear();
  fT1Finalized = fRecordsFinalized = 0;
  fHaveEvent = false;
  fHaveFirstFrame = false;
  fFrameSpan = 0;
  fTimeFrames = 0;
}

void E88Module::Process(const format::TimeFrameView& tf, const monitor::FrameContext& ctx) {
  ++fTimeFrames;
  const std::uint32_t tfFrame = tf.header->timeFrameId & format::kFrameNumberMask;
  if (!fHaveFirstFrame) {
    fHaveFirstFrame = true;
    fPrevFrame = tfFrame;
  } else {
    fFrameSpan += format::FrameDistance(fPrevFrame, tfFrame);
    fPrevFrame = tfFrame;
  }
  fTfStartNs = static_cast<double>(fFrameSpan) * kFrameNs;
  if (!ctx.contiguous) {
    fHaveEvent = false;
    FinalizePending(1e300);
  }
  for (auto& s : fPlanes) {
    for (auto& v : s.hits) v.clear();
  }
  fOrigins.clear();
  fT1Times.clear();
  fRecordTimes.clear();

  CollectFrame(tf, ctx);

  // Two-ended coincidences; SC pairs give the time origins.
  const auto& planes = fConfig.detectors.planes;
  std::vector<char> used;
  for (std::size_t pi = 0; pi < fPlanes.size(); ++pi) {
    auto& s = fPlanes[pi];
    if (s.pairWindow <= 0) continue;
    const bool isSc = planes[pi].detector == "SC";
    for (int seg = 0; seg < planes[pi].nSegments; ++seg) {
      auto& a = s.hits[seg * 2];
      auto& b = s.hits[seg * 2 + 1];
      if (a.empty() || b.empty()) continue;
      auto byTime = [](const Hit& x, const Hit& y) { return x.t < y.t; };
      std::sort(a.begin(), a.end(), byTime);
      std::sort(b.begin(), b.end(), byTime);
      used.assign(b.size(), 0);
      for (const auto& ha : a) {
        auto it = std::lower_bound(b.begin(), b.end(), ha.t - s.pairWindow,
                                   [](const Hit& h, double v) { return h.t < v; });
        int best = -1;
        double bestDt = 0;
        for (; it != b.end() && it->t <= ha.t + s.pairWindow; ++it) {
          const int j = static_cast<int>(it - b.begin());
          const double dt = ha.t - it->t;
          if (!used[j] && (best < 0 || std::fabs(dt) < std::fabs(bestDt))) {
            best = j;
            bestDt = dt;
          }
        }
        if (best < 0) continue;
        used[best] = 1;
        ++s.pairsTotal;
        s.pairs->Fill(seg);
        s.pairDt->Fill(seg, bestDt);
        if (isSc) fOrigins.push_back(0.5 * (ha.t + b[best].t));
      }
    }
  }
  std::sort(fOrigins.begin(), fOrigins.end());

  // Every hit relative to the nearest SC mean time.
  if (!fOrigins.empty()) {
    for (std::size_t pi = 0; pi < fPlanes.size(); ++pi) {
      auto& s = fPlanes[pi];
      for (std::size_t k = 0; k < s.hits.size(); ++k) {
        for (const auto& h : s.hits[k]) {
          auto it = std::lower_bound(fOrigins.begin(), fOrigins.end(), h.t);
          double dt = 1e300;
          if (it != fOrigins.end()) dt = h.t - *it;
          if (it != fOrigins.begin() && std::fabs(h.t - *(it - 1)) < std::fabs(dt)) dt = h.t - *(it - 1);
          s.dtOrigin->Fill(static_cast<double>(k), dt);
        }
      }
    }
  }

  // T1 relative to the SC mean times before it.
  std::sort(fT1Times.begin(), fT1Times.end());
  for (double t1 : fT1Times) {
    auto it = std::lower_bound(fOrigins.begin(), fOrigins.end(), t1 - fConfig.t1SearchNs[1]);
    for (; it != fOrigins.end() && *it <= t1 - fConfig.t1SearchNs[0]; ++it) fT1MinusSc->Fill(t1 - *it);
  }

  // T1 and B2TT records, matched across time frames in run time.
  for (double t1 : fT1Times) AddT1(fTfStartNs + t1);
  for (double tr : fRecordTimes) AddRecord(fTfStartNs + tr);
  FinalizePending(fTfStartNs - (fConfig.b2ttSearchNs[1] - fConfig.b2ttSearchNs[0]) - 2 * kFrameNs);
}

void E88Module::CollectFrame(const format::TimeFrameView& tf, const monitor::FrameContext& ctx) {
  static const format::WordStream kPlainWords;
  const format::WordStream& stream = ctx.words ? *ctx.words : kPlainWords;
  const std::uint32_t tfFrame = tf.header->timeFrameId & format::kFrameNumberMask;
  const double runSeconds = static_cast<double>(fFrameSpan) * format::kHeartbeatFrameSeconds;
  std::vector<std::uint64_t> record;
  std::size_t recordSize = 0;
  for (const auto& stf : tf.subFrames) {
    const std::uint32_t fem = stf.header->femId;
    const auto kind = format::KindOfFemType(stf.header->femType);
    const double unit = kind == format::TdcKind::kLowResolution ? 1.0 : fConfig.tdcLsbNs;
    for (const auto& hbf : stf.frames) {
      // The first delimiter (at the end of the frame) gives the frame number.
      bool haveFrame = false;
      std::uint32_t frame = 0;
      for (std::size_t i = hbf.nWords; i-- > 0;) {
        if (format::HeadOf(hbf.words[i]) == format::kHeartbeat1) {
          frame = format::DecodeHeartbeat1(hbf.words[i]).frameNumber;
          haveFrame = true;
          break;
        }
      }
      if (!haveFrame) continue;
      ++fFemFrames[fem];
      const double frameStart = static_cast<double>(SignedFrameDiff(frame, tfFrame)) * kFrameNs;
      record.clear();
      recordSize = 0;
      stream.ForEach(hbf, [&](std::uint64_t w, format::WordRole role, std::size_t size) {
        if (role == format::WordRole::kRecordHeader) {
          if (!record.empty()) ++fCounts.b2ttUndecodable;  // previous record was cut short
          record.clear();
          recordSize = 0;
          if (static_cast<int>(format::HeadOf(w)) != fConfig.b2ttDataType) return;
          record.push_back(w);
          recordSize = size;
        } else if (role == format::WordRole::kRecordPayload) {
          if (recordSize == 0) return;
          record.push_back(w);
        } else {
          if (format::HeadOf(w) != format::kLeading || kind == format::TdcKind::kUnknown) return;
          const auto hit = format::DecodeTdc(w, kind);
          const auto* mc = fConfig.channels.Find(fem, static_cast<int>(hit.channel));
          if (!mc) {
            ++fUnmapped[ChannelMap::Address(fem, static_cast<int>(hit.channel))];
            ++fCounts.unmappedHits;
            return;
          }
          const int index = static_cast<int>(mc - fConfig.channels.channels.data());
          ++fChannelHits[index];
          auto& s = fPlanes[mc->plane];
          const double t = frameStart + hit.tdc * unit;
          const double tot = hit.tot * unit;
          s.hits[mc->segment * s.sideCount + mc->side].push_back(Hit{t, tot});
          s.hitPattern->Fill(mc->segment, mc->side);
          if (hit.tot > 0) s.tot->Fill(mc->segment * s.sideCount + mc->side, tot);
          if (s.grid) s.grid->Fill(mc->segment % 3, mc->segment / 3);
          if (index == fT1) {
            fT1Times.push_back(t);
            ++fCounts.t1;
            fT1VsTime->Fill(runSeconds);
          }
        }
        if (recordSize > 0 && record.size() == recordSize) {
          HandleRecord(record.data(), record.size(), tfFrame, runSeconds);
          record.clear();
          recordSize = 0;
        }
      });
      if (!record.empty()) ++fCounts.b2ttUndecodable;
    }
  }
}

void E88Module::HandleRecord(const std::uint64_t* words, std::size_t n, std::uint32_t tfFrame, double runSeconds) {
  B2ttRecord r;
  if (!DecodeB2ttRecord(words, n, r)) {
    ++fCounts.b2ttUndecodable;
    return;
  }
  if (r.recordType == B2ttRecord::kError) {
    ++fCounts.b2ttError;
    return;
  }
  if (r.recordType != B2ttRecord::kTrigger) {
    ++fCounts.b2ttOther;
    return;
  }
  ++fCounts.b2ttTrigger;
  fB2ttVsTime->Fill(runSeconds);
  if (r.hasId) {
    if (!fHaveEvent || r.spillId != fLastSpill) {
      ++fCounts.spills;
    } else {
      const auto step = static_cast<std::int32_t>(r.eventId - fLastEvent);
      fEventIdStep->Fill(step);
      if (step == 0) ++fCounts.repeatedEventIds;
      if (step > 1 || step < 0) ++fCounts.skippedEventIds;
    }
    fHaveEvent = true;
    fLastSpill = r.spillId;
    fLastEvent = r.eventId;
  }
  if (r.formatVersion >= 2 && r.timeValid) {
    fRecordTimes.push_back(static_cast<double>(SignedFrameDiff(r.frameNumber, tfFrame)) * kFrameNs +
                           r.heartbeatCount * kHeartbeatCountNs);
  } else {
    ++fCounts.b2ttWithoutTime;
  }
}

void E88Module::AddT1(double t) {
  // Records already seen with record - T1 inside the search window.
  bool matched = false;
  for (auto& rec : fPendingRecords) {
    const double dt = rec.t - t;
    if (dt >= fConfig.b2ttSearchNs[0] && dt <= fConfig.b2ttSearchNs[1]) {
      fB2ttMinusT1->Fill(dt);
      rec.matched = true;
      matched = true;
    }
  }
  fPendingT1.push_back(Pending{t, matched});
}

void E88Module::AddRecord(double t) {
  bool matched = false;
  for (auto& t1 : fPendingT1) {
    const double dt = t - t1.t;
    if (dt >= fConfig.b2ttSearchNs[0] && dt <= fConfig.b2ttSearchNs[1]) {
      fB2ttMinusT1->Fill(dt);
      t1.matched = true;
      matched = true;
    }
  }
  fPendingRecords.push_back(Pending{t, matched});
}

void E88Module::FinalizePending(double before) {
  auto flush = [before](std::deque<Pending>& q, std::uint64_t& finalized, std::uint64_t& matched) {
    while (!q.empty() && q.front().t < before) {
      ++finalized;
      if (q.front().matched) ++matched;
      q.pop_front();
    }
  };
  // Keep both queues ordered by time: entries of one time frame are added in time order per kind,
  // and time frames arrive in order.
  flush(fPendingT1, fT1Finalized, fCounts.t1WithRecord);
  flush(fPendingRecords, fRecordsFinalized, fCounts.recordsWithT1);
}

void E88Module::EndRun() { FinalizePending(1e300); }

void E88Module::Update() {
  fCounters->SetBinContent(kBinT1, static_cast<double>(fCounts.t1));
  fCounters->SetBinContent(kBinB2ttTrigger, static_cast<double>(fCounts.b2ttTrigger));
  fCounters->SetBinContent(kBinB2ttError, static_cast<double>(fCounts.b2ttError));
  fCounters->SetBinContent(kBinB2ttOther, static_cast<double>(fCounts.b2ttOther));
  fCounters->SetBinContent(kBinUndecodable, static_cast<double>(fCounts.b2ttUndecodable));
  fCounters->SetBinContent(kBinWithoutTime, static_cast<double>(fCounts.b2ttWithoutTime));
  fCounters->SetBinContent(kBinT1WithRecord, static_cast<double>(fCounts.t1WithRecord));
  fCounters->SetBinContent(kBinRecordsWithT1, static_cast<double>(fCounts.recordsWithT1));
  fCounters->SetBinContent(kBinRepeated, static_cast<double>(fCounts.repeatedEventIds));
  fCounters->SetBinContent(kBinSkipped, static_cast<double>(fCounts.skippedEventIds));
  fCounters->SetBinContent(kBinUnmapped, static_cast<double>(fCounts.unmappedHits));

  fDead.clear();
  for (std::size_t i = 0; i < fChannelHits.size(); ++i) {
    const auto& c = fConfig.channels.channels[i];
    auto it = fFemFrames.find(c.femId);
    const double live = it == fFemFrames.end() ? 0 : it->second * format::kHeartbeatFrameSeconds;
    if (live >= fConfig.deadAfterSeconds && fChannelHits[i] == 0) fDead.push_back(c.name);
  }
}

void E88Module::Report(monitor::WarningList& out) const {
  using monitor::Check;
  using monitor::Severity;
  for (const auto& w : fConfig.channels.warnings) out.push_back({Check::kDetectors, Severity::kWarning, Name(), "channel map: " + w});
  if (fTimeFrames >= 100) {
    for (auto fem : fConfig.channels.fems) {
      if (!fFemFrames.count(fem)) {
        out.push_back({Check::kDetectors, Severity::kError, Name(),
                       "module " + modules::FemLabel(fem) + " of the channel map sends no data"});
      }
    }
  }
  if (!fUnmapped.empty()) {
    std::vector<std::string> items;
    for (const auto& [address, hits] : fUnmapped) {
      items.push_back(modules::FemLabel(static_cast<std::uint32_t>(address >> 8)) + "/" +
                      std::to_string(address & 0xFF) + "(" + std::to_string(hits) + ")");
    }
    out.push_back({Check::kDetectors, Severity::kWarning, Name(),
                   std::to_string(fCounts.unmappedHits) + " hits on channels not in the channel map: " + List(items, 8)});
  }
  if (!fDead.empty()) {
    out.push_back({Check::kChannels, Severity::kWarning, Name(), "channels without hits: " + List(fDead)});
  }
  const auto& c = fCounts;
  if (c.b2ttUndecodable > 0) {
    out.push_back({Check::kTrigger, Severity::kWarning, Name(), std::to_string(c.b2ttUndecodable) + " undecodable B2TT records"});
  }
  if (c.b2ttWithoutTime > 0) {
    out.push_back({Check::kTrigger, Severity::kWarning, Name(),
                   std::to_string(c.b2ttWithoutTime) + " B2TT trigger records without a valid E88 time"});
  }
  if (c.repeatedEventIds > 0 || c.skippedEventIds > 0) {
    out.push_back({Check::kTrigger, Severity::kWarning, Name(),
                   "B2TT event id repeated " + std::to_string(c.repeatedEventIds) + " times, skipped " +
                       std::to_string(c.skippedEventIds) + " times"});
  }
  if (fT1 >= 0 && fConfig.b2ttDataType >= 0) {
    const double n = static_cast<double>(std::max(c.t1, c.b2ttTrigger));
    if (n >= 100 && std::fabs(static_cast<double>(c.t1) - static_cast<double>(c.b2ttTrigger)) > fConfig.triggerMismatchFraction * n) {
      out.push_back({Check::kTrigger, Severity::kWarning, Name(),
                     "T1 " + std::to_string(c.t1) + " vs B2TT trigger records " + std::to_string(c.b2ttTrigger)});
    }
    if (fT1Finalized >= 100 &&
        c.t1WithRecord < (1 - fConfig.triggerMismatchFraction) * static_cast<double>(fT1Finalized)) {
      out.push_back({Check::kTrigger, Severity::kWarning, Name(),
                     std::to_string(fT1Finalized - c.t1WithRecord) + " of " + std::to_string(fT1Finalized) +
                         " T1 without a B2TT record in the search window"});
    }
    if (fRecordsFinalized >= 100 &&
        c.recordsWithT1 < (1 - fConfig.triggerMismatchFraction) * static_cast<double>(fRecordsFinalized)) {
      out.push_back({Check::kTrigger, Severity::kWarning, Name(),
                     std::to_string(fRecordsFinalized - c.recordsWithT1) + " of " + std::to_string(fRecordsFinalized) +
                         " B2TT records without a T1 in the search window"});
    }
  }
}

std::string E88Module::JsonSummary() const {
  const auto& c = fCounts;
  std::ostringstream os;
  os << "{\"t1\": " << c.t1 << ", \"b2tt_trigger\": " << c.b2ttTrigger << ", \"b2tt_error\": " << c.b2ttError
     << ", \"b2tt_other\": " << c.b2ttOther << ", \"b2tt_undecodable\": " << c.b2ttUndecodable
     << ", \"b2tt_without_time\": " << c.b2ttWithoutTime << ", \"t1_finalized\": " << fT1Finalized
     << ", \"t1_with_record\": " << c.t1WithRecord << ", \"records_finalized\": " << fRecordsFinalized
     << ", \"records_with_t1\": " << c.recordsWithT1 << ", \"repeated_event_ids\": " << c.repeatedEventIds
     << ", \"skipped_event_ids\": " << c.skippedEventIds << ", \"spills\": " << c.spills
     << ", \"unmapped_hits\": " << c.unmappedHits << ", \"dead_channels\": " << fDead.size()
     << ", \"t1_minus_sc_mean\": " << fT1MinusSc->GetMean() << ", \"t1_minus_sc_entries\": " << fT1MinusSc->GetEntries()
     << ", \"b2tt_minus_t1_mean\": " << fB2ttMinusT1->GetMean() << ", \"b2tt_minus_t1_rms\": " << fB2ttMinusT1->GetRMS()
     << ", \"pairs\": {";
  for (std::size_t i = 0; i < fPlanes.size(); ++i) {
    os << (i ? ", " : "") << "\"" << fConfig.detectors.planes[i].Label() << "\": " << fPlanes[i].pairsTotal;
  }
  os << "}, \"hits\": {";
  for (std::size_t i = 0; i < fPlanes.size(); ++i) {
    os << (i ? ", " : "") << "\"" << fConfig.detectors.planes[i].Label() << "\": " << fPlanes[i].hitPattern->GetEntries();
  }
  os << "}}";
  return os.str();
}

}  // namespace nda::e88
