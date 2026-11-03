// Unit tests of the E88 parameters, the B2TT record decoding and the E88 monitor module.
#include <TROOT.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include "Builder.hh"
#include "e88/B2ttRecord.hh"
#include "e88/E88Module.hh"
#include "e88/Params.hh"
#include "e88/Setup.hh"
#include "modules/DaqHealthModule.hh"
#include "monitor/Engine.hh"
#include "monitor/HistRegistry.hh"

using namespace nda;

namespace {

constexpr std::uint32_t kFem = 0xc0a80a10;       // 192.168.10.16: detectors
constexpr std::uint32_t kReceiver = 0xc0a80a0a;  // 192.168.10.10: B2TT records
constexpr double kLsb = 1.0 / 1024;              // ns

std::string gDir;

std::string WriteFile(const std::string& name, const std::string& text) {
  const std::string path = gDir + "/" + name;
  std::ofstream(path) << text;
  return path;
}

const char* kDetectors =
    "#! format detector_def v1\n"
    "# detector module layer n_segment n_side edges description\n"
    "SC    0 0 2 2 LT  test counters\n"
    "MRPC  0 0 1 2 LT  test strip\n"
    "AC    0 0 9 1 LT  3 x 3\n"
    "TRIG  0 0 1 1 LT  T1\n";

const char* kChannels =
    "#! format channel_map v1\n"
    "#! valid_runs 0 100\n"
    "#! description \"test # map\"\n"
    "192.168.10.16  0 SC   0 0 0 0 SC-0-U\n"
    "192.168.10.16  1 SC   0 0 0 1 SC-0-D\n"
    "192.168.10.16  2 SC   0 0 1 0 SC-1-U\n"
    "192.168.10.16  3 SC   0 0 1 1 SC-1-D\n"
    "192.168.10.16  4 MRPC 0 0 0 0 MRPC-0-0\n"
    "192.168.10.16  5 MRPC 0 0 0 1 MRPC-0-1\n"
    "192.168.10.16  6 TRIG 0 0 0 0 T1\n"
    "192.168.10.16  8 AC   0 0 4 0 AC-4   # only one AC segment is mapped\n";

const char* kSettings =
    "#! format settings v1\n"
    "tdc_lsb_ns        0.0009765625\n"
    "b2tt_data_type    0x2b\n"
    "sc_pair_window    50\n"
    "mrpc_pair_window  50\n"
    "t1_search         0 400\n"
    "b2tt_search       -100 2000\n";

std::uint64_t Hit(int ch, double tNs, double totNs = 20) {
  return ndatest::Leading(ch, static_cast<std::uint32_t>(std::lround(totNs / kLsb)),
                          static_cast<std::uint32_t>(std::lround(tNs / kLsb)));
}

std::vector<std::uint64_t> TriggerRecord(std::uint32_t seq, std::uint16_t spill, std::uint32_t event,
                                         std::uint32_t frame, std::uint16_t count) {
  const std::uint64_t h = (std::uint64_t{0x2b} << 58) | (std::uint64_t{2} << 54) | (std::uint64_t{1} << 50) |
                          (std::uint64_t{5} << 44) | (std::uint64_t{seq} << 12);
  const std::uint64_t w1 = (std::uint64_t{0xABCD} << 32) | (std::uint64_t{1} << 24) | (std::uint64_t{1} << 22) |
                           (std::uint64_t{1} << 21) | (std::uint64_t{1} << 20) | (std::uint64_t{1} << 19);
  const std::uint64_t w2 = (std::uint64_t{spill} << 48) | (std::uint64_t{event} << 16);
  const std::uint64_t w3 = (std::uint64_t{0x123456789} << 16) | count;
  const std::uint64_t w4 = (std::uint64_t{frame & 0xFFFFFF} << 40) | (std::uint64_t{3} << 32) | seq;
  return {h, w1, w2, w3, w4};
}

ndatest::Stf Frame(std::uint32_t fem, std::uint32_t femType, std::uint32_t frame, std::vector<std::uint64_t> data) {
  ndatest::Stf s;
  s.femId = fem;
  s.femType = femType;
  const auto n = static_cast<std::uint32_t>(data.size());
  data.push_back(ndatest::Delimiter1(frame));
  data.push_back(ndatest::Delimiter2(8 * n, 8 * n));
  s.frames.push_back(data);
  return s;
}

void TestParams() {
  e88::DetectorDef def;
  std::string error;
  CHECK(e88::ReadDetectorDef(WriteFile("det.txt", kDetectors), def, error));
  CHECK_EQ(def.planes.size(), std::size_t{4});
  CHECK_EQ(def.Find("MRPC", 0, 0), 1);
  e88::ChannelMap map;
  CHECK(e88::ReadChannelMap(WriteFile("map.txt", kChannels), def, map, error));
  CHECK_EQ(map.channels.size(), std::size_t{8});
  CHECK_EQ(map.lastRun, std::uint64_t{100});
  const auto* t1 = map.Find(kFem, 6);
  CHECK(t1 && t1->name == "T1");
  CHECK(map.Find(kFem, 7) == nullptr);
  CHECK_EQ(map.warnings.size(), std::size_t{1});  // AC: 8 segments missing
  CHECK(map.warnings.size() == 1 && map.warnings[0].find("AC: 8") != std::string::npos);

  std::string bad = std::string(kChannels) + "192.168.10.16  1 SC 0 0 1 1 SC-X\n";
  CHECK(!e88::ReadChannelMap(WriteFile("bad1.txt", bad), def, map, error));
  CHECK(error.find("appears twice") != std::string::npos);
  bad = std::string(kChannels) + "192.168.10.16  9 SC 0 0 2 0 SC-2-U\n";
  CHECK(!e88::ReadChannelMap(WriteFile("bad2.txt", bad), def, map, error));
  CHECK(error.find("out of range") != std::string::npos);
  bad = std::string(kChannels) + "192.168.10.16  9 HODO 0 0 0 0 H0\n";
  CHECK(!e88::ReadChannelMap(WriteFile("bad3.txt", bad), def, map, error));

  e88::Settings settings;
  CHECK(e88::ReadSettings(WriteFile("settings.txt", kSettings), settings, error));
  CHECK_EQ(settings.Integer("b2tt_data_type", 0, -1), 0x2bL);
  CHECK_EQ(settings.Number("b2tt_search", 1, 0), 2000.0);

  WriteFile("index.txt",
            "#! format param_index v1\n"
            "detector_def 0 999 yes det.txt\n"
            "channel_map  0 100 yes map.txt\n"
            "channel_map  101 999 yes other-map.txt\n"
            "settings     0 999 yes settings.txt\n");
  e88::ParamFiles files;
  CHECK(e88::ReadParamIndex(gDir + "/index.txt", 50, files, error));
  CHECK_EQ(files.channelMap, gDir + "/map.txt");
  CHECK(e88::ReadParamIndex(gDir + "/index.txt", 200, files, error));
  CHECK_EQ(files.channelMap, gDir + "/other-map.txt");

  e88::ParamOptions options;
  options.index = gDir + "/index.txt";
  e88::E88Config config;
  std::vector<std::string> used;
  CHECK(e88::LoadConfig(options, 50, config, used, error));
  CHECK_EQ(config.b2ttDataType, 0x2b);
  CHECK_EQ(config.b2ttSearchNs[0], -100.0);
  CHECK(!e88::LoadConfig(options, 200, config, used, error));  // other-map.txt does not exist
}

void TestRecordDecoding() {
  const auto w = TriggerRecord(7, 3, 1234, 0x700001, 182);
  e88::B2ttRecord r;
  CHECK(e88::DecodeB2ttRecord(w.data(), w.size(), r));
  CHECK_EQ(r.formatVersion, 2);
  CHECK_EQ(r.recordType, int(e88::B2ttRecord::kTrigger));
  CHECK_EQ(r.localSequence, 7u);
  CHECK_EQ(r.triggerTag, 0xABCDu);
  CHECK(r.hasId && r.hasTime && r.timeValid);
  CHECK_EQ(r.spillId, 3);
  CHECK_EQ(r.eventId, 1234u);
  CHECK_EQ(r.timestamp, std::uint64_t{0x123456789});
  CHECK_EQ(r.heartbeatCount, 182);
  CHECK_EQ(r.frameNumber, 0x700001u);
  CHECK_EQ(int(r.resyncCount), 3);
  CHECK_EQ(r.streamSequence, 7u);
  CHECK(!e88::DecodeB2ttRecord(w.data(), 3, r));  // too short
  auto v3 = w;
  v3[0] = (v3[0] & ~(std::uint64_t{0xF} << 54)) | (std::uint64_t{3} << 54);
  CHECK(!e88::DecodeB2ttRecord(v3.data(), v3.size(), r));  // unknown version
}

void TestModule() {
  e88::ParamOptions options;
  options.detectorDef = gDir + "/det.txt";
  options.channelMap = gDir + "/map.txt";
  options.settings = gDir + "/settings.txt";
  e88::E88Config config;
  std::vector<std::string> used;
  std::string error;
  CHECK(e88::LoadConfig(options, 1, config, used, error));

  monitor::HistRegistry registry;
  monitor::Engine engine(registry, monitor::Engine::Options{1, {0x2b}});
  auto daq = std::make_unique<modules::DaqHealthModule>(modules::DaqHealthModule::Options{});
  auto mod = std::make_unique<e88::E88Module>(std::move(config));
  auto* daqPtr = daq.get();
  auto* e = mod.get();
  engine.Add(std::move(daq));
  engine.Add(std::move(mod));
  engine.Book();
  engine.BeginRun(monitor::RunInfo{1});

  // Frame numbers from 0x700000: the upper bits of record word 4 look like a delimiter (0x1C).
  const std::uint32_t f0 = 0x700000;
  // SC segment 0: up at 1000 ns, down at 1010 ns -> mean time 1005 ns. T1 at 1155 ns (T1 - SC = 150 ns).
  // The B2TT record time is 182 x 8 ns = 1456 ns (record - T1 = 301 ns). Channel 60 is not in the map.
  auto tf0 = ndatest::BuildTimeFrame(
      f0, {Frame(kFem, ::SubTimeFrame::TDC64H, f0, {Hit(0, 1000), Hit(1, 1010), Hit(6, 1155), Hit(4, 1006), Hit(60, 50)}),
           Frame(kReceiver, ::SubTimeFrame::TDC64L_V2, f0, TriggerRecord(1, 5, 100, f0, 182))});
  // Next frame: a record with the same event id (repeated), no T1.
  auto tf1 = ndatest::BuildTimeFrame(f0 + 1, {Frame(kFem, ::SubTimeFrame::TDC64H, f0 + 1, {}),
                                               Frame(kReceiver, ::SubTimeFrame::TDC64L_V2, f0 + 1,
                                                     TriggerRecord(2, 5, 100, f0 + 1, 10))});
  for (const auto* tf : {&tf0, &tf1}) {
    CHECK(engine.Process(reinterpret_cast<const std::uint8_t*>(tf->data()), ndatest::Bytes(*tf)));
  }
  engine.EndRun();

  const auto& c = e->GetCounts();
  CHECK_EQ(c.t1, std::uint64_t{1});
  CHECK_EQ(c.b2ttTrigger, std::uint64_t{2});
  CHECK_EQ(c.b2ttUndecodable, std::uint64_t{0});
  CHECK_EQ(c.repeatedEventIds, std::uint64_t{1});
  CHECK_EQ(c.unmappedHits, std::uint64_t{1});
  CHECK_EQ(c.t1WithRecord, std::uint64_t{1});
  CHECK_EQ(c.recordsWithT1, std::uint64_t{1});
  CHECK_EQ(e->Pairs(0), std::uint64_t{1});  // SC
  CHECK_EQ(e->Pairs(1), std::uint64_t{0});  // MRPC: only one end
  const std::string json = e->JsonSummary();
  CHECK(json.find("\"t1_minus_sc_mean\": 150") != std::string::npos);
  CHECK(json.find("\"b2tt_minus_t1_mean\": 301") != std::string::npos);

  // The record words are not taken as delimiters or hits.
  const int rx = daqPtr->Fems().Find(kReceiver);
  CHECK(rx >= 0);
  CHECK_EQ(daqPtr->Counters(rx).frames, std::uint64_t{2});
  CHECK_EQ(daqPtr->Counters(rx).records, std::uint64_t{2});
  CHECK_EQ(daqPtr->Counters(rx).missingFrames, std::uint64_t{0});
  CHECK_EQ(daqPtr->Counters(rx).words[modules::DaqHealthModule::kKindLeading], std::uint64_t{0});

  bool unmappedWarning = false, repeatWarning = false;
  for (const auto& w : engine.Warnings()) {
    unmappedWarning |= w.text.find("1 hits on channels not in the channel map: 192.168.10.16/60(1)") != std::string::npos;
    repeatWarning |= w.text.find("repeated 1 times") != std::string::npos;
  }
  CHECK(unmappedWarning);
  CHECK(repeatWarning);
}

}  // namespace

int main() {
  gROOT->SetBatch(true);
  char dir[] = "/tmp/nda-e88-XXXXXX";
  if (!mkdtemp(dir)) return 1;
  gDir = dir;
  TestParams();
  TestRecordDecoding();
  TestModule();
  std::system(("rm -rf " + gDir).c_str());
  if (gFailures) {
    std::cerr << gFailures << " check(s) failed\n";
    return 1;
  }
  std::cout << "test_e88: all checks passed\n";
  return 0;
}
