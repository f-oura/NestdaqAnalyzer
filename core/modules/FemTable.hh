// Assigns a histogram bin to each front-end module (FEM) in the order they appear.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nda::modules {

// The FEM id is built from the SiTCP IP address of the board: 192.168.10.16 -> 0xc0a80a10.
std::string FemLabel(std::uint32_t femId);
// Parses "192.168.10.16" or "0xc0a80a10". Returns false on a malformed string.
bool ParseFemId(const std::string& text, std::uint32_t& femId);

class FemTable {
 public:
  explicit FemTable(int capacity) : fCapacity(capacity) {}
  // Returns the 0-based index of `femId`, adding it if needed, or -1 when the table is full.
  // `isNew` is set when the FEM was added by this call.
  int Index(std::uint32_t femId, bool& isNew);
  int Find(std::uint32_t femId) const;
  int Size() const { return static_cast<int>(fIds.size()); }
  int Capacity() const { return fCapacity; }
  std::uint32_t IdAt(int index) const { return fIds[index]; }

 private:
  int fCapacity;
  std::vector<std::uint32_t> fIds;
  std::unordered_map<std::uint32_t, int> fIndex;
};

}  // namespace nda::modules
