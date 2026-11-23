#include "buffer/page_guard.h"
#include <utility>
#include "buffer/buffer_pool_manager.h"
#include "storage/page.h"

namespace letty {

PageGuard::PageGuard() = default;

PageGuard::PageGuard(BufferPoolManager* bpm, Page* page)
    : bpm_(bpm), page_(page) {}

PageGuard::~PageGuard() {
  drop();
}

PageGuard::PageGuard(PageGuard&& other) noexcept
    : bpm_(other.bpm_), page_(other.page_) {
  other.bpm_ = nullptr;
  other.page_ = nullptr;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
  if (this != &other) {
    drop();
    bpm_ = other.bpm_;
    page_ = other.page_;
    other.bpm_ = nullptr;
    other.page_ = nullptr;
  }
  return *this;
}

void PageGuard::drop() {
  if (bpm_ != nullptr && page_ != nullptr) {
    bpm_->unpin_page(page_->get_page_id(), false);
    bpm_ = nullptr;
    page_ = nullptr;
  }
}

bool PageGuard::is_valid() const {
  return page_ != nullptr && bpm_ != nullptr;
}

PageGuard::operator bool() const {
  return is_valid();
}

page_id_t PageGuard::get_page_id() const {
  assert(page_ != nullptr);
  return page_->get_page_id();
}

char* PageGuard::get_data() {
  assert(page_ != nullptr);
  return page_->get_data();
}

const char* PageGuard::get_data() const {
  assert(page_ != nullptr);
  return page_->get_data();
}

char* PageGuard::get_body() {
  assert(page_ != nullptr);
  return page_->get_body();
}

const char* PageGuard::get_body() const {
  assert(page_ != nullptr);
  return page_->get_body();
}

void PageGuard::mark_dirty() {
  if (bpm_ != nullptr && page_ != nullptr) {
    bpm_->mark_page_dirty(page_->get_page_id());
  }
}

bool PageGuard::is_dirty() const {
  if (bpm_ == nullptr || page_ == nullptr) {
    return false;
  }
  return bpm_->is_page_dirty(page_->get_page_id());
}

Page* PageGuard::get_page() {
  return page_;
}

const Page* PageGuard::get_page() const {
  return page_;
}

}
