#include "storage/config.h"
#include "storage_inspector.h"
#include "storage/storage_def.h"
#include "storage/slotted_page.h"
#include "storage/tuple.h"
#include "storage/page_utils.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>

namespace letty {

StorageInspector::StorageInspector(BufferPoolManager& bpm, ExtentManager& em,
                                   IamManager& iam, CatalogManager& cm)
    : buffer_pool_(bpm), extent_manager_(em),
      iam_manager_(iam), catalog_manager_(cm) {}

std::string StorageInspector::get_summary() {
    nlohmann::json j;
    
    page_id_t physical_pages = buffer_pool_.get_file_size_in_pages();
    j["total_pages"] = physical_pages;
    j["physical_pages"] = physical_pages;
    
    uint32_t allocated_extents = 0;
    uint32_t total_extents = 0;
    uint32_t highest_allocated_extent = 0;
    bool has_allocated_extent = false;
    
    page_id_t curr_gam = FIRST_GAM_PAGE_ID;
    while (curr_gam != INVALID_PAGE_ID) {
        Page* page = buffer_pool_.fetch_page(curr_gam);
        if (page) {
            auto gam_page = load_page_layout<GAMPage>(page);

            Bitmap bitmap(gam_page.bitmap, GAM_BITMAP_ARRAY_SIZE * 8);
            
            for (size_t i = 0; i < bitmap.get_size_in_bits(); ++i) {
                if (bitmap.is_set(i)) {
                    allocated_extents++;
                    highest_allocated_extent = total_extents + static_cast<uint32_t>(i);
                    has_allocated_extent = true;
                }
            }

            total_extents += (GAM_BITMAP_ARRAY_SIZE * 8);
            
            page_id_t next = gam_page.next_page_id;
            buffer_pool_.unpin_page(curr_gam, false);
            curr_gam = next;
        } else {
            break; 
        }
    }
    
    j["allocated_extents"] = allocated_extents;
    uint32_t physical_extents = (physical_pages + EXTENT_SIZE - 1) / EXTENT_SIZE;
    uint32_t logical_extents = has_allocated_extent ? highest_allocated_extent + 1 : physical_extents;
    logical_extents = std::max(logical_extents, physical_extents);
    page_id_t logical_pages = static_cast<page_id_t>(logical_extents * EXTENT_SIZE);
    j["logical_pages"] = logical_pages;
    j["percent_full"] = logical_extents > 0 ? (100.0 * allocated_extents / logical_extents) : 0.0;
    
    j["extent_manager"] = {
        {"total_extents", total_extents},
        {"file_extents", physical_extents},
        {"logical_extents", logical_extents},
        {"used_extents", allocated_extents},
        {"free_extents", total_extents > allocated_extents ? total_extents - allocated_extents : 0}
    };
    
    // Cache stats
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

std::string StorageInspector::get_gam() {
    nlohmann::json result = nlohmann::json::array();
    
    page_id_t curr_gam = FIRST_GAM_PAGE_ID;
    page_id_t physical_pages = buffer_pool_.get_file_size_in_pages();
    int gam_idx = 0;

    // We also want to know who owns these extents to color them.
    // This is expensive (scan all IAMs), but useful.
    // For now, get_gam just returns the bitmap status (ALLOCATED/FREE).
    // The frontend can combine this with get_page_map info or we can do it here.
    // Let's just return the bitmap status first.

    while (curr_gam != INVALID_PAGE_ID) {
        Page* page = buffer_pool_.fetch_page(curr_gam);
        if (!page) break;

	  	auto gam_page = load_page_layout<GAMPage>(page);
        Bitmap bitmap(gam_page.bitmap, GAM_BITMAP_ARRAY_SIZE * 8);
        
        std::vector<int> allocation;
        size_t physical_extents = (physical_pages + EXTENT_SIZE - 1) / EXTENT_SIZE;
        size_t logical_extents = physical_extents;
        for (size_t i = 0; i < bitmap.get_size_in_bits(); ++i) {
            if (bitmap.is_set(i)) {
                logical_extents = std::max(logical_extents, i + 1);
            }
        }

        size_t limit = std::min(static_cast<size_t>(bitmap.get_size_in_bits()), logical_extents);
        
        for (size_t i = 0; i < limit; ++i) {
            allocation.push_back(bitmap.is_set(i) ? 1 : 0);
        }

        nlohmann::json gam_node;
        gam_node["page_id"] = curr_gam;
        gam_node["index"] = gam_idx++;
        gam_node["allocation"] = allocation; // Arrays of 0/1
        result.push_back(gam_node);

        page_id_t next = gam_page.next_page_id;
        buffer_pool_.unpin_page(curr_gam, false);
        curr_gam = next;
    }
    
    return result.dump();
}

std::string StorageInspector::get_page_map() {
    page_id_t num_pages = buffer_pool_.get_file_size_in_pages();
    
    // structure: {id, type, owner, object_name}
    std::vector<nlohmann::json> map(num_pages);
    for(page_id_t i=0; i<num_pages; ++i) {
        map[i] = {
            {"id", i},
            {"type", "UNKNOWN"}, 
            {"owner", "free"},
            {"info", ""}
        };
    }

    // Header
    if (num_pages > 0) {
        map[0]["type"] = "HEADER";
        map[0]["owner"] = "system";
        map[0]["info"] = "Database Header";
    }

    // GAM pages
    page_id_t curr_gam = FIRST_GAM_PAGE_ID;
    while (curr_gam != INVALID_PAGE_ID && curr_gam < num_pages) {
         map[curr_gam]["type"] = "GAM";
         map[curr_gam]["owner"] = "system";
         map[curr_gam]["info"] = "Global Allocation Map";
         
         Page* p = buffer_pool_.fetch_page(curr_gam);
         if (p) {
             auto gam_page = load_page_layout<GAMPage>(p);
             page_id_t next = gam_page.next_page_id;
             buffer_pool_.unpin_page(curr_gam, false);
             curr_gam = next;
         } else {
             break;
         }
    }

    page_id_t sys_tables_iam = INVALID_PAGE_ID;
    page_id_t sys_cols_iam = INVALID_PAGE_ID;

    {
        Page* db_header_frame = buffer_pool_.fetch_page(HEADER_PAGE_ID);
        if (db_header_frame) {
            auto db_header_page = load_page_layout<DatabaseHeader>(db_header_frame);
            sys_tables_iam = db_header_page.sys_tables_iam_page;
            sys_cols_iam = db_header_page.sys_columns_iam_page;
            buffer_pool_.unpin_page(0, false);
        }
    }

    // Helper to walk IAM chain and mark pages
    auto mark_table_pages = [&](page_id_t iam_head, const std::string& name) {
        if (iam_head == INVALID_PAGE_ID) return;

        // Mark the IAM pages themselves
        page_id_t curr_iam = iam_head;
        while(curr_iam != INVALID_PAGE_ID && curr_iam < num_pages) {
             map[curr_iam]["type"] = "IAM";
             map[curr_iam]["owner"] = name;
             map[curr_iam]["info"] = "IAM Page";

             // Unused pages of the IAM extent are reserved for chain growth.
             // Later chain pages override this with type IAM as the walk continues.
             page_id_t iam_extent_start = (curr_iam / EXTENT_SIZE) * EXTENT_SIZE;
             for(int i=0; i<EXTENT_SIZE; ++i) {
                 page_id_t pid = iam_extent_start + i;
                 if (pid < num_pages && map[pid]["type"] == "UNKNOWN") {
                     map[pid]["type"] = "IAM_RESERVED";
                     map[pid]["owner"] = name;
                 }
             }
             
             Page* raw_page = buffer_pool_.fetch_page(curr_iam);
             if (!raw_page) break;
             
             auto iam_page = load_page_layout<IAMPage>(raw_page);
             
             // Mark all extents owned by this IAM page
             for(uint16_t i=0; i<iam_page.extent_count; ++i) {
                 uint32_t extent_id = iam_page.extent_ids[i];
                 page_id_t start_page = extent_id * EXTENT_SIZE;
                 for(int k=0; k<EXTENT_SIZE; ++k) {
                     page_id_t pid = start_page + k;
                     if(pid < num_pages) {
                         map[pid]["type"] = "DATA";
                         map[pid]["owner"] = name;
                     }
                 }
             }
             
             page_id_t next = iam_page.next_page_id;
             buffer_pool_.unpin_page(curr_iam, false);
             curr_iam = next;
        }
    };

    // System tables
    mark_table_pages(sys_tables_iam, SYS_TABLES_NAME);
    mark_table_pages(sys_cols_iam, SYS_COLUMNS_NAME);

    // User tables
    // Need to read sys_tables to find them.
    // We can use the catalog manager directly if it exposes a way to list tables,
    // or we can scan sys_tables physically.
    // Since CatalogManager doesn't have "list_tables", let's scan physically using our new power.
    // Actually, scanning sys_tables physically is hard because we need to parse tuples.
    // EXCEPT! We already marked sys_tables pages.
    // Let's rely on CatalogManager::get_table() if we knew the names.
    // But we don't know the names.
    // Wait, CatalogManager::table_cache_ might have them if they were accessed? No, likely empty on start.
    
    // We MUST scan sys_tables to act as a proper inspector.
    // sys_tables is a normal heap table.
    // Iterate its IAM chain -> extents -> pages.
    // For each page, interpret as SlottedPage.
    // For each slot, deserialize Tuple.
    // Extract table name and iam_page_id (IAM head).
    
    auto process_sys_tables = [&](page_id_t iam_head) {
        if (iam_head == INVALID_PAGE_ID) return;
        
        // Define sys_tables schema specifically for deserialization
        // Schema: {oid: INT, name: VARCHAR, iam_page_id: INT, column_count: INT}
        Schema st_schema({
            Column("oid", DataType::INTEGER, 4, 0),
            Column("name", DataType::VARCHAR, 32, 4),
            Column("iam_page_id", DataType::INTEGER, 4, 36),
            Column("column_count", DataType::INTEGER, 4, 40)
        });

        page_id_t curr_iam = iam_head;
        while (curr_iam != INVALID_PAGE_ID && curr_iam < num_pages) {
            Page* raw_page = buffer_pool_.fetch_page(curr_iam);
            if(!raw_page) break;
            auto iam_page = load_page_layout<IAMPage>(raw_page);
            
            // For each extent in sys_tables
            for (uint16_t i=0; i < iam_page.extent_count; ++i) {
                uint32_t extent_id = iam_page.extent_ids[i];
                page_id_t start_page = extent_id * EXTENT_SIZE;
                 for(int k=0; k<EXTENT_SIZE; ++k) {
                     page_id_t pid = start_page + k;
                     if (pid >= num_pages) continue;
                     
                     // Read data page
                     Page* p_data = buffer_pool_.fetch_page(pid);
                     if(p_data) {
                         SlottedPage sp(p_data->get_data());
                         for (uint16_t slot = 0; slot < sp.get_num_slots(); ++slot) {
                             uint32_t tuple_size;
                             const char* tuple_ptr = sp.get_tuple(slot, &tuple_size);
                             if (!tuple_ptr) continue;
                             try {
                                 Tuple t = Tuple::deserialize(st_schema, tuple_ptr, tuple_size);
                                 if (t.size() >= 3) {
                                     std::string tname = std::get<std::string>(t.get_value(1));
                                     int32_t first_pg_int = std::get<int32_t>(t.get_value(2));
                                     page_id_t t_iam = static_cast<page_id_t>(first_pg_int);

                                     if (tname != SYS_TABLES_NAME && tname != SYS_COLUMNS_NAME) {
                                         mark_table_pages(t_iam, tname);
                                     }
                                 }
                             } catch(...) {
                                 // Invalid tuple or schema mismatch, skip
                             }
                         }
                         buffer_pool_.unpin_page(pid, false);
                     }
                 }
            }
            page_id_t next = iam_page.next_page_id;
            buffer_pool_.unpin_page(curr_iam, false);
            curr_iam = next;
        }
    };
    
    process_sys_tables(sys_tables_iam);

    // Final pass: Mark free pages (those still UNKNOWN) as FREE if they are in a FREE extent in GAM?
    // Or just leave them as UNKNOWN (which implies unallocated).
    // Actually, if we didn't mark them, they aren't in any IAM or Metadata pool or GAM.
    // So they are effectively free (or 'leaked' if we have a bug).
    // We can check GAM to confirm.
    
    // For visualization, turn "UNKNOWN" into "FREE".
    for(auto& node : map) {
        if (node["type"] == "UNKNOWN") {
            node["type"] = "FREE";
            node["owner"] = "free";
        }
    }

    nlohmann::json j_array = map;
    return j_array.dump();
}

std::string StorageInspector::get_page_detail(page_id_t page_id, const std::string& owner) {
    nlohmann::json j;
    j["id"] = page_id;
    
    Page* raw_page = buffer_pool_.fetch_page(page_id);
    if (!raw_page) {
        j["error"] = "Failed to fetch page";
        return j.dump();
    }
    
    // Hex dump
    j["hex"] = bytes_to_hex(raw_page->get_data(), PAGE_SIZE);
    
    // Attempt to decode structure based on what it handles.
    auto slotted_page_hdr = load_page_layout<SlottedPageHeader>(raw_page);
    
    bool looks_like_slotted = (slotted_page_hdr.num_slots < 4096 && slotted_page_hdr.free_space_pointer <= PAGE_SIZE);
    
    if (looks_like_slotted) {
        j["header"] = {
            {"num_slots", slotted_page_hdr.num_slots},
            {"free_space_ptr", slotted_page_hdr.free_space_pointer},
            {"lsn", slotted_page_hdr.lsn}
        };
        
        nlohmann::json slots = nlohmann::json::array();
        nlohmann::json decoded_tuples = nlohmann::json::array();
        
        // Resolve schema if owner is provided and valid
        Schema schema;
        bool has_schema = false;

        if (!owner.empty() && owner != "free" && owner != "system") {
            if (owner == SYS_TABLES_NAME) {
                schema = CatalogManager::sys_tables_schema();
                has_schema = true;
            } else if (owner == SYS_COLUMNS_NAME) {
                schema = CatalogManager::sys_columns_schema();
                has_schema = true;
            } else {
                const TableMetadata* table_meta = catalog_manager_.get_table(owner);
                if (table_meta) {
                    schema = table_meta->schema;
                    has_schema = true;
                }
            }
        }
        
        for(uint16_t i=0; i<slotted_page_hdr.num_slots; ++i) {
             size_t slot_offset = sizeof(SlottedPageHeader) + (i * sizeof(Slot));
             if (slot_offset + sizeof(Slot) <= PAGE_SIZE) {
                 auto slot = load_page_layout<Slot>(raw_page, slot_offset);
                 slots.push_back({
                     {"index", i},
                     {"offset", slot.offset},
                     {"length", slot.length}
                 });
                 
                 // Try to decode tuple
                 if (has_schema && slot.length > 0) {
                     uint32_t offset = slot.offset;
                     if (offset < PAGE_SIZE && offset + slot.length <= PAGE_SIZE) {
                         try {
                             Tuple t = Tuple::deserialize(schema, raw_page->get_data() + offset, slot.length);
                             
                             nlohmann::json val_arr = nlohmann::json::array();
                             for(size_t k=0; k<t.size(); ++k) {
                                  const auto& val = t.get_value(k);
                                  std::visit([&](auto&& arg) {
                                      using T = std::decay_t<decltype(arg)>;
                                      if constexpr (std::is_same_v<T, std::monostate>) {
                                          val_arr.push_back(nullptr);
                                      } else {
                                          val_arr.push_back(arg);
                                      }
                                  }, val);
                             }
                             decoded_tuples.push_back({
                                 {"slot", i},
                                 {"offset", offset},
                                 {"values", val_arr}
                             });
                         } catch(...) {
                             // decoding failed
                         }
                     }
                 }
             }
        }
        j["slots"] = slots;
        j["decoded_tuples"] = decoded_tuples;
    }

    // Special handling for GAM page
    if (page_id == FIRST_GAM_PAGE_ID) { // Or other GAMs in chain
         // TODO: Decode GAM fields
         j["type_hint"] = "GAM";
    }

    buffer_pool_.unpin_page(page_id, false);
    return j.dump();
}

std::string StorageInspector::get_iam_chain(const std::string& table_name) {
    const TableMetadata* meta = catalog_manager_.get_table(table_name);
    if (!meta) return "{}";
    
    nlohmann::json chain = nlohmann::json::array();
    
    page_id_t curr = meta->iam_page_id;
    while(curr != INVALID_PAGE_ID) {
        Page* p = buffer_pool_.fetch_page(curr);
        if(!p) break;
        
        auto iam = load_page_layout<IAMPage>(p);
        nlohmann::json node;
        node["page_id"] = curr;
        node["extent_count"] = iam.extent_count;
        
        std::vector<uint32_t> exts;
        for(int i=0; i<iam.extent_count; ++i) exts.push_back(iam.extent_ids[i]);
        node["extents"] = exts;
        
        chain.push_back(node);
        
        page_id_t next = iam.next_page_id;
        buffer_pool_.unpin_page(curr, false);
        curr = next;
    }
    
    return chain.dump();
}

std::string StorageInspector::get_catalog() {
    // Just dump sys_tables
    return get_iam_chain(SYS_TABLES_NAME);
}

std::string StorageInspector::bytes_to_hex(const char* data, uint32_t size) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    // Group by 16 bytes
    for (uint32_t i = 0; i < size; ++i) {
        oss << std::setw(2) << (static_cast<unsigned int>(data[i]) & 0xFF) << " ";
        if ((i + 1) % 16 == 0) oss << "\n";
    }
    return oss.str();
}

nlohmann::json StorageInspector::slotted_page_to_json(page_id_t id, const char* data) {
    return {};
}

std::string StorageInspector::resolve_page_owner(page_id_t page_id) {
    return "unknown";
}

} // namespace letty
