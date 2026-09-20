#include "startorch/lexical/file_names.h"
#include "startorch/lexical/merge_inverted_blocks.h"
#include "startorch/lexical/construct_doc_len_list.h"
#include "startorch/lexical/construct_inverted_blocks.h"
#include "startorch/lexical/build_index.h"
#include "startorch/lexical/build_params.h"
#include "startorch/lexical/bm25.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <limits>
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

namespace MergeInvertedBlocksTest {
    class MergeInvertedBlocksTest : public testing::Test {
    protected:

        void SetUp() override {
            tmp_path = makeUniqueTempDir();
            block_dir = tmp_path / "blocks";
            merge_dir = tmp_path / "merged";
            fs::create_directory(block_dir);
            fs::create_directory(merge_dir);
        }

        void TearDown() override {
            fs::remove_all(tmp_path);
        }

        fs::path tmp_path, block_dir, merge_dir;

        // Writes a partial block (write_partial_index / Stream wire format)
        // containing a single term's postings.
        void write_raw_block(
            const std::string& file_name,
            const std::string& term,
            const std::vector<std::pair<unsigned long long, unsigned int>>& postings
        ) {
            SafeFile out(block_dir / file_name, "wb");
            unsigned int dict_size = 1;
            fwrite(&dict_size, sizeof(dict_size), 1, out.get());

            unsigned short term_size = term.size();
            unsigned int posting_list_size = postings.size();
            fwrite(&term_size, sizeof(term_size), 1, out.get());
            fwrite(&posting_list_size, sizeof(posting_list_size), 1, out.get());
            fwrite(term.c_str(), sizeof(char), term.size(), out.get());

            unsigned long long last = 0;
            unsigned char buf[BUFFER_LIMIT];
            for (auto [doc_id, freq] : postings) {
                int len = vbe_encode(doc_id - last, buf);
                fwrite(buf, sizeof(unsigned char), len, out.get());
                fwrite(&freq, sizeof(freq), 1, out.get());
                last = doc_id;
            }
        }

        // Writes a partial block containing multiple terms, dictionary
        // order matching the order given (mirrors write_partial_index,
        // which requires the dictionary to be sorted for a real one).
        void write_raw_block_multi(
            const std::string& file_name,
            const std::vector<std::pair<std::string, std::vector<std::pair<unsigned long long, unsigned int>>>>& terms
        ) {
            SafeFile out(block_dir / file_name, "wb");
            unsigned int dict_size = terms.size();
            fwrite(&dict_size, sizeof(dict_size), 1, out.get());

            for (const auto& [term, postings] : terms) {
                unsigned short term_size = term.size();
                unsigned int posting_list_size = postings.size();
                fwrite(&term_size, sizeof(term_size), 1, out.get());
                fwrite(&posting_list_size, sizeof(posting_list_size), 1, out.get());
                fwrite(term.c_str(), sizeof(char), term.size(), out.get());

                unsigned long long last = 0;
                unsigned char buf[BUFFER_LIMIT];
                for (auto [doc_id, freq] : postings) {
                    int len = vbe_encode(doc_id - last, buf);
                    fwrite(buf, sizeof(unsigned char), len, out.get());
                    fwrite(&freq, sizeof(freq), 1, out.get());
                    last = doc_id;
                }
            }
        }

        // Writes doc_len_list.bin AND doc_len_meta.bin into merge_dir -
        // merge_inverted_blocks now derives both paths from out_dir itself,
        // so they must live alongside its other output rather than in a
        // separate directory. total_docs/total_frequency are derived
        // directly from entries, matching how construct_doc_len_list
        // computes them during its own single pass.
        void write_doc_len_list(const std::vector<std::pair<unsigned long long, unsigned int>>& entries) {
            SafeFile out(merge_dir / file_names::DOC_LEN_LIST, "wb");
            unsigned long long last = 0;
            unsigned char buf[BUFFER_LIMIT];
            unsigned long long total_frequency = 0;
            for (auto [doc_id, len] : entries) {
                int enc_len = vbe_encode(doc_id - last, buf);
                fwrite(buf, sizeof(unsigned char), enc_len, out.get());
                fwrite(&len, sizeof(len), 1, out.get());
                last = doc_id;
                total_frequency += len;
            }
            write_doc_len_meta(merge_dir / file_names::DOC_LEN_META, entries.size(), total_frequency);
        }

        fs::path doc_len_path() const {
            return merge_dir / file_names::DOC_LEN_LIST;
        }

        // Reads count postings starting at (file_index, start_addr).
        std::vector<std::pair<unsigned long long, unsigned int>> read_postings(
            unsigned int file_index, size_t start_addr, int count
        ) {
            std::vector<std::pair<unsigned long long, unsigned int>> out;
            SafeFile fp(merge_dir / file_names::posting_file_name(file_index), "rb");
            fseek(fp.get(), (long)start_addr, SEEK_SET);

            unsigned long long doc_id = 0;
            for (int k = 0; k < count; k++) {
                unsigned char buf[BUFFER_LIMIT];
                if (!read_vbe(fp.get(), buf)) break;
                doc_id += vbe_decode(buf);
                unsigned int freq;
                fread(&freq, sizeof(freq), 1, fp.get());
                out.push_back({doc_id, freq});
            }
            return out;
        }

