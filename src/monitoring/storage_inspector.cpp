#include "storage_inspector.h"
#include "storage/storage_def.h"
#include "storage/slotted_page.h"
#include "storage/tuple.h"
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
    
    // Basic stats
    page_id_t total_pages = buffer_pool_.get_file_size_in_pages();
    j["total_pages"] = total_pages;
    
    // Scan GAM for allocation stats
    // We start at page 1 and walk the chain
    uint32_t allocated_extents = 0;
    uint32_t total_extents = 0;
    
    page_id_t curr_gam = FIRST_GAM_PAGE_ID;
    while (curr_gam != INVALID_PAGE_ID && curr_gam < total_pages) {
        Page* page = buffer_pool_.fetch_page(curr_gam);
        if (page) {
            auto* gam = reinterpret_cast<GAMPage*>(page->get_data());
            Bitmap bitmap(gam->bitmap, GAM_BITMAP_ARRAY_SIZE * 8);
            
            // Count bits
            for (size_t i = 0; i < bitmap.get_size_in_bits(); ++i) {
                if (bitmap.is_set(i)) {
                    allocated_extents++;
                }
            }
            // Count capacity (approx, depends on file size vs GAM capacity)
            // But we can just report what the GAM *covers*
            total_extents += (GAM_BITMAP_ARRAY_SIZE * 8);
            
            page_id_t next = gam->next_page_id;
            buffer_pool_.unpin_page(curr_gam, false);
            curr_gam = next;
        } else {
            break; 
        }
    }
    
    j["allocated_extents"] = allocated_extents;
    j["percent_full"] = total_extents > 0 ? (100.0 * allocated_extents / total_extents) : 0.0;
    
    // Extent stats
    j["extent_manager"] = {
        {"total_extents", total_extents}, // Capacity covered by current GAMs
        {"file_extents", (total_pages + EXTENT_SIZE - 1) / EXTENT_SIZE}, // Current file size in extents
        {"used_extents", allocated_extents},
        {"free_extents", (GAM_BITMAP_ARRAY_SIZE * 8) - allocated_extents} // Approximate from GAM
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
    page_id_t total_pages = buffer_pool_.get_file_size_in_pages();
    int gam_idx = 0;

    // We also want to know who owns these extents to color them.
    // This is expensive (scan all IAMs), but useful.
    // For now, get_gam just returns the bitmap status (ALLOCATED/FREE).
    // The frontend can combine this with get_page_map info or we can do it here.
    // Let's just return the bitmap status first.

    while (curr_gam != INVALID_PAGE_ID && curr_gam < total_pages) {
        Page* page = buffer_pool_.fetch_page(curr_gam);
        if (!page) break;

        auto* gam = reinterpret_cast<GAMPage*>(page->get_data());
        Bitmap bitmap(gam->bitmap, GAM_BITMAP_ARRAY_SIZE * 8);
        
        std::vector<int> allocation;
        // Optimization: don't dump 32k bits if file is small. 
        // Only dump up to file size / 8 (extent size).
        size_t max_extent = (total_pages + EXTENT_SIZE - 1) / EXTENT_SIZE;
        // Also respect GAM coverage
        size_t limit = std::min((size_t)bitmap.get_size_in_bits(), max_extent);
        
        for (size_t i = 0; i < limit; ++i) {
            allocation.push_back(bitmap.is_set(i) ? 1 : 0);
        }

        nlohmann::json gam_node;
        gam_node["page_id"] = curr_gam;
        gam_node["index"] = gam_idx++;
        gam_node["allocation"] = allocation; // Arrays of 0/1
        result.push_back(gam_node);

        page_id_t next = gam->next_page_id;
        buffer_pool_.unpin_page(curr_gam, false);
        curr_gam = next;
    }
    
    return result.dump();
}

