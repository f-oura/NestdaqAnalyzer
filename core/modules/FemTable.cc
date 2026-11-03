#include "modules/FemTable.hh"

#include <cstdlib>
#include <sstream>

namespace nda::modules {

std::string FemLabel(std::uint32_t femId) {
  std::ostringstream os;
  os << ((femId >> 24) & 0xFF) << '.' << ((femId >> 16) & 0xFF) << '.' << ((femId >> 8) & 0xFF) << '.'
     << (femId & 0xFF);
  return os.str();
}

bool ParseFemId(const std::string& text, std::uint32_t& femId) {
  if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(text.c_str() + 2, &end, 16);
    if (end == text.c_str() + 2 || *end != '\0' || v > 0xFFFFFFFFul) return false;
    femId = static_cast<std::uint32_t>(v);
    return true;
  }
  std::uint32_t value = 0;
  int parts = 0;
  std::size_t start = 0;
  while (start <= text.size()) {
    auto end = text.find('.', start);
    if (end == std::string::npos) end = text.size();
    const std::string part = text.substr(start, end - start);
    if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos) return false;
    const int byte = std::atoi(part.c_str());
    if (byte > 255) return false;
    value = (value << 8) | static_cast<std::uint32_t>(byte);
    ++parts;
    start = end + 1;
    if (end == text.size()) break;
  }
  if (parts != 4) return false;
  femId = value;
  return true;
}

int FemTable::Index(std::uint32_t femId, bool& isNew) {
  isNew = false;
  auto it = fIndex.find(femId);
  if (it != fIndex.end()) return it->second;
  if (Size() >= fCapacity) return -1;
  const int index = Size();
  fIds.push_back(femId);
  fIndex.emplace(femId, index);
  isNew = true;
  return index;
}

int FemTable::Find(std::uint32_t femId) const {
  auto it = fIndex.find(femId);
  return it == fIndex.end() ? -1 : it->second;
}

}  // namespace nda::modules