        // Reads count postings starting at (file_index, start_addr) and
        // returns the file offset immediately after the last one read - for
        // checking TermMeta.end_addr's strict-endpoint property directly,
        // rather than hand-computing expected byte offsets.
        size_t read_postings_end_addr(unsigned int file_index, size_t start_addr, int count) {
            SafeFile fp(merge_dir / file_names::posting_file_name(file_index), "rb");
            fseek(fp.get(), (long)start_addr, SEEK_SET);

            for (int k = 0; k < count; k++) {
                unsigned char buf[BUFFER_LIMIT];
                read_vbe(fp.get(), buf);
                unsigned int freq;
                fread(&freq, sizeof(freq), 1, fp.get());
            }
            return (size_t)ftell(fp.get());
        }

        std::unordered_map<std::string, TermMeta> run_merge(
            float k1 = 1.2f, float b = 0.75f, int block_size = 2, size_t split_size = (1ull << 30)
        ) {
            merge_inverted_blocks(block_dir, merge_dir, k1, b, block_size, split_size);
            auto all = read_block_meta_file(merge_dir / file_names::BLOCK_META);
            std::unordered_map<std::string, TermMeta> out;
            for (auto& [term, tm] : all) out[term] = tm;
            return out;
        }
    };

    TEST_F(MergeInvertedBlocksTest, MergesMultiShardMultiTermCorpus) {
        // Same 8-doc / 6-term / 3-shard corpus. Bound values below were
        // recomputed in float32 for IDF = ln((N - df + 0.5) / (df + 0.5) + 1).
        // doc lengths: [3,2,4,1,2,3,1,2] for docs 0..7 -> N=8, avgdl=2.25.
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2},{5,3},{6,1},{7,2}});

        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"alpha", {{0,1}}},
            {"beta", {{0,1}}},
            {"delta", {{2,2}}},
            {"epsilon", {{2,1}}},
            {"gamma", {{0,1}}},
        });
        write_raw_block_multi(file_names::partial_block_file_name(1), {
            {"alpha", {{1,1},{2,1},{3,1},{5,1}}},
            {"beta", {{1,1},{4,1},{5,1}}},
            {"gamma", {{4,1},{5,1},{7,1}}},
        });
        write_raw_block_multi(file_names::partial_block_file_name(2), {
            {"alpha", {{7,1}}},
            {"zeta", {{6,1}}},
        });

        auto tm = run_merge(/*k1=*/1.2f, /*b=*/0.75f, /*block_size=*/2);

        ASSERT_EQ(tm.size(), 6);

        // doc_count for every term.
        EXPECT_EQ(tm["alpha"].doc_count, 6);
        EXPECT_EQ(tm["beta"].doc_count, 4);
        EXPECT_EQ(tm["gamma"].doc_count, 4);
        EXPECT_EQ(tm["delta"].doc_count, 1);
        EXPECT_EQ(tm["epsilon"].doc_count, 1);
        EXPECT_EQ(tm["zeta"].doc_count, 1);

        // end_addr: [block_meta_list[0].start_addr, end_addr) must hold
        // exactly this term's doc_count postings and nothing more or less
        // (strict endpoint property).
        for (const std::string& term : {"alpha", "beta", "gamma", "delta", "epsilon", "zeta"}) {
            const TermMeta& term_meta = tm[term];
            EXPECT_EQ(
                read_postings_end_addr(
                    term_meta.file_index, term_meta.block_meta_list[0].start_addr, term_meta.doc_count
                ),
                term_meta.end_addr
            ) << "term \"" << term << "\" end_addr mismatch";
        }

        // "alpha": 6 postings / block_size=2 -> 3 blocks: [0,1],[2,3],[5,7].
        // block_meta_list[i].doc_id is each block's absolute start_doc_id.
        ASSERT_EQ(tm["alpha"].block_meta_list.size(), 3);
        EXPECT_EQ(tm["alpha"].block_meta_list[0].doc_id, 0);
        EXPECT_EQ(tm["alpha"].block_meta_list[1].doc_id, 2);
        EXPECT_EQ(tm["alpha"].block_meta_list[2].doc_id, 5);
        EXPECT_NEAR(tm["alpha"].block_meta_list[0].block_ub, 0.340919f, 1e-4);
        EXPECT_NEAR(tm["alpha"].block_meta_list[1].block_ub, 0.421135f, 1e-4);
        EXPECT_NEAR(tm["alpha"].block_meta_list[2].block_ub, 0.340919f, 1e-4);
        EXPECT_NEAR(tm["alpha"].term_ub, 0.421135f, 1e-4);

        EXPECT_EQ(
            read_postings(tm["alpha"].file_index, tm["alpha"].block_meta_list[1].start_addr, 2),
            (std::vector<std::pair<unsigned long long, unsigned int>>{{2,1},{3,1}})
        );

        // "zeta": singleton, one partial block.
        ASSERT_EQ(tm["zeta"].block_meta_list.size(), 1);
        EXPECT_EQ(tm["zeta"].block_meta_list[0].doc_id, 6);
        EXPECT_NEAR(tm["zeta"].block_meta_list[0].block_ub, 2.318748f, 1e-3);
        EXPECT_NEAR(tm["zeta"].term_ub, 2.318748f, 1e-3);
        EXPECT_EQ(
            read_postings(tm["zeta"].file_index, tm["zeta"].block_meta_list[0].start_addr, 1),
            (std::vector<std::pair<unsigned long long, unsigned int>>{{6,1}})
        );
    }

    TEST_F(MergeInvertedBlocksTest, StreamsWithMultipleTermsAllGetMerged) {
        // Regression test: streams weren't being re-pushed into the term
        // heap after their first term was drained, so any stream
        // contributing more than one distinct term would silently lose
        // every term after its first. This shard has 3 terms in one
        // stream, none of which share a doc_id, so if the bug were back
        // only "aaa" would show up.
        write_doc_len_list({{1,1},{2,1},{3,1}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"aaa", {{1,1}}},
            {"bbb", {{2,1}}},
            {"ccc", {{3,1}}},
        });

        auto tm = run_merge();

        ASSERT_EQ(tm.size(), 3) << "not every term from a multi-term stream was merged";
        EXPECT_TRUE(tm.count("aaa"));
        EXPECT_TRUE(tm.count("bbb"));
        EXPECT_TRUE(tm.count("ccc"));
    }

    TEST_F(MergeInvertedBlocksTest, DuplicateDocIdSplitAcrossShardsAtBlockBoundaryMerges) {
        // A document's tokens for a term can be split across two SPIMI
        // partial blocks (build_partial_index's memory-limit check is
        // per-token, not per-document). This must merge into one posting,
        // even when the duplicate arrives exactly when the block buffer is
        // already full (block_size=2 here: doc5 then doc10's first copy
        // exactly fill the buffer before doc10's second copy arrives).
        write_doc_len_list({{5,2},{10,3}});
        write_raw_block(file_names::partial_block_file_name(0), "omega", {{5,1},{10,3}});
        write_raw_block(file_names::partial_block_file_name(1), "omega", {{10,2}});

        auto tm = run_merge(1.2f, 0.75f, /*block_size=*/2);

        ASSERT_EQ(tm.size(), 1);
        const TermMeta& omega = tm["omega"];
        EXPECT_EQ(omega.doc_count, 2) << "doc10 counted as two documents instead of one";
        ASSERT_EQ(omega.block_meta_list.size(), 1);

        auto postings = read_postings(
            omega.file_index, omega.block_meta_list[0].start_addr, 2
        );
        ASSERT_EQ(postings.size(), 2);
        EXPECT_EQ(postings[0], (std::pair<unsigned long long, unsigned int>{5, 1}));
        EXPECT_EQ(postings[1], (std::pair<unsigned long long, unsigned int>{10, 5}))
            << "doc10's freq should be merged 3+2=5, not left as two separate entries";

        EXPECT_EQ(
            read_postings_end_addr(omega.file_index, omega.block_meta_list[0].start_addr, omega.doc_count),
            omega.end_addr
        );
    }

    TEST_F(MergeInvertedBlocksTest, EndAddrMarksExclusiveEndOfTermsByteRange) {
        write_doc_len_list({{0,1},{1,1},{2,1}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"aaa", {{0,1}}},
            {"bbb", {{1,1},{2,1}}},
        });

        // block_size=128 comfortably exceeds either term's posting count,
        // so each term lands in exactly one block, both in the same file
        // (posting_0000.bin), written back to back: "aaa" first, "bbb"
        // immediately after.
        auto tm = run_merge(1.2f, 0.75f, /*block_size=*/128);

        ASSERT_EQ(tm["aaa"].block_meta_list.size(), 1);
        ASSERT_EQ(tm["bbb"].block_meta_list.size(), 1);

        // Delta-encoding resets to 0 at each block's first item, so "aaa"'s
        // sole posting (doc_id=0) VBE-encodes its delta (0) in 1 byte, plus
        // a 4-byte freq = 5 bytes total. "bbb" starts immediately after.
        EXPECT_EQ(tm["aaa"].block_meta_list[0].start_addr, 0u);
        EXPECT_EQ(tm["aaa"].end_addr, 5u);
        EXPECT_EQ(tm["bbb"].block_meta_list[0].start_addr, 5u);

        // General property, not tied to the hand-verified byte count above:
        // reading exactly doc_count postings from the first block's
        // start_addr must land the cursor exactly on end_addr - no gap,
        // no overlap into whatever comes next.
        for (const std::string& term : {"aaa", "bbb"}) {
            const TermMeta& term_meta = tm[term];
            EXPECT_EQ(
                read_postings_end_addr(
                    term_meta.file_index, term_meta.block_meta_list[0].start_addr, term_meta.doc_count
                ),
                term_meta.end_addr
            ) << "term \"" << term << "\" end_addr mismatch";
        }
    }

    TEST_F(MergeInvertedBlocksTest, EmptyBlockDirProducesEmptyBlockMetaFile) {
        write_doc_len_list({{0, 1}});
        // No block_*.bin written at all.

        std::unordered_map<std::string, TermMeta> tm;
        ASSERT_NO_THROW(tm = run_merge());
        ASSERT_EQ(tm.size(), 0);
    }

    TEST_F(MergeInvertedBlocksTest, SplitSizeRoutesTermsAcrossMultiplePostingFiles) {
        write_doc_len_list({{0,1},{1,1},{2,1}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"aaa", {{0,1}}},
            {"bbb", {{1,1}}},
            {"ccc", {{2,1}}},
        });

        // The split check runs *after* build_posting_list has already
        // written the current term's data, so crossing split_size while
        // writing term N only takes effect for term N+1 - term N itself
        // still lands wherever out_file already pointed. With split_size=1,
        // "aaa" (first term, check never fires yet) and "bbb" (crosses the
        // threshold, but too late to redirect its own write) both land in
        // file 0; "ccc" is the first term guaranteed to land in file 1.
        auto tm = run_merge(1.2f, 0.75f, /*block_size=*/128, /*split_size=*/1);

        ASSERT_EQ(tm.size(), 3);
        ASSERT_EQ(tm["aaa"].block_meta_list.size(), 1);
        ASSERT_EQ(tm["bbb"].block_meta_list.size(), 1);
        ASSERT_EQ(tm["ccc"].block_meta_list.size(), 1);

        unsigned int aaa_file = tm["aaa"].file_index;
        unsigned int bbb_file = tm["bbb"].file_index;
        unsigned int ccc_file = tm["ccc"].file_index;

        EXPECT_EQ(aaa_file, bbb_file) << "the term that crosses split_size still lands in the file already open";
        EXPECT_NE(bbb_file, ccc_file) << "the *next* term after crossing split_size should roll to a new file";

        EXPECT_EQ(
            read_postings(aaa_file, tm["aaa"].block_meta_list[0].start_addr, 1),
            (std::vector<std::pair<unsigned long long, unsigned int>>{{0,1}})
        );
        EXPECT_EQ(
            read_postings(bbb_file, tm["bbb"].block_meta_list[0].start_addr, 1),
            (std::vector<std::pair<unsigned long long, unsigned int>>{{1,1}})
        );
        EXPECT_EQ(
            read_postings(ccc_file, tm["ccc"].block_meta_list[0].start_addr, 1),
            (std::vector<std::pair<unsigned long long, unsigned int>>{{2,1}})
        );
    }

    TEST_F(MergeInvertedBlocksTest, MergeInvertedBlocksWritesMetadataFile) {
        write_doc_len_list({{0,1},{1,1}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"aaa", {{0,1}}},
            {"bbb", {{1,1}}},
        });

        merge_inverted_blocks(block_dir, merge_dir, /*k1=*/1.3f, /*b=*/0.6f, /*block_size=*/4, /*split_size=*/(1ull << 20));

        // write_metadata writes both a human-readable .txt and the
        // authoritative .bin twin read_metadata actually parses.
        ASSERT_TRUE(fs::exists(merge_dir / file_names::METADATA_TXT));
        ASSERT_TRUE(fs::exists(merge_dir / file_names::METADATA_BIN));

        float k1, b, avgdl;
        int block_size;
        size_t split_size;
        ASSERT_NO_THROW(read_metadata(merge_dir / file_names::METADATA_BIN, k1, b, avgdl, block_size, split_size));

        EXPECT_EQ(k1, 1.3f);
        EXPECT_EQ(b, 0.6f);
        EXPECT_EQ(avgdl, 1.0f);  // 2 tokens over 2 documents
        EXPECT_EQ(block_size, 4);
        EXPECT_EQ(split_size, (1ull << 20));
    }

    TEST_F(MergeInvertedBlocksTest, AvgdlCountsOnlyDocumentsWithTokensAndMatchesBlockBounds) {
        // Regression test: merge used to divide by doc_len_list.size(),
        // which also counts doc_id gaps, while the query engine divided by
        // documents with tokens. A smaller build-time avgdl made block upper
        // bounds lower than real query-time scores, so pruning could drop
        // documents. Doc ids 1 and 2 have no tokens here: doc_len_list spans
        // 4 slots but only 2 documents exist, 6 tokens total.
        write_doc_len_list({{0,2},{3,4}});
        write_raw_block(file_names::partial_block_file_name(0), "omega", {{0,1},{3,2}});

        const float k1 = 1.2f, b = 0.75f;
        auto tm = run_merge(k1, b, /*block_size=*/128);

        float read_k1, read_b, avgdl;
        int block_size;
        size_t split_size;
        read_metadata(merge_dir / file_names::METADATA_BIN, read_k1, read_b, avgdl, block_size, split_size);

        EXPECT_EQ(avgdl, 3.0f) << "avgdl should be 6 tokens / 2 documents, not 6 / doc_len_list.size() = 1.5";

        // Scoring with the stored avgdl must never exceed the stored bounds.
        const unsigned long long N = 4;  // doc_len_list.size(), as both merge and query use
        const float doc0 = calc_BM25(N, /*df_t=*/2, /*tf=*/1.0f, /*doc_len=*/2.0f, avgdl, k1, b);
        const float doc3 = calc_BM25(N, /*df_t=*/2, /*tf=*/2.0f, /*doc_len=*/4.0f, avgdl, k1, b);

        ASSERT_EQ(tm["omega"].block_meta_list.size(), 1);
        const float block_ub = tm["omega"].block_meta_list[0].block_ub;
        EXPECT_GE(block_ub, doc0);
        EXPECT_GE(block_ub, doc3);
        EXPECT_FLOAT_EQ(block_ub, std::max(doc0, doc3));
        EXPECT_FLOAT_EQ(tm["omega"].term_ub, std::max(doc0, doc3));
    }

    // --- load_index / read_term_df_mapping ---

    TEST_F(MergeInvertedBlocksTest, LoadIndexReturnsStoredParametersAndTerms) {
        write_doc_len_list({{0,1},{1,2},{2,3}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"aaa", {{0,1},{2,1}}},
            {"bbb", {{1,2},{2,2}}},
        });
        merge_inverted_blocks(block_dir, merge_dir, /*k1=*/1.3f, /*b=*/0.6f, /*block_size=*/4, /*split_size=*/(1ull << 20));

        IndexMeta index = load_index(merge_dir / file_names::METADATA_BIN);

        EXPECT_EQ(index.posting_dir, merge_dir);
        EXPECT_EQ(index.k1, 1.3f);
        EXPECT_EQ(index.b, 0.6f);
        EXPECT_EQ(index.avgdl, 2.0f);  // 6 tokens over 3 documents
        EXPECT_EQ(index.block_size, 4);
        EXPECT_EQ(index.split_size, (1ull << 20));

        auto expected = read_block_meta_file(merge_dir / file_names::BLOCK_META);
        ASSERT_EQ(index.terms.size(), expected.size());
        for (size_t i = 0; i < expected.size(); i++) {
            EXPECT_EQ(index.terms[i].first, expected[i].first);
            EXPECT_EQ(index.terms[i].second.doc_count, expected[i].second.doc_count);
            EXPECT_EQ(index.terms[i].second.block_meta_list.size(), expected[i].second.block_meta_list.size());
        }
    }

    TEST_F(MergeInvertedBlocksTest, LoadIndexStillLoadsAfterIndexFolderIsRenamed) {
        // The loader finds every index file next to metadata.bin, so a
        // renamed index folder (e.g. full_en -> full-en) still loads.
        write_doc_len_list({{0,1},{1,1}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"aaa", {{0,1}}},
            {"bbb", {{1,1}}},
        });
        run_merge();

        fs::path renamed_dir = tmp_path / "renamed";
        fs::rename(merge_dir, renamed_dir);
        ASSERT_FALSE(fs::exists(merge_dir / file_names::BLOCK_META));

        IndexMeta index;
        ASSERT_NO_THROW(index = load_index(renamed_dir / file_names::METADATA_BIN));
        EXPECT_EQ(index.posting_dir, renamed_dir);
        EXPECT_EQ(index.terms.size(), 2u);
    }

    TEST_F(MergeInvertedBlocksTest, ReadTermDfMappingIsSortedByDescendingDf) {
        write_doc_len_list({{0,1},{1,2},{2,3}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"aaa", {{0,1}}},
            {"bbb", {{0,1},{1,1},{2,1}}},
            {"ccc", {{1,1},{2,2}}},
        });
        run_merge();

        auto mapping = read_term_df_mapping(merge_dir / file_names::METADATA_BIN);

        std::vector<std::pair<std::string, unsigned int>> expected = {{"bbb", 3}, {"ccc", 2}, {"aaa", 1}};
        EXPECT_EQ(mapping, expected);
    }

    // --- Token stream order ---

    TEST_F(MergeInvertedBlocksTest, ShuffledAndSortedDocOrderBuildIdenticalIndexes) {
        // The tokenizer writes documents in corpus file order, not doc_id
        // order. The whole build (doc lengths, SPIMI, merge) must give the
        // same index either way. A tiny SPIMI memory limit splits each token
        // file into several partial blocks, whose doc_id ranges overlap in
        // the shuffled build.
        const unsigned long long doc_count = 40;
        auto doc_tokens = [](unsigned long long doc) {
            std::vector<std::string> tokens;
            if (doc == 13) return tokens;  // no tokens: leaves a doc_id gap
            for (unsigned long long j = 0; j < 1 + doc % 4; j++) {
                tokens.push_back(std::format("t{}", (doc * 7 + j) % 5));
                tokens.push_back(std::format("t{}", (doc + j) % 3));  // repeats give tf > 1
            }
            return tokens;
        };

        auto build = [&](const std::string& name, const std::vector<unsigned long long>& doc_order) {
            fs::path root = tmp_path / name;
            fs::path token_dir = root / "tokens", partial_dir = root / "partial", posting_dir = root / "posting";
            fs::create_directories(token_dir);
            fs::create_directories(partial_dir);
            fs::create_directories(posting_dir);

            // Two token files, split halfway through doc_order.
            const size_t half = doc_order.size() / 2;
            for (size_t file = 0; file < 2; file++) {
                SafeFile fp(token_dir / std::format("token_{:04}.bin", file), "wb");
                for (size_t i = (file == 0 ? 0 : half); i < (file == 0 ? half : doc_order.size()); i++) {
                    unsigned long long doc = doc_order[i];
                    for (const auto& term : doc_tokens(doc)) {
                        unsigned short term_size = term.size();
                        fwrite(&doc, sizeof(doc), 1, fp.get());
                        fwrite(&term_size, sizeof(term_size), 1, fp.get());
                        fwrite(term.c_str(), sizeof(char), term_size, fp.get());
                    }
                }
            }

            construct_doc_len_list(token_dir, posting_dir);
            construct_inverted_blocks(token_dir, partial_dir, /*mem_limit=*/600);
            merge_inverted_blocks(partial_dir, posting_dir, 1.2f, 0.75f, /*block_size=*/4, /*split_size=*/64);

            EXPECT_GT(glob_files(partial_dir, "", file_names::PARTIAL_BLOCK_EXT).size(), 2u)
                << name << ": mem_limit should force several partial blocks";
            return posting_dir;
        };

        std::vector<unsigned long long> sorted_order, shuffled_order;
        for (unsigned long long i = 0; i < doc_count; i++) {
            sorted_order.push_back(i);
            shuffled_order.push_back((i * 17) % doc_count);  // a permutation: gcd(17, 40) = 1
        }
        fs::path sorted_dir = build("sorted", sorted_order);
        fs::path shuffled_dir = build("shuffled", shuffled_order);

        auto read_bytes = [](const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };

        std::vector<std::string> files = {file_names::DOC_LEN_LIST, file_names::DOC_LEN_META, file_names::BLOCK_META};
        std::vector<fs::path> posting_files = glob_files(sorted_dir, "posting_", ".bin");
        ASSERT_GT(posting_files.size(), 1u) << "split_size should force several posting files";
        for (const auto& path : posting_files) files.push_back(path.filename().string());
        ASSERT_EQ(glob_files(shuffled_dir, "posting_", ".bin").size(), posting_files.size());

        for (const auto& file : files) {
            ASSERT_TRUE(fs::exists(shuffled_dir / file)) << file;
            EXPECT_TRUE(read_bytes(sorted_dir / file) == read_bytes(shuffled_dir / file))
                << file << " differs between sorted and shuffled doc order";
        }

        // metadata.bin stores each build's own folder, so compare parameters.
        IndexMeta sorted_index = load_index(sorted_dir / file_names::METADATA_BIN);
        IndexMeta shuffled_index = load_index(shuffled_dir / file_names::METADATA_BIN);
        EXPECT_EQ(sorted_index.avgdl, shuffled_index.avgdl);
        EXPECT_EQ(sorted_index.terms.size(), shuffled_index.terms.size());
    }
}

