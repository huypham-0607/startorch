#include "startorch/retrieval/construct_inverted_blocks.h"
#include "startorch/retrieval/file_names.h"
#include "startorch/retrieval/posting_list.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <gtest/gtest.h>
#include <map>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <unordered_map>
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

namespace BuildPartialIndexTest {
    class BuildPartialIndexTest : public testing::Test {
    protected:

        void SetUp() override {
            tmp_path = makeUniqueTempDir();
        }

        void TearDown() override {
            fs::remove_all(tmp_path);
        }

        fs::path tmp_path;

        void create_stream(
            const std::vector<std::pair<unsigned long long, std::string>>& v,
            fs::path file_name
        ) {
            SafeFile file_fp(file_name, "wb");

            for (int i = 0; i < v.size(); i++){
                fwrite(&v[i].first, sizeof(v[i].first), 1, file_fp.get());
                unsigned short term_length = v[i].second.size();
                fwrite(&term_length, sizeof(term_length), 1, file_fp.get());
                fwrite(v[i].second.c_str(), sizeof(char), term_length, file_fp.get());
            }
        }
    };

    TEST_F(BuildPartialIndexTest, ValidDistinctInput) {
        // Fabricate data
        std::vector<std::pair<unsigned long long, std::string>> v;
        v.push_back({33, "maxverstappen"});
        v.push_back({2000, "y2k"});
        v.push_back({6767, "sixseven"});
        v.push_back({(1LL<<60),"bignumber"});

        sort(v.begin(),v.end());

        fs::path file_name = tmp_path / "token_stream.bin";

        // Create stream at file_name
        create_stream(v, file_name);

        SafeFile file_fp(file_name, "rb");

        std::unordered_map<std::string, PostingList> posting_list_mapping;
        std::vector<std::string> dictionary;

        bool res;

        ASSERT_NO_THROW(
            res = build_partial_index(
                file_fp,
                (size_t)2*(1LL<<20),
                posting_list_mapping,
                dictionary
            )
        );

        ASSERT_TRUE(res);

        sort(v.begin(), v.end(), [&](auto x, auto y) {
            return x.second < y.second;
        });

        ASSERT_EQ(dictionary.size(), v.size());
        for (int i = 0; i < v.size(); i++){
            ASSERT_EQ(dictionary[i], v[i].second);
        }

        ASSERT_EQ(posting_list_mapping.size(), v.size());

        for (int i = 0; i < v.size(); i++){
            ASSERT_TRUE(
                posting_list_mapping.find(v[i].second) != posting_list_mapping.end()
            );

            ASSERT_TRUE(
                posting_list_mapping[v[i].second][0].doc_id == v[i].first
            );

            ASSERT_TRUE(
                posting_list_mapping[v[i].second][0].freq == 1
            );
        }
    }

    TEST_F(BuildPartialIndexTest, ValidDuplicateInput) {
        // Fabricate data
        std::vector<std::pair<unsigned long long, std::string>> v;
        v.push_back({33, "maxverstappen"});
        v.push_back({33, "maxverstappen"});
        v.push_back({33, "lewishamilton"});
        v.push_back({2000, "y2k"});
        v.push_back({2000, "y2k"});
        v.push_back({2000, "y2k"});
        v.push_back({2000, "k2y"});
        v.push_back({(1LL<<60),"bignumber"});

        sort(v.begin(),v.end());

        fs::path file_name = tmp_path / "token_stream.bin";

        // Create stream at file_name
        create_stream(v, file_name);

        SafeFile file_fp(file_name, "rb");

        std::unordered_map<std::string, PostingList> posting_list_mapping;
        std::vector<std::string> dictionary;
        bool res;

        ASSERT_NO_THROW(
            res = build_partial_index(
                file_fp,
                (size_t)2*(1LL<<20),
                posting_list_mapping,
                dictionary
            )
        );
        ASSERT_TRUE(res);

        // Setting up validation
        sort(v.begin(),v.end(), [&](auto x, auto y){
            if (x.second == y.second) {
                return x.first < y.first;
            }
            else return x.second < y.second;
        });

        // Counting frequency of doc_id : term pair
        std::map<std::pair<unsigned long long, std::string>,int> freq;
        for (auto key_value_pair : v){
            freq[key_value_pair]++;
        }

        v.resize(unique(v.begin(),v.end()) - v.begin());

        std::vector<std::string> true_dictionary;
        for (auto x : v) true_dictionary.push_back(x.second);

        // Validate dictionary.
        ASSERT_EQ(dictionary, true_dictionary);

        ASSERT_EQ(posting_list_mapping.size(), true_dictionary.size());

        // Validate posting_list_mapping
        std::string previous_term = "\n";
        unsigned long long idx = 0;
        for (const auto& key_value_pair : v){
            if (key_value_pair.second != previous_term) idx = 0;

            ASSERT_EQ(
                posting_list_mapping[key_value_pair.second][idx].doc_id,
                key_value_pair.first
            );

            ASSERT_EQ(
                posting_list_mapping[key_value_pair.second][idx].freq,
                freq[key_value_pair]
            );

            previous_term = key_value_pair.second;
            ++idx;
        }
    }