std::string StorageInspector::get_page_map() {
    page_id_t num_pages = buffer_pool_.get_file_size_in_pages();
    
    // Pre-allocate map with UNKNOWN
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
             auto* gp = reinterpret_cast<GAMPage*>(p->get_data());
             page_id_t next = gp->next_page_id;
             buffer_pool_.unpin_page(curr_gam, false);
             curr_gam = next;
         } else {
             break;
         }
    }

    // Metadata Pool pages
    page_id_t meta_head = INVALID_PAGE_ID;
    page_id_t sys_tables_iam = INVALID_PAGE_ID;
    page_id_t sys_cols_iam = INVALID_PAGE_ID;

    {
        Page* p0 = buffer_pool_.fetch_page(0);
        if (p0) {
            auto* header = reinterpret_cast<DatabaseHeader*>(p0->get_data());
            meta_head = header->metadata_pool_page_id;
            sys_tables_iam = header->sys_tables_iam_page;
            sys_cols_iam = header->sys_columns_iam_page;
            buffer_pool_.unpin_page(0, false);
        }
    }

    // Walk Metadata Pool
    page_id_t curr_pool = meta_head;
    while (curr_pool != INVALID_PAGE_ID && curr_pool < num_pages) {
        // Mark the entire extent (8 pages) as Metadata Pool
        // The pool is allocated in extents.
        // Wait, metadata_pool_page_id points to the *first page* of the pool extent.
        // Pools are linked via next_pool_page in the directory page (page 0 of extent).
        
        // Mark the directory page itself
        map[curr_pool]["type"] = "METADATA_DIR";
        map[curr_pool]["owner"] = "system";
        
        // Mark the other 7 pages in the extent as "METADATA_SLOT" (initially)
        // We will override them if they are used as IAM pages later
        for(int i=1; i<EXTENT_SIZE; ++i) {
             if (curr_pool + i < num_pages) {
                 map[curr_pool + i]["type"] = "METADATA";
                 map[curr_pool + i]["owner"] = "system";
             }
        }

        Page* p = buffer_pool_.fetch_page(curr_pool);
        if (p) {
            auto* mp = reinterpret_cast<MetadataPoolDirectoryPage*>(p->get_data());
            page_id_t next = mp->next_pool_page;
            buffer_pool_.unpin_page(curr_pool, false);
            curr_pool = next;
        } else {
            break;
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
             
             Page* p = buffer_pool_.fetch_page(curr_iam);
             if (!p) break;
             
             auto* iam = reinterpret_cast<IAMPage*>(p->get_data());
             
             // Mark all extents owned by this IAM page
             for(uint16_t i=0; i<iam->extent_count; ++i) {
                 uint32_t extent_id = iam->extent_ids[i];
                 page_id_t start_page = extent_id * EXTENT_SIZE;
                 for(int k=0; k<EXTENT_SIZE; ++k) {
                     page_id_t pid = start_page + k;
                     if(pid < num_pages) {
                         map[pid]["type"] = "DATA";
                         map[pid]["owner"] = name;
                     }
                 }
             }
             
             page_id_t next = iam->next_page_id;
             buffer_pool_.unpin_page(curr_iam, false);
             curr_iam = next;
        }
    };

    // System tables
    mark_table_pages(sys_tables_iam, "sys_tables");
    mark_table_pages(sys_cols_iam, "sys_columns");

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
            Page* p_iam = buffer_pool_.fetch_page(curr_iam);
            if(!p_iam) break;
            auto* iam = reinterpret_cast<IAMPage*>(p_iam->get_data());
            
            // For each extent in sys_tables
            for (uint16_t i=0; i<iam->extent_count; ++i) {
                uint32_t extent_id = iam->extent_ids[i];
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

                                     if (tname != "sys_tables" && tname != "sys_columns") {
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
            page_id_t next = iam->next_page_id;
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
    
    Page* page = buffer_pool_.fetch_page(page_id);
    if (!page) {
        j["error"] = "Failed to fetch page";
        return j.dump();
    }
    
    // Hex dump
    j["hex"] = bytes_to_hex(page->get_data(), PAGE_SIZE);
    
    // Attempt to decode structure based on what it handles
    // Manual cast to SlottedPageHeader since SlottedPage class hides it
    auto* header = reinterpret_cast<SlottedPageHeader*>(page->get_data());
    
    // Heuristic: valid LSN and reasonable slot count?
    // Note: uninitialized pages might have random junk, but usually 0s
    bool looks_like_slotted = (header->num_slots < 4096 && header->free_space_pointer <= PAGE_SIZE);
    
    if (looks_like_slotted) {
        j["header"] = {
            {"num_slots", header->num_slots},
            {"free_space_ptr", header->free_space_pointer},
            {"lsn", header->lsn}
        };
        
        nlohmann::json slots = nlohmann::json::array();
        nlohmann::json decoded_tuples = nlohmann::json::array();
        
        // Array of slots follows the header
        auto* slot_array = reinterpret_cast<Slot*>(page->get_data() + sizeof(SlottedPageHeader));
        
        // Resolve schema if owner is provided and valid
        // Resolve schema if owner is provided and valid
        Schema schema;
        bool has_schema = false;

        if (!owner.empty() && owner != "free" && owner != "system") {
            if (owner == "sys_tables") {
                schema = CatalogManager::sys_tables_schema();
                has_schema = true;
            } else if (owner == "sys_columns") {
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
        
        for(uint16_t i=0; i<header->num_slots; ++i) {
             // Check bounds
             if ((char*)&slot_array[i] + sizeof(Slot) <= page->get_data() + PAGE_SIZE) {
                 slots.push_back({
                     {"index", i},
                     {"offset", slot_array[i].offset},
                     {"length", slot_array[i].length}
                 });
                 
                 // Try to decode tuple
                 if (has_schema && slot_array[i].length > 0) {
                     uint32_t offset = slot_array[i].offset;
                     if (offset < PAGE_SIZE && offset + slot_array[i].length <= PAGE_SIZE) {
                         try {
                             Tuple t = Tuple::deserialize(schema, page->get_data() + offset, slot_array[i].length);
                             
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
        
        auto* iam = reinterpret_cast<IAMPage*>(p->get_data());
        nlohmann::json node;
        node["page_id"] = curr;
        node["extent_count"] = iam->extent_count;
        
        std::vector<uint32_t> exts;
        for(int i=0; i<iam->extent_count; ++i) exts.push_back(iam->extent_ids[i]);
        node["extents"] = exts;
        
        chain.push_back(node);
        
        page_id_t next = iam->next_page_id;
        buffer_pool_.unpin_page(curr, false);
        curr = next;
    }
    
    return chain.dump();
}

std::string StorageInspector::get_catalog() {
    // Just dump sys_tables
    return get_iam_chain("sys_tables");
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