namespace BuildIndexTest {
    class BuildIndexTest : public testing::Test {
    protected:
        void SetUp() override { tmp_path = makeUniqueTempDir(); }
        void TearDown() override { fs::remove_all(tmp_path); }

        fs::path tmp_path;

        // 40 documents in a shuffled order across two token files; doc 13
        // has no tokens, and repeated terms give tf > 1.
        fs::path write_tokens(const std::string& name) {
            fs::path token_dir = tmp_path / name;
            fs::create_directories(token_dir);
            std::vector<unsigned long long> order;
            for (unsigned long long i = 0; i < 40; i++) order.push_back((i * 17) % 40);
            for (size_t file = 0; file < 2; file++) {
                SafeFile fp(token_dir / std::format("token_{:04}.bin", file), "wb");
                for (size_t i = (file == 0 ? 0 : 20); i < (file == 0 ? 20 : 40); i++) {
                    unsigned long long doc = order[i];
                    if (doc == 13) continue;
                    for (unsigned long long j = 0; j < 1 + doc % 4; j++) {
                        for (const std::string term : {std::format("t{}", (doc * 7 + j) % 5), std::format("t{}", (doc + j) % 3)}) {
                            unsigned short term_size = term.size();
                            fwrite(&doc, sizeof(doc), 1, fp.get());
                            fwrite(&term_size, sizeof(term_size), 1, fp.get());
                            fwrite(term.c_str(), sizeof(char), term_size, fp.get());
                        }
                    }
                }
            }
            return token_dir;
        }

