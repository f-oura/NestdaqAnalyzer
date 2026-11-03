// Loads the E88 parameters given on the command line and builds the configuration of E88Module.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "e88/E88Module.hh"

namespace nda::e88 {

struct ParamOptions {
  std::string index;        // param_index v1; selects the files by run number
  std::string detectorDef;  // overrides the index
  std::string channelMap;   // overrides the index
  std::string settings;     // overrides the index (optional)
  bool Given() const { return !index.empty() || !detectorDef.empty() || !channelMap.empty() || !settings.empty(); }
};

// Returns false with `error` set when a file is missing or malformed. `used` lists the files read.
bool LoadConfig(const ParamOptions& options, std::uint64_t run, E88Config& config, std::vector<std::string>& used,
                std::string& error);

}  // namespace nda::e88
