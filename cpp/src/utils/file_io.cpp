#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

namespace fs = std::filesystem;

SafeFile::SafeFile(const fs::path& path, const char* mode)
    : fp(std::fopen(path.string().c_str(), mode)), file_path(path) {
    if (fp == nullptr) {
        throw std::runtime_error(
            std::format("Failed to open file {}.", path.string())
        );
    }
}

SafeFile::~SafeFile() {
    if (fp != nullptr) {
        int return_val = std::fclose(fp);
        // Maybe should log something if fclose fail.
    }
}

SafeFile::SafeFile(SafeFile&& other) noexcept : fp(other.fp), file_path(other.file_path) {
    other.fp = nullptr;
    other.file_path = fs::path();
}

SafeFile& SafeFile::operator=(SafeFile&& other) noexcept {
    if (this != &other) {
        if (fp != nullptr) {
            std::fclose(fp);
        }
        fp = other.fp;
        file_path = other.file_path;
        other.fp = nullptr;
        other.file_path = fs::path();
    }
    return *this;
}

FILE* SafeFile::get() const noexcept {
    return fp;
}

// Manual close
void SafeFile::close() {
    if (fp == nullptr) return;
    FILE* tmp = fp;
    fp = nullptr;
    if (std::fclose(tmp) != 0) {
        throw std::runtime_error(std::format(
            "Failed to close file {}.",
            file_path.string()
        ));
    }
}

bool SafeFile::fread(
    void* const buffer,
    const std::size_t size,
    const std::size_t count,
    bool throw_on_eof
) {
    // Counting bytes rather than items tells a clean end of file (nothing
    // read) from a truncated one without an ftell per call.
    const std::size_t bytes = size * count;
    const std::size_t got = std::fread(buffer, 1, bytes, fp);
    if (got != bytes) {
        if ((!throw_on_eof) && got == 0) return false;
        throw std::runtime_error(std::format(
            "Failed to read file {}.",
            file_path.string()
        ));
    }
    return true;
}

bool SafeFile::fwrite(void* const buffer, const std::size_t size, const std::size_t count) {
    std::size_t arg_count = std::fwrite(buffer, size, count, fp);
    if (arg_count != count) {
        throw std::runtime_error(std::format(
            "Failed to write file {}.",
            file_path.string()
        ));
    }
    return true;
}

BufferedWriter::BufferedWriter(const fs::path& _path, const size_t _buf_size)
    : fd(-1), file_path(_path), capacity(_buf_size), ptr(0), flushed(0) {
    if (_buf_size < MIN_BUF_SIZE) throw std::runtime_error(std::format(
        "Failed to initialize BufferedWriter: _buf_size {} is too small (min. {}).",
        _buf_size, MIN_BUF_SIZE
    ));
    fd = open(file_path.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1) {
        throw std::runtime_error(std::format("Failed to open file {}.", file_path.string()));
    }
    buffer.reset(new unsigned char[capacity]);
}

void BufferedWriter::write_all(const unsigned char* data, size_t n) {
    while (n > 0) {
        ssize_t written = ::write(fd, data, n);
        if (written < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::format("Failed to write file {}.", file_path.string()));
        }
        data += written;
        n -= static_cast<size_t>(written);
    }
}

void BufferedWriter::flush() {
    if (ptr == 0) return;
    write_all(buffer.get(), ptr);
    flushed += ptr;
    ptr = 0;
}

void BufferedWriter::fwrite(const void* const src, const size_t n, const size_t count) {
    const size_t bytes = n * count;
    const unsigned char* data = static_cast<const unsigned char*>(src);
    if (bytes > capacity - ptr) {
        flush();
        // Too big to be worth copying through the buffer.
        if (bytes >= capacity) {
            write_all(data, bytes);
            flushed += bytes;
            return;
        }
    }
    // Only transmittable between machines of the same byte order.
    std::memcpy(buffer.get() + ptr, data, bytes);
    ptr += bytes;
}

BufferedWriter::~BufferedWriter() {
    try {
        flush();
    } catch (const std::runtime_error&) {
        // Maybe should log something if the final flush fails.
    }
    if (fd != -1) ::close(fd);
}

const long BufferedWriter::ftell() {
    return static_cast<long>(flushed + ptr);
}

BufferedReader::BufferedReader(const fs::path& _path, const size_t buf_size)
    : fd(-1), file_path(_path), capacity(buf_size), pos(0), end(0), eof(false) {
    if (buf_size < MIN_READ_BUFFER_SIZE) throw std::runtime_error(std::format(
        "Failed to initialize BufferedReader: buf_size {} is too small (min. {}).",
        buf_size, MIN_READ_BUFFER_SIZE
    ));
    fd = open(file_path.string().c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        throw std::runtime_error(std::format("Failed to open file {}.", file_path.string()));
    }
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    buffer.reset(new unsigned char[capacity]);
}

BufferedReader::~BufferedReader() {
    if (fd != -1) ::close(fd);
}

BufferedReader::BufferedReader(BufferedReader&& other) noexcept
    : fd(other.fd), file_path(std::move(other.file_path)), buffer(std::move(other.buffer)),
      capacity(other.capacity), pos(other.pos), end(other.end), eof(other.eof) {
    other.fd = -1;
}

