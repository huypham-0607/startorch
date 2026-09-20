#include "startorch/lexical/token_stream.h"
#include "startorch/utils/file_io.h"

#include <cstdio>
#include <filesystem>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#include <random>

namespace fs = std::filesystem;

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

namespace ReadTokenTest{
    class ReadTokenTest : public testing::Test {
    protected:

        void SetUp() override {
            tmp_path = makeUniqueTempDir();
        }

        void TearDown() override {
            fs::remove_all(tmp_path);
        }

        fs::path tmp_path;
    };

    TEST_F(ReadTokenTest, ValidInput) {
        std::vector<std::pair<unsigned long long, std::string>> v;
        v.push_back({6767, "sixseven"});
        v.push_back({2000, "y2k"});
        v.push_back({33, "maxverstappen"});
        v.push_back({(1LL<<60),"bignumber"});

        fs::path file_name = tmp_path / "token_stream.bin";

        SafeFile file_fp(file_name, "wb");

        for (int i = 0; i < v.size(); i++){
            fwrite(&v[i].first, sizeof(v[i].first), 1, file_fp.get());
            unsigned short term_length = v[i].second.size();
            fwrite(&term_length, sizeof(term_length), 1, file_fp.get());
            fwrite(v[i].second.c_str(), sizeof(char), term_length, file_fp.get());
        }

        file_fp = SafeFile(file_name, "rb");

        unsigned long long doc_id;
        std::string buffer;

        for (int i = 0; i < v.size(); i++){
            bool res;
            ASSERT_NO_THROW(res = read_token(file_fp, &doc_id, &buffer));

            ASSERT_TRUE(res);
            ASSERT_EQ(doc_id, v[i].first);
            ASSERT_EQ(buffer, v[i].second);
        }

        bool res;

        ASSERT_NO_THROW(res = read_token(file_fp, &doc_id, &buffer));
        ASSERT_FALSE(res) << "doc_id=" << doc_id << " buffer=" << buffer;
    }

    TEST_F(ReadTokenTest, EmptyStream) {
        fs::path file_name = tmp_path / "token_stream.bin";

        SafeFile file_fp(file_name, "wb");

        file_fp = SafeFile(file_name, "rb");

        unsigned long long doc_id;
        std::string buffer;
        bool res;

        ASSERT_NO_THROW(res = read_token(file_fp, &doc_id, &buffer));
        ASSERT_FALSE(res) << "doc_id=" << doc_id << " buffer=" << buffer;
    }

    TEST_F(ReadTokenTest, PartialDocId) {
        fs::path file_name = tmp_path / "token_stream.bin";

        SafeFile file_fp(file_name, "wb");

        unsigned int doc_id_4_bytes = 177013;
        fwrite(&doc_id_4_bytes, sizeof(doc_id_4_bytes), 1, file_fp.get());

        file_fp = SafeFile(file_name, "rb");

        unsigned long long doc_id;
        std::string buffer;
        bool res;

        ASSERT_THROW(res = read_token(file_fp, &doc_id, &buffer), std::runtime_error);
    }

    TEST_F(ReadTokenTest, PartialTermLength) {
        fs::path file_name = tmp_path / "token_stream.bin";

        SafeFile file_fp(file_name, "wb");

        unsigned long long valid_doc_id = 177013;
        unsigned char term_length_1_bytes = 10;
        fwrite(&valid_doc_id, sizeof(valid_doc_id), 1, file_fp.get());
        fwrite(&term_length_1_bytes, sizeof(term_length_1_bytes), 1, file_fp.get());

        file_fp = SafeFile(file_name, "rb");

        unsigned long long doc_id;
        std::string buffer;
        bool res;

        ASSERT_THROW(res = read_token(file_fp, &doc_id, &buffer), std::runtime_error);
    }

