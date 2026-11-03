#include "modules/DaqHealthModule.hh"

#include <TH1D.h>
#include <TH2D.h>

#include <sstream>

#include "format/TdcWord.hh"
#include "monitor/HistRegistry.hh"

namespace nda::modules {

namespace {

const char* const kKindLabels[DaqHealthModule::kNumKinds] = {
    "leading", "trailing", "delimiter 1", "delimiter 2", "thr. T1 start", "thr. T1 end", "thr. T2 start",
    "thr. T2 end", "other"};

enum FrameBin { kBinFrames = 1, kBinMissing, kBinRepeated, kBinFlagged, kBinThrottledStf, kNumFrameBins = 5 };
enum TfBin { kBinComplete = 1, kBinIncomplete, kBinTimeout, kBinDepthLimit, kBinSourceMismatch, kBinFrameIdMismatch };

DaqHealthModule::Kind KindOf(std::uint8_t head) {
  using namespace format;
  switch (head) {
    case kLeading:
      return DaqHealthModule::kKindLeading;
    case kTrailing:
      return DaqHealthModule::kKindTrailing;
    case kHeartbeat1:
      return DaqHealthModule::kKindHeartbeat1;
    case kHeartbeat2:
      return DaqHealthModule::kKindHeartbeat2;
    case kThrottlingT1Start:
      return DaqHealthModule::kKindThrottlingT1Start;
    case kThrottlingT1End:
      return DaqHealthModule::kKindThrottlingT1End;
    case kThrottlingT2Start:
      return DaqHealthModule::kKindThrottlingT2Start;
    case kThrottlingT2End:
      return DaqHealthModule::kKindThrottlingT2End;
    default:
      return DaqHealthModule::kKindOther;
  }
}

}  // namespace

DaqHealthModule::DaqHealthModule(Options options) : fOptions(std::move(options)), fFems(fOptions.maxFems) {
  // Expected modules get the first bins so that a missing one shows up as an empty bin.
  for (auto id : fOptions.expectedFems) {
    bool isNew = false;
    fFems.Index(id, isNew);
  }
  fCounters.resize(fFems.Size());
}

void DaqHealthModule::Book(monitor::HistRegistry& r) {
  const int n = fOptions.maxFems;
  fTfFlags = r.Book1D("DAQ", "tf_flags", "Time frame flags;;time frames", 6, 0.5, 6.5);
  const char* tfLabels[] = {"complete", "incomplete", "timeout", "depth limit", "#STF != numSource", "STF id differ"};
  for (int i = 0; i < 6; ++i) fTfFlags->GetXaxis()->SetBinLabel(i + 1, tfLabels[i]);
  fTfVsTime = r.Book1D("DAQ", "tf_vs_time", "Processed time frames;run time [s];time frames / s", 600, 0, 600);
  fTfVsTime->SetCanExtend(TH1::kXaxis);
  fWords = r.Book2D("DAQ", "fem_words", "Words per module and type;module;", n, 0, n, kNumKinds, 0, kNumKinds);
  for (int k = 0; k < kNumKinds; ++k) fWords->GetYaxis()->SetBinLabel(k + 1, kKindLabels[k]);
  fHbdFlags = r.Book2D("DAQ", "fem_delimiter_flags", "Heartbeat delimiter flag bits;module;flag bit", n, 0, n, 16, 0,
                       16);
  fFrames = r.Book2D("DAQ", "fem_frames", "Heartbeat frames per module;module;", n, 0, n, kNumFrameBins, 0.5,
                     static_cast<double>(kNumFrameBins) + 0.5);
  const char* frameLabels[] = {"frames", "missing", "repeated", "flagged", "throttled STF"};
  for (int i = 0; i < kNumFrameBins; ++i) fFrames->GetYaxis()->SetBinLabel(i + 1, frameLabels[i]);
  fDataRate = r.Book1D("DAQ", "fem_data_rate", "Data words (excluding delimiters) per second;module;words / s", n, 0,
                       n);
  for (int i = 0; i < fFems.Size(); ++i) AddFem(i, fFems.IdAt(i));
}

void DaqHealthModule::AddFem(int index, std::uint32_t femId) {
  if (static_cast<int>(fCounters.size()) <= index) fCounters.resize(index + 1);
  const std::string label = FemLabel(femId);
  for (TH1* h : {static_cast<TH1*>(fWords), static_cast<TH1*>(fHbdFlags), static_cast<TH1*>(fFrames),
                 static_cast<TH1*>(fDataRate)}) {
    if (h) h->GetXaxis()->SetBinLabel(index + 1, label.c_str());
  }
}

void DaqHealthModule::BeginRun(const monitor::RunInfo&) {
  for (auto& c : fCounters) c = FemCounters{};
  fTotals = Totals{};
  fLastSequence = 0;
  fHaveFirstFrame = false;
  fFrameSpan = 0;
}

double DaqHealthModule::RunSeconds() const { return static_cast<double>(fFrameSpan) * format::kHeartbeatFrameSeconds; }

void DaqHealthModule::Process(const format::TimeFrameView& tf, const monitor::FrameContext& ctx) {
  fLastSequence = ctx.sequence;
  ++fTotals.timeFrames;
  const auto& h = *tf.header;
  bool complete = true;
  if (format::IsIncomplete(h)) {
    fTfFlags->Fill(kBinIncomplete);
    ++fTotals.incomplete;
    complete = false;
  }
  if (format::IsTimeout(h)) {
    fTfFlags->Fill(kBinTimeout);
    complete = false;
  }
  if (format::IsDepthLimit(h)) {
    fTfFlags->Fill(kBinDepthLimit);
    complete = false;
  }
  if (complete) fTfFlags->Fill(kBinComplete);
  if (tf.subFrames.size() != h.numSource) {
    fTfFlags->Fill(kBinSourceMismatch);
    ++fTotals.sourceCountMismatch;
  }

  // Run time from the frame number of the first sub time frame (extended past the 24-bit wrap).
  if (!tf.subFrames.empty()) {
    const std::uint32_t frame = tf.subFrames.front().header->timeFrameId & format::kFrameNumberMask;
    if (!fHaveFirstFrame) {
      fHaveFirstFrame = true;
      fFirstFrame = fPrevFrame = frame;
    } else {
      fFrameSpan += format::FrameDistance(fPrevFrame, frame);
      fPrevFrame = frame;
    }
    fTfVsTime->Fill(RunSeconds());
  }

  bool idDiffers = false;
  for (const auto& stf : tf.subFrames) {
    ++fTotals.subTimeFrames;
    if (stf.header->timeFrameId != tf.subFrames.front().header->timeFrameId) idDiffers = true;
    bool isNew = false;
    const int fi = fFems.Index(stf.header->femId, isNew);
    if (fi < 0) continue;  // more modules than bins
    if (isNew) AddFem(fi, stf.header->femId);
    auto& c = fCounters[fi];
    if (!ctx.contiguous || ctx.sequence != c.lastSequence + 1) c.haveLastFrame = false;
    c.lastSequence = ctx.sequence;
    if (stf.header->type != 0) {
      ++c.throttledStfs;
      fFrames->Fill(fi, kBinThrottledStf);
    }
    for (const auto& hbf : stf.frames) {
      ++fTotals.heartbeatFrames;
      for (std::size_t i = 0; i < hbf.nWords; ++i) {
        const std::uint64_t w = hbf.words[i];
        const Kind kind = KindOf(format::HeadOf(w));
        ++c.words[kind];
        if (kind != kKindHeartbeat1) continue;
        const auto d = format::DecodeHeartbeat1(w);
        ++c.frames;
        fFrames->Fill(fi, kBinFrames);
        if (d.flags != 0) {
          ++c.flaggedFrames;
          c.flagBitsSeen |= d.flags;
          fFrames->Fill(fi, kBinFlagged);
          for (int b = 0; b < 16; ++b) {
            if (d.flags & (1u << b)) fHbdFlags->Fill(fi, b);
          }
        }
        if (c.haveLastFrame) {
          const std::uint32_t dist = format::FrameDistance(c.lastFrame, d.frameNumber);
          if (dist == 0) {
            ++c.repeatedFrames;
            fFrames->Fill(fi, kBinRepeated);
          } else if (dist > 1) {
            c.missingFrames += dist - 1;
            fFrames->Fill(fi, kBinMissing, dist - 1);
          }
        }
        c.lastFrame = d.frameNumber;
        c.haveLastFrame = true;
      }
    }
  }
  if (idDiffers) {
    fTfFlags->Fill(kBinFrameIdMismatch);
    ++fTotals.frameIdMismatch;
  }
}

void DaqHealthModule::Update() {
  for (int i = 0; i < fFems.Size() && i < static_cast<int>(fCounters.size()); ++i) {
    const auto& c = fCounters[i];
    for (int k = 0; k < kNumKinds; ++k) fWords->SetBinContent(i + 1, k + 1, static_cast<double>(c.words[k]));
    const double live = static_cast<double>(c.frames) * format::kHeartbeatFrameSeconds;
    const double data = static_cast<double>(c.words[kKindLeading] + c.words[kKindTrailing] + c.words[kKindOther]);
    fDataRate->SetBinContent(i + 1, live > 0 ? data / live : 0);
  }
}

void DaqHealthModule::Report(monitor::WarningList& out) const {
  using monitor::Check;
  using monitor::Severity;
  auto add = [&](Severity s, const std::string& text) { out.push_back({Check::kDaq, s, Name(), text}); };
  if (fTotals.timeFrames == 0) return;
  for (auto id : fOptions.expectedFems) {
    const int i = fFems.Find(id);
    if (i < 0 || i >= static_cast<int>(fCounters.size()) || fCounters[i].frames == 0) {
      add(Severity::kError, "expected module " + FemLabel(id) + " sends no data");
    }
  }
  for (int i = 0; i < fFems.Size() && i < static_cast<int>(fCounters.size()); ++i) {
    const auto& c = fCounters[i];
    const std::string fem = FemLabel(fFems.IdAt(i));
    if (c.frames > 0 && fLastSequence > c.lastSequence + fOptions.absentFrames) {
      add(Severity::kError, fem + " absent for the last " + std::to_string(fLastSequence - c.lastSequence) +
                                " time frames");
    }
    if (c.missingFrames > 0) add(Severity::kWarning, fem + ": " + std::to_string(c.missingFrames) + " missing heartbeat frames");
    if (c.repeatedFrames > 0) add(Severity::kWarning, fem + ": " + std::to_string(c.repeatedFrames) + " repeated heartbeat frames");
    if (c.flaggedFrames > 0) {
      std::ostringstream os;
      os << fem << ": " << c.flaggedFrames << " delimiters with flags (bits seen 0x" << std::hex << c.flagBitsSeen
         << ")";
      add(Severity::kWarning, os.str());
    }
    const std::uint64_t throttling = c.words[kKindThrottlingT1Start] + c.words[kKindThrottlingT2Start];
    if (throttling > 0 || c.throttledStfs > 0) {
      add(Severity::kWarning, fem + ": throttling (" + std::to_string(throttling) + " start words, " +
                                  std::to_string(c.throttledStfs) + " flagged sub time frames)");
    }
  }
  if (fTotals.incomplete > 0) add(Severity::kWarning, std::to_string(fTotals.incomplete) + " incomplete time frames");
  if (fTotals.sourceCountMismatch > 0) {
    add(Severity::kWarning, std::to_string(fTotals.sourceCountMismatch) + " time frames with fewer sub time frames than numSource");
  }
  if (fTotals.frameIdMismatch > 0) {
    add(Severity::kError, std::to_string(fTotals.frameIdMismatch) + " time frames whose modules disagree on the frame id");
  }
}

}  // namespace nda::modules
