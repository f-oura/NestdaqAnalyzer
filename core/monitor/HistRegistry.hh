// Owns the histograms of all modules, publishes them on an optional THttpServer,
// and writes them to a ROOT file with one directory per folder.
#pragma once

#include <memory>
#include <string>
#include <vector>

class TDirectory;
class TH1;
class TH1D;
class TH2D;
class THttpServer;
class TObject;

namespace nda::monitor {

class HistRegistry {
 public:
  explicit HistRegistry(THttpServer* server = nullptr);
  ~HistRegistry();
  HistRegistry(const HistRegistry&) = delete;
  HistRegistry& operator=(const HistRegistry&) = delete;

  // `folder` is a slash-separated path such as "DAQ" or "Channels/192.168.10.16".
  TH1D* Book1D(const std::string& folder, const std::string& name, const std::string& title, int nx, double xlo,
               double xhi);
  TH2D* Book2D(const std::string& folder, const std::string& name, const std::string& title, int nx, double xlo,
               double xhi, int ny, double ylo, double yhi);
  // Registers an object owned by the caller (for example a TNamed with the warnings) for display only.
  void Publish(const std::string& folder, TObject* object);
  void Unpublish(TObject* object);

  void ResetAll();
  void Write(TDirectory* dir) const;
  THttpServer* Server() const { return fServer; }

 private:
  struct Entry {
    std::string folder;
    std::unique_ptr<TH1> hist;
  };
  void Register(const std::string& folder, TH1* hist);

  THttpServer* fServer = nullptr;
  std::vector<Entry> fEntries;
};

}  // namespace nda::monitor