    TEST_F(ReadTokenTest, InvalidTermLength) {
        fs::path file_name = tmp_path / "token_stream.bin";

        SafeFile file_fp(file_name, "wb");

        unsigned long long valid_doc_id = 177013;
        unsigned short invalid_term_length = (1<<10);
        fwrite(&valid_doc_id, sizeof(valid_doc_id), 1, file_fp.get());
        fwrite(&invalid_term_length, sizeof(invalid_term_length), 1, file_fp.get());

        file_fp = SafeFile(file_name, "rb");

        unsigned long long doc_id;
        std::string buffer;
        bool res;

        ASSERT_THROW(res = read_token(file_fp, &doc_id, &buffer), std::runtime_error);
    }

    TEST_F(ReadTokenTest, PartialTermValue) {
        fs::path file_name = tmp_path / "token_stream.bin";

        SafeFile file_fp(file_name, "wb");

        unsigned long long valid_doc_id = 177013;
        std::string term_value = "catmemes";
        unsigned short valid_term_length = term_value.size();
        fwrite(&valid_doc_id, sizeof(valid_doc_id), 1, file_fp.get());
        fwrite(&valid_term_length, sizeof(valid_term_length), 1, file_fp.get());
        fwrite(term_value.c_str(), sizeof(char), valid_term_length - 1, file_fp.get());

        file_fp = SafeFile(file_name.string().c_str(), "rb");

        unsigned long long doc_id;
        std::string buffer;
        bool res;

        ASSERT_THROW(res = read_token(file_fp, &doc_id, &buffer), std::runtime_error);
    }

    // TODO: Add Randomized Stress Test to ReadTokenTest
}

namespace ReadTokenBufferedTest {
    class ReadTokenBufferedTest : public testing::Test {
    protected:
        void SetUp() override { tmp_path = makeUniqueTempDir(); }
        void TearDown() override { fs::remove_all(tmp_path); }
        fs::path tmp_path;
    };

    TEST_F(ReadTokenBufferedTest, MatchesSafeFileOverloadRecordForRecord) {
        fs::path path = tmp_path / "tokens.bin";
        std::mt19937_64 mt(9);
        {
            SafeFile out(path, "wb");
            for (int i = 0; i < 3000; i++) {
                unsigned long long doc_id = mt() % 1000000;
                std::string term(mt() % 30, 'a');
                for (char& c : term) c = (char)('a' + mt() % 26);
                unsigned short len = term.size();
                fwrite(&doc_id, sizeof(doc_id), 1, out.get());
                fwrite(&len, sizeof(len), 1, out.get());
                fwrite(term.data(), 1, term.size(), out.get());
            }
        }

        SafeFile legacy(path, "rb");
        BufferedReader buffered(path, MIN_READ_BUFFER_SIZE);
        unsigned long long legacy_id, buffered_id;
        std::string legacy_term, buffered_term;
        int records = 0;
        while (read_token(legacy, &legacy_id, &legacy_term)) {
            ASSERT_TRUE(read_token(buffered, &buffered_id, &buffered_term));
            ASSERT_EQ(buffered_id, legacy_id);
            ASSERT_EQ(buffered_term, legacy_term);
            ++records;
        }
        EXPECT_EQ(records, 3000);
        EXPECT_FALSE(read_token(buffered, &buffered_id, &buffered_term));
    }

    TEST_F(ReadTokenBufferedTest, TruncatedRecordThrowsAndEmptyStreamReturnsFalse) {
        fs::path empty = tmp_path / "empty.bin";
        { SafeFile out(empty, "wb"); }
        BufferedReader empty_in(empty, MIN_READ_BUFFER_SIZE);
        unsigned long long doc_id;
        std::string term;
        EXPECT_FALSE(read_token(empty_in, &doc_id, &term));

        fs::path partial = tmp_path / "partial.bin";
        {
            SafeFile out(partial, "wb");
            unsigned long long id = 7;
            unsigned short len = 5;
            fwrite(&id, sizeof(id), 1, out.get());
            fwrite(&len, sizeof(len), 1, out.get());
            fwrite("ab", 1, 2, out.get());   // 2 of 5 term bytes
        }
        BufferedReader partial_in(partial, MIN_READ_BUFFER_SIZE);
        EXPECT_THROW(read_token(partial_in, &doc_id, &term), std::runtime_error);
    }
}
