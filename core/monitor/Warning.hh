// Warnings produced by monitor modules. Display and (later) notification use the same structure.
#pragma once

#include <string>
#include <vector>

namespace nda::monitor {

// What a shift member wants to judge from the monitor.
enum class Check {
  kDaq,         // data are taken without losses (modules present, frames continuous, delimiter flags, throttling)
  kChannels,    // every channel is alive (dead, hot)
  kDetectors,   // detectors respond as expected (experiment specific)
  kTrigger,     // trigger and external links work (experiment specific)
  kAnalyzer,    // the online analysis keeps up
};
constexpr int kNumChecks = 5;
const char* CheckName(Check c);

enum class Severity { kOk = 0, kWarning = 1, kError = 2 };
const char* SeverityName(Severity s);

struct Warning {
  Check check = Check::kDaq;
  Severity severity = Severity::kWarning;
  std::string source;  // module name
  std::string text;
};

using WarningList = std::vector<Warning>;

}  // namespace nda::monitor
