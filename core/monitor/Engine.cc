#include "monitor/Engine.hh"

#include <TH1D.h>
#include <TNamed.h>

#include <algorithm>
#include <sstream>

#include "format/Frames.hh"
#include "monitor/HistRegistry.hh"

namespace nda::monitor {

namespace {
enum CounterBin { kBinReceived = 1, kBinProcessed, kBinSkipped, kBinMalformed };
}

Engine::Engine(HistRegistry& registry, Options options) : fRegistry(registry), fOptions(options) {
  if (fOptions.sampleEvery == 0) fOptions.sampleEvery = 1;
}

Engine::~Engine() {
  if (fWarningText) fRegistry.Unpublish(fWarningText.get());
}

void Engine::Add(std::unique_ptr<Module> module) { fModules.push_back(std::move(module)); }

void Engine::Book() {
  fCounters = fRegistry.Book1D("Summary", "analyzer_frames", "Time frames seen by the analyzer;;time frames", 4, 0.5,
                               4.5);
  fCounters->GetXaxis()->SetBinLabel(kBinReceived, "received");
  fCounters->GetXaxis()->SetBinLabel(kBinProcessed, "processed");
  fCounters->GetXaxis()->SetBinLabel(kBinSkipped, "skipped (sampling)");
  fCounters->GetXaxis()->SetBinLabel(kBinMalformed, "malformed");
  fStatus = fRegistry.Book1D("Summary", "status", "Status (0 OK, 1 warning, 2 error);;severity", kNumChecks, 0.5,
                             kNumChecks + 0.5);
  fStatus->SetMaximum(2.5);
  fStatus->SetMinimum(0);
  for (int i = 0; i < kNumChecks; ++i) fStatus->GetXaxis()->SetBinLabel(i + 1, CheckName(static_cast<Check>(i)));
  fWarningText = std::make_unique<TNamed>("warnings", "no warnings");
  fRegistry.Publish("Summary", fWarningText.get());
  for (auto& m : fModules) m->Book(fRegistry);
}

void Engine::BeginRun(const RunInfo& run) {
  fRegistry.ResetAll();
  fReceived = fProcessed = fMalformed = 0;
  fGap = false;
  fWarnings.clear();
  for (auto& m : fModules) m->BeginRun(run);
}

bool Engine::Process(const std::uint8_t* data, std::size_t size) {
  const std::uint64_t sequence = fReceived++;
  fCounters->Fill(kBinReceived);
  if (sequence % fOptions.sampleEvery != 0) {
    fCounters->Fill(kBinSkipped);
    fGap = true;
    return true;
  }
  format::TimeFrameView tf;
  if (!format::ParseTimeFrame(data, size, tf, fLastError)) {
    ++fMalformed;
    fCounters->Fill(kBinMalformed);
    fGap = true;
    return false;
  }
  FrameContext ctx;
  ctx.sequence = sequence;
  ctx.contiguous = !fGap;
  fGap = false;
  for (auto& m : fModules) m->Process(tf, ctx);
  ++fProcessed;
  fCounters->Fill(kBinProcessed);
  return true;
}

void Engine::Update() {
  for (auto& m : fModules) m->Update();
  fWarnings.clear();
  if (fMalformed > 0) {
    fWarnings.push_back(Warning{Check::kAnalyzer, Severity::kError, "Engine",
                                std::to_string(fMalformed) + " malformed time frames, last: " + fLastError});
  }
  for (const auto& m : fModules) m->Report(fWarnings);
  int worst[kNumChecks] = {0, 0, 0, 0, 0};
  for (const auto& w : fWarnings) {
    auto& v = worst[static_cast<int>(w.check)];
    v = std::max(v, static_cast<int>(w.severity));
  }
  for (int i = 0; i < kNumChecks; ++i) fStatus->SetBinContent(i + 1, worst[i]);
  fWarningText->SetTitle(WarningText().c_str());
}

void Engine::EndRun() {
  Update();
  for (auto& m : fModules) m->EndRun();
}

std::string Engine::WarningText() const {
  if (fWarnings.empty()) return "no warnings";
  std::ostringstream os;
  for (const auto& w : fWarnings) {
    os << "[" << SeverityName(w.severity) << "] " << CheckName(w.check) << " / " << w.source << ": " << w.text
       << "\n";
  }
  return os.str();
}

}  // namespace nda::monitor
