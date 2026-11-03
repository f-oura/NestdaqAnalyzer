#include "monitor/HistRegistry.hh"

#include <TDirectory.h>
#include <TH1D.h>
#include <TH2D.h>
#include <THttpServer.h>

#include "monitor/Warning.hh"

namespace nda::monitor {

const char* CheckName(Check c) {
  switch (c) {
    case Check::kDaq:
      return "DAQ";
    case Check::kChannels:
      return "Channels";
    case Check::kDetectors:
      return "Detectors";
    case Check::kTrigger:
      return "Trigger";
    case Check::kAnalyzer:
      return "Analyzer";
  }
  return "?";
}

const char* SeverityName(Severity s) {
  switch (s) {
    case Severity::kOk:
      return "OK";
    case Severity::kWarning:
      return "WARN";
    case Severity::kError:
      return "ERROR";
  }
  return "?";
}

HistRegistry::HistRegistry(THttpServer* server) : fServer(server) {}

HistRegistry::~HistRegistry() {
  if (fServer) {
    for (auto& e : fEntries) fServer->Unregister(e.hist.get());
  }
}

void HistRegistry::Register(const std::string& folder, TH1* hist) {
  hist->SetDirectory(nullptr);
  if (fServer) fServer->Register(("/" + folder).c_str(), hist);
  fEntries.push_back(Entry{folder, std::unique_ptr<TH1>(hist)});
}

TH1D* HistRegistry::Book1D(const std::string& folder, const std::string& name, const std::string& title, int nx,
                           double xlo, double xhi) {
  auto* h = new TH1D(name.c_str(), title.c_str(), nx, xlo, xhi);
  Register(folder, h);
  return h;
}

TH2D* HistRegistry::Book2D(const std::string& folder, const std::string& name, const std::string& title, int nx,
                           double xlo, double xhi, int ny, double ylo, double yhi) {
  auto* h = new TH2D(name.c_str(), title.c_str(), nx, xlo, xhi, ny, ylo, yhi);
  Register(folder, h);
  return h;
}

void HistRegistry::Publish(const std::string& folder, TObject* object) {
  if (fServer) fServer->Register(("/" + folder).c_str(), object);
}

void HistRegistry::Unpublish(TObject* object) {
  if (fServer) fServer->Unregister(object);
}

void HistRegistry::ResetAll() {
  for (auto& e : fEntries) e.hist->Reset();
}

void HistRegistry::Write(TDirectory* dir) const {
  for (const auto& e : fEntries) {
    TDirectory* d = dir;
    std::size_t start = 0;
    while (start < e.folder.size()) {
      auto end = e.folder.find('/', start);
      if (end == std::string::npos) end = e.folder.size();
      const std::string part = e.folder.substr(start, end - start);
      if (!part.empty()) {
        TDirectory* sub = d->GetDirectory(part.c_str());
        d = sub ? sub : d->mkdir(part.c_str());
      }
      start = end + 1;
    }
    d->WriteTObject(e.hist.get(), e.hist->GetName(), "Overwrite");
  }
}

}  // namespace nda::monitor
