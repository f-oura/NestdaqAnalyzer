// nda-online-monitor: FairMQ device that runs the online monitor modules on time frames
// received from a channel, for example the "dqm" or "decimator" output of the TimeFrameBuilder.
//
// The device is a thin wrapper: the analysis is done by the same modules as nda-replay.
// NOTE: this file has not been compiled or run yet (FairMQ is not available on the development host).
#include <fairmq/runDevice.h>

#include <TFile.h>
#include <THttpServer.h>
#include <TROOT.h>
#include <TSystem.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include "modules/ChannelModule.hh"
#include "modules/DaqHealthModule.hh"
#include "modules/FemTable.hh"
#include "monitor/Engine.hh"
#include "monitor/HistRegistry.hh"

namespace bpo = boost::program_options;

namespace {

struct OptionKey {
  static constexpr const char* InputChannelName = "in-chan-name";
  static constexpr const char* Http = "http";
  static constexpr const char* UpdateSeconds = "update-sec";
  static constexpr const char* SampleEvery = "sample";
  static constexpr const char* ExpectFem = "expect-fem";
  static constexpr const char* OutputDir = "output-dir";
  static constexpr const char* RunNumber = "monitor-run-number";
  static constexpr const char* RecordType = "record-type";
};

class OnlineMonitor : public fair::mq::Device {
 protected:
  void InitTask() override {
    fInputChannel = fConfig->GetProperty<std::string>(OptionKey::InputChannelName);
    fHttp = fConfig->GetProperty<std::string>(OptionKey::Http);
    fUpdateSeconds = std::stod(fConfig->GetProperty<std::string>(OptionKey::UpdateSeconds));
    fOutputDir = fConfig->GetProperty<std::string>(OptionKey::OutputDir);
    const auto sample = std::stoul(fConfig->GetProperty<std::string>(OptionKey::SampleEvery));

    std::vector<std::uint32_t> expected;
    std::stringstream ss(fConfig->GetProperty<std::string>(OptionKey::ExpectFem));
    std::string item;
    while (std::getline(ss, item, ',')) {
      std::uint32_t id = 0;
      if (item.empty()) continue;
      if (!nda::modules::ParseFemId(item, id)) throw std::runtime_error("bad module id in --expect-fem: " + item);
      expected.push_back(id);
    }

    gROOT->SetBatch(true);
    if (!fServer && !fHttp.empty()) {
      fServer = std::make_unique<THttpServer>(fHttp.c_str());
      fServer->SetReadOnly(true);
      fServer->SetItemField("/", "_monitoring", std::to_string(static_cast<int>(fUpdateSeconds * 1000)).c_str());
      LOG(info) << "THttpServer started: " << fHttp;
    }
    if (!fEngine) {
      nda::monitor::Engine::Options engineOptions;
      engineOptions.sampleEvery = static_cast<std::uint32_t>(sample);
      std::stringstream types(fConfig->GetProperty<std::string>(OptionKey::RecordType));
      while (std::getline(types, item, ',')) {
        if (!item.empty()) engineOptions.recordTypes.push_back(static_cast<std::uint8_t>(std::stoul(item, nullptr, 0) & 0x3F));
      }
      fRegistry = std::make_unique<nda::monitor::HistRegistry>(fServer.get());
      fEngine = std::make_unique<nda::monitor::Engine>(*fRegistry, engineOptions);
      nda::modules::DaqHealthModule::Options daqOptions;
      daqOptions.expectedFems = expected;
      fEngine->Add(std::make_unique<nda::modules::DaqHealthModule>(daqOptions));
      fEngine->Add(std::make_unique<nda::modules::ChannelModule>(nda::modules::ChannelModule::Options{}));
      fEngine->Book();
    }
  }

  void PreRun() override {
    // TODO: take the run number from the NestDAQ run information once it is known how it is provided.
    fRunNumber = std::stoull(fConfig->GetProperty<std::string>(OptionKey::RunNumber));
    fEngine->BeginRun(nda::monitor::RunInfo{fRunNumber});
    fLastUpdate = Clock::now();
    LOG(info) << "OnlineMonitor: run " << fRunNumber << " started, histograms reset";
  }

