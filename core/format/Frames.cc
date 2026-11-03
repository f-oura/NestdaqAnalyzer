#include "format/Frames.hh"

#include <sstream>

namespace nda::format {

namespace {

std::string Hex(std::uint64_t v) {
  std::ostringstream os;
  os << "0x" << std::hex << v;
  return os.str();
}

}  // namespace

bool ParseTimeFrame(const std::uint8_t* data, std::size_t size, TimeFrameView& out, std::string& error) {
  out = TimeFrameView{};
  if (size < sizeof(::TimeFrame::Header)) {
    error = "buffer shorter than a time frame header";
    return false;
  }
  const auto* tf = reinterpret_cast<const ::TimeFrame::Header*>(data);
  if (tf->magic != ::TimeFrame::MAGIC) {
    error = "bad time frame magic " + Hex(tf->magic);
    return false;
  }
  if (tf->length > size || tf->hLength < sizeof(::TimeFrame::Header) || tf->hLength > tf->length) {
    error = "bad time frame length " + std::to_string(tf->length) + " (buffer " + std::to_string(size) + ")";
    return false;
  }
  out.header = tf;
  out.subFrames.reserve(tf->numSource);

  std::size_t pos = tf->hLength;
  const std::size_t tfEnd = tf->length;
  while (pos < tfEnd) {
    if (tfEnd - pos < sizeof(::SubTimeFrame::Header)) {
      error = "truncated sub time frame header at offset " + std::to_string(pos);
      return false;
    }
    const auto* stf = reinterpret_cast<const ::SubTimeFrame::Header*>(data + pos);
    if (stf->magic != ::SubTimeFrame::MAGIC) {
      error = "bad sub time frame magic " + Hex(stf->magic) + " at offset " + std::to_string(pos);
      return false;
    }
    if (stf->length > tfEnd - pos || stf->hLength < sizeof(::SubTimeFrame::Header) || stf->hLength > stf->length) {
      error = "bad sub time frame length " + std::to_string(stf->length) + " at offset " + std::to_string(pos);
      return false;
    }
    SubTimeFrameView sv;
    sv.header = stf;
    std::size_t hpos = pos + stf->hLength;
    const std::size_t stfEnd = pos + stf->length;
    while (hpos < stfEnd) {
      if (stfEnd - hpos < sizeof(::HeartbeatFrame::Header)) {
        error = "truncated heartbeat frame header at offset " + std::to_string(hpos);
        return false;
      }
      const auto* hbf = reinterpret_cast<const ::HeartbeatFrame::Header*>(data + hpos);
      if (hbf->magic != ::HeartbeatFrame::MAGIC) {
        error = "bad heartbeat frame magic " + Hex(hbf->magic) + " at offset " + std::to_string(hpos);
        return false;
      }
      if (hbf->length > stfEnd - hpos || hbf->hLength < sizeof(::HeartbeatFrame::Header) ||
          hbf->hLength > hbf->length || (hbf->length - hbf->hLength) % 8 != 0) {
        error = "bad heartbeat frame length " + std::to_string(hbf->length) + " at offset " + std::to_string(hpos);
        return false;
      }
      HeartbeatFrameView hv;
      hv.words = reinterpret_cast<const std::uint64_t*>(data + hpos + hbf->hLength);
      hv.nWords = (hbf->length - hbf->hLength) / 8;
      sv.frames.push_back(hv);
      hpos += hbf->length;
    }
    out.subFrames.push_back(std::move(sv));
    pos = stfEnd;
  }
  return true;
}

bool IsIncomplete(const ::TimeFrame::Header& h) { return (h.type & ::TimeFrame::TF_INCOMPLETE) != 0; }
bool IsTimeout(const ::TimeFrame::Header& h) { return (h.type & ::TimeFrame::TF_TIMEOUT) != 0; }
bool IsDepthLimit(const ::TimeFrame::Header& h) { return (h.type & ::TimeFrame::TF_DEPTH_LIMIT) != 0; }

}  // namespace nda::format
