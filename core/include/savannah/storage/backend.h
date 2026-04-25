#pragma once

#include "savannah/jungle/v1/storage.h"

#include <string_view>

namespace savannah::storage {

class IStorageBackend {
 public:
  virtual ~IStorageBackend() = default;
  virtual jungle::storage::v1::Collection& collection(
      std::string_view db, std::string_view coll) = 0;
};

}  // namespace savannah::storage
