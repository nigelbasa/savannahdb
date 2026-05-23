#pragma once

#include <cstdint>

namespace savannah::storage {

// Stable logical document id used as the storage-level tie-break for equal
// sort keys. Durable backends should preserve RecordId semantics even if the
// physical document bytes move during compaction or vacuum.
using RecordId = std::uint64_t;

constexpr RecordId kInvalidRecordId = 0;

}  // namespace savannah::storage
