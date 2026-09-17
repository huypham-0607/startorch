#include "startorch/retrieval/construct_doc_len_list.h"
#include "startorch/retrieval/file_names.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"

#include <cstdio>
#include <filesystem>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

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

namespace WriteDocLenEntryTest {
    class WriteDocLenEntryTest : public testing::Test {
    protected:

        void SetUp() override {
            tmp_path = makeUniqueTempDir();
        }

        void TearDown() override {
            fs::remove_all(tmp_path);
        }

        fs::path tmp_path;
    };

    TEST_F(WriteDocLenEntryTest, WritesDeltaAndFreq) {
        fs::path file_name = tmp_path / "doc_len_entry.bin";

        {
            BufferedWriter out_fp(file_name, MIN_BUF_SIZE);
            ASSERT_NO_THROW(write_doc_len_entry(out_fp, 300, 42));
        }

        SafeFile in_fp(file_name, "rb");
        unsigned char buffer[8];
        bool res = read_vbe(in_fp.get(), buffer);
        ASSERT_TRUE(res);
        ASSERT_EQ(vbe_decode(buffer), 300);

        unsigned int freq;
        size_t arg_count = fread(&freq, sizeof(freq), 1, in_fp.get());
        ASSERT_EQ(arg_count, 1);
        ASSERT_EQ(freq, 42);
    }

    TEST_F(WriteDocLenEntryTest, WritesMultipleEntriesSequentially) {
        fs::path file_name = tmp_path / "doc_len_entry.bin";

        std::vector<std::pair<unsigned long long, unsigned int>> entries = {
            {0, 1}, {127, 5}, {128, 255}, {200, 100000}, {(1LL<<40), 10}
        };

        {
            BufferedWriter out_fp(file_name, MIN_BUF_SIZE);
            for (const auto& [delta, freq] : entries) {
                ASSERT_NO_THROW(write_doc_len_entry(out_fp, delta, freq));
            }
        }

        SafeFile in_fp(file_name, "rb");
        for (const auto& [delta, freq] : entries) {
            unsigned char buffer[8];
            ASSERT_TRUE(read_vbe(in_fp.get(), buffer));
            ASSERT_EQ(vbe_decode(buffer), delta);

            unsigned int read_freq;
            ASSERT_EQ(fread(&read_freq, sizeof(read_freq), 1, in_fp.get()), 1);
            ASSERT_EQ(read_freq, freq);
        }
    }
}

namespace DocLenMetaTest {
    class DocLenMetaTest : public testing::Test {
    protected:

        void SetUp() override {
            tmp_path = makeUniqueTempDir();
        }

        void TearDown() override {
            fs::remove_all(tmp_path);
        }

        fs::path tmp_path;
    };

    TEST_F(DocLenMetaTest, WriteThenReadRoundTrips) {
        fs::path meta_path = tmp_path / file_names::DOC_LEN_META;

        ASSERT_NO_THROW(write_doc_len_meta(meta_path, 12345, 987654321));

        auto [total_docs, total_frequency] = read_doc_len_meta(meta_path);
        EXPECT_EQ(total_docs, 12345);
        EXPECT_EQ(total_frequency, 987654321);
    }

    TEST_F(DocLenMetaTest, WriteThenReadRoundTripsWithZeroValues) {
        // Edge case: an empty corpus (construct_doc_len_list ran over no
        // documents at all) still needs a well-formed meta file.
        fs::path meta_path = tmp_path / file_names::DOC_LEN_META;

        ASSERT_NO_THROW(write_doc_len_meta(meta_path, 0, 0));

        auto [total_docs, total_frequency] = read_doc_len_meta(meta_path);
        EXPECT_EQ(total_docs, 0);
        EXPECT_EQ(total_frequency, 0);
    }

    TEST_F(DocLenMetaTest, WriteThenReadRoundTripsWithValuesNearUnsignedLongLongMax) {
        // Both fields are stored raw (no VBE/delta encoding), so this is a
        // straightforward "no silent truncation" check rather than a
        // regression for a specific past bug.
        fs::path meta_path = tmp_path / file_names::DOC_LEN_META;
        unsigned long long near_max = ~0ULL - 1;

        ASSERT_NO_THROW(write_doc_len_meta(meta_path, near_max, near_max));

        auto [total_docs, total_frequency] = read_doc_len_meta(meta_path);
        EXPECT_EQ(total_docs, near_max);
        EXPECT_EQ(total_frequency, near_max);
    }

