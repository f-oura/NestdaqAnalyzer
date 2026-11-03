// Reads time frames from a file written by the NestDAQ FileSink device.
// In follow mode it waits for a file that is still being written, so that it can be used
// as an online input that is not connected to the DAQ chain.
// This file depends on neither ROOT nor FairMQ.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace nda::format {

struct FileInfo {
  bool hasFileSinkHeader = false;
  bool hasFileSinkTrailer = false;
  std::uint64_t runNumber = 0;
  std::int64_t startUnixtime = 0;
  std::int64_t stopUnixtime = 0;
  std::string comments;
};

class FileReader {
 public:
  struct Options {
    bool follow = false;          // wait for more data at the end of the file
    int pollMilliseconds = 500;   // sleep between checks in follow mode
    double idleTimeoutSeconds = 0;  // follow mode: give up after this long without new data (0: never)
  };

  enum class Status { kFrame, kEnd, kStopped, kError };

  FileReader() = default;
  explicit FileReader(Options options) : fOptions(options) {}
  ~FileReader();
  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  // Opens the file and reads the FileSink header if there is one.
  // In follow mode it waits for the header to appear.
  bool Open(const std::string& path, std::string& error);

  // Reads the next time frame into `buffer` (8-byte aligned storage).
  // kEnd: FileSink trailer or end of file reached; kStopped: `stop` was set while waiting.
  Status Next(std::vector<std::uint64_t>& buffer, std::size_t& sizeInBytes, std::string& error);

  // Set from another thread or a signal handler to interrupt waiting in follow mode.
  void SetStopFlag(const std::atomic<bool>* stop) { fStop = stop; }

  const FileInfo& Info() const { return fInfo; }
  std::uint64_t Offset() const { return fOffset; }

 private:
  enum class Wait { kReady, kEnd, kStopped, kError };
  // Waits until at least `bytes` bytes are available after the current offset.
  Wait WaitFor(std::uint64_t bytes, std::string& error);
  bool ReadAt(std::uint64_t offset, void* dest, std::size_t bytes, std::string& error);
  bool ReadFileSinkBlock(bool header, std::string& error);

  Options fOptions;
  int fFd = -1;
  std::string fPath;
  std::uint64_t fOffset = 0;
  FileInfo fInfo;
  const std::atomic<bool>* fStop = nullptr;
};

}  // namespace nda::format