BufferedReader& BufferedReader::operator=(BufferedReader&& other) noexcept {
    if (this != &other) {
        if (fd != -1) ::close(fd);
        fd = other.fd;
        file_path = std::move(other.file_path);
        buffer = std::move(other.buffer);
        capacity = other.capacity;
        pos = other.pos;
        end = other.end;
        eof = other.eof;
        other.fd = -1;
    }
    return *this;
}

const fs::path& BufferedReader::path() const noexcept {
    return file_path;
}

bool BufferedReader::refill() {
    if (eof) return false;
    const size_t unread = end - pos;
    if (pos > 0) {
        std::memmove(buffer.get(), buffer.get() + pos, unread);
        pos = 0;
        end = unread;
    }
    // Callers refill only with fewer unread bytes than a VBE value, so
    // there is always room after them.
    while (true) {
        ssize_t got = ::read(fd, buffer.get() + end, capacity - end);
        if (got > 0) {
            end += static_cast<size_t>(got);
            return true;
        }
        if (got == 0) {
            eof = true;
            return false;
        }
        if (errno != EINTR) {
            throw std::runtime_error(std::format("Failed to read file {}.", file_path.string()));
        }
    }
}

bool BufferedReader::fread_refill(void* const dst, const size_t bytes) {
    unsigned char* out = static_cast<unsigned char*>(dst);
    size_t copied = 0;
    while (copied < bytes) {
        if (pos == end) {
            if (bytes - copied >= capacity && !eof) {
                // Large remainder: read straight into dst instead of through the buffer.
                ssize_t got = ::read(fd, out + copied, bytes - copied);
                if (got > 0) {
                    copied += static_cast<size_t>(got);
                    continue;
                }
                if (got == 0) eof = true;
                else if (errno == EINTR) continue;
                else throw std::runtime_error(std::format("Failed to read file {}.", file_path.string()));
            }
            else {
                refill();
            }
            if (pos == end) {
                if (copied == 0) return false;
                throw std::runtime_error(std::format(
                    "Unexpected end of file {}: record cut short ({} of {} bytes).",
                    file_path.string(), copied, bytes
                ));
            }
        }
        const size_t take = std::min(bytes - copied, end - pos);
        std::memcpy(out + copied, buffer.get() + pos, take);
        pos += take;
        copied += take;
    }
    return true;
}

bool BufferedReader::read_vbe(unsigned long long& value) {
    while (true) {
        const size_t available = end - pos;
        const size_t used = vbe_decode_from(buffer.get() + pos, available, value);
        if (used > 0) {
            pos += used;
            return true;
        }
        if (!refill()) {
            if (available == 0) return false;
            throw std::runtime_error(std::format(
                "Unexpected end of file {}: VBE value cut short.",
                file_path.string()
            ));
        }
    }
}

SafeFileMmap::SafeFileMmap(fs::path _file_path) :file_path(_file_path) {
    int fd = open(file_path.string().c_str(), O_RDONLY, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        throw std::runtime_error(std::format(
            "Error opening file {}", file_path.string()
        ));
    }

    if (fstat(fd,&sb) == -1) {
        close(fd);
        throw std::runtime_error(std::format(
            "Error getting file size for {}", file_path.string()
        ));
    }
    if (sb.st_size == 0) {
        close(fd);
        throw std::runtime_error(std::format(
            "SafeFileMmap disallow opening empty file {}", file_path.string()
        ));
    }

    data_ptr = static_cast<unsigned char*>(
        mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0)
    );
    close(fd);

    if (data_ptr == MAP_FAILED) {
        throw std::runtime_error(std::format(
            "Error while memory-mapping file {}", file_path.string()
        ));
    }

}

SafeFileMmap::~SafeFileMmap() {
    if (data_ptr != NULL) {
        munmap(data_ptr, sb.st_size);
    }
}

SafeFileMmap::SafeFileMmap(SafeFileMmap&& other) noexcept: sb(other.sb), data_ptr(other.data_ptr), file_path(other.file_path) {
    other.data_ptr = nullptr;
    other.file_path = fs::path();
}

SafeFileMmap& SafeFileMmap::operator=(SafeFileMmap&& other) noexcept {
    if (this != &other) {
        if (data_ptr != nullptr) {
            munmap(data_ptr, sb.st_size);
        }
        data_ptr = other.data_ptr;
        sb = other.sb;
        file_path = other.file_path;

        other.data_ptr = nullptr;
        other.file_path = fs::path();
    }
    return *this;
}

unsigned char SafeFileMmap::operator[](size_t idx) const {
    if (idx >= sb.st_size) {
        throw std::runtime_error(std::format(
            "Index {} out of bound for object SafeFileMmap.",
            idx
        ));
    }
    return data_ptr[idx];
}

const unsigned char* SafeFileMmap::data() const noexcept {
    return data_ptr;
}

size_t SafeFileMmap::size() const noexcept {
    return static_cast<size_t>(sb.st_size);
}

std::vector<fs::path> glob_files(
    const fs::path dir,
    const std::string prefix,
    const std::string extension
) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()
            && entry.path().filename().string().starts_with(prefix)
            && entry.path().extension() == extension) {
            files.push_back(entry.path());
        }
    }
    sort(files.begin(), files.end());
    return files;
}