    TEST_F(DocLenMetaTest, ReadThrowsOnMissingFile) {
        fs::path meta_path = tmp_path / "does_not_exist.bin";
        ASSERT_THROW(read_doc_len_meta(meta_path), std::runtime_error);
    }

    TEST_F(DocLenMetaTest, ReadThrowsOnFileTruncatedMidSecondField) {
        fs::path meta_path = tmp_path / file_names::DOC_LEN_META;
        write_doc_len_meta(meta_path, 42, 1000);

        // total_docs (8 bytes) is intact; total_frequency is cut short.
        auto full_size = fs::file_size(meta_path);
        std::error_code ec;
        fs::resize_file(meta_path, full_size - 4, ec);
        ASSERT_FALSE(ec);

        ASSERT_THROW(read_doc_len_meta(meta_path), std::runtime_error);
    }

    TEST_F(DocLenMetaTest, ReadThrowsOnEmptyFile) {
        fs::path meta_path = tmp_path / file_names::DOC_LEN_META;
        { SafeFile fp(meta_path, "wb"); }

        ASSERT_THROW(read_doc_len_meta(meta_path), std::runtime_error);
    }
}

namespace ConstructDocLenListTest {
    class ConstructDocLenListTest : public testing::Test {
    protected:

        void SetUp() override {
            tmp_path = makeUniqueTempDir();
            in_dir = tmp_path / "in";
            out_dir = tmp_path / "out";
            fs::create_directory(in_dir);
            fs::create_directory(out_dir);
        }

        void TearDown() override {
            fs::remove_all(tmp_path);
        }

        fs::path tmp_path;
        fs::path in_dir;
        fs::path out_dir;

        void write_stream(
            const std::string& file_name,
            const std::vector<std::pair<unsigned long long, std::string>>& v
        ) {
            SafeFile fp(in_dir / file_name, "wb");
            for (const auto& [doc_id, term] : v) {
                fwrite(&doc_id, sizeof(doc_id), 1, fp.get());
                unsigned short term_length = term.size();
                fwrite(&term_length, sizeof(term_length), 1, fp.get());
                fwrite(term.c_str(), sizeof(char), term_length, fp.get());
            }
        }

        // Reads doc_len_list.bin back into a list of (doc_id, freq),
        // reversing the delta encoding write_doc_len_entry applied.
        std::vector<std::pair<unsigned long long, unsigned int>> read_doc_len_table() {
            std::vector<std::pair<unsigned long long, unsigned int>> out;
            SafeFile fp(out_dir / file_names::DOC_LEN_LIST, "rb");

            unsigned long long doc_id = 0;
            unsigned char buffer[8];
            while (read_vbe(fp.get(), buffer)) {
                doc_id += vbe_decode(buffer);

                unsigned int freq;
                fread(&freq, sizeof(freq), 1, fp.get());
                out.push_back({doc_id, freq});
            }
            return out;
        }
    };

    TEST_F(ConstructDocLenListTest, SingleFileMultipleDocs) {
        write_stream("token_0000.bin", {
            {3, "a"}, {3, "b"}, {5, "c"}, {5, "d"}, {5, "e"}, {8, "f"}
        });

        ASSERT_NO_THROW(construct_doc_len_list(in_dir, out_dir));

        auto result = read_doc_len_table();
        std::vector<std::pair<unsigned long long, unsigned int>> expected = {
            {3, 2}, {5, 3}, {8, 1}
        };
        ASSERT_EQ(result, expected);
    }

    TEST_F(ConstructDocLenListTest, FirstDocIdIsZero) {
        // Regression test: the very first document processed must not be
        // silently dropped when its doc_id happens to equal the loop's
        // zero-initialized sentinel.
        write_stream("token_0000.bin", {
            {0, "a"}, {0, "b"}, {3, "c"}, {5, "d"}
        });

        ASSERT_NO_THROW(construct_doc_len_list(in_dir, out_dir));

        auto result = read_doc_len_table();
        std::vector<std::pair<unsigned long long, unsigned int>> expected = {
            {0, 2}, {3, 1}, {5, 1}
        };
        ASSERT_EQ(result, expected);
    }