        static std::string read_bytes(const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // Every file of an index folder (not subfolders), by name.
        static std::map<std::string, std::string> index_files(const fs::path& dir) {
            std::map<std::string, std::string> files;
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file()) files[entry.path().filename().string()] = read_bytes(entry.path());
            }
            return files;
        }
    };

    TEST_F(BuildIndexTest, MatchesThreeStepBuildByteForByte) {
        fs::path token_dir = write_tokens("tokens");
        BuildParams params;
        params.block_size = 4;
        params.split_size = 64;     // several posting files
        params.mem_limit = 600;     // several partial blocks

        // Three separate steps, two passes over the token stream.
        fs::path three_partial = tmp_path / "three" / "partial", three_out = tmp_path / "three" / "posting";
        fs::create_directories(three_partial);
        fs::create_directories(three_out);
        construct_doc_len_list(token_dir, three_out);
        construct_inverted_blocks(token_dir, three_partial, params.mem_limit);
        merge_inverted_blocks(three_partial, three_out, params);

        // One entry point, one pass.
        fs::path one_partial = tmp_path / "one" / "partial", one_out = tmp_path / "one" / "posting";
        build_index(token_dir, one_partial, one_out, params);

        auto three = index_files(three_out);
        auto one = index_files(one_out);
        ASSERT_GT(three.size(), 6u) << "expected doc lengths, block meta, metadata and several posting files";
        ASSERT_EQ(one.size(), three.size());
        for (const auto& [name, bytes] : three) {
            ASSERT_TRUE(one.contains(name)) << name;
            EXPECT_TRUE(one[name] == bytes) << name << " differs between build_index and the three-step build";
        }
    }

    TEST_F(BuildIndexTest, RemovesPartialBlocksLeftByAnEarlierBuild) {
        fs::path token_dir = write_tokens("tokens");
        BuildParams params;
        params.block_size = 4;

        fs::path clean_out = tmp_path / "clean" / "posting";
        build_index(token_dir, tmp_path / "clean" / "partial", clean_out, params);

        // A stale block with a term the real stream doesn't have.
        fs::path partial = tmp_path / "dirty" / "partial", out = tmp_path / "dirty" / "posting";
        fs::create_directories(partial);
        {
            std::unordered_map<std::string, PostingList> mapping;
            mapping["stale"].add_document(3);
            std::vector<std::string> dictionary = {"stale"};
            write_partial_index(partial / file_names::partial_block_file_name(99), mapping, dictionary);
        }
        build_index(token_dir, partial, out, params);

        EXPECT_FALSE(fs::exists(partial / file_names::partial_block_file_name(99)));
        EXPECT_TRUE(index_files(out) == index_files(clean_out)) << "the stale block must not be merged";
    }

    TEST_F(BuildIndexTest, InvalidParamsThrowBeforeReadingAnything) {
        BuildParams params;
        params.k1 = 0.0f;
        EXPECT_THROW(build_index(tmp_path / "missing", tmp_path / "partial", tmp_path / "out", params), std::invalid_argument);
        EXPECT_FALSE(fs::exists(tmp_path / "out"));
    }
}