    TEST_F(BuildPartialIndexTest, EmptyStream) {
        std::vector<std::pair<unsigned long long, std::string>> v;
        fs::path file_name = tmp_path / "token_stream.bin";

        create_stream(v, file_name);

        SafeFile file_fp(file_name, "rb");

        std::unordered_map<std::string, PostingList> posting_list_mapping;
        std::vector<std::string> dictionary;
        bool res;

        ASSERT_NO_THROW(res = build_partial_index(
            file_fp,
            (size_t) 2*(1<<20),
            posting_list_mapping,
            dictionary
        ));
        ASSERT_FALSE(res);
    }

    TEST_F(BuildPartialIndexTest, PartialDocId) {
        fs::path file_name = tmp_path / "token_stream.bin";

        SafeFile file_fp(file_name, "wb");

        unsigned int doc_id_4_bytes = 177013;
        fwrite(&doc_id_4_bytes, sizeof(doc_id_4_bytes), 1, file_fp.get());

        file_fp = SafeFile(file_name, "rb");

        std::unordered_map<std::string, PostingList> posting_list_mapping;
        std::vector<std::string> dictionary;
        bool res;

        ASSERT_THROW(res = build_partial_index(
            file_fp,
            (size_t) 2*(1<<20),
            posting_list_mapping,
            dictionary
        ), std::runtime_error);
    }

    TEST_F(BuildPartialIndexTest, PartialTermLength) {
        fs::path file_name = tmp_path / "token_stream.bin";

        SafeFile file_fp(file_name, "wb");

        unsigned long long valid_doc_id = 177013;
        unsigned char term_length_1_bytes = 10;
        fwrite(&valid_doc_id, sizeof(valid_doc_id), 1, file_fp.get());
        fwrite(&term_length_1_bytes, sizeof(term_length_1_bytes), 1, file_fp.get());

        file_fp = SafeFile(file_name, "rb");

        std::unordered_map<std::string, PostingList> posting_list_mapping;
        std::vector<std::string> dictionary;
        bool res;

        ASSERT_THROW(res = build_partial_index(
            file_fp,
            (size_t) 2*(1<<20),
            posting_list_mapping,
            dictionary
        ), std::runtime_error);
    }

    TEST_F(BuildPartialIndexTest, InvalidTermLength) {
        fs::path file_name = tmp_path / "token_stream.bin";

        SafeFile file_fp(file_name, "wb");

        unsigned long long valid_doc_id = 177013;
        unsigned short invalid_term_length = (1<<10);
        fwrite(&valid_doc_id, sizeof(valid_doc_id), 1, file_fp.get());
        fwrite(&invalid_term_length, sizeof(invalid_term_length), 1, file_fp.get());


        file_fp = SafeFile(file_name, "rb");

        std::unordered_map<std::string, PostingList> posting_list_mapping;
        std::vector<std::string> dictionary;
        bool res;

        ASSERT_THROW(res = build_partial_index(
            file_fp,
            (size_t) 2*(1<<20),
            posting_list_mapping,
            dictionary
        ), std::runtime_error);
    }