  bool ConditionalRun() override {
    fair::mq::Parts parts;
    const auto received = Receive(parts, fInputChannel, 0, 200);  // timeout in ms
    if (received > 0) {
      // The multipart message holds the time frame header, the sub time frame headers and the
      // heartbeat frames in the same order as the FileSink writes them, so concatenating the parts
      // gives the layout the parser expects.
      std::size_t size = 0;
      for (const auto& part : parts) size += part->GetSize();
      fBuffer.resize((size + 7) / 8);
      auto* out = reinterpret_cast<std::uint8_t*>(fBuffer.data());
      for (const auto& part : parts) {
        std::memcpy(out, part->GetData(), part->GetSize());
        out += part->GetSize();
      }
      if (!fEngine->Process(reinterpret_cast<const std::uint8_t*>(fBuffer.data()), size)) {
        LOG(warn) << "malformed time frame: " << fEngine->LastError();
      }
    }
    if (fServer) gSystem->ProcessEvents();
    if (std::chrono::duration<double>(Clock::now() - fLastUpdate).count() >= fUpdateSeconds) {
      fEngine->Update();
      fLastUpdate = Clock::now();
      for (const auto& w : fEngine->Warnings()) {
        LOG(warn) << nda::monitor::CheckName(w.check) << " / " << w.source << ": " << w.text;
      }
    }
    return true;
  }

  void PostRun() override {
    fEngine->EndRun();
    LOG(info) << "OnlineMonitor: received " << fEngine->Received() << ", processed " << fEngine->Processed()
              << ", malformed " << fEngine->Malformed() << " time frames";
    if (!fOutputDir.empty()) {
      const std::string path = fOutputDir + "/online-monitor-run" + std::to_string(fRunNumber) + ".root";
      TFile f(path.c_str(), "RECREATE");
      if (f.IsZombie()) {
        LOG(error) << "cannot write " << path;
      } else {
        fRegistry->Write(&f);
        LOG(info) << "histograms written to " << path;
      }
    }
  }

 private:
  using Clock = std::chrono::steady_clock;
  std::string fInputChannel;
  std::string fHttp;
  std::string fOutputDir;
  double fUpdateSeconds = 2;
  std::uint64_t fRunNumber = 0;
  std::unique_ptr<THttpServer> fServer;
  std::unique_ptr<nda::monitor::HistRegistry> fRegistry;
  std::unique_ptr<nda::monitor::Engine> fEngine;
  std::vector<std::uint64_t> fBuffer;
  Clock::time_point fLastUpdate;
};

}  // namespace

void addCustomOptions(bpo::options_description& options) {
  options.add_options()
    (OptionKey::InputChannelName, bpo::value<std::string>()->default_value("in"), "Name of the input channel")
    (OptionKey::Http, bpo::value<std::string>()->default_value(""),
     "THttpServer engine, e.g. http:8080 (empty: no server)")
    (OptionKey::UpdateSeconds, bpo::value<std::string>()->default_value("2"), "Refresh interval of rates and warnings [s]")
    (OptionKey::SampleEvery, bpo::value<std::string>()->default_value("1"), "Process one time frame out of N")
    (OptionKey::ExpectFem, bpo::value<std::string>()->default_value(""),
     "Comma-separated module IPs (or 0x ids) that must send data")
    (OptionKey::OutputDir, bpo::value<std::string>()->default_value(""),
     "Directory for the histogram file written at the end of each run (empty: none)")
    (OptionKey::RunNumber, bpo::value<std::string>()->default_value("0"),
     "Run number used in the output file name")
    (OptionKey::RecordType, bpo::value<std::string>()->default_value(""),
     "Comma-separated 6-bit word types that start multi-word records (word count in bits 49:44)");
}

std::unique_ptr<fair::mq::Device> getDevice(fair::mq::ProgOptions& /*config*/) {
  return std::make_unique<OnlineMonitor>();
}