namespace BuildParamsTest {
    TEST(BuildParamsTest, DefaultsAreValid) {
        EXPECT_NO_THROW(BuildParams{}.validate());
    }

    TEST(BuildParamsTest, AcceptsBothCorporasParameters) {
        BuildParams openalex;                    // k1 1.2, b 0.75
        BuildParams msmarco;
        msmarco.k1 = 0.82f;                      // Anserini's MS MARCO setting,
        msmarco.b = 0.68f;                       // outside the old documented [1, 2]
        EXPECT_NO_THROW(openalex.validate());
        EXPECT_NO_THROW(msmarco.validate());
    }

    TEST(BuildParamsTest, RejectsEachOutOfRangeField) {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (float k1 : {0.0f, -1.0f, 5.01f, nan}) {
            BuildParams p;
            p.k1 = k1;
            EXPECT_THROW(p.validate(), std::invalid_argument) << "k1 = " << k1;
        }
        for (float b : {-0.01f, 1.01f, nan}) {
            BuildParams p;
            p.b = b;
            EXPECT_THROW(p.validate(), std::invalid_argument) << "b = " << b;
        }
        BuildParams block;
        block.block_size = 0;
        EXPECT_THROW(block.validate(), std::invalid_argument);
        BuildParams split;
        split.split_size = 0;
        EXPECT_THROW(split.validate(), std::invalid_argument);
        BuildParams mem;
        mem.mem_limit = 0;
        EXPECT_THROW(mem.validate(), std::invalid_argument);
    }

