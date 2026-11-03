// nda-replay: run the online monitor modules on a file written by the NestDAQ FileSink.
//
// With --follow it keeps reading a file that is still being written, which makes it an online
// monitor that is not connected to the DAQ chain. With --http the histograms are served by a
// THttpServer (JSROOT) while the file is processed.
#include <TFile.h>
#include <THttpServer.h>
#include <TROOT.h>
#include <TSystem.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "format/FileReader.hh"
#include "modules/ChannelModule.hh"
#include "modules/DaqHealthModule.hh"
#include "modules/FemTable.hh"
#include "monitor/Engine.hh"
#include "monitor/HistRegistry.hh"

namespace {

std::atomic<bool> gStop{false};
void OnSignal(int) { gStop = true; }

struct Args {
  std::string input;
  std::string output;
  std::string summaryJson;
  std::string http;  // THttpServer engine, for example "http:8080" or "http:127.0.0.1:8080"
  bool follow = false;
  bool keepAlive = false;
  int pollMs = 500;
  double idleTimeout = 0;
  double updateSeconds = 2;
  std::uint32_t sampleEvery = 1;
  std::uint64_t maxFrames = 0;
  std::vector<std::uint32_t> expectedFems;
  bool quiet = false;
};

void Usage() {
  std::cout << "Usage: nda-replay --input FILE [options]\n"
               "  --input FILE          file written by the NestDAQ FileSink\n"
               "  --output FILE.root    write the histograms at the end\n"
               "  --summary-json FILE   write counts and warnings as JSON at the end\n"
               "  --http ENGINE         serve the histograms, e.g. http:8080 or http:127.0.0.1:8080\n"
               "  --keep-alive          with --http, keep serving after the end of the input (stop with Ctrl-C)\n"
               "  --follow              wait for more data at the end of the file (stops at the FileSink trailer)\n"
               "  --poll-ms N           follow mode: check the file every N ms (default 500)\n"
               "  --idle-timeout S      follow mode: stop after S seconds without new data (default: never)\n"
               "  --update S            refresh rates and warnings every S seconds of wall time (default 2)\n"
               "  --sample N            process one time frame out of N (default 1)\n"
               "  --max-frames N        stop after N time frames\n"
               "  --expect-fem LIST     comma-separated module IPs (or 0x ids) that must send data\n"
               "  --quiet               do not print warnings while running\n";
}

bool ParseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << k << "\n";
        return false;
      }
      out = argv[++i];
      return true;
    };
    std::string v;
    if (k == "--help" || k == "-h") {
      Usage();
      std::exit(0);
    } else if (k == "--input") {
      if (!value(a.input)) return false;
    } else if (k == "--output") {
      if (!value(a.output)) return false;
    } else if (k == "--summary-json") {
      if (!value(a.summaryJson)) return false;
    } else if (k == "--http") {
      if (!value(a.http)) return false;
    } else if (k == "--keep-alive") {
      a.keepAlive = true;
    } else if (k == "--follow") {
      a.follow = true;
    } else if (k == "--quiet") {
      a.quiet = true;
    } else if (k == "--poll-ms") {
      if (!value(v)) return false;
      a.pollMs = std::atoi(v.c_str());
    } else if (k == "--idle-timeout") {
      if (!value(v)) return false;
      a.idleTimeout = std::atof(v.c_str());
    } else if (k == "--update") {
      if (!value(v)) return false;
      a.updateSeconds = std::atof(v.c_str());
    } else if (k == "--sample") {
      if (!value(v)) return false;
      a.sampleEvery = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    } else if (k == "--max-frames") {
      if (!value(v)) return false;
      a.maxFrames = std::strtoull(v.c_str(), nullptr, 10);
    } else if (k == "--expect-fem") {
      if (!value(v)) return false;
      std::stringstream ss(v);
      std::string item;
      while (std::getline(ss, item, ',')) {
        std::uint32_t id = 0;
        if (!nda::modules::ParseFemId(item, id)) {
          std::cerr << "bad module id " << item << "\n";
          return false;
        }
        a.expectedFems.push_back(id);
      }
    } else {
      std::cerr << "unknown option " << k << "\n";
      return false;
    }
  }
  if (a.input.empty()) {
    std::cerr << "--input is required\n";
    return false;
  }
  return true;
}