    TEST_F(BuildPartialIndexTest, PartialTermValue) {
        fs::path file_name = tmp_path / "token_stream.bin";

        SafeFile file_fp(file_name, "wb");

        unsigned long long valid_doc_id = 177013;
        std::string term_value = "catmemes";
        unsigned short valid_term_length = term_value.size();
        fwrite(&valid_doc_id, sizeof(valid_doc_id), 1, file_fp.get());
        fwrite(&valid_term_length, sizeof(valid_term_length), 1, file_fp.get());
        fwrite(term_value.c_str(), sizeof(char), valid_term_length - 1, file_fp.get());

        file_fp = SafeFile(file_name, "rb");

        std::unordered_map<std::string, PostingList> posting_list_mapping;
        std::vector<std::string> dictionary;
        bool res;

        ASSERT_THROW(res = build_partial_index(
            file_fp,
            (size_t) 2*(1<<20),
            posting_list_mapping,
            dictionary
        ), std::runtime_error);
    }

    TEST_F(BuildPartialIndexTest, LowMemLimit) {
        // Fabricate data
        std::vector<std::pair<unsigned long long, std::string>> v;
        for (int i = 0; i < 1000; i++){
            v.push_back({33, std::format("maxverstappen{}",i)});
        }

        sort(v.begin(),v.end());

        fs::path file_name = tmp_path / "token_stream.bin";

        // Create stream at file_name
        create_stream(v, file_name);

        SafeFile file_fp(file_name, "rb");

        std::unordered_map<std::string, PostingList> posting_list_mapping;
        std::vector<std::string> dictionary;
        bool res;

        for (int i = 0; i < 3; i++){
            ASSERT_NO_THROW(
                res = build_partial_index(
                    file_fp,
                    (size_t)(1000),
                    posting_list_mapping,
                    dictionary
                )
            );
            ASSERT_TRUE(res) << "i: " << i;
            posting_list_mapping.clear();
            dictionary.clear();
        }
    }

    // TODO: Add Randomized Stress Test to BuildPartialIndexTest
}

namespace WritePartialIndexTest {
    class WritePartialIndexTest : public testing::Test {
    protected:

        void SetUp() override {
            tmp_path = makeUniqueTempDir();
        }

        void TearDown() override {
            fs::remove_all(tmp_path);
        }

        fs::path tmp_path;
    };

    TEST_F(WritePartialIndexTest, ValidInput) {
        std::vector<std::pair<unsigned long long, std::string>> v;

        fs::path file_name = tmp_path / "token_stream.bin";

        v.push_back({33, "maxverstappen"});
        v.push_back({33, "maxverstappen"});
        v.push_back({33, "lewishamilton"});
        v.push_back({2000, "y2k"});
        v.push_back({2000, "y2k"});
        v.push_back({2000, "y2k"});
        v.push_back({2000, "k2y"});
        v.push_back({(1LL<<48),"bignumber"});

        sort(v.begin(),v.end());
        std::unordered_map<std::string, PostingList> posting_list_mapping;
        std::vector<std::string> dictionary;

        for (const auto& [doc_id, term] : v) {
            posting_list_mapping[term].add_document(doc_id);
            dictionary.push_back(term);
        }

        sort(dictionary.begin(), dictionary.end());
        dictionary.resize(unique(dictionary.begin(),dictionary.end()) - dictionary.begin());

        ASSERT_NO_THROW(
            write_partial_index(
                file_name,
                posting_list_mapping,
                dictionary
            )
        );

        SafeFile file_fp(file_name, "rb");

        unsigned int dict_size;

        int arg_count = fread(&dict_size, sizeof(dict_size), 1, file_fp.get());
        ASSERT_EQ(arg_count, 1);
        ASSERT_EQ(dict_size, dictionary.size());

        for (const std::string term : dictionary) {
            unsigned short term_size;
            unsigned int posting_list_size;
            std::string read_term;
            unsigned long long offset = 0;
            unsigned int freq;

            size_t arg_count;
            arg_count = fread(&term_size, sizeof(term_size), 1, file_fp.get());
            ASSERT_EQ(arg_count, 1);
            ASSERT_EQ(term_size, term.size());

            arg_count = fread(&posting_list_size, sizeof(posting_list_size), 1, file_fp.get());
            ASSERT_EQ(arg_count, 1);
            ASSERT_EQ(posting_list_size, posting_list_mapping[term].size());

            read_term.resize(term_size);
            arg_count = fread(&read_term[0], sizeof(char), term_size, file_fp.get());
            ASSERT_EQ(arg_count, term_size);
            ASSERT_EQ(read_term, term);

            unsigned char buffer[8];
            for (int idx = 0; idx < posting_list_mapping[term].size(); idx++) {
                bool res = read_vbe(file_fp.get(), buffer);
                ASSERT_TRUE(res);

                unsigned long long delta;
                ASSERT_NO_THROW(delta = vbe_decode(buffer));
                offset += delta;
                ASSERT_EQ(offset, posting_list_mapping[term][idx].doc_id);

                arg_count = fread(&freq, sizeof(freq), 1, file_fp.get());
                ASSERT_EQ(arg_count, 1);
                ASSERT_EQ(freq, posting_list_mapping[term][idx].freq);
            }
        }
    }

