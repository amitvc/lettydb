#include "monitoring/storage_inspector.h"

#include <iomanip>
#include <sstream>
#include <vector>

#include "storage/config.h"
#include "storage/page_utils.h"
#include "storage/slotted_page.h"
#include "storage/storage_def.h"
#include "storage/tuple.h"

namespace letty {

StorageInspector::StorageInspector(BufferPoolManager& bpm, CatalogManager& cm)
    : buffer_pool_(bpm), catalog_manager_(cm) {}

std::string StorageInspector::page_type_to_string(PageType type) {
  switch (type) {
    case PageType::DATABASE_HEADER: return "DATABASE_HEADER";
    case PageType::DATA:            return "DATA";
    case PageType::INDEX:           return "INDEX";
    case PageType::FREE:            return "FREE";
    default:                        return "INVALID";
  }
}

std::string StorageInspector::bytes_to_hex(const char* data, uint32_t size) {
  std::ostringstream oss;
  for (uint32_t i = 0; i < size; ++i) {
    if (i > 0 && i % 16 == 0) {
      oss << "\n";
    } else if (i > 0) {
      oss << " ";
    }
    oss << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(static_cast<unsigned char>(data[i]));
  }
  return oss.str();
}

nlohmann::json StorageInspector::value_to_json(const Value& val) {
  if (std::holds_alternative<std::monostate>(val)) {
    return nullptr;
  }
  if (std::holds_alternative<int8_t>(val)) {
    return static_cast<int>(std::get<int8_t>(val));
  }
  if (std::holds_alternative<int16_t>(val)) {
    return std::get<int16_t>(val);
  }
  if (std::holds_alternative<int32_t>(val)) {
    return std::get<int32_t>(val);
  }
  if (std::holds_alternative<int64_t>(val)) {
    return std::get<int64_t>(val);
  }
  if (std::holds_alternative<double>(val)) {
    return std::get<double>(val);
  }
  if (std::holds_alternative<bool>(val)) {
    return std::get<bool>(val);
  }
  if (std::holds_alternative<std::string>(val)) {
    return std::get<std::string>(val);
  }
  return nullptr;
}

std::string StorageInspector::get_summary() {
  nlohmann::json j;

  auto db_meta = load_page_body_value<DatabaseMetadata>(buffer_pool_, DATABASE_HEADER_PAGE_ID);
  uint32_t total_pages = db_meta ? db_meta->page_count : static_cast<uint32_t>(buffer_pool_.get_file_size_in_pages());
  uint32_t free_pages = 0;

  if (db_meta) {
    page_id_t curr = db_meta->free_page_list_head_page;
    while (curr != INVALID_PAGE_ID && free_pages < total_pages) {
      auto header = load_page_value<PageHeader>(buffer_pool_, curr);
      if (!header) {
        break;
      }
      ++free_pages;
      curr = header->next_page_id;
    }
  }

  uint32_t used_pages = total_pages > free_pages ? total_pages - free_pages : 0;

  j["total_pages"] = total_pages;
  j["used_pages"] = used_pages;
  j["free_pages"] = free_pages;
  j["table_count"] = db_meta ? db_meta->table_count : 0;
  j["free_list_head"] = db_meta ? db_meta->free_page_list_head_page : INVALID_PAGE_ID;
  j["percent_full"] = total_pages > 0 ? (100.0 * used_pages / total_pages) : 0.0;

  auto stats = buffer_pool_.get_cache_stats();
  j["bpm"] = {
      {"pool_size", buffer_pool_.get_pool_size()},
      {"hits", stats.hits},
      {"misses", stats.misses},
      {"evictions", stats.evictions},
      {"dirty_evictions", stats.dirty_evictions},
      {"hit_ratio", stats.hit_ratio()}
  };

  return j.dump();
}

std::string StorageInspector::get_freelist() {
  nlohmann::json j;
  nlohmann::json pages = nlohmann::json::array();

  auto db_meta = load_page_body_value<DatabaseMetadata>(buffer_pool_, DATABASE_HEADER_PAGE_ID);
  if (!db_meta) {
    j["error"] = "Failed to load database metadata";
    return j.dump();
  }

  page_id_t curr = db_meta->free_page_list_head_page;
  uint32_t visited = 0;
  while (curr != INVALID_PAGE_ID && visited < db_meta->page_count) {
    pages.push_back(curr);
    auto header = load_page_value<PageHeader>(buffer_pool_, curr);
    if (!header) {
      break;
    }
    curr = header->next_page_id;
    ++visited;
  }

  j["head_page_id"] = db_meta->free_page_list_head_page;
  j["count"] = pages.size();
  j["pages"] = pages;
  return j.dump();
}

std::string StorageInspector::get_page_map() {
  nlohmann::json result = nlohmann::json::array();

  auto db_meta = load_page_body_value<DatabaseMetadata>(buffer_pool_, DATABASE_HEADER_PAGE_ID);
  uint32_t page_count = db_meta ? db_meta->page_count : static_cast<uint32_t>(buffer_pool_.get_file_size_in_pages());

  for (uint32_t pid = 0; pid < page_count; ++pid) {
    PageGuard guard = buffer_pool_.fetch_page_guard(static_cast<page_id_t>(pid));
    if (!guard) {
      continue;
    }

    PageHeader header = load_page_layout<PageHeader>(guard);
    nlohmann::json entry;
    entry["page_id"] = pid;
    entry["type"] = page_type_to_string(header.page_type);
    entry["table_id"] = header.table_id;
    entry["owner"] = resolve_page_owner(header.table_id);
    entry["prev_page_id"] = header.prev_page_id;
    entry["next_page_id"] = header.next_page_id;
    entry["lsn"] = header.page_lsn;

    if (header.page_type == PageType::DATA) {
      SlottedPage sp = SlottedPage::from_page(guard);
      entry["num_slots"] = sp.get_num_slots();
      entry["free_space"] = sp.get_free_space();
    }

    result.push_back(entry);
  }

  return result.dump();
}

nlohmann::json StorageInspector::slotted_page_to_json(page_id_t id, const PageGuard& guard, const std::string& owner) {
  nlohmann::json j;
  PageHeader page_header = load_page_layout<PageHeader>(guard);
  SlottedPage sp = SlottedPage::from_page(const_cast<PageGuard&>(guard));
  SlotDirectoryHeader dir_header = load_header_from_bytes(reinterpret_cast<const Byte*>(guard.get_body()));

  j["page_id"] = id;
  j["header"] = {
      {"num_slots", dir_header.num_slots},
      {"free_space_ptr", dir_header.free_space_pointer},
      {"free_space", sp.get_free_space()},
      {"lsn", page_header.page_lsn}
  };

  nlohmann::json slots = nlohmann::json::array();
  for (uint16_t i = 0; i < sp.get_num_slots(); ++i) {
    auto slot = sp.get_slot(i);
    if (!slot) {
      continue;
    }
    slots.push_back({
        {"index", i},
        {"offset", slot->offset},
        {"length", slot->length}
    });
  }
  j["slots"] = slots;

  std::optional<Schema> schema;
  if (!owner.empty()) {
    if (owner == SYS_TABLES_NAME) {
      schema = CatalogManager::sys_tables_schema();
    } else if (owner == SYS_COLUMNS_NAME) {
      schema = CatalogManager::sys_columns_schema();
    } else if (auto meta = catalog_manager_.get_table(owner); meta.has_value()) {
      schema = meta->get().schema;
    }
  }

  if (schema.has_value()) {
    nlohmann::json decoded = nlohmann::json::array();
    for (uint16_t i = 0; i < sp.get_num_slots(); ++i) {
      uint32_t sz = 0;
      const Byte* raw = sp.get_tuple(i, &sz);
      if (!raw) {
        continue;
      }
      auto slot = sp.get_slot(i);
      Tuple t = Tuple::deserialize(*schema, raw, sz);
      nlohmann::json vals = nlohmann::json::array();
      for (size_t c = 0; c < t.size(); ++c) {
        vals.push_back(value_to_json(t.get_value(c)));
      }
      decoded.push_back({
          {"slot", i},
          {"offset", slot ? slot->offset : 0},
          {"values", vals}
      });
    }
    j["decoded_tuples"] = decoded;
  }

  return j;
}

std::string StorageInspector::get_page_detail(page_id_t page_id, const std::string& owner) {
  PageGuard guard = buffer_pool_.fetch_page_guard(page_id);
  if (!guard) {
    return nlohmann::json{{"error", "Failed to fetch page " + std::to_string(page_id)}}.dump();
  }

  PageHeader header = load_page_layout<PageHeader>(guard);
  std::string resolved_owner = owner.empty() ? resolve_page_owner(header.table_id) : owner;

  nlohmann::json j;
  if (header.page_type == PageType::DATA) {
    j = slotted_page_to_json(page_id, guard, resolved_owner);
  } else {
    j["page_id"] = page_id;
  }

  j["page_header"] = {
      {"type", page_type_to_string(header.page_type)},
      {"table_id", header.table_id},
      {"lsn", header.page_lsn},
      {"prev_page_id", header.prev_page_id},
      {"next_page_id", header.next_page_id}
  };
  j["owner"] = resolved_owner;

  if (header.page_type == PageType::DATABASE_HEADER) {
    DatabaseMetadata meta = load_page_body<DatabaseMetadata>(guard);
    j["database_metadata"] = {
        {"version", meta.version},
        {"page_size", meta.page_size},
        {"table_count", meta.table_count},
        {"next_table_id", meta.next_table_id},
        {"page_count", meta.page_count},
        {"free_page_list_head_page", meta.free_page_list_head_page},
        {"sys_tables_first_page", meta.sys_tables_first_page},
        {"sys_columns_first_page", meta.sys_columns_first_page}
    };
  }

  j["hex"] = bytes_to_hex(guard.get_data(), 64);
  return j.dump();
}

std::string StorageInspector::get_table_chain(const std::string& table_name) {
  auto meta = catalog_manager_.get_table(table_name);
  if (!meta) {
    return nlohmann::json{{"error", "Table not found: " + table_name}}.dump();
  }

  nlohmann::json pages = nlohmann::json::array();
  page_id_t curr = meta->get().first_page_id;
  while (curr != INVALID_PAGE_ID) {
    PageGuard guard = buffer_pool_.fetch_page_guard(curr);
    if (!guard) {
      break;
    }

    PageHeader header = load_page_layout<PageHeader>(guard);
    SlottedPage sp = SlottedPage::from_page(guard);
    pages.push_back({
        {"page_id", curr},
        {"prev_page_id", header.prev_page_id},
        {"next_page_id", header.next_page_id},
        {"num_slots", sp.get_num_slots()},
        {"free_space", sp.get_free_space()}
    });
    curr = header.next_page_id;
  }

  nlohmann::json result;
  result["table_name"] = meta->get().name;
  result["table_id"] = meta->get().table_id;
  result["first_page_id"] = meta->get().first_page_id;
  result["last_page_id"] = meta->get().last_page_id;
  result["pages"] = pages;
  return result.dump();
}

std::string StorageInspector::get_catalog() {
  return get_table_chain(SYS_TABLES_NAME);
}

std::string StorageInspector::resolve_page_owner(uint16_t table_id) {
  if (table_id == 0) {
    return "";
  }
  if (table_id == SYS_TABLES_TABLE_OID) {
    return SYS_TABLES_NAME;
  }
  if (table_id == SYS_COLUMNS_TABLE_OID) {
    return SYS_COLUMNS_NAME;
  }

  auto db_meta = load_page_body_value<DatabaseMetadata>(buffer_pool_, DATABASE_HEADER_PAGE_ID);
  if (!db_meta) {
    return "";
  }

  Schema st_schema = CatalogManager::sys_tables_schema();
  page_id_t curr = db_meta->sys_tables_first_page;
  while (curr != INVALID_PAGE_ID) {
    PageGuard guard = buffer_pool_.fetch_page_guard(curr);
    if (!guard) {
      break;
    }
    SlottedPage sp = SlottedPage::from_page(guard);
    for (uint16_t slot = 0; slot < sp.get_num_slots(); ++slot) {
      uint32_t sz = 0;
      const Byte* raw = sp.get_tuple(slot, &sz);
      if (!raw) {
        continue;
      }
      Tuple t = Tuple::deserialize(st_schema, raw, sz);
      uint32_t oid = static_cast<uint32_t>(std::get<int32_t>(t.get_value(0)));
      if (oid == table_id) {
        return std::get<std::string>(t.get_value(1));
      }
    }
    PageHeader header = load_page_layout<PageHeader>(guard);
    curr = header.next_page_id;
  }

  return "";
}

}
