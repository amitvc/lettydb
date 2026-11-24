#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog_manager.h"

namespace letty {

class StorageInspector {
 public:
  StorageInspector(BufferPoolManager& bpm, CatalogManager& cm);

  std::string get_summary();
  std::string get_freelist();
  std::string get_page_map();
  std::string get_page_detail(page_id_t page_id, const std::string& owner = "");
  std::string get_table_chain(const std::string& table_name);
  std::string get_catalog();

 private:
  BufferPoolManager& buffer_pool_;
  CatalogManager& catalog_manager_;

  static std::string page_type_to_string(PageType type);
  static std::string bytes_to_hex(const char* data, uint32_t size);
  static nlohmann::json value_to_json(const Value& val);
  nlohmann::json slotted_page_to_json(page_id_t id, const PageGuard& guard, const std::string& owner);
  std::string resolve_page_owner(uint16_t table_id);
};

}
