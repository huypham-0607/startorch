#ifndef FILE_IO_H
#define FILE_IO_H

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

namespace fs = std::filesystem;

/**
 * @brief C style FileIO wrapper for safe file descriptor handling.
 *
 */

// Smallest BufferedWriter buffer.
const size_t MIN_BUF_SIZE = (1<<16);

// Default buffer sizes. Buffers are heap-allocated once per reader or writer:
// a stack array this large would overflow the default 8 MB stack, and the
// merge keeps one reader per partial block open at the same time.
constexpr size_t READ_BUFFER_SIZE = size_t(64) << 20;         // sequential passes over large files
constexpr size_t STREAM_READ_BUFFER_SIZE = size_t(4) << 20;   // merge: one reader per partial block
constexpr size_t WRITE_BUFFER_SIZE = size_t(64) << 20;

// Smallest BufferedReader buffer: it must hold a whole VBE value.
constexpr size_t MIN_READ_BUFFER_SIZE = 16;

class SafeFile{
public:
    SafeFile(const fs::path& path, const char* mode);
    ~SafeFile();

    SafeFile(const SafeFile&) = delete;
    SafeFile& operator=(const SafeFile&) = delete;
    SafeFile(SafeFile&& other) noexcept;
    SafeFile& operator=(SafeFile&& other) noexcept;

    FILE* get() const noexcept;
    bool fread(
        void* const buffer,
        const std::size_t size,
        const std::size_t count,
        const bool throw_on_eof = true
    );
    bool fwrite(
        void* const buffer,
        const std::size_t size,
        const std::size_t count
    );
    void close();
private:
    FILE* fp;
    fs::path file_path;
};

/**
 * @brief Buffered sequential writer. Bytes collect in one large buffer and
 * reach the file in a single write() once the buffer can't take the next
 * call's bytes, or on destruction. A call larger than the whole buffer
 * bypasses it.
 *
 * Same shape as BufferedReader: write(value) for one value, fwrite for
 * count consecutive items.
 */
class BufferedWriter{
public:
    BufferedWriter(const fs::path& path, const size_t buf_size = WRITE_BUFFER_SIZE);
    ~BufferedWriter();

    BufferedWriter(const BufferedWriter&) = delete;
    BufferedWriter& operator=(const BufferedWriter&) = delete;
    BufferedWriter(BufferedWriter&& other) = delete;
    BufferedWriter& operator=(const BufferedWriter&&) = delete;

    // Write one fixed-width value in the machine's byte order. Takes an
    // lvalue, so the width on disk is always a declared variable's type,
    // never whatever an expression happened to evaluate to.
    template <typename T>
    void write(T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "BufferedWriter::write needs a trivially copyable type");
        fwrite(&value, sizeof(T), 1);
    }

    // Write exactly count items of n bytes each, stored consecutively at src.
    void fwrite(const void* const src, const size_t n, const size_t count);

    // Bytes written so far, including those still buffered.
    const long ftell();
private:
    int fd;
    fs::path file_path;
    std::unique_ptr<unsigned char[]> buffer;
    size_t capacity;
    size_t ptr;
    size_t flushed;

    void flush();
    void write_all(const unsigned char* data, size_t n);
};

/**
 * @brief Buffered sequential reader: one read() per refill into a large
 * owned buffer, with typed and VBE reads served from memory.
 *
 * End of file: every read returns false if the file had already ended
 * before it (a clean end, between records), and throws if the file ends
 * partway through the value (a truncated file).
 */
class BufferedReader {
public:
    explicit BufferedReader(const fs::path& path, const size_t buf_size = READ_BUFFER_SIZE);
    ~BufferedReader();

    BufferedReader(const BufferedReader&) = delete;
    BufferedReader& operator=(const BufferedReader&) = delete;
    BufferedReader(BufferedReader&& other) noexcept;
    BufferedReader& operator=(BufferedReader&& other) noexcept;

    // Read one fixed-width value in the machine's byte order.
    template <typename T>
    bool read(T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "BufferedReader::read needs a trivially copyable type");
        return fread(&value, sizeof(T));
    }

    // Read exactly count items of n bytes each into consecutive memory at
    // dst. Inline fast path for bytes already buffered, which is nearly
    // every call; refills go out of line.
    bool fread(void* const dst, const size_t n, const size_t count = 1) {
        const size_t bytes = n * count;
        if (end - pos >= bytes) {
            std::memcpy(dst, buffer.get() + pos, bytes);
            pos += bytes;
            return true;
        }
        return fread_refill(dst, bytes);
    }

    // Read one VBE-encoded value.
    bool read_vbe(unsigned long long& value);

    const fs::path& path() const noexcept;

private:
    int fd;
    fs::path file_path;
    std::unique_ptr<unsigned char[]> buffer;
    size_t capacity;
    size_t pos;     // next unread byte
    size_t end;     // one past the last buffered byte
    bool eof;

    // Keep the unread bytes, moved to the front, and read more after them.
    // Returns false once the file has nothing more.
    bool refill();

    // fread when the buffer doesn't already hold all the bytes.
    bool fread_refill(void* const dst, const size_t bytes);
};

/**
 * @brief C style Mmap wrapper for safe file descriptor handling.
 *
 */
class SafeFileMmap {
public:
    SafeFileMmap(fs::path _file_path);
    ~SafeFileMmap();

    SafeFileMmap(const SafeFileMmap&) = delete;
    SafeFileMmap& operator=(const SafeFileMmap&) = delete;

    SafeFileMmap(SafeFileMmap&& other) noexcept;
    SafeFileMmap& operator=(SafeFileMmap&& other) noexcept;

    unsigned char operator[](size_t idx) const;

    // Unchecked access for hot loops that bound-check against size() themselves.
    const unsigned char* data() const noexcept;
    size_t size() const noexcept;

private:
    struct stat sb;
    unsigned char* data_ptr;
    fs::path file_path;
};

/**
 * @brief Returns list of files in a directory with matching prefix and extension.
 *
 * This does not search recursively.
 *
 * @param dir Path to directory
 * @param prefix Prefix to match
 * @param extension Extension suffix to match (including the dot)
 * @return std::vector<fs::path>
 */
std::vector<fs::path> glob_files(
    const fs::path dir,
    const std::string prefix,
    const std::string extension
);

#endif