    TEST(BuildParamsTest, AcceptsRangeEdges) {
        BuildParams p;
        p.k1 = 5.0f;
        p.b = 0.0f;
        EXPECT_NO_THROW(p.validate());
        p.b = 1.0f;
        EXPECT_NO_THROW(p.validate());
    }
}

namespace MetadataTest {
    class MetadataTest : public testing::Test {
    protected:

        void SetUp() override {
            tmp_path = makeUniqueTempDir();
        }

        void TearDown() override {
            fs::remove_all(tmp_path);
        }

        fs::path tmp_path;
    };

    TEST_F(MetadataTest, WriteThenReadRoundTrips) {
        fs::path txt_path = tmp_path / file_names::METADATA_TXT;
        fs::path bin_path = tmp_path / file_names::METADATA_BIN;

        write_metadata(txt_path, 1.2f, 0.75f, 39.72734f, 128, (1ull << 30));
        ASSERT_TRUE(fs::exists(bin_path));

        float k1, b, avgdl;
        int block_size;
        size_t split_size;
        read_metadata(bin_path, k1, b, avgdl, block_size, split_size);

        // Binary storage is exact - no tolerance needed.
        EXPECT_EQ(k1, 1.2f);
        EXPECT_EQ(b, 0.75f);
        EXPECT_EQ(avgdl, 39.72734f);
        EXPECT_EQ(block_size, 128);
        EXPECT_EQ(split_size, (1ull << 30));
    }

