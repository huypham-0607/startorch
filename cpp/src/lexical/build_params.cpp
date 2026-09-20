#include "startorch/lexical/build_params.h"

#include <format>
#include <stdexcept>

void BuildParams::validate() const {
    // Written as !(in range) so NaN fails too.
    if (!(k1 > 0.0f && k1 <= 5.0f)) {
        throw std::invalid_argument(std::format("k1 = {} is out of range (0, 5].", k1));
    }
    if (!(b >= 0.0f && b <= 1.0f)) {
        throw std::invalid_argument(std::format("b = {} is out of range [0, 1].", b));
    }
    if (block_size <= 0) {
        throw std::invalid_argument(std::format("block_size = {} must be positive.", block_size));
    }
    if (split_size == 0) {
        throw std::invalid_argument("split_size must be positive.");
    }
    if (mem_limit == 0) {
        throw std::invalid_argument("mem_limit must be positive.");
    }
}
