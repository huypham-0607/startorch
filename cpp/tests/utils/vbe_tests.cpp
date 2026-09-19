#include "startorch/utils/vbe.h"
#include "startorch/utils/file_io.h"

#include <random>
#include <vector>
#include <filesystem>
#include <unistd.h>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

unsigned long long rd(unsigned long long l, unsigned long long r, std::mt19937_64 &mt) {
    return std::uniform_int_distribution<unsigned long long> (l,r) (mt);
}

fs::path makeUniqueTempDir() {
    fs::path base = fs::temp_directory_path();
    for (int i = 0; i < 100; ++i) {
        auto candidate = base / (
            "gtest_" + std::to_string(::getpid())
            + "_" + std::to_string(i)
            + "_" + std::to_string(std::rand())
        );
        if (fs::exists(candidate)) continue;
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) return candidate;
    }
    throw std::runtime_error("could not create temp dir");
}

TEST(VBETest, HandlesZero) {
    unsigned char buffer[8];
    memset(buffer,0,sizeof(buffer));
    size_t length = vbe_encode((unsigned long long) 0, buffer);

    ASSERT_EQ(length, 1);
    ASSERT_EQ(buffer[0], 128);

    unsigned long long decoded = vbe_decode(buffer);
    ASSERT_EQ(decoded, 0);
}

TEST(VBETest, Boundary) {
    unsigned long long val = (1LL<<7);
    size_t expected_length = (63 - __builtin_clzll(val))/7+1;
    
    unsigned char buffer[8];
    memset(buffer,0,sizeof(buffer));

    size_t length = vbe_encode(val,buffer);

    ASSERT_EQ(length, expected_length);
    ASSERT_EQ(buffer[0], 0);
    ASSERT_EQ(buffer[1], (1LL<<7) + 1);

    unsigned long long decoded = vbe_decode(buffer);
    ASSERT_EQ(decoded, val);
}

TEST(VBETest, MaxValue) {
    unsigned long long val = (1LL<<56)-1;
    size_t expected_length = (63 - __builtin_clzll(val))/7+1;
    
    unsigned char buffer[8];
    memset(buffer,0,sizeof(buffer));

    size_t length = vbe_encode(val,buffer);

    ASSERT_EQ(length, expected_length);
    for (int i = 0; i < 8; i++){
        ASSERT_EQ(buffer[i], (1<<7)-1 + ((i==7)?(1<<7):0));
    }

    unsigned long long decoded = vbe_decode(buffer);
    ASSERT_EQ(decoded, val);
}

TEST(VBETest, BufferOverflow) {
    unsigned long long val = (1LL<<60);
    
    unsigned char buffer[8];
    memset(buffer,0,sizeof(buffer));

    ASSERT_THROW({
        size_t length = vbe_encode(val,buffer);
    }, std::runtime_error);
}

TEST(VBETest, RandomInput) {
    std:: mt19937_64 mt(42);

    unsigned long long l = 1;
    unsigned long long r = (1LL<<56) - 1;
    int n = 10000;

    unsigned char buffer[8];
    memset(buffer,0,sizeof(buffer));
    for (int i=0; i<n; i++){
        unsigned long long val = rd(l,r,mt);
        size_t expected_length = (63 - __builtin_clzll(val))/7+1;
        size_t length = vbe_encode(val, buffer);

        ASSERT_EQ(length, expected_length);

        unsigned long long decoded = vbe_decode(buffer);
        ASSERT_EQ(decoded, val);
    }
}

TEST(ReadVbeTest, ReadsSingleByteValue) {
    fs::path tmp_path = makeUniqueTempDir();
    fs::path file_name = tmp_path / "vbe_stream.bin";

    unsigned char encode_buffer[8];
    size_t encode_len = vbe_encode(5, encode_buffer);

    {
        SafeFile out_fp(file_name, "wb");
        fwrite(encode_buffer, sizeof(unsigned char), encode_len, out_fp.get());
    }

    SafeFile in_fp(file_name, "rb");
    unsigned char decode_buffer[8];
    bool res = read_vbe(in_fp.get(), decode_buffer);

    ASSERT_TRUE(res);
    ASSERT_EQ(vbe_decode(decode_buffer), 5);

    fs::remove_all(tmp_path);
}

