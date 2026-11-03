#include "e88/Setup.hh"

namespace nda::e88 {

bool LoadConfig(const ParamOptions& options, std::uint64_t run, E88Config& config, std::vector<std::string>& used,
                std::string& error) {
  ParamFiles files;
  if (!options.index.empty()) {
    if (!ReadParamIndex(options.index, run, files, error)) return false;
    used.push_back(options.index);
  }
  if (!options.detectorDef.empty()) files.detectorDef = options.detectorDef;
  if (!options.channelMap.empty()) files.channelMap = options.channelMap;
  if (!options.settings.empty()) files.settings = options.settings;
  if (files.detectorDef.empty() || files.channelMap.empty()) {
    error = "the E88 monitor needs a detector definition and a channel map (or a parameter index)";
    return false;
  }
  config = E88Config{};
  if (!ReadDetectorDef(files.detectorDef, config.detectors, error)) return false;
  used.push_back(files.detectorDef);
  if (!ReadChannelMap(files.channelMap, config.detectors, config.channels, error)) return false;
  used.push_back(files.channelMap);
  if (run < config.channels.firstRun || run > config.channels.lastRun) {
    error = files.channelMap + ": run " + std::to_string(run) + " is outside valid_runs";
    return false;
  }
  if (!files.settings.empty()) {
    if (!ReadSettings(files.settings, config.settings, error)) return false;
    used.push_back(files.settings);
  }
  config.ApplySettings();
  return true;
}

}  // namespace nda::e88