std::string JsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"':
        o += "\\\"";
        break;
      case '\\':
        o += "\\\\";
        break;
      case '\n':
        o += "\\n";
        break;
      default:
        o += c;
    }
  }
  return o;
}

void WriteSummary(const std::string& path, const nda::format::FileInfo& info, const nda::monitor::Engine& engine,
                  const nda::modules::DaqHealthModule& daq, const nda::modules::ChannelModule& ch) {
  using nda::modules::DaqHealthModule;
  std::ofstream os(path);
  const auto& t = daq.GetTotals();
  os << "{\n  \"format\": \"nda-replay-summary-v1\",\n";
  os << "  \"run_number\": " << info.runNumber << ",\n";
  os << "  \"filesink_header\": " << (info.hasFileSinkHeader ? "true" : "false") << ",\n";
  os << "  \"filesink_trailer\": " << (info.hasFileSinkTrailer ? "true" : "false") << ",\n";
  os << "  \"received\": " << engine.Received() << ",\n  \"processed\": " << engine.Processed()
     << ",\n  \"malformed\": " << engine.Malformed() << ",\n";
  os << "  \"time_frames\": " << t.timeFrames << ",\n  \"sub_time_frames\": " << t.subTimeFrames
     << ",\n  \"heartbeat_frames\": " << t.heartbeatFrames << ",\n  \"incomplete\": " << t.incomplete
     << ",\n  \"run_seconds\": " << daq.RunSeconds() << ",\n";
  os << "  \"modules\": [\n";
  const char* kinds[DaqHealthModule::kNumKinds] = {"leading", "trailing", "delimiter1", "delimiter2", "t1_start",
                                                    "t1_end", "t2_start", "t2_end", "other"};
  for (int i = 0; i < daq.Fems().Size(); ++i) {
    const auto& c = daq.Counters(i);
    os << "    {\"id\": \"" << nda::modules::FemLabel(daq.Fems().IdAt(i)) << "\", \"frames\": " << c.frames
       << ", \"missing_frames\": " << c.missingFrames << ", \"repeated_frames\": " << c.repeatedFrames
       << ", \"flagged_frames\": " << c.flaggedFrames << ", \"words\": {";
    for (int k = 0; k < DaqHealthModule::kNumKinds; ++k) os << (k ? ", " : "") << "\"" << kinds[k] << "\": " << c.words[k];
    os << "}}" << (i + 1 < daq.Fems().Size() ? "," : "") << "\n";
  }
  os << "  ],\n  \"channels\": [\n";
  for (int i = 0; i < ch.NumFems(); ++i) {
    const auto& d = ch.Fem(i);
    std::uint64_t lead = 0, trail = 0;
    int active = 0;
    for (int c = 0; c < d.nChannels; ++c) {
      lead += d.leading[c];
      trail += d.trailing[c];
      if (d.leading[c] > 0) ++active;
    }
    os << "    {\"id\": \"" << nda::modules::FemLabel(d.femId) << "\", \"leading\": " << lead
       << ", \"trailing\": " << trail << ", \"leading_without_tot\": " << d.leadingWithoutTot
       << ", \"active_channels\": " << active << "}" << (i + 1 < ch.NumFems() ? "," : "") << "\n";
  }
  os << "  ],\n  \"warnings\": [";
  const auto& ws = engine.Warnings();
  for (std::size_t i = 0; i < ws.size(); ++i) {
    os << (i ? ", " : "") << "\n    {\"check\": \"" << nda::monitor::CheckName(ws[i].check) << "\", \"severity\": \""
       << nda::monitor::SeverityName(ws[i].severity) << "\", \"source\": \"" << ws[i].source << "\", \"text\": \""
       << JsonEscape(ws[i].text) << "\"}";
  }
  os << (ws.empty() ? "]\n" : "\n  ]\n") << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, args)) {
    Usage();
    return 2;
  }
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  gROOT->SetBatch(true);

  std::unique_ptr<THttpServer> server;
  if (!args.http.empty()) {
    server = std::make_unique<THttpServer>(args.http.c_str());
    server->SetReadOnly(true);
    server->SetItemField("/", "_monitoring", std::to_string(static_cast<int>(args.updateSeconds * 1000)).c_str());
  }

  nda::monitor::HistRegistry registry(server.get());
  nda::monitor::Engine engine(registry, nda::monitor::Engine::Options{args.sampleEvery});
  nda::modules::DaqHealthModule::Options daqOptions;
  daqOptions.expectedFems = args.expectedFems;
  auto daq = std::make_unique<nda::modules::DaqHealthModule>(daqOptions);
  auto channels = std::make_unique<nda::modules::ChannelModule>(nda::modules::ChannelModule::Options{});
  auto* daqPtr = daq.get();
  auto* channelsPtr = channels.get();
  engine.Add(std::move(daq));
  engine.Add(std::move(channels));
  engine.Book();

  nda::format::FileReader::Options readerOptions;
  readerOptions.follow = args.follow;
  readerOptions.pollMilliseconds = args.pollMs;
  readerOptions.idleTimeoutSeconds = args.idleTimeout;
  nda::format::FileReader reader(readerOptions);
  reader.SetStopFlag(&gStop);
  std::string error;
  if (!reader.Open(args.input, error)) {
    std::cerr << "nda-replay: " << error << "\n";
    return 1;
  }
  engine.BeginRun(nda::monitor::RunInfo{reader.Info().runNumber});
  std::cout << "nda-replay: input " << args.input << ", run " << reader.Info().runNumber << "\n";

  using Clock = std::chrono::steady_clock;
  auto lastUpdate = Clock::now();
  const auto started = Clock::now();
  std::vector<std::uint64_t> buffer;
  int status = 0;
  while (!gStop) {
    std::size_t size = 0;
    const auto s = reader.Next(buffer, size, error);
    if (s == nda::format::FileReader::Status::kEnd || s == nda::format::FileReader::Status::kStopped) break;
    if (s == nda::format::FileReader::Status::kError) {
      std::cerr << "nda-replay: " << error << "\n";
      status = 1;
      break;
    }
    if (!engine.Process(reinterpret_cast<const std::uint8_t*>(buffer.data()), size)) {
      std::cerr << "nda-replay: malformed time frame: " << engine.LastError() << "\n";
    }
    if (server) gSystem->ProcessEvents();
    if (std::chrono::duration<double>(Clock::now() - lastUpdate).count() >= args.updateSeconds) {
      engine.Update();
      lastUpdate = Clock::now();
      if (!args.quiet && !engine.Warnings().empty()) std::cout << engine.WarningText();
    }
    if (args.maxFrames > 0 && engine.Received() >= args.maxFrames) break;
  }
  engine.EndRun();
  const double wall = std::chrono::duration<double>(Clock::now() - started).count();

  std::cout << "nda-replay: received " << engine.Received() << ", processed " << engine.Processed() << ", malformed "
            << engine.Malformed() << " time frames in " << wall << " s"
            << (reader.Info().hasFileSinkTrailer ? " (FileSink trailer found)" : "") << "\n";
  std::cout << engine.WarningText() << (engine.Warnings().empty() ? "\n" : "");

  if (!args.output.empty()) {
    TFile f(args.output.c_str(), "RECREATE");
    if (f.IsZombie()) {
      std::cerr << "nda-replay: cannot write " << args.output << "\n";
      status = 1;
    } else {
      registry.Write(&f);
      f.Close();
    }
  }
  if (!args.summaryJson.empty()) WriteSummary(args.summaryJson, reader.Info(), engine, *daqPtr, *channelsPtr);

  if (server && args.keepAlive) {
    std::cout << "nda-replay: serving histograms, stop with Ctrl-C\n";
    gStop = false;
    while (!gStop) {
      gSystem->ProcessEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  return status;
}
