// Runs the monitor modules on time frames and keeps the analyzer's own counters
// (received, processed, skipped by sampling, malformed) and the summary of warnings.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "monitor/Module.hh"
#include "monitor/Warning.hh"

class TH1D;
class TNamed;
class TDirectory;

namespace nda::monitor {

class HistRegistry;

class Engine {
 public:
  struct Options {
    std::uint32_t sampleEvery = 1;  // process one time frame out of this many
  };

  Engine(HistRegistry& registry, Options options);
  ~Engine();

  void Add(std::unique_ptr<Module> module);
  void Book();
  void BeginRun(const RunInfo& run);
  // Returns false if the frame was malformed (it is then counted and skipped).
  bool Process(const std::uint8_t* data, std::size_t size);
  // Marks that time frames were lost before the next one (for example dropped by the transport).
  void NotifyGap() { fGap = true; }
  void Update();
  void EndRun();

  const WarningList& Warnings() const { return fWarnings; }
  std::string WarningText() const;

  std::uint64_t Received() const { return fReceived; }
  std::uint64_t Processed() const { return fProcessed; }
  std::uint64_t Malformed() const { return fMalformed; }
  const std::string& LastError() const { return fLastError; }

 private:
  HistRegistry& fRegistry;
  Options fOptions;
  std::vector<std::unique_ptr<Module>> fModules;
  std::uint64_t fReceived = 0;
  std::uint64_t fProcessed = 0;
  std::uint64_t fMalformed = 0;
  bool fGap = false;
  std::string fLastError;
  WarningList fWarnings;
  TH1D* fCounters = nullptr;
  TH1D* fStatus = nullptr;
  std::unique_ptr<TNamed> fWarningText;  // name "warnings", the text is the title
};

}  // namespace nda::monitor
