#include "startorch/lexical/token_stream.h"

#include <cstdio>
#include <stdexcept>

bool read_token(
    const SafeFile& token_stream,
    unsigned long long* const ptr_doc_id,
    std::string* const ptr_term
) {
    // Counting bytes rather than items tells a clean end of stream from a
    // truncated doc_id without an ftell per token.
    size_t got = fread(ptr_doc_id, 1, sizeof(*ptr_doc_id), token_stream.get());
    if (got != sizeof(*ptr_doc_id)) {
        if (got == 0) return false;
        throw std::runtime_error("I/O error reading doc_id.");
    }

    unsigned short term_size;
    int arg_count = fread(&term_size, sizeof(term_size), 1, token_stream.get());
    if (arg_count != 1) throw std::runtime_error("I/O error reading term_size.");

    if (term_size > MAX_TERM_LENGTH) throw std::runtime_error("Erroneous term_size.");
    ptr_term->resize(term_size);

    arg_count = fread(&(*ptr_term)[0], sizeof(char), term_size, token_stream.get());
    if (arg_count != term_size) throw std::runtime_error("I/O error reading term_value.");

    return true;
}

bool read_token(
    BufferedReader& token_stream,
    unsigned long long* const ptr_doc_id,
    std::string* const ptr_term
) {
    if (!token_stream.read(*ptr_doc_id)) return false;

    unsigned short term_size;
    if (!token_stream.read(term_size)) throw std::runtime_error("I/O error reading term_size.");

    if (term_size > MAX_TERM_LENGTH) throw std::runtime_error("Erroneous term_size.");
    ptr_term->resize(term_size);

    if (!token_stream.fread(ptr_term->data(), sizeof(char), term_size)) throw std::runtime_error("I/O error reading term_value.");

    return true;
}
