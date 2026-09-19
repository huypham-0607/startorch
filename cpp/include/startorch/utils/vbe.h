#ifndef VBE_H
#define VBE_H

#include <cstdlib>
#include <cstdio>

constexpr int BUFFER_LIMIT = 8;

size_t vbe_encode(unsigned long long num, unsigned char buffer[]);

unsigned long long vbe_decode(const unsigned char buffer[]);

/**
 * @brief Decode one VBE value straight from memory.
 *
 * @param buf first byte of the value
 * @param available bytes readable from buf
 * @param value decoded value, set only on success
 * @return bytes the value occupies, or 0 if its terminating byte lies beyond
 * available (the value continues past the end of this buffer)
 * @throws std::runtime_error if BUFFER_LIMIT bytes pass without a terminator
 */
size_t vbe_decode_from(const unsigned char* buf, size_t available, unsigned long long& value);

bool read_vbe(FILE* const fp, unsigned char buffer[]);

#endif