    TEST_F(WritePartialIndexTest, InvalidPath) {
        std::vector<std::pair<unsigned long long, std::string>> v;

        // Since tmp_path is an empty folder
        fs::path file_name = tmp_path / "meow" / "token_stream.bin";

        v.push_back({33, "maxverstappen"});
        v.push_back({33, "maxverstappen"});
        v.push_back({33, "lewishamilton"});
        v.push_back({2000, "y2k"});
        v.push_back({2000, "y2k"});
        v.push_back({2000, "y2k"});
        v.push_back({2000, "k2y"});
        v.push_back({(1LL<<48),"bignumber"});

        sort(v.begin(),v.end());
        std::unordered_map<std::string, PostingList> posting_list_mapping;
        std::vector<std::string> dictionary;

        for (const auto& [doc_id, term] : v) {
            posting_list_mapping[term].add_document(doc_id);
            dictionary.push_back(term);
        }

        sort(dictionary.begin(), dictionary.end());
        dictionary.resize(unique(dictionary.begin(),dictionary.end()) - dictionary.begin());

        for (const auto& [doc_id, term] : v) {
            posting_list_mapping[term].add_document(doc_id);
            dictionary.push_back(term);
        }

        sort(dictionary.begin(), dictionary.end());
        dictionary.resize(unique(dictionary.begin(),dictionary.end()) - dictionary.begin());

        ASSERT_THROW(
            write_partial_index(
                file_name,
                posting_list_mapping,
                dictionary
            ), std::runtime_error
        );
    }

    // TODO: Add Randomized Stress Test to WritePartialIndexTest
}

namespace ConstructInvertedBlocksTest {
    class ConstructInvertedBlocksTest : public testing::Test {
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

        // Reads a single block_*.bin (write_partial_index wire format) back
        // into term -> sorted (doc_id,freq) pairs, mirroring
        // WritePartialIndexTest's own verification approach.
        std::map<std::string, std::vector<std::pair<unsigned long long, unsigned int>>>
        read_block(const fs::path& block_path) {
            std::map<std::string, std::vector<std::pair<unsigned long long, unsigned int>>> out;
            SafeFile fp(block_path, "rb");

            unsigned int dict_size;
            fread(&dict_size, sizeof(dict_size), 1, fp.get());

            for (unsigned int i = 0; i < dict_size; i++) {
                unsigned short term_size;
                unsigned int posting_list_size;
                fread(&term_size, sizeof(term_size), 1, fp.get());
                fread(&posting_list_size, sizeof(posting_list_size), 1, fp.get());

                std::string term(term_size, '\0');
                fread(&term[0], sizeof(char), term_size, fp.get());

                unsigned long long offset = 0;
                for (unsigned int j = 0; j < posting_list_size; j++) {
                    unsigned char buffer[8];
                    read_vbe(fp.get(), buffer);
                    offset += vbe_decode(buffer);

                    unsigned int freq;
                    fread(&freq, sizeof(freq), 1, fp.get());
                    out[term].push_back({offset, freq});
                }
            }
            return out;
        }
    };

