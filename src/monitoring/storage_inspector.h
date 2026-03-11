#pragma once

#include "buffer/buffer_pool_manager.h"
#include "storage/extent_manager.h"
#include "storage/iam_manager.h"
#include "catalog/catalog_manager.h"
#include <nlohmann/json.hpp>
#include <string>

namespace letty {

class StorageInspector {
public:
    StorageInspector(BufferPoolManager& bpm, ExtentManager& em,
                     IamManager& iam, CatalogManager& cm);

    // Main JSON endpoints
    std::string get_summary();
    std::string get_gam();
    std::string get_page_map();
    std::string get_page_detail(page_id_t page_id, const std::string& owner = "");
    std::string get_iam_chain(const std::string& table_name);
    std::string get_catalog();

private:
    BufferPoolManager& buffer_pool_;
    ExtentManager& extent_manager_;
    IamManager& iam_manager_;
    CatalogManager& catalog_manager_;

    // Helpers
    std::string bytes_to_hex(const char* data, uint32_t size);
    nlohmann::json slotted_page_to_json(page_id_t id, const char* data);
    std::string resolve_page_owner(page_id_t page_id);
};

}
