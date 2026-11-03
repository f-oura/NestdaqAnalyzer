// Unit tests of the monitor engine and the generic modules.
#include <TROOT.h>

#include <memory>

#include "Builder.hh"
#include "modules/ChannelModule.hh"
#include "modules/DaqHealthModule.hh"
#include "modules/FemTable.hh"
#include "monitor/Engine.hh"
#include "monitor/HistRegistry.hh"

using namespace nda;

namespace {

constexpr std::uint32_t kFemA = 0xc0a80a10;  // 192.168.10.16
constexpr std::uint32_t kFemB = 0xc0a80a0a;  // 192.168.10.10

ndatest::Stf Frame(std::uint32_t fem, std::uint32_t frame, std::vector<std::uint64_t> data, std::uint16_t flags = 0) {
  ndatest::Stf s;
  s.femId = fem;
  const auto n = static_cast<std::uint32_t>(data.size());
  data.push_back(ndatest::Delimiter1(frame, flags));
  data.push_back(ndatest::Delimiter2(8 * n, 8 * n));
  s.frames.push_back(data);
  return s;
}

struct Setup {
  monitor::HistRegistry registry;
  monitor::Engine engine;
  modules::DaqHealthModule* daq = nullptr;
  modules::ChannelModule* channels = nullptr;
  explicit Setup(std::uint32_t sample = 1, std::vector<std::uint32_t> expected = {})
      : engine(registry, monitor::Engine::Options{sample}) {
    modules::DaqHealthModule::Options o;
    o.expectedFems = expected;
    auto d = std::make_unique<modules::DaqHealthModule>(o);
    auto c = std::make_unique<modules::ChannelModule>(modules::ChannelModule::Options{});
    daq = d.get();
    channels = c.get();
    engine.Add(std::move(d));
    engine.Add(std::move(c));
    engine.Book();
    engine.BeginRun(monitor::RunInfo{1});
  }
  void Feed(const std::vector<std::uint64_t>& tf) {
    CHECK(engine.Process(reinterpret_cast<const std::uint8_t*>(tf.data()), ndatest::Bytes(tf)));
  }
  bool HasWarning(const std::string& text) const {
    for (const auto& w : engine.Warnings()) {
      if (w.text.find(text) != std::string::npos) return true;
    }
    return false;
  }
};

void TestFemLabels() {
  CHECK_EQ(modules::FemLabel(kFemA), std::string("192.168.10.16"));
  std::uint32_t id = 0;
  CHECK(modules::ParseFemId("192.168.10.10", id));
  CHECK_EQ(id, kFemB);
  CHECK(modules::ParseFemId("0xc0a80a10", id));
  CHECK_EQ(id, kFemA);
  CHECK(!modules::ParseFemId("192.168.10", id));
  CHECK(!modules::ParseFemId("192.168.10.300", id));
}

void TestCountsAndContinuity() {
  Setup s;
  // Frames 100, 101, then 104 for module A (2 missing); module B is continuous.
  const std::uint32_t framesA[] = {100, 101, 104};
  for (int i = 0; i < 3; ++i) {
    const std::uint32_t f = framesA[i];
    auto stfB = Frame(kFemB, 100 + i, {ndatest::Leading(7, 0, 3000)});
    stfB.frameId = 100 + i;
    s.Feed(ndatest::BuildTimeFrame(f, {Frame(kFemA, f, {ndatest::Leading(5, 10, 1000), ndatest::Trailing(5, 2000)}),
                                       stfB}));
  }
  s.engine.Update();
  const auto& t = s.daq->GetTotals();
  CHECK_EQ(t.timeFrames, std::uint64_t{3});
  CHECK_EQ(t.subTimeFrames, std::uint64_t{6});
  CHECK_EQ(t.heartbeatFrames, std::uint64_t{6});
  CHECK_EQ(t.frameIdMismatch, std::uint64_t{1});  // the third frame: A says 104, B says 102
  const int a = s.daq->Fems().Find(kFemA);
  const int b = s.daq->Fems().Find(kFemB);
  CHECK(a >= 0 && b >= 0);
  CHECK_EQ(s.daq->Counters(a).missingFrames, std::uint64_t{2});
  CHECK_EQ(s.daq->Counters(b).missingFrames, std::uint64_t{0});
  CHECK_EQ(s.daq->Counters(a).words[modules::DaqHealthModule::kKindLeading], std::uint64_t{3});
  CHECK_EQ(s.daq->Counters(a).words[modules::DaqHealthModule::kKindTrailing], std::uint64_t{3});
  CHECK_EQ(s.daq->Counters(a).words[modules::DaqHealthModule::kKindHeartbeat2], std::uint64_t{3});
  CHECK(s.HasWarning("192.168.10.16: 2 missing heartbeat frames"));
  CHECK(s.HasWarning("disagree on the frame id"));

  CHECK_EQ(s.channels->NumFems(), 2);
  const auto& ca = s.channels->Fem(0);
  CHECK_EQ(ca.leading[5], std::uint64_t{3});
  CHECK_EQ(ca.trailing[5], std::uint64_t{3});
  CHECK_EQ(ca.frames, std::uint64_t{3});
  const auto& cb = s.channels->Fem(1);
  CHECK_EQ(cb.leadingWithoutTot, std::uint64_t{3});
}

void TestFlagsAndExpected() {
  Setup s(1, {kFemA, kFemB});
  s.Feed(ndatest::BuildTimeFrame(1, {Frame(kFemA, 1, {}, 0x0200)}, 2, ::TimeFrame::TF_INCOMPLETE));
  s.engine.Update();
  CHECK(s.HasWarning("expected module 192.168.10.10 sends no data"));
  CHECK(s.HasWarning("delimiters with flags (bits seen 0x200)"));
  CHECK(s.HasWarning("1 incomplete time frames"));
  CHECK(s.HasWarning("fewer sub time frames than numSource"));
}

void TestSamplingDoesNotCountGaps() {
  Setup s(2);
  for (std::uint32_t f = 0; f < 6; ++f) s.Feed(ndatest::BuildTimeFrame(f, {Frame(kFemA, f, {})}));
  s.engine.Update();
  CHECK_EQ(s.engine.Received(), std::uint64_t{6});
  CHECK_EQ(s.engine.Processed(), std::uint64_t{3});
  CHECK_EQ(s.daq->Counters(0).missingFrames, std::uint64_t{0});
}

void TestMalformed() {
  Setup s;
  auto tf = ndatest::BuildTimeFrame(1, {Frame(kFemA, 1, {})});
  tf[0] = 0;  // destroy the magic
  CHECK(!s.engine.Process(reinterpret_cast<const std::uint8_t*>(tf.data()), 8 * tf.size()));
  s.engine.Update();
  CHECK_EQ(s.engine.Malformed(), std::uint64_t{1});
  CHECK(s.HasWarning("malformed"));
}

void TestHotChannel() {
  Setup s;
  for (std::uint32_t f = 0; f < 4; ++f) {
    std::vector<std::uint64_t> words = {ndatest::Leading(1, 5, 10), ndatest::Leading(2, 5, 10), ndatest::Leading(3, 5, 10)};
    for (int i = 0; i < 40; ++i) words.push_back(ndatest::Leading(9, 5, 100 + i));
    s.Feed(ndatest::BuildTimeFrame(f, {Frame(kFemA, f, words)}));
  }
  s.engine.Update();
  CHECK(s.HasWarning("hot channels 9"));
}

}  // namespace

int main() {
  gROOT->SetBatch(true);
  TestFemLabels();
  TestCountsAndContinuity();
  TestFlagsAndExpected();
  TestSamplingDoesNotCountGaps();
  TestMalformed();
  TestHotChannel();
  if (gFailures) {
    std::cerr << gFailures << " check(s) failed\n";
    return 1;
  }
  std::cout << "test_modules: all checks passed\n";
  return 0;
}
