#include "format/FileReader.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "format/NestdaqHeaders.hh"

namespace nda::format {

namespace {

constexpr std::uint64_t kFileSinkBlockBytes = sizeof(::FileSinkHeader::Header);
static_assert(sizeof(::FileSinkHeader::Header) == 304, "unexpected FileSink header size");
static_assert(sizeof(::FileSinkTrailer::Trailer) == 304, "unexpected FileSink trailer size");

}  // namespace

FileReader::~FileReader() {
  if (fFd >= 0) ::close(fFd);
}

bool FileReader::Open(const std::string& path, std::string& error) {
  fPath = path;
  fFd = ::open(path.c_str(), O_RDONLY);
  if (fFd < 0) {
    error = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  fOffset = 0;
  fInfo = FileInfo{};
  switch (WaitFor(sizeof(std::uint64_t), error)) {
    case Wait::kReady:
      break;
    case Wait::kEnd:
      error = "empty file " + path;
      return false;
    case Wait::kStopped:
      error = "stopped while waiting for " + path;
      return false;
    case Wait::kError:
      return false;
  }
  std::uint64_t magic = 0;
  if (!ReadAt(0, &magic, sizeof(magic), error)) return false;
  if (magic == ::FileSinkHeader::MAGIC || magic == ::FileSinkHeader::v0::MAGIC) {
    return ReadFileSinkBlock(true, error);
  }
  if (magic != ::TimeFrame::MAGIC) {
    error = "file does not start with a FileSink header or a time frame";
    return false;
  }
  return true;  // a bare stream of time frames
}

bool FileReader::ReadFileSinkBlock(bool header, std::string& error) {
  switch (WaitFor(kFileSinkBlockBytes, error)) {
    case Wait::kReady:
      break;
    case Wait::kEnd:
      error = "truncated FileSink block";
      return false;
    case Wait::kStopped:
      error = "stopped";
      return false;
    case Wait::kError:
      return false;
  }
  ::FileSinkHeader::Header block;  // the trailer has the same layout
  if (!ReadAt(fOffset, &block, sizeof(block), error)) return false;
  fOffset += kFileSinkBlockBytes;
  if (header) {
    fInfo.hasFileSinkHeader = true;
    fInfo.runNumber = block.runNumber;
    fInfo.startUnixtime = block.startUnixtime;
    fInfo.comments.assign(block.comments, strnlen(block.comments, sizeof(block.comments)));
  } else {
    fInfo.hasFileSinkTrailer = true;
    fInfo.stopUnixtime = block.stopUnixtime;
  }
  return true;
}

FileReader::Status FileReader::Next(std::vector<std::uint64_t>& buffer, std::size_t& sizeInBytes,
                                    std::string& error) {
  sizeInBytes = 0;
  switch (WaitFor(sizeof(std::uint64_t), error)) {
    case Wait::kReady:
      break;
    case Wait::kEnd:
      return Status::kEnd;
    case Wait::kStopped:
      return Status::kStopped;
    case Wait::kError:
      return Status::kError;
  }
  std::uint64_t magic = 0;
  if (!ReadAt(fOffset, &magic, sizeof(magic), error)) return Status::kError;
  if (magic == ::FileSinkTrailer::MAGIC || magic == ::FileSinkTrailer::v0::MAGIC) {
    if (!ReadFileSinkBlock(false, error)) return Status::kError;
    return Status::kEnd;
  }
  if (magic != ::TimeFrame::MAGIC) {
    error = "bad time frame magic at offset " + std::to_string(fOffset);
    return Status::kError;
  }

  ::TimeFrame::Header header;
  switch (WaitFor(sizeof(header), error)) {
    case Wait::kReady:
      break;
    case Wait::kEnd:
      error = "truncated time frame header at offset " + std::to_string(fOffset);
      return Status::kError;
    case Wait::kStopped:
      return Status::kStopped;
    case Wait::kError:
      return Status::kError;
  }
  if (!ReadAt(fOffset, &header, sizeof(header), error)) return Status::kError;
  if (header.length < sizeof(header) || header.length % 8 != 0) {
    error = "bad time frame length " + std::to_string(header.length) + " at offset " + std::to_string(fOffset);
    return Status::kError;
  }
  switch (WaitFor(header.length, error)) {
    case Wait::kReady:
      break;
    case Wait::kEnd:
      error = "truncated time frame at offset " + std::to_string(fOffset);
      return Status::kError;
    case Wait::kStopped:
      return Status::kStopped;
    case Wait::kError:
      return Status::kError;
  }
  buffer.resize(header.length / 8);
  if (!ReadAt(fOffset, buffer.data(), header.length, error)) return Status::kError;
  fOffset += header.length;
  sizeInBytes = header.length;
  return Status::kFrame;
}

FileReader::Wait FileReader::WaitFor(std::uint64_t bytes, std::string& error) {
  using Clock = std::chrono::steady_clock;
  auto lastGrowth = Clock::now();
  std::uint64_t lastSize = 0;
  while (true) {
    struct stat st;
    if (::fstat(fFd, &st) != 0) {
      error = "cannot stat " + fPath + ": " + std::strerror(errno);
      return Wait::kError;
    }
    const auto size = static_cast<std::uint64_t>(st.st_size);
    if (size >= fOffset + bytes) return Wait::kReady;
    if (!fOptions.follow) return Wait::kEnd;
    if (fStop && fStop->load()) return Wait::kStopped;
    if (size != lastSize) {
      lastSize = size;
      lastGrowth = Clock::now();
    } else if (fOptions.idleTimeoutSeconds > 0 &&
               std::chrono::duration<double>(Clock::now() - lastGrowth).count() > fOptions.idleTimeoutSeconds) {
      return Wait::kEnd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(fOptions.pollMilliseconds));
  }
}

bool FileReader::ReadAt(std::uint64_t offset, void* dest, std::size_t bytes, std::string& error) {
  auto* out = static_cast<char*>(dest);
  std::size_t done = 0;
  while (done < bytes) {
    const ssize_t n = ::pread(fFd, out + done, bytes - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      error = "read error in " + fPath + ": " + std::strerror(errno);
      return false;
    }
    if (n == 0) {
      error = "unexpected end of " + fPath;
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace nda::format