    TEST_F(ConstructInvertedBlocksTest, SingleFileSingleBlock) {
        write_stream("token_0000.bin", {
            {10, "alpha"}, {10, "beta"}, {20, "alpha"}
        });

        ASSERT_NO_THROW(construct_inverted_blocks(in_dir, out_dir, (size_t)2*(1LL<<20)));

        std::vector<fs::path> blocks = glob_files(out_dir, "", file_names::PARTIAL_BLOCK_EXT);
        ASSERT_EQ(blocks.size(), 1);

        auto result = read_block(blocks[0]);
        ASSERT_EQ(result.size(), 2);
        ASSERT_EQ(result["alpha"], (std::vector<std::pair<unsigned long long, unsigned int>>{{10,1},{20,1}}));
        ASSERT_EQ(result["beta"], (std::vector<std::pair<unsigned long long, unsigned int>>{{10,1}}));
    }

    TEST_F(ConstructInvertedBlocksTest, MultipleInputFilesEachProduceABlock) {
        write_stream("token_0000.bin", { {10, "alpha"} });
        write_stream("token_0001.bin", { {20, "alpha"}, {20, "beta"} });

        ASSERT_NO_THROW(construct_inverted_blocks(in_dir, out_dir, (size_t)2*(1LL<<20)));

        std::vector<fs::path> blocks = glob_files(out_dir, "", file_names::PARTIAL_BLOCK_EXT);
        ASSERT_EQ(blocks.size(), 2);

        auto first = read_block(blocks[0]);
        auto second = read_block(blocks[1]);

        ASSERT_EQ(first["alpha"], (std::vector<std::pair<unsigned long long, unsigned int>>{{10,1}}));
        ASSERT_EQ(second["alpha"], (std::vector<std::pair<unsigned long long, unsigned int>>{{20,1}}));
        ASSERT_EQ(second["beta"], (std::vector<std::pair<unsigned long long, unsigned int>>{{20,1}}));
    }

    TEST_F(ConstructInvertedBlocksTest, LowMemLimitProducesMultipleBlocksPerFile) {
        std::vector<std::pair<unsigned long long, std::string>> v;
        for (int i = 0; i < 500; i++) {
            v.push_back({(unsigned long long)i, std::format("term{}", i)});
        }
        write_stream("token_0000.bin", v);

        ASSERT_NO_THROW(construct_inverted_blocks(in_dir, out_dir, (size_t)1000));

        std::vector<fs::path> blocks = glob_files(out_dir, "", file_names::PARTIAL_BLOCK_EXT);
        ASSERT_GT(blocks.size(), 1);
    }

    TEST_F(ConstructInvertedBlocksTest, UnsortedDocIdsProduceSortedPostingLists) {
        // Documents arrive out of doc_id order (tokens of each document stay
        // contiguous). Gap encoding used to underflow here.
        write_stream("token_0000.bin", {
            {20, "alpha"}, {20, "beta"}, {20, "alpha"},
            {10, "alpha"},
            {30, "beta"},
            {5, "alpha"}, {5, "beta"}
        });

        ASSERT_NO_THROW(construct_inverted_blocks(in_dir, out_dir, (size_t)2*(1LL<<20)));

        std::vector<fs::path> blocks = glob_files(out_dir, "", file_names::PARTIAL_BLOCK_EXT);
        ASSERT_EQ(blocks.size(), 1);

        auto result = read_block(blocks[0]);
        ASSERT_EQ(result["alpha"], (std::vector<std::pair<unsigned long long, unsigned int>>{{5,1},{10,1},{20,2}}));
        ASSERT_EQ(result["beta"], (std::vector<std::pair<unsigned long long, unsigned int>>{{5,1},{20,1},{30,1}}));
    }

    TEST_F(ConstructInvertedBlocksTest, EmptyInputDirProducesNoBlocks) {
        ASSERT_NO_THROW(construct_inverted_blocks(in_dir, out_dir, (size_t)2*(1LL<<20)));

        std::vector<fs::path> blocks = glob_files(out_dir, "", file_names::PARTIAL_BLOCK_EXT);
        ASSERT_EQ(blocks.size(), 0);
    }
}
