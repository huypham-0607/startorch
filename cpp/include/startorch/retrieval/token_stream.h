#ifndef TOKEN_STREAM_H
#define TOKEN_STREAM_H

#include "startorch/utils/file_io.h"
#include <string>

constexpr unsigned int MAX_TERM_LENGTH = (1<<15); // Might change depending on dataset

/**
 * @brief Read a single doc_id - term pair.
 *
 * token_stream is a binary stream, with layout:
 *
 *      <doc_id><term_length><term_value>...
 *
 * Each character in term_value is 1 byte (ASCII)
 *
 * @return true if a record was read, false on a clean end of stream
 */
bool read_token(
    const SafeFile& token_stream,
    unsigned long long* const ptr_doc_id,
    std::string* const ptr_term
);

// Same record format and end-of-stream rules, read through a BufferedReader.
// This is the overload the build uses.
bool read_token(
    BufferedReader& token_stream,
    unsigned long long* const ptr_doc_id,
    std::string* const ptr_term
);

#endif
