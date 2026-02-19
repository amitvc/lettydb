//
// Created by Amit Chavan on 9/12/25.
//

#pragma once

namespace  letty {
enum class IOResult {
  SUCCESS,
  FILE_NOT_OPEN,
  SEEK_ERROR,
  IO_ERROR,
  WRITE_ERROR,
  READ_ERROR,
};
}