    TEST_F(MetadataTest, WriteThenReadRoundTripsWithDifferentValues) {
        // Regression guard against field-order/field-name mixups (eg. the
        // earlier split_size/block_size key collision), and against values
        // that would NOT survive "%f" round-tripping (1.69161642f is one of
        // the mismatching values found while investigating the text
        // format's precision loss) - proves the binary path is unaffected.
        fs::path txt_path = tmp_path / file_names::METADATA_TXT;
        fs::path bin_path = tmp_path / file_names::METADATA_BIN;
        float k1 = 1.69161642f;
        float b = 0.30673251f;
        float avgdl = 2.71828183f;

        write_metadata(txt_path, k1, b, avgdl, 256, 12345678ull);

        float read_k1, read_b, read_avgdl;
        int block_size;
        size_t split_size;
        read_metadata(bin_path, read_k1, read_b, read_avgdl, block_size, split_size);

        EXPECT_EQ(read_k1, k1);
        EXPECT_EQ(read_b, b);
        EXPECT_EQ(read_avgdl, avgdl);
        EXPECT_EQ(block_size, 256);
        EXPECT_EQ(split_size, 12345678ull);
    }

    TEST_F(MetadataTest, WriteMetadataAlsoWritesHumanReadableTextFile) {
        fs::path txt_path = tmp_path / file_names::METADATA_TXT;

        write_metadata(txt_path, 1.2f, 0.75f, 2.5f, 128, (1ull << 30));
        ASSERT_TRUE(fs::exists(txt_path));

        SafeFile in(txt_path, "r");
        std::string content;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), in.get()) != nullptr) {
            content += buf;
        }

        EXPECT_NE(content.find(std::format("format_version={}", METADATA_FORMAT_VERSION)), std::string::npos);
        EXPECT_NE(content.find("k1=1.2\n"), std::string::npos);
        EXPECT_NE(content.find("b=0.75\n"), std::string::npos);
        EXPECT_NE(content.find("avgdl=2.5\n"), std::string::npos);
        EXPECT_NE(content.find("block_size=128\n"), std::string::npos);
        EXPECT_NE(content.find("split_size=1073741824\n"), std::string::npos);
        EXPECT_EQ(content.find("_dir="), std::string::npos) << "format 2 stores no paths";
    }

    TEST_F(MetadataTest, ReadThrowsOnMissingFile) {
        fs::path bin_path = tmp_path / "does_not_exist.bin";
        float k1, b, avgdl;
        int block_size;
        size_t split_size;
        ASSERT_THROW(
            read_metadata(bin_path, k1, b, avgdl, block_size, split_size),
            std::runtime_error
        );
    }

    TEST_F(MetadataTest, ReadThrowsOnFileTruncatedBeforeTrailingField) {
        fs::path txt_path = tmp_path / file_names::METADATA_TXT;
        fs::path bin_path = tmp_path / file_names::METADATA_BIN;
        write_metadata(txt_path, 1.2f, 0.75f, 2.5f, 128, (1ull << 30));

        // Every field up through block_size is intact; split_size (an
        // 8-byte trailing field) is cut short.
        auto full_size = fs::file_size(bin_path);
        std::error_code ec;
        fs::resize_file(bin_path, full_size - 4, ec);
        ASSERT_FALSE(ec);

        float k1, b, avgdl;
        int block_size;
        size_t split_size;
        ASSERT_THROW(
            read_metadata(bin_path, k1, b, avgdl, block_size, split_size),
            std::runtime_error
        );
    }

    TEST_F(MetadataTest, ReadThrowsOnFileTruncatedInsideMagic) {
        fs::path txt_path = tmp_path / file_names::METADATA_TXT;
        fs::path bin_path = tmp_path / file_names::METADATA_BIN;
        write_metadata(txt_path, 1.2f, 0.75f, 2.5f, 128, (1ull << 30));

        std::error_code ec;
        fs::resize_file(bin_path, 1, ec);
        ASSERT_FALSE(ec);

        float k1, b, avgdl;
        int block_size;
        size_t split_size;
        ASSERT_THROW(
            read_metadata(bin_path, k1, b, avgdl, block_size, split_size),
            std::runtime_error
        );
    }

    TEST_F(MetadataTest, ReadRejectsFormatOneFileWithRebuildMessage) {
        // Format 1 began with a length-prefixed absolute path.
        fs::path bin_path = tmp_path / file_names::METADATA_BIN;
        {
            SafeFile out(bin_path, "wb");
            std::string path = "/data/scholar_rank/posting/full_en/posting";
            unsigned short len = path.size();
            fwrite(&len, sizeof(len), 1, out.get());
            fwrite(path.data(), 1, path.size(), out.get());
            float k1 = 1.2f;
            fwrite(&k1, sizeof(k1), 1, out.get());
        }

        float k1, b, avgdl;
        int block_size;
        size_t split_size;
        try {
            read_metadata(bin_path, k1, b, avgdl, block_size, split_size);
            FAIL() << "a format 1 file must not load";
        }
        catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find("Rebuild the index"), std::string::npos) << e.what();
        }
    }

    TEST_F(MetadataTest, ReadRejectsUnknownFormatVersion) {
        fs::path bin_path = tmp_path / file_names::METADATA_BIN;
        {
            SafeFile out(bin_path, "wb");
            fwrite("STMD", 1, 4, out.get());
            unsigned int version = METADATA_FORMAT_VERSION + 1;
            fwrite(&version, sizeof(version), 1, out.get());
        }

        float k1, b, avgdl;
        int block_size;
        size_t split_size;
        ASSERT_THROW(
            read_metadata(bin_path, k1, b, avgdl, block_size, split_size),
            std::runtime_error
        );
    }
}
