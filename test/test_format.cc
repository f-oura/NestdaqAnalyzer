// Unit tests of the data format layer (no ROOT).
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "Builder.hh"
#include "format/NestdaqHeaders.hh"
#include "format/FileReader.hh"
#include "format/Frames.hh"
#include "format/TdcWord.hh"
#include "format/WordStream.hh"

using namespace nda::format;

namespace {

void TestWordDecoding() {
  const auto lead = ndatest::Leading(42, 12345, 0x1ABCDEF);
  CHECK_EQ(int(HeadOf(lead)), int(kLeading));
  const auto hit = DecodeTdc(lead, TdcKind::kHighResolution);
  CHECK(hit.leading);
  CHECK_EQ(hit.channel, 42u);
  CHECK_EQ(hit.tot, 12345u);
  CHECK_EQ(hit.tdc, 0x1ABCDEFu);
  CHECK(!DecodeTdc(ndatest::Trailing(3, 7), TdcKind::kHighResolution).leading);

  // Low resolution: ch [57:50], tot [49:34], tdc [33:15].
  const std::uint64_t lr = (std::uint64_t{kLeading} << 58) | (std::uint64_t{200} << 50) | (std::uint64_t{0xBEEF} << 34) |
                           (std::uint64_t{0x5A5A5} << 15);
  const auto lhit = DecodeTdc(lr, TdcKind::kLowResolution);
  CHECK_EQ(lhit.channel, 200u);
  CHECK_EQ(lhit.tot, 0xBEEFu);
  CHECK_EQ(lhit.tdc, 0x5A5A5u);

  const auto d1 = DecodeHeartbeat1(ndatest::Delimiter1(0xABCDEF, 0x0201, 0x1234));
  CHECK_EQ(d1.frameNumber, 0xABCDEFu);
  CHECK_EQ(d1.flags, 0x0201);
  CHECK_EQ(d1.timeOffset, 0x1234);
  const auto d2 = DecodeHeartbeat2(ndatest::Delimiter2(800, 808));
  CHECK_EQ(d2.transferredSize, 800u);
  CHECK_EQ(d2.generatedSize, 808u);

  CHECK_EQ(FrameDistance(10, 11), 1u);
  CHECK_EQ(FrameDistance(0xFFFFFF, 0), 1u);
  CHECK_EQ(FrameDistance(0xFFFFFE, 2), 4u);
  CHECK_EQ(int(KindOfFemType(::SubTimeFrame::TDC64H)), int(TdcKind::kHighResolution));
  CHECK_EQ(int(KindOfFemType(::SubTimeFrame::TDC64L_V2)), int(TdcKind::kLowResolution));
}

ndatest::Stf MakeStf(std::uint32_t fem, std::uint32_t frame, int nHits) {
  ndatest::Stf s;
  s.femId = fem;
  std::vector<std::uint64_t> words;
  for (int i = 0; i < nHits; ++i) words.push_back(ndatest::Leading(i, 100, 1000 * i));
  words.push_back(ndatest::Delimiter1(frame));
  words.push_back(ndatest::Delimiter2(8 * nHits, 8 * nHits));
  s.frames.push_back(words);
  return s;
}

void TestParse() {
  const auto tf = ndatest::BuildTimeFrame(7, {MakeStf(0xc0a80a10, 7, 3), MakeStf(0xc0a80a0a, 7, 0)});
  TimeFrameView view;
  std::string error;
  CHECK(ParseTimeFrame(reinterpret_cast<const std::uint8_t*>(tf.data()), ndatest::Bytes(tf), view, error));
  CHECK_EQ(view.subFrames.size(), std::size_t{2});
  CHECK_EQ(view.header->numSource, 2u);
  CHECK_EQ(view.subFrames[0].header->femId, 0xc0a80a10u);
  CHECK_EQ(view.subFrames[0].frames.size(), std::size_t{1});
  CHECK_EQ(view.subFrames[0].frames[0].nWords, std::size_t{5});
  CHECK_EQ(view.subFrames[1].frames[0].nWords, std::size_t{2});
  CHECK_EQ(int(HeadOf(view.subFrames[0].frames[0].words[3])), int(kHeartbeat1));

  // Corrupt the magic of the second sub time frame.
  auto bad = tf;
  auto* bytes = reinterpret_cast<std::uint8_t*>(bad.data());
  const std::size_t second = sizeof(::TimeFrame::Header) + view.subFrames[0].header->length;
  bytes[second] ^= 0xFF;
  CHECK(!ParseTimeFrame(bytes, ndatest::Bytes(bad), view, error));
  CHECK(error.find("sub time frame magic") != std::string::npos);

  // Truncated buffer.
  CHECK(!ParseTimeFrame(reinterpret_cast<const std::uint8_t*>(tf.data()), ndatest::Bytes(tf) - 8, view, error));
}

void TestFileReader() {
  char path[] = "/tmp/nda-test-XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  close(fd);
  {
    std::ofstream os(path, std::ios::binary);
    ::FileSinkHeader::Header h{};
    h.magic = ::FileSinkHeader::MAGIC;
    h.runNumber = 123;
    std::snprintf(h.comments, sizeof(h.comments), "test file");
    os.write(reinterpret_cast<const char*>(&h), sizeof(h));
    for (std::uint32_t f = 0; f < 3; ++f) {
      const auto tf = ndatest::BuildTimeFrame(f, {MakeStf(0xc0a80a10, f, 2)});
      os.write(reinterpret_cast<const char*>(tf.data()), ndatest::Bytes(tf));
    }
    ::FileSinkTrailer::Trailer t{};
    t.magic = ::FileSinkTrailer::MAGIC;
    t.stopUnixtime = 99;
    os.write(reinterpret_cast<const char*>(&t), sizeof(t));
  }
  FileReader reader;
  std::string error;
  CHECK(reader.Open(path, error));
  CHECK(reader.Info().hasFileSinkHeader);
  CHECK_EQ(reader.Info().runNumber, std::uint64_t{123});
  CHECK_EQ(reader.Info().comments, std::string("test file"));
  std::vector<std::uint64_t> buffer;
  std::size_t size = 0;
  int frames = 0;
  FileReader::Status s;
  while ((s = reader.Next(buffer, size, error)) == FileReader::Status::kFrame) {
    TimeFrameView view;
    CHECK(ParseTimeFrame(reinterpret_cast<const std::uint8_t*>(buffer.data()), size, view, error));
    CHECK_EQ(view.header->timeFrameId, std::uint32_t(frames));
    ++frames;
  }
  CHECK(s == FileReader::Status::kEnd);
  CHECK_EQ(frames, 3);
  CHECK(reader.Info().hasFileSinkTrailer);
  CHECK_EQ(reader.Info().stopUnixtime, std::int64_t{99});
  std::remove(path);
}

void TestWordStream() {
  // A 3-word record of type 0x2b whose payload words look like a leading edge and a delimiter.
  const std::uint64_t header = (std::uint64_t{0x2b} << 58) | (std::uint64_t{3} << 44);
  const std::uint64_t fakeLeading = ndatest::Leading(1, 2, 3);
  const std::uint64_t fakeDelimiter = ndatest::Delimiter1(77);
  std::vector<std::uint64_t> words = {ndatest::Leading(5, 1, 1), header, fakeLeading, fakeDelimiter,
                                      ndatest::Delimiter1(10), ndatest::Delimiter2(8, 8)};
  HeartbeatFrameView hbf{words.data(), words.size()};
  WordStream plain;
  int leading = 0, delimiters = 0;
  plain.ForEach(hbf, [&](std::uint64_t w, WordRole, std::size_t) {
    leading += HeadOf(w) == kLeading;
    delimiters += HeadOf(w) == kHeartbeat1;
  });
  CHECK_EQ(leading, 2);
  CHECK_EQ(delimiters, 2);

  WordStream records;
  records.AddRecordType(0x2b);
  leading = delimiters = 0;
  int headers = 0, payload = 0;
  const auto cut = records.ForEach(hbf, [&](std::uint64_t w, WordRole role, std::size_t size) {
    if (role == WordRole::kRecordHeader) {
      ++headers;
      CHECK_EQ(size, std::size_t{3});
    } else if (role == WordRole::kRecordPayload) {
      ++payload;
    } else {
      leading += HeadOf(w) == kLeading;
      delimiters += HeadOf(w) == kHeartbeat1;
    }
  });
  CHECK_EQ(cut, std::size_t{0});
  CHECK_EQ(headers, 1);
  CHECK_EQ(payload, 2);
  CHECK_EQ(leading, 1);
  CHECK_EQ(delimiters, 1);

  // A record announcing 5 words with only 1 payload word before the delimiters: cut short, delimiters kept.
  const std::uint64_t longHeader = (std::uint64_t{0x2b} << 58) | (std::uint64_t{5} << 44);
  std::vector<std::uint64_t> shortRecord = {longHeader, fakeLeading, ndatest::Delimiter1(11), ndatest::Delimiter2(16, 16)};
  delimiters = payload = 0;
  const auto cut2 = records.ForEach(HeartbeatFrameView{shortRecord.data(), shortRecord.size()},
                                    [&](std::uint64_t w, WordRole role, std::size_t) {
                                      payload += role == WordRole::kRecordPayload;
                                      delimiters += role == WordRole::kWord && HeadOf(w) == kHeartbeat1;
                                    });
  CHECK_EQ(cut2, std::size_t{1});
  CHECK_EQ(payload, 1);
  CHECK_EQ(delimiters, 1);
}

}  // namespace

int main() {
  TestWordDecoding();
  TestParse();
  TestFileReader();
  TestWordStream();
  if (gFailures) {
    std::cerr << gFailures << " check(s) failed\n";
    return 1;
  }
  std::cout << "test_format: all checks passed\n";
  return 0;
}