TEST(ReadVbeTest, ReadsMultiByteValue) {
    fs::path tmp_path = makeUniqueTempDir();
    fs::path file_name = tmp_path / "vbe_stream.bin";

    unsigned long long val = (1LL<<40) + 12345;
    unsigned char encode_buffer[8];
    size_t encode_len = vbe_encode(val, encode_buffer);

    {
        SafeFile out_fp(file_name, "wb");
        fwrite(encode_buffer, sizeof(unsigned char), encode_len, out_fp.get());
    }

    SafeFile in_fp(file_name, "rb");
    unsigned char decode_buffer[8];
    bool res = read_vbe(in_fp.get(), decode_buffer);

    ASSERT_TRUE(res);
    ASSERT_EQ(vbe_decode(decode_buffer), val);

    fs::remove_all(tmp_path);
}

TEST(ReadVbeTest, EmptyStreamReturnsFalse) {
    fs::path tmp_path = makeUniqueTempDir();
    fs::path file_name = tmp_path / "vbe_stream.bin";

    { SafeFile out_fp(file_name, "wb"); }

    SafeFile in_fp(file_name, "rb");
    unsigned char decode_buffer[8];
    bool res = read_vbe(in_fp.get(), decode_buffer);

    ASSERT_FALSE(res);

    fs::remove_all(tmp_path);
}

TEST(ReadVbeTest, TruncatedMidValueReturnsFalse) {
    fs::path tmp_path = makeUniqueTempDir();
    fs::path file_name = tmp_path / "vbe_stream.bin";

    unsigned long long val = (1LL<<40) + 12345;
    unsigned char encode_buffer[8];
    size_t encode_len = vbe_encode(val, encode_buffer);
    ASSERT_GT(encode_len, 1u);

    {
        // Write every byte except the terminator, so the stream ends
        // mid-value with no continuation-bit-set byte ever written.
        SafeFile out_fp(file_name, "wb");
        fwrite(encode_buffer, sizeof(unsigned char), encode_len - 1, out_fp.get());
    }

    SafeFile in_fp(file_name, "rb");
    unsigned char decode_buffer[8];
    bool res = read_vbe(in_fp.get(), decode_buffer);

    ASSERT_FALSE(res);

    fs::remove_all(tmp_path);
}

TEST(ReadVbeTest, ReadsSequentialEntries) {
    fs::path tmp_path = makeUniqueTempDir();
    fs::path file_name = tmp_path / "vbe_stream.bin";

    std::vector<unsigned long long> values = {0, 1, 127, 128, (1LL<<30)};

    {
        SafeFile out_fp(file_name, "wb");
        unsigned char encode_buffer[8];
        for (unsigned long long v : values) {
            size_t encode_len = vbe_encode(v, encode_buffer);
            fwrite(encode_buffer, sizeof(unsigned char), encode_len, out_fp.get());
        }
    }

    SafeFile in_fp(file_name, "rb");
    unsigned char decode_buffer[8];
    for (unsigned long long v : values) {
        bool res = read_vbe(in_fp.get(), decode_buffer);
        ASSERT_TRUE(res);
        ASSERT_EQ(vbe_decode(decode_buffer), v);
    }

    bool res = read_vbe(in_fp.get(), decode_buffer);
    ASSERT_FALSE(res);

    fs::remove_all(tmp_path);
}

TEST(VbeDecodeFromTest, RoundTripsEveryEncodedLength) {
    std::mt19937_64 mt(3);
    for (int i = 0; i < 10000; i++) {
        unsigned long long v = rd(0, (1ULL << (7 * rd(1, 8, mt))) - 1, mt);
        unsigned char buf[BUFFER_LIMIT];
        size_t n = vbe_encode(v, buf);
        unsigned long long decoded = 0;
        ASSERT_EQ(vbe_decode_from(buf, n, decoded), n);
        ASSERT_EQ(decoded, v);
        ASSERT_EQ(decoded, vbe_decode(buf)) << "must agree with vbe_decode";
    }
}

TEST(VbeDecodeFromTest, ReturnsZeroWhenValueContinuesPastAvailable) {
    unsigned char buf[BUFFER_LIMIT];
    size_t n = vbe_encode(1ULL << 30, buf);   // several bytes
    ASSERT_GT(n, 1u);
    unsigned long long decoded = 42;
    EXPECT_EQ(vbe_decode_from(buf, n - 1, decoded), 0u);
    EXPECT_EQ(decoded, 42u) << "value is left untouched when incomplete";
    EXPECT_EQ(vbe_decode_from(buf, 0, decoded), 0u);
}

TEST(VbeDecodeFromTest, ThrowsWithoutTerminatorWithinLimit) {
    unsigned char buf[BUFFER_LIMIT + 1] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    unsigned long long decoded;
    EXPECT_THROW(vbe_decode_from(buf, sizeof(buf), decoded), std::runtime_error);
}
