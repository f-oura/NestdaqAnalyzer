#include "e88/Params.hh"

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>

#include "modules/FemTable.hh"

namespace nda::e88 {

namespace {

struct Line {
  int number = 0;
  std::vector<std::string> tokens;
  bool directive = false;  // "#!" line
};

// Reads non-empty lines split into tokens; comments are removed (a '#' inside a quoted string is kept).
bool ReadLines(const std::string& path, std::vector<Line>& lines, std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  std::string text;
  int number = 0;
  while (std::getline(in, text)) {
    ++number;
    Line line;
    line.number = number;
    std::size_t start = 0;
    if (text.rfind("#!", 0) == 0) {
      line.directive = true;
      start = 2;
    }
    std::string token;
    bool quoted = false;
    for (std::size_t i = start; i < text.size(); ++i) {
      const char c = text[i];
      if (c == '"') {
        quoted = !quoted;
        continue;
      }
      if (!quoted && c == '#') break;
      if (!quoted && (c == ' ' || c == '\t' || c == '\r')) {
        if (!token.empty()) line.tokens.push_back(token);
        token.clear();
        continue;
      }
      token += c;
    }
    if (!token.empty()) line.tokens.push_back(token);
    if (!line.tokens.empty()) lines.push_back(std::move(line));
  }
  return true;
}

std::string Where(const std::string& path, const Line& line) { return path + ":" + std::to_string(line.number) + ": "; }

bool ToInt(const std::string& s, long& v) {
  char* end = nullptr;
  const int base = (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) ? 16 : 10;
  v = std::strtol(s.c_str(), &end, base);
  return end != s.c_str() && *end == '\0';
}

bool CheckFormat(const std::string& path, const std::vector<Line>& lines, const std::string& format,
                 std::string& error) {
  for (const auto& l : lines) {
    if (l.directive && l.tokens.size() >= 3 && l.tokens[0] == "format") {
      if (l.tokens[1] == format && l.tokens[2] == "v1") return true;
      error = Where(path, l) + "expected format " + format + " v1";
      return false;
    }
  }
  error = path + ": missing '#! format " + format + " v1'";
  return false;
}

}  // namespace

std::string Plane::Label() const {
  if (module == 0 && layer == 0) return detector;
  return detector + "_m" + std::to_string(module) + "_l" + std::to_string(layer);
}

int DetectorDef::Find(const std::string& detector, int module, int layer) const {
  for (std::size_t i = 0; i < planes.size(); ++i) {
    if (planes[i].detector == detector && planes[i].module == module && planes[i].layer == layer) return static_cast<int>(i);
  }
  return -1;
}

bool ReadDetectorDef(const std::string& path, DetectorDef& out, std::string& error) {
  out = DetectorDef{};
  std::vector<Line> lines;
  if (!ReadLines(path, lines, error) || !CheckFormat(path, lines, "detector_def", error)) return false;
  for (const auto& l : lines) {
    if (l.directive) continue;
    if (l.tokens.size() < 6) {
      error = Where(path, l) + "expected: detector module layer n_segment n_side edges";
      return false;
    }
    Plane p;
    long module, layer, nseg, nside;
    if (!ToInt(l.tokens[1], module) || !ToInt(l.tokens[2], layer) || !ToInt(l.tokens[3], nseg) ||
        !ToInt(l.tokens[4], nside) || module < 0 || layer < 0 || nseg <= 0 || nside <= 0) {
      error = Where(path, l) + "bad number";
      return false;
    }
    p.detector = l.tokens[0];
    p.module = static_cast<int>(module);
    p.layer = static_cast<int>(layer);
    p.nSegments = static_cast<int>(nseg);
    p.nSides = static_cast<int>(nside);
    p.edges = l.tokens[5];
    if (out.Find(p.detector, p.module, p.layer) >= 0) {
      error = Where(path, l) + "duplicate plane " + p.Label();
      return false;
    }
    out.planes.push_back(p);
  }
  if (out.planes.empty()) {
    error = path + ": no detector";
    return false;
  }
  return true;
}

bool ReadChannelMap(const std::string& path, const DetectorDef& def, ChannelMap& out, std::string& error) {
  out = ChannelMap{};
  std::vector<Line> lines;
  if (!ReadLines(path, lines, error) || !CheckFormat(path, lines, "channel_map", error)) return false;
  std::set<std::tuple<int, int, int>> keys;
  std::set<std::string> names;
  std::set<std::uint32_t> fems;
  bool haveRuns = false;
  for (const auto& l : lines) {
    if (l.directive) {
      if (l.tokens.size() >= 3 && l.tokens[0] == "valid_runs") {
        long a, b;
        if (!ToInt(l.tokens[1], a) || !ToInt(l.tokens[2], b) || a < 0 || b < a) {
          error = Where(path, l) + "bad valid_runs";
          return false;
        }
        out.firstRun = static_cast<std::uint64_t>(a);
        out.lastRun = static_cast<std::uint64_t>(b);
        haveRuns = true;
      }
      continue;
    }
    if (l.tokens.size() < 8) {
      error = Where(path, l) + "expected: fem_ip ch detector module layer segment side name";
      return false;
    }
    MappedChannel c;
    long ch, module, layer, seg, side;
    if (!modules::ParseFemId(l.tokens[0], c.femId)) {
      error = Where(path, l) + "bad module address " + l.tokens[0];
      return false;
    }
    if (!ToInt(l.tokens[1], ch) || !ToInt(l.tokens[3], module) || !ToInt(l.tokens[4], layer) ||
        !ToInt(l.tokens[5], seg) || !ToInt(l.tokens[6], side) || ch < 0 || ch > 255) {
      error = Where(path, l) + "bad number";
      return false;
    }
    c.channel = static_cast<int>(ch);
    c.plane = def.Find(l.tokens[2], static_cast<int>(module), static_cast<int>(layer));
    if (c.plane < 0) {
      error = Where(path, l) + "detector " + l.tokens[2] + " module " + l.tokens[3] + " layer " + l.tokens[4] +
              " is not in the detector definition";
      return false;
    }
    const auto& p = def.planes[c.plane];
    if (seg < 0 || seg >= p.nSegments || side < 0 || side >= p.nSides) {
      error = Where(path, l) + "segment or side out of range for " + p.Label();
      return false;
    }
    c.segment = static_cast<int>(seg);
    c.side = static_cast<int>(side);
    c.name = l.tokens[7];
    const auto address = ChannelMap::Address(c.femId, c.channel);
    if (out.byAddress.count(address)) {
      error = Where(path, l) + "channel " + l.tokens[0] + " " + l.tokens[1] + " appears twice";
      return false;
    }
    if (!keys.emplace(c.plane, c.segment, c.side).second) {
      error = Where(path, l) + "logical channel of " + c.name + " appears twice";
      return false;
    }
    if (!names.insert(c.name).second) {
      error = Where(path, l) + "name " + c.name + " appears twice";
      return false;
    }
    out.byAddress.emplace(address, static_cast<int>(out.channels.size()));
    out.channels.push_back(c);
    if (fems.insert(c.femId).second) out.fems.push_back(c.femId);
  }
  if (!haveRuns) {
    error = path + ": missing '#! valid_runs first last'";
    return false;
  }
  for (std::size_t pi = 0; pi < def.planes.size(); ++pi) {
    const auto& p = def.planes[pi];
    int missing = 0;
    for (int s = 0; s < p.nSegments; ++s) {
      for (int side = 0; side < p.nSides; ++side) missing += keys.count({static_cast<int>(pi), s, side}) ? 0 : 1;
    }
    if (missing > 0) out.warnings.push_back(p.Label() + ": " + std::to_string(missing) + " (segment, side) not in the channel map");
  }
  return true;
}

const std::vector<std::string>* Settings::Get(const std::string& key) const {
  auto it = fValues.find(key);
  return it == fValues.end() ? nullptr : &it->second;
}

double Settings::Number(const std::string& key, std::size_t index, double fallback) const {
  const auto* v = Get(key);
  if (!v || index >= v->size()) return fallback;
  char* end = nullptr;
  const double d = std::strtod((*v)[index].c_str(), &end);
  return (end != (*v)[index].c_str() && *end == '\0') ? d : fallback;
}

long Settings::Integer(const std::string& key, std::size_t index, long fallback) const {
  const auto* v = Get(key);
  long value = 0;
  if (!v || index >= v->size() || !ToInt((*v)[index], value)) return fallback;
  return value;
}

bool ReadSettings(const std::string& path, Settings& out, std::string& error) {
  out = Settings{};
  std::vector<Line> lines;
  if (!ReadLines(path, lines, error) || !CheckFormat(path, lines, "settings", error)) return false;
  for (const auto& l : lines) {
    if (l.directive) continue;
    if (out.Has(l.tokens[0])) {
      error = Where(path, l) + "key " + l.tokens[0] + " appears twice";
      return false;
    }
    out.Set(l.tokens[0], std::vector<std::string>(l.tokens.begin() + 1, l.tokens.end()));
  }
  return true;
}

bool ReadParamIndex(const std::string& path, std::uint64_t run, ParamFiles& out, std::string& error) {
  out = ParamFiles{};
  std::vector<Line> lines;
  if (!ReadLines(path, lines, error) || !CheckFormat(path, lines, "param_index", error)) return false;
  const auto slash = path.rfind('/');
  const std::string dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);
  for (const auto& l : lines) {
    if (l.directive) continue;
    long first, last;
    if (l.tokens.size() < 5 || !ToInt(l.tokens[1], first) || !ToInt(l.tokens[2], last)) {
      error = Where(path, l) + "expected: kind first last required file";
      return false;
    }
    if (run < static_cast<std::uint64_t>(first) || run > static_cast<std::uint64_t>(last)) continue;
    const std::string file = l.tokens[4].rfind('/', 0) == 0 ? l.tokens[4] : dir + l.tokens[4];
    std::string* slot = nullptr;
    if (l.tokens[0] == "detector_def") slot = &out.detectorDef;
    if (l.tokens[0] == "channel_map") slot = &out.channelMap;
    if (l.tokens[0] == "settings") slot = &out.settings;
    if (!slot) continue;  // other kinds (calibration ...) are not used online
    if (!slot->empty()) {
      error = Where(path, l) + "two " + l.tokens[0] + " files for run " + std::to_string(run);
      return false;
    }
    *slot = file;
  }
  if (out.detectorDef.empty() || out.channelMap.empty()) {
    error = path + ": no detector_def or channel_map for run " + std::to_string(run);
    return false;
  }
  return true;
}

}  // namespace nda::e88
