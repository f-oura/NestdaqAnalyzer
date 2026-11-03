#include "modules/ChannelModule.hh"

#include <TH1D.h>
#include <TH2D.h>

#include <algorithm>
#include <sstream>

#include "monitor/HistRegistry.hh"

namespace nda::modules {

namespace {
enum KindBin { kBinLeading = 1, kBinTrailing, kBinLeadingWithoutTot };

std::string ChannelList(const std::vector<std::uint32_t>& channels) {
  std::ostringstream os;
  const std::size_t shown = std::min<std::size_t>(channels.size(), 16);
  for (std::size_t i = 0; i < shown; ++i) os << (i ? " " : "") << channels[i];
  if (shown < channels.size()) os << " ...";
  return os.str();
}
}  // namespace

ChannelModule::ChannelModule(Options options) : fOptions(options), fFems(options.maxFems) {}

void ChannelModule::Book(monitor::HistRegistry& registry) { fRegistry = &registry; }

void ChannelModule::BeginRun(const monitor::RunInfo&) {
  for (auto& d : fData) {
    std::fill(d->leading.begin(), d->leading.end(), 0);
    std::fill(d->trailing.begin(), d->trailing.end(), 0);
    std::fill(d->leadingAtLastUpdate.begin(), d->leadingAtLastUpdate.end(), 0);
    d->leadingWithoutTot = d->frames = d->framesAtLastUpdate = 0;
    d->silentChannels.clear();
    d->hotChannels.clear();
  }
}

ChannelModule::FemData& ChannelModule::Get(std::uint32_t femId, std::uint32_t femType) {
  bool isNew = false;
  const int index = fFems.Index(femId, isNew);
  if (!isNew) return *fData[index];
  auto d = std::make_unique<FemData>();
  d->femId = femId;
  d->kind = format::KindOfFemType(femType);
  d->nChannels = format::NumChannelBins(d->kind);
  d->leading.assign(d->nChannels, 0);
  d->trailing.assign(d->nChannels, 0);
  d->leadingAtLastUpdate.assign(d->nChannels, 0);
  const std::string label = FemLabel(femId);
  const std::string folder = "Channels/" + label;
  const int n = d->nChannels;
  const double totMax = d->kind == format::TdcKind::kLowResolution ? fOptions.totMaxNsLow : fOptions.totMaxNsHigh;
  const double frameUs = format::kHeartbeatFrameSeconds * 1e6;
  d->hitPattern = fRegistry->Book1D(folder, "hit_pattern", (label + " leading edges;channel;hits").c_str(), n, 0, n);
  d->rate = fRegistry->Book1D(folder, "rate", (label + " leading edge rate;channel;rate [Hz]").c_str(), n, 0, n);
  d->tot = fRegistry->Book2D(folder, "tot", (label + " ToT;channel;ToT [ns]").c_str(), n, 0, n, fOptions.totBins, 0,
                             totMax);
  d->time = fRegistry->Book2D(folder, "time_in_frame",
                              (label + " leading edge time in the heartbeat frame;channel;time [#mus]").c_str(), n, 0,
                              n, fOptions.timeBins, 0, frameUs);
  d->kinds = fRegistry->Book1D(folder, "edge_kinds", (label + " TDC words;;words").c_str(), 3, 0.5, 3.5);
  d->kinds->GetXaxis()->SetBinLabel(kBinLeading, "leading");
  d->kinds->GetXaxis()->SetBinLabel(kBinTrailing, "trailing");
  d->kinds->GetXaxis()->SetBinLabel(kBinLeadingWithoutTot, "leading, ToT = 0");
  fData.push_back(std::move(d));
  return *fData.back();
}

void ChannelModule::Process(const format::TimeFrameView& tf, const monitor::FrameContext&) {
  for (const auto& stf : tf.subFrames) {
    if (fFems.Find(stf.header->femId) < 0 && fFems.Size() >= fFems.Capacity()) continue;
    auto& d = Get(stf.header->femId, stf.header->femType);
    if (d.kind == format::TdcKind::kUnknown) continue;
    const double unit = format::TdcUnitNs(d.kind);
    for (const auto& hbf : stf.frames) {
      for (std::size_t i = 0; i < hbf.nWords; ++i) {
        const std::uint64_t w = hbf.words[i];
        const std::uint8_t head = format::HeadOf(w);
        if (head == format::kHeartbeat1) {
          ++d.frames;
          continue;
        }
        if (head != format::kLeading && head != format::kTrailing) continue;
        const auto hit = format::DecodeTdc(w, d.kind);
        if (static_cast<int>(hit.channel) >= d.nChannels) continue;
        if (!hit.leading) {
          ++d.trailing[hit.channel];
          d.kinds->Fill(kBinTrailing);
          continue;
        }
        ++d.leading[hit.channel];
        d.kinds->Fill(kBinLeading);
        d.time->Fill(hit.channel, hit.tdc * unit * 1e-3);
        if (hit.tot == 0) {
          ++d.leadingWithoutTot;
          d.kinds->Fill(kBinLeadingWithoutTot);
        } else {
          d.tot->Fill(hit.channel, hit.tot * unit);
        }
      }
    }
  }
}

void ChannelModule::Update() {
  for (auto& dp : fData) {
    auto& d = *dp;
    const double live = static_cast<double>(d.frames) * format::kHeartbeatFrameSeconds;
    std::vector<double> active;
    for (int ch = 0; ch < d.nChannels; ++ch) {
      d.hitPattern->SetBinContent(ch + 1, static_cast<double>(d.leading[ch]));
      const double r = live > 0 ? d.leading[ch] / live : 0;
      d.rate->SetBinContent(ch + 1, r);
      if (r > 0) active.push_back(r);
    }
    d.hotChannels.clear();
    if (active.size() >= 3) {
      std::nth_element(active.begin(), active.begin() + active.size() / 2, active.end());
      const double median = active[active.size() / 2];
      for (int ch = 0; ch < d.nChannels; ++ch) {
        if (live > 0 && d.leading[ch] / live > fOptions.hotFactor * median) d.hotChannels.push_back(ch);
      }
    }
    const double interval = static_cast<double>(d.frames - d.framesAtLastUpdate) * format::kHeartbeatFrameSeconds;
    if (interval >= fOptions.silentMinSeconds) {
      d.silentChannels.clear();
      for (int ch = 0; ch < d.nChannels; ++ch) {
        const double r = live > 0 ? d.leading[ch] / live : 0;
        if (r >= fOptions.silentMinRateHz && r * interval >= 5 && d.leading[ch] == d.leadingAtLastUpdate[ch]) {
          d.silentChannels.push_back(ch);
        }
      }
      d.leadingAtLastUpdate = d.leading;
      d.framesAtLastUpdate = d.frames;
    }
  }
}

void ChannelModule::Report(monitor::WarningList& out) const {
  for (const auto& dp : fData) {
    const auto& d = *dp;
    const std::string fem = FemLabel(d.femId);
    if (!d.hotChannels.empty()) {
      out.push_back({monitor::Check::kChannels, monitor::Severity::kWarning, Name(),
                     fem + ": hot channels " + ChannelList(d.hotChannels)});
    }
    if (!d.silentChannels.empty()) {
      out.push_back({monitor::Check::kChannels, monitor::Severity::kError, Name(),
                     fem + ": channels went silent " + ChannelList(d.silentChannels)});
    }
  }
}

}  // namespace nda::modules
