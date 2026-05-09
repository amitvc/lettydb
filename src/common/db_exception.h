#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace letty {

enum class DbErrorCode {
  Internal,
  InvalidArgument,
  ParseError,
  NotFound,
  AlreadyExists,
  UndefinedTable,
  DuplicateTable,
  ConstraintViolation,
  NotNullViolation,
  TypeMismatch,
  IOError,
  Corruption,
  NoSpace,
};

inline const char* db_error_code_to_string(DbErrorCode code) {
  switch (code) {
    case DbErrorCode::Internal:
      return "Internal";
    case DbErrorCode::InvalidArgument:
      return "InvalidArgument";
    case DbErrorCode::ParseError:
      return "ParseError";
    case DbErrorCode::NotFound:
      return "NotFound";
    case DbErrorCode::AlreadyExists:
      return "AlreadyExists";
    case DbErrorCode::UndefinedTable:
      return "UndefinedTable";
    case DbErrorCode::DuplicateTable:
      return "DuplicateTable";
    case DbErrorCode::ConstraintViolation:
      return "ConstraintViolation";
    case DbErrorCode::NotNullViolation:
      return "NotNullViolation";
    case DbErrorCode::TypeMismatch:
      return "TypeMismatch";
    case DbErrorCode::IOError:
      return "IOError";
    case DbErrorCode::Corruption:
      return "Corruption";
    case DbErrorCode::NoSpace:
      return "NoSpace";
  }

  return "Unknown";
}

class DbException : public std::runtime_error {
 public:
  DbException(DbErrorCode code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}

  DbErrorCode code() const noexcept { return code_; }

 private:
  DbErrorCode code_;
};

}  // namespace letty