    TEST_F(ConstructDocLenListTest, LastDocumentIsNotDropped) {
        // Regression test: entries only used to get flushed on a doc_id
        // transition, silently dropping whichever document was last.
        write_stream("token_0000.bin", { {1, "a"}, {2, "b"}, {2, "c"} });

        ASSERT_NO_THROW(construct_doc_len_list(in_dir, out_dir));

        auto result = read_doc_len_table();
        std::vector<std::pair<unsigned long long, unsigned int>> expected = {
            {1, 1}, {2, 2}
        };
        ASSERT_EQ(result, expected);
    }

    TEST_F(ConstructDocLenListTest, MultipleInputFilesContinueAcrossBoundary) {
        write_stream("token_0000.bin", { {1, "a"}, {1, "b"} });
        write_stream("token_0001.bin", { {4, "c"}, {4, "d"}, {4, "e"} });

        ASSERT_NO_THROW(construct_doc_len_list(in_dir, out_dir));

        auto result = read_doc_len_table();
        std::vector<std::pair<unsigned long long, unsigned int>> expected = {
            {1, 2}, {4, 3}
        };
        ASSERT_EQ(result, expected);
    }

    TEST_F(ConstructDocLenListTest, UnsortedDocIdsProduceSameTableAsSorted) {
        // Documents arrive out of doc_id order, including across files.
        // Gap encoding used to underflow here.
        write_stream("token_0000.bin", { {8, "f"}, {3, "a"}, {3, "b"} });
        write_stream("token_0001.bin", { {5, "c"}, {5, "d"}, {5, "e"}, {0, "g"} });

        ASSERT_NO_THROW(construct_doc_len_list(in_dir, out_dir));

        auto result = read_doc_len_table();
        std::vector<std::pair<unsigned long long, unsigned int>> expected = {
            {0, 1}, {3, 2}, {5, 3}, {8, 1}
        };
        ASSERT_EQ(result, expected);

        auto [total_docs, total_frequency] = read_doc_len_meta(out_dir / file_names::DOC_LEN_META);
        EXPECT_EQ(total_docs, 4u);
        EXPECT_EQ(total_frequency, 7u);
    }

    TEST_F(ConstructDocLenListTest, EmptyInputProducesEmptyTable) {
        ASSERT_NO_THROW(construct_doc_len_list(in_dir, out_dir));

        auto result = read_doc_len_table();
        ASSERT_TRUE(result.empty());
    }

    TEST_F(ConstructDocLenListTest, DocumentLengthExceedsUnsignedCharBound) {
        // Regression test: freq (document length in tokens) used to be
        // unsigned char (max 255), which is fine for this corpus but not
        // for other literature datasets with much longer documents. Doc 0
        // gets 300 tokens - well past the old bound - to prove it round
        // trips intact instead of silently wrapping (300 mod 256 = 44).
        std::vector<std::pair<unsigned long long, std::string>> v;
        for (int i = 0; i < 300; i++) v.push_back({0, "x"});
        v.push_back({1, "y"});
        write_stream("token_0000.bin", v);

        ASSERT_NO_THROW(construct_doc_len_list(in_dir, out_dir));

        auto result = read_doc_len_table();
        std::vector<std::pair<unsigned long long, unsigned int>> expected = {
            {0, 300}, {1, 1}
        };
        ASSERT_EQ(result, expected);
    }

    TEST_F(ConstructDocLenListTest, DocumentLengthAtOldUnsignedCharWraparoundPoint) {
        // The sharpest possible regression check: exactly 256 tokens is the
        // first value that would have silently become 0 under the old
        // unsigned char type (256 mod 256 = 0).
        std::vector<std::pair<unsigned long long, std::string>> v;
        for (int i = 0; i < 256; i++) v.push_back({0, "x"});
        write_stream("token_0000.bin", v);

        ASSERT_NO_THROW(construct_doc_len_list(in_dir, out_dir));

        auto result = read_doc_len_table();
        std::vector<std::pair<unsigned long long, unsigned int>> expected = {
            {0, 256}
        };
        ASSERT_EQ(result, expected);
    }
}
