#include "startorch/retrieval/query_engine.h"
#include "startorch/retrieval/file_names.h"
#include "startorch/retrieval/merge_inverted_blocks.h"
#include "startorch/retrieval/construct_doc_len_list.h"
#include "startorch/retrieval/bm25.h"
#include "startorch/utils/file_io.h"
#include "startorch/utils/vbe.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <gtest/gtest.h>
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

// ===========================================================================
// PostingPointer
// ===========================================================================
namespace PostingPointerTest {
    class PostingPointerTest : public testing::Test {
    protected:

        void SetUp() override {
            tmp_path = makeUniqueTempDir();
        }

        void TearDown() override {
            fs::remove_all(tmp_path);
        }

        fs::path tmp_path;

        // Writes a posting_*.bin file directly, one contiguous block after
        // another - mirrors flush_buffer's wire format (VBE delta resets to
        // 0 at each block's first item, freq is a raw 4-byte native int).
        // Returns the resulting BlockMeta list (absolute doc_id + real
        // start_addr; block_ub taken from block_ubs).
        std::vector<BlockMeta> write_posting_file(
            const fs::path& path,
            const std::vector<std::vector<std::pair<unsigned long long, unsigned int>>>& blocks,
            const std::vector<float>& block_ubs
        ) {
            SafeFile out(path, "wb");
            std::vector<BlockMeta> block_metas;
            for (size_t bi = 0; bi < blocks.size(); bi++) {
                size_t start_addr = (size_t)ftell(out.get());
                unsigned long long last = 0;
                unsigned char buf[BUFFER_LIMIT];
                for (auto [doc_id, freq] : blocks[bi]) {
                    int len = vbe_encode(doc_id - last, buf);
                    fwrite(buf, sizeof(unsigned char), len, out.get());
                    fwrite(&freq, sizeof(freq), 1, out.get());
                    last = doc_id;
                }
                block_metas.push_back(BlockMeta(blocks[bi][0].first, start_addr, block_ubs[bi]));
            }
            return block_metas;
        }

        TermMeta make_term_meta(
            const std::vector<BlockMeta>& block_metas,
            unsigned int doc_count,
            unsigned int file_index,
            size_t end_addr,
            float term_ub
        ) {
            TermMeta tm;
            tm.term_ub = term_ub;
            tm.end_addr = end_addr;
            tm.doc_count = doc_count;
            tm.file_index = file_index;
            tm.block_meta_list = block_metas;
            return tm;
        }

        std::unordered_map<std::string, TermMeta> term_meta_mapping;
        std::unordered_map<unsigned int, SafeFileMmap> file_index_mapping;

        // "alpha": 3 blocks (block_size=2), file_index=0.
        //   block0: [(0,f=1),(1,f=1)]           block_ub=0.2
        //   block1: [(5,f=2),(6,f=1)]            block_ub=0.5
        //   block2: [(10,f=3)]                   block_ub=0.9  (partial last block)
        // doc_count=5, term_ub=0.9.
        // Byte layout per block (delta resets to 0 per block, freq=4 bytes):
        //   block0: (0)+f1, (1)+f1               = 1+4 + 1+4 = 10 bytes -> end_addr(block0..1 boundary)=10
        //   block1: (5)+f2, (1)+f1                = 1+4 + 1+4 = 10 bytes -> starts at 10, ends at 20
        //   block2: (10)+f3                       = 1+4 = 5 bytes        -> starts at 20, ends at 25
        void setupAlpha() {
            fs::path path = tmp_path / file_names::posting_file_name(0);
            auto blocks = write_posting_file(
                path,
                {
                    {{0,1},{1,1}},
                    {{5,2},{6,1}},
                    {{10,3}},
                },
                {0.2f, 0.5f, 0.9f}
            );
            term_meta_mapping["alpha"] = make_term_meta(blocks, /*doc_count=*/5, /*file_index=*/0, /*end_addr=*/25, /*term_ub=*/0.9f);
            file_index_mapping.emplace(0u, path);
        }

        // "beta": single block (also last block), file_index=1.
        //   block0: [(1,f=1),(2,f=1)]  block_ub=0.3
        // doc_count=2, term_ub=0.3. Bytes: (1)+f1,(1)+f1 = 5+5=10 -> end_addr=10.
        void setupBeta() {
            fs::path path = tmp_path / file_names::posting_file_name(1);
            auto blocks = write_posting_file(
                path,
                { {{1,1},{2,1}} },
                {0.3f}
            );
            term_meta_mapping["beta"] = make_term_meta(blocks, /*doc_count=*/2, /*file_index=*/1, /*end_addr=*/10, /*term_ub=*/0.3f);
            file_index_mapping.emplace(1u, path);
        }

        PostingPointer makeAlpha() {
            return PostingPointer("alpha", /*_block_size=*/2, term_meta_mapping, file_index_mapping);
        }

        PostingPointer makeBeta() {
            return PostingPointer("beta", /*_block_size=*/2, term_meta_mapping, file_index_mapping);
        }
    };

    // --- Constructor ---

    TEST_F(PostingPointerTest, ConstructorInitializesToFirstBlock) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        EXPECT_EQ(pp.get_doc_id(), 0u);
        EXPECT_EQ(pp.get_shallow_block_id(), 0u);
        EXPECT_EQ(pp.get_doc_count(), 5u);
        EXPECT_EQ(pp.get_block_count(), 3u);
        EXPECT_FLOAT_EQ(pp.get_term_upper_bound(), 0.9f);
        EXPECT_FLOAT_EQ(pp.get_block_upper_bound(), 0.2f);
    }

    TEST_F(PostingPointerTest, ConstructorThrowsOnUnknownTerm) {
        setupAlpha();
        ASSERT_THROW(
            PostingPointer("does_not_exist", 2, term_meta_mapping, file_index_mapping),
            std::runtime_error
        );
    }

    TEST_F(PostingPointerTest, ConstructorThrowsOnMissingFileIndex) {
        // "alpha" points at file_index=0, but never registering that entry
        // in file_index_mapping should surface as a clear construction
        // error, not a default-constructed/garbage SafeFileMmap.
        fs::path path = tmp_path / file_names::posting_file_name(0);
        auto blocks = write_posting_file(path, {{{0,1}}}, {0.1f});
        term_meta_mapping["alpha"] = make_term_meta(blocks, 1, 0, 5, 0.1f);
        // file_index_mapping intentionally left empty.
        ASSERT_THROW(
            PostingPointer("alpha", 2, term_meta_mapping, file_index_mapping),
            std::runtime_error
        );
    }

    // --- Getters ---

    TEST_F(PostingPointerTest, GetCurBlockSizeNonLastBlockReturnsFixedSize) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        EXPECT_EQ(pp.get_cur_block_size(0), 2);
        EXPECT_EQ(pp.get_cur_block_size(1), 2);
    }

    TEST_F(PostingPointerTest, GetCurBlockSizeLastBlockReturnsRemainder) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        // doc_count=5, block_size=2 -> last block (index 2) holds 5-2*2=1.
        EXPECT_EQ(pp.get_cur_block_size(2), 1);
    }

    TEST_F(PostingPointerTest, GetNextBlockDocIdReturnsNextBlocksStart) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        EXPECT_EQ(pp.get_next_block_doc_id(), 5u);
        pp.next_shallow(10);
        EXPECT_EQ(pp.get_shallow_block_id(), 2u);
        EXPECT_EQ(pp.get_next_block_doc_id(), MAX_DOC_ID) << "no block after the last one";
    }

    TEST_F(PostingPointerTest, GetNextBlockDocIdSingleBlockTermIsAlwaysMax) {
        setupBeta();
        PostingPointer pp = makeBeta();
        EXPECT_EQ(pp.get_next_block_doc_id(), MAX_DOC_ID);
    }

    TEST_F(PostingPointerTest, GetBlockUpperBoundTracksCurrentBlock) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        EXPECT_FLOAT_EQ(pp.get_block_upper_bound(), 0.2f);
        pp.next_shallow(5);
        EXPECT_FLOAT_EQ(pp.get_block_upper_bound(), 0.5f);
        pp.next_shallow(10);
        EXPECT_FLOAT_EQ(pp.get_block_upper_bound(), 0.9f);
    }

    TEST_F(PostingPointerTest, GetBlockUpperBoundThrowsAfterExhaustion) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next(11); // beyond every posting -> exhausted
        ASSERT_EQ(pp.get_doc_id(), MAX_DOC_ID);
        ASSERT_THROW(pp.get_block_upper_bound(), std::runtime_error);
    }

    TEST_F(PostingPointerTest, GetFrequencyReadsCurrentPosting) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        EXPECT_EQ(pp.get_frequency(), 1u); // doc0
        pp.next(1);
        EXPECT_EQ(pp.get_frequency(), 1u); // doc1
        pp.next(6);
        EXPECT_EQ(pp.get_frequency(), 1u); // doc6
        pp.next(10);
        EXPECT_EQ(pp.get_frequency(), 3u); // doc10 (multi-digit freq, exercises full 4-byte reconstruction)
    }

    TEST_F(PostingPointerTest, GetFrequencyThrowsWhenExhausted) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next(11);
        ASSERT_EQ(pp.get_doc_id(), MAX_DOC_ID);
        ASSERT_THROW(pp.get_frequency(), std::runtime_error);
    }

    // --- next_shallow() ---

    TEST_F(PostingPointerTest, NextShallowStaysInCurrentBlockWhenTargetDoesNotCrossBoundary) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next_shallow(1); // block1 starts at 5, 1 doesn't cross it
        EXPECT_EQ(pp.get_shallow_block_id(), 0u);
        EXPECT_EQ(pp.get_doc_id(), 0u) << "shallow move must not touch doc_id/cur_addr within a block";
    }

    TEST_F(PostingPointerTest, NextShallowMovesToImmediateNextBlock) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next_shallow(5); // exactly block1's start
        EXPECT_EQ(pp.get_shallow_block_id(), 1u);
        // next_shallow only moves the shallow (block-index) pointer - it
        // never touches the real/deep doc_id, so this stays at its
        // constructor value until a real next() call decodes forward.
        EXPECT_EQ(pp.get_doc_id(), 0u);
    }

    TEST_F(PostingPointerTest, NextShallowSkipsMultipleBlocksInOneCall) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next_shallow(10); // block2's start - skips block1 entirely
        EXPECT_EQ(pp.get_shallow_block_id(), 2u);
        // Same as above: shallow-only move, real doc_id is untouched.
        EXPECT_EQ(pp.get_doc_id(), 0u);
    }

    TEST_F(PostingPointerTest, NextShallowThrowsOnBackwardMove) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next_shallow(10); // move forward to block2 first
        ASSERT_EQ(pp.get_shallow_block_id(), 2u);
        ASSERT_THROW(pp.next_shallow(3), std::runtime_error) << "moving to an earlier block should never happen in BMW usage";
    }

    // --- next() ---

    TEST_F(PostingPointerTest, NextLandsOnExactMatchWithinSameBlock) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next(1);
        EXPECT_EQ(pp.get_shallow_block_id(), 0u);
        EXPECT_EQ(pp.get_doc_id(), 1u);
        EXPECT_EQ(pp.get_frequency(), 1u);
    }

    TEST_F(PostingPointerTest, NextLandsOnNextLargerDocIdWhenTargetIsAbsent) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next(4); // 4 isn't a real doc_id; must land on 5
        EXPECT_EQ(pp.get_doc_id(), 5u);
        EXPECT_EQ(pp.get_frequency(), 2u);
    }

    TEST_F(PostingPointerTest, NextSwitchesBlockAndLandsExactlyOnNewBlocksFirstEntry) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next(5); // block1 starts exactly here - no scanning needed after the switch
        EXPECT_EQ(pp.get_shallow_block_id(), 1u);
        EXPECT_EQ(pp.get_doc_id(), 5u);
        EXPECT_EQ(pp.get_frequency(), 2u);
    }

    TEST_F(PostingPointerTest, NextSwitchesBlockThenScansWithinNewBlock) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next(6); // must switch to block1, then advance one more entry inside it
        EXPECT_EQ(pp.get_shallow_block_id(), 1u);
        EXPECT_EQ(pp.get_doc_id(), 6u);
        EXPECT_EQ(pp.get_frequency(), 1u);
    }

    TEST_F(PostingPointerTest, NextExhaustsCurrentBlockAndRollsToNextBlockNaturally) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next(7); // no doc in block1 satisfies >=7; must roll into block2
        EXPECT_EQ(pp.get_shallow_block_id(), 2u);
        EXPECT_EQ(pp.get_doc_id(), 10u);
        EXPECT_EQ(pp.get_frequency(), 3u);
    }

    TEST_F(PostingPointerTest, NextPastEveryPostingExhaustsThePointer) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next(11); // beyond doc10, the last posting in the whole term
        EXPECT_EQ(pp.get_doc_id(), MAX_DOC_ID);
    }

    TEST_F(PostingPointerTest, NextCallingAgainAfterExhaustionThrows) {
        // Documented behavior, not exercised by query()'s own loop (which
        // always checks doc_id == MAX_DOC_ID and stops before advancing an
        // exhausted pointer again) - cur_block_id sits one past the last
        // valid index once exhausted, so any further next_shallow() finds
        // new_block < cur_block_id and rejects it as an "impossible" move.
        setupAlpha();
        PostingPointer pp = makeAlpha();
        pp.next(11);
        ASSERT_EQ(pp.get_doc_id(), MAX_DOC_ID);
        ASSERT_THROW(pp.next(20), std::runtime_error);
    }

    TEST_F(PostingPointerTest, NextWalksEveryPostingInOrder) {
        setupAlpha();
        PostingPointer pp = makeAlpha();
        std::vector<std::pair<unsigned long long, unsigned int>> expected = {
            {0,1}, {1,1}, {5,2}, {6,1}, {10,3}
        };
        // Already sitting on the first posting after construction.
        EXPECT_EQ(pp.get_doc_id(), expected[0].first);
        EXPECT_EQ(pp.get_frequency(), expected[0].second);
        for (size_t i = 1; i < expected.size(); i++) {
            pp.next(pp.get_doc_id() + 1);
            EXPECT_EQ(pp.get_doc_id(), expected[i].first) << "at step " << i;
            EXPECT_EQ(pp.get_frequency(), expected[i].second) << "at step " << i;
        }
        pp.next(pp.get_doc_id() + 1);
        EXPECT_EQ(pp.get_doc_id(), MAX_DOC_ID) << "one past the last real posting must exhaust";
    }

    // --- read_posting_entry() (private; only reachable through get_frequency()/next()) ---

    TEST_F(PostingPointerTest, ReadPostingEntryThrowsOnUnterminatedVbe) {
        // 9 bytes, all < 128 (continuation bit never set) - read_posting_entry's
        // own VBE scan never finds a terminator within BUFFER_LIMIT(8) bytes.
        fs::path path = tmp_path / file_names::posting_file_name(0);
        {
            SafeFile out(path, "wb");
            unsigned char junk[9] = {1,1,1,1,1,1,1,1,1};
            fwrite(junk, sizeof(unsigned char), 9, out.get());
        }
        std::vector<BlockMeta> blocks = { BlockMeta(0, 0, 0.1f) };
        term_meta_mapping["broken"] = make_term_meta(blocks, 1, 0, 9, 0.1f);
        file_index_mapping.emplace(0u, path);

        PostingPointer pp("broken", 1, term_meta_mapping, file_index_mapping);
        ASSERT_THROW(pp.get_frequency(), std::runtime_error);
    }
}

// ===========================================================================
// query()'s helper functions
// ===========================================================================
namespace QueryHelpersTest {
    class QueryHelpersTest : public testing::Test {
    protected:

        void SetUp() override {
            tmp_path = makeUniqueTempDir();
        }

        void TearDown() override {
            fs::remove_all(tmp_path);
        }

        fs::path tmp_path;

        std::vector<BlockMeta> write_posting_file(
            const fs::path& path,
            const std::vector<std::vector<std::pair<unsigned long long, unsigned int>>>& blocks,
            const std::vector<float>& block_ubs
        ) {
            SafeFile out(path, "wb");
            std::vector<BlockMeta> block_metas;
            for (size_t bi = 0; bi < blocks.size(); bi++) {
                size_t start_addr = (size_t)ftell(out.get());
                unsigned long long last = 0;
                unsigned char buf[BUFFER_LIMIT];
                for (auto [doc_id, freq] : blocks[bi]) {
                    int len = vbe_encode(doc_id - last, buf);
                    fwrite(buf, sizeof(unsigned char), len, out.get());
                    fwrite(&freq, sizeof(freq), 1, out.get());
                    last = doc_id;
                }
                block_metas.push_back(BlockMeta(blocks[bi][0].first, start_addr, block_ubs[bi]));
            }
            return block_metas;
        }

        TermMeta make_term_meta(
            const std::vector<BlockMeta>& block_metas,
            unsigned int doc_count,
            unsigned int file_index,
            size_t end_addr,
            float term_ub
        ) {
            TermMeta tm;
            tm.term_ub = term_ub;
            tm.end_addr = end_addr;
            tm.doc_count = doc_count;
            tm.file_index = file_index;
            tm.block_meta_list = block_metas;
            return tm;
        }

        std::unordered_map<std::string, TermMeta> term_meta_mapping;
        std::unordered_map<unsigned int, SafeFileMmap> file_index_mapping;

        // Corpus: doc_len_list {0:2,1:3,2:2,3:4,4:2} -> N=5, avgdl=2.6.
        //
        // t1 (term_ub=1.0, df_t=3): 2 blocks (block_size=2), file_index=0
        //   block0: [(0,f=1),(2,f=2)]  block_ub=0.7
        //   block1: [(4,f=1)]          block_ub=1.0
        // t2 (term_ub=0.8, df_t=2): 1 block (also last), file_index=1
        //   block0: [(0,f=1),(3,f=1)]  block_ub=0.8
        // t3 (term_ub=0.6, df_t=1): 1 block (also last), file_index=2
        //   block0: [(2,f=1)]          block_ub=0.6
        //
        // t1/t2/t3 have distinct doc_counts (3,2,1) so IDF-based tie-breaks
        // in advance_one (with std::less or std::less_equal) are unambiguous,
        // and distinct term_ub (1.0/0.8/0.6) so a posting's identity can be
        // recovered after sort_posting() reorders the vector.
        std::vector<unsigned int> doc_len_list = {2,3,2,4,2};
        const float avgdl = 2.6f;
        const float k1 = 1.2f;
        const float b = 0.75f;

        void setupCorpus() {
            fs::path p1 = tmp_path / file_names::posting_file_name(0);
            auto b1 = write_posting_file(p1, { {{0,1},{2,2}}, {{4,1}} }, {0.7f, 1.0f});
            term_meta_mapping["t1"] = make_term_meta(b1, 3, 0, /*end_addr=*/15, 1.0f);
            file_index_mapping.emplace(0u, p1);

            fs::path p2 = tmp_path / file_names::posting_file_name(1);
            auto b2 = write_posting_file(p2, { {{0,1},{3,1}} }, {0.8f});
            term_meta_mapping["t2"] = make_term_meta(b2, 2, 1, /*end_addr=*/10, 0.8f);
            file_index_mapping.emplace(1u, p2);

            fs::path p3 = tmp_path / file_names::posting_file_name(2);
            auto b3 = write_posting_file(p3, { {{2,1}} }, {0.6f});
            term_meta_mapping["t3"] = make_term_meta(b3, 1, 2, /*end_addr=*/5, 0.6f);
            file_index_mapping.emplace(2u, p3);
        }

        std::vector<PostingPointer> makeSortedPostings() {
            std::vector<PostingPointer> postings;
            postings.push_back(PostingPointer("t1", 2, term_meta_mapping, file_index_mapping));
            postings.push_back(PostingPointer("t2", 2, term_meta_mapping, file_index_mapping));
            postings.push_back(PostingPointer("t3", 1, term_meta_mapping, file_index_mapping));
            sort_posting(postings);
            return postings;
        }

        // Identifies a posting by its (unique) term_ub, since PostingPointer
        // exposes no term-name getter.
        int indexOf(std::vector<PostingPointer>& postings, float term_ub) {
            for (size_t i = 0; i < postings.size(); i++) {
                if (postings[i].get_term_upper_bound() == term_ub) return (int)i;
            }
            return -1;
        }
    };

    TEST_F(QueryHelpersTest, SortPostingOrdersByAscendingDocId) {
        setupCorpus();
        auto postings = makeSortedPostings();
        ASSERT_EQ(postings.size(), 3u);
        EXPECT_LE(postings[0].get_doc_id(), postings[1].get_doc_id());
        EXPECT_LE(postings[1].get_doc_id(), postings[2].get_doc_id());
        // t3 (doc_id=2) must sort last; t1/t2 (both doc_id=0) sort before it,
        // in either relative order.
        int t3 = indexOf(postings, 0.6f);
        ASSERT_EQ(t3, 2);
        EXPECT_EQ(postings[2].get_doc_id(), 2u);
        EXPECT_EQ(postings[0].get_doc_id(), 0u);
        EXPECT_EQ(postings[1].get_doc_id(), 0u);
    }

    TEST_F(QueryHelpersTest, FindPivotReturnsZeroWhenThetaIsBelowEveryBound) {
        setupCorpus();
        auto postings = makeSortedPostings();
        EXPECT_EQ(find_pivot(postings, -1.0f), 0);
    }

    TEST_F(QueryHelpersTest, FindPivotAccumulatesUntilThresholdExceeded) {
        setupCorpus();
        auto postings = makeSortedPostings();
        // Cumulative term_ub after both doc_id=0 entries is 1.0+0.8=1.8
        // regardless of their relative order, so pivot=1 deterministically.
        EXPECT_EQ(find_pivot(postings, 1.5f), 1);
    }

    TEST_F(QueryHelpersTest, FindPivotReturnsSizeWhenNoPrefixExceedsTheta) {
        setupCorpus();
        auto postings = makeSortedPostings();
        // Sum of all term_ub = 1.0+0.8+0.6 = 2.4.
        EXPECT_EQ(find_pivot(postings, 2.5f), (int)postings.size());
    }

    TEST_F(QueryHelpersTest, CheckBlockMaxUsesTighterBlockLevelBound) {
        setupCorpus();
        auto postings = makeSortedPostings();
        // pivot=1 -> doc=0 -> sums current block_ub for doc_id<=0: t1's
        // block0 (0.7) + t2's block0 (0.8) = 1.5.
        EXPECT_TRUE(check_block_max(postings, 1, 1.0f));
        EXPECT_FALSE(check_block_max(postings, 1, 2.0f));
    }

    TEST_F(QueryHelpersTest, EvaluatePrefixMatchesIndependentBM25Computation) {
        setupCorpus();
        auto postings = makeSortedPostings();
        float expected =
            calc_BM25(5, 3, 1.0f, 2.0f, avgdl, k1, b)  // t1 @ doc0 (tf=1, dl=2)
            + calc_BM25(5, 2, 1.0f, 2.0f, avgdl, k1, b); // t2 @ doc0 (tf=1, dl=2)
        float actual = evaluate_prefix(postings, 1, doc_len_list, avgdl, k1, b);
        EXPECT_NEAR(actual, expected, 1e-4f);
    }

    TEST_F(QueryHelpersTest, EvaluatePrefixThrowsWhenPrefixIsNotATiedRun) {
        setupCorpus();
        // Deliberately not sorted/tied: index0 (doc=0) precedes pivot's doc (2).
        std::vector<PostingPointer> postings;
        postings.push_back(PostingPointer("t1", 2, term_meta_mapping, file_index_mapping));
        postings.push_back(PostingPointer("t3", 1, term_meta_mapping, file_index_mapping));
        ASSERT_NE(postings[0].get_doc_id(), postings[1].get_doc_id());
        ASSERT_THROW(
            evaluate_prefix(postings, 1, doc_len_list, avgdl, k1, b),
            std::runtime_error
        );
    }

    TEST_F(QueryHelpersTest, AdvancePrefixAdvancesEveryTiedEntryPastTarget) {
        setupCorpus();
        auto postings = makeSortedPostings();
        int t1 = indexOf(postings, 1.0f), t2 = indexOf(postings, 0.8f), t3 = indexOf(postings, 0.6f);

        advance_prefix(postings, 1, 1); // advance both doc_id==0 entries to >=1

        EXPECT_EQ(postings[t1].get_doc_id(), 2u) << "t1's next posting >=1 is doc2";
        EXPECT_EQ(postings[t2].get_doc_id(), 3u) << "t2's next posting >=1 is doc3";
        EXPECT_EQ(postings[t3].get_doc_id(), 2u) << "t3 wasn't part of the tied prefix and must be untouched";
    }

    TEST_F(QueryHelpersTest, AdvancePrefixThrowsWhenPrefixIsNotATiedRun) {
        setupCorpus();
        std::vector<PostingPointer> postings;
        postings.push_back(PostingPointer("t1", 2, term_meta_mapping, file_index_mapping));
        postings.push_back(PostingPointer("t3", 1, term_meta_mapping, file_index_mapping));
        ASSERT_THROW(advance_prefix(postings, 1, 5), std::runtime_error);
    }

    TEST_F(QueryHelpersTest, AdvanceOneWithLessPicksHighestIdfAmongStrictlyEarlierEntries) {
        setupCorpus();
        auto postings = makeSortedPostings();
        int t1 = indexOf(postings, 1.0f), t2 = indexOf(postings, 0.8f), t3 = indexOf(postings, 0.6f);

        // pivot=2 (t3, doc=2). Strictly-before-2 candidates: t1(df_t=3), t2(df_t=2).
        // Lower df_t -> higher IDF -> t2 is picked.
        advance_one(postings, 2, doc_len_list.size(), 2, std::less<>{});

        EXPECT_EQ(postings[t1].get_doc_id(), 0u) << "t1 (lower IDF) must be untouched";
        EXPECT_EQ(postings[t2].get_doc_id(), 3u) << "t2 (higher IDF) should have advanced";
        EXPECT_EQ(postings[t3].get_doc_id(), 2u) << "pivot itself is untouched by std::less";
    }

    TEST_F(QueryHelpersTest, AdvanceOneWithLessEqualPicksHighestIdfIncludingPivot) {
        setupCorpus();
        auto postings = makeSortedPostings();
        int t1 = indexOf(postings, 1.0f), t2 = indexOf(postings, 0.8f), t3 = indexOf(postings, 0.6f);

        // pivot=2 (t3, doc=2). Candidates doc<=2: t1(df_t=3), t2(df_t=2),
        // t3(df_t=1). t3 has the lowest df_t -> highest IDF -> picked, and
        // since t3 has only the one posting, advancing it exhausts it.
        advance_one(postings, 2, doc_len_list.size(), 3, std::less_equal<>{});

        EXPECT_EQ(postings[t1].get_doc_id(), 0u);
        EXPECT_EQ(postings[t2].get_doc_id(), 0u);
        EXPECT_EQ(postings[t3].get_doc_id(), MAX_DOC_ID) << "t3's only posting was consumed";
    }

    TEST_F(QueryHelpersTest, GetNewCandidateReturnsRealNextBlockStartWhenAvailable) {
        setupCorpus();
        auto postings = makeSortedPostings();
        // pivot=1 -> doc=0 -> prefix candidates t1 (still on block0, next
        // block starts at doc4) and t2 (single/last block -> MAX_DOC_ID),
        // giving min(4, MAX_DOC_ID) = 4. But get_new_candidate also checks
        // the first posting past the prefix (t3, real doc_id=2, not yet
        // shallow-advanced) as a possible nearer target, and 2 < 4 - so the
        // real answer is 2, not 4. Regression guard: this must NOT come
        // back as MAX_DOC_ID just because a fallback default happens to be
        // that.
        unsigned long long target = get_new_candidate(postings, 1);
        EXPECT_EQ(target, 2u);
    }

    TEST_F(QueryHelpersTest, GetNewCandidateReturnsMaxDocIdWhenEveryCandidateIsOnItsLastBlock) {
        // Dedicated minimal fixture: two single-block terms tied at the same
        // doc_id, so both report MAX_DOC_ID as their "next block" - the one
        // case where returning MAX_DOC_ID is actually correct.
        fs::path p1 = tmp_path / "posting_u1.bin";
        auto bu1 = write_posting_file(p1, { {{5,1}} }, {0.4f});
        term_meta_mapping["u1"] = make_term_meta(bu1, 1, 10, 5, 0.4f);
        file_index_mapping.emplace(10u, p1);

        fs::path p2 = tmp_path / "posting_u2.bin";
        auto bu2 = write_posting_file(p2, { {{5,2}} }, {0.4f});
        term_meta_mapping["u2"] = make_term_meta(bu2, 1, 11, 5, 0.35f);
        file_index_mapping.emplace(11u, p2);

        std::vector<PostingPointer> postings;
        postings.push_back(PostingPointer("u1", 1, term_meta_mapping, file_index_mapping));
        postings.push_back(PostingPointer("u2", 1, term_meta_mapping, file_index_mapping));

        unsigned long long target = get_new_candidate(postings, 1);
        EXPECT_EQ(target, MAX_DOC_ID);
    }
}

// ===========================================================================
// query() - end to end
// ===========================================================================
namespace QueryEndToEndTest {
    class QueryEndToEndTest : public testing::Test {
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

        fs::path build_index(float k1 = 1.2f, float b = 0.75f, int block_size = 128, size_t split_size = (1ull << 30)) {
            merge_inverted_blocks(block_dir, merge_dir, k1, b, block_size, split_size);
            return merge_dir / file_names::METADATA_BIN;
        }
    };

    TEST_F(QueryEndToEndTest, ReturnsTopKRankedAgainstIndependentBM25Reference) {
        // doc_len_list: {0:3,1:2,2:4,3:1,4:2} -> N=5, avgdl=2.4.
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"cat",  {{0,1},{2,2},{4,1}}},
            {"dog",  {{1,1},{2,1}}},
            {"bird", {{3,1}}},
        });

        const float k1 = 1.2f, b = 0.75f, avgdl = 2.4f;
        fs::path meta_path = build_index(k1, b);

        // Independent reference: brute-force combined "cat"+"dog" score per doc.
        auto ref = [&](unsigned long long doc, float doc_len, float cat_tf, float dog_tf) {
            float s = 0.0f;
            if (cat_tf > 0) s += calc_BM25(5, 3, cat_tf, doc_len, avgdl, k1, b);
            if (dog_tf > 0) s += calc_BM25(5, 2, dog_tf, doc_len, avgdl, k1, b);
            return s;
        };
        float score0 = ref(0, 3.0f, 1, 0);
        float score1 = ref(1, 2.0f, 0, 1);
        float score2 = ref(2, 4.0f, 2, 1);
        float score4 = ref(4, 2.0f, 1, 0);

        std::vector<std::pair<float, unsigned long long>> expected = {
            {score0, 0}, {score1, 1}, {score2, 2}, {score4, 4}
        };
        std::sort(expected.begin(), expected.end());
        // query() reverses the min-heap pop order before returning, so
        // results come back best-first (non-increasing scores).

        auto [res, elapsed] = QueryEngine(meta_path).query({"cat", "dog"}, 2);
        ASSERT_EQ(res.size(), 2u);
        // The two best (highest-score) candidates are the last two of `expected`.
        EXPECT_EQ(res[0].second, expected[3].second);
        EXPECT_NEAR(res[0].first, expected[3].first, 1e-4f);
        EXPECT_EQ(res[1].second, expected[2].second);
        EXPECT_NEAR(res[1].first, expected[2].first, 1e-4f);
    }

    TEST_F(QueryEndToEndTest, ResultsAreNonIncreasingInScore) {
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"cat",  {{0,1},{2,2},{4,1}}},
            {"dog",  {{1,1},{2,1}}},
            {"bird", {{3,1}}},
        });
        fs::path meta_path = build_index();

        auto [res, elapsed] = QueryEngine(meta_path).query({"cat", "dog", "bird"}, 4);
        ASSERT_GE(res.size(), 2u) << "need at least 2 results for a non-trivial ordering check";
        for (size_t i = 1; i < res.size(); i++) {
            EXPECT_GE(res[i-1].first, res[i].first)
                << "res[" << i-1 << "].first=" << res[i-1].first
                << " must be >= res[" << i << "].first=" << res[i].first;
        }
    }

    TEST_F(QueryEndToEndTest, ReturnsExactlyOneResultWhenOnlyOneDocumentMatches) {
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"bird", {{3,1}}},
        });
        fs::path meta_path = build_index();

        auto [res, elapsed] = QueryEngine(meta_path).query({"bird"}, 1);
        ASSERT_EQ(res.size(), 1u);
        EXPECT_EQ(res[0].second, 3u);
    }

    TEST_F(QueryEndToEndTest, DoesNotPadResultsWithSentinelsWhenKExceedsMatchCount) {
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"bird", {{3,1}}},
        });
        fs::path meta_path = build_index();

        auto [res, elapsed] = QueryEngine(meta_path).query({"bird"}, 5);
        ASSERT_EQ(res.size(), 1u) << "must not pad out to k with (-1, MAX_DOC_ID) sentinels";
        EXPECT_EQ(res[0].second, 3u);
    }

    TEST_F(QueryEndToEndTest, ReturnsExactlyKWhenMoreCandidatesExistThanK) {
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"cat", {{0,1},{2,2},{4,1}}},
        });
        fs::path meta_path = build_index();

        auto [res, elapsed] = QueryEngine(meta_path).query({"cat"}, 2);
        EXPECT_EQ(res.size(), 2u) << "cat matches 3 docs but k=2 must cap the result count";
    }

    TEST_F(QueryEndToEndTest, ReturnsEmptyWhenNoQueryTermIsIndexed) {
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"cat", {{0,1}}},
        });
        fs::path meta_path = build_index();

        auto [res, elapsed] = QueryEngine(meta_path).query({"nonexistent_term"}, 3);
        EXPECT_TRUE(res.empty());
    }

    TEST_F(QueryEndToEndTest, ReturnsEmptyForEmptyQuery) {
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"cat", {{0,1}}},
        });
        fs::path meta_path = build_index();

        auto [res, elapsed] = QueryEngine(meta_path).query({}, 3);
        EXPECT_TRUE(res.empty());
    }

    TEST_F(QueryEndToEndTest, RecallIsCompleteAcrossManyBlockBoundariesWithNoTies) {
        // Regression guard: get_new_candidate previously discarded its own
        // computed value and always returned MAX_DOC_ID, which made
        // advance_one (std::less_equal) exhaust postings prematurely instead of
        // skipping to the real next candidate - silently dropping matching
        // documents from the result set. block_size=1 forces every single
        // posting into its own block, and "x"/"y" never share a doc_id, so
        // this path (check_block_max failing -> get_new_candidate ->
        // advance_one with std::less_equal) is exercised repeatedly.
        write_doc_len_list({{0,2},{1,2},{2,2},{3,2},{4,2},{5,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"x", {{0,1},{2,1},{4,1}}},
            {"y", {{1,1},{3,1},{5,1}}},
        });
        fs::path meta_path = build_index(1.2f, 0.75f, /*block_size=*/1);

        auto [res, elapsed] = QueryEngine(meta_path).query({"x", "y"}, 6);
        ASSERT_EQ(res.size(), 6u) << "every one of the 6 matching documents must be returned";

        std::vector<unsigned long long> doc_ids;
        for (auto& [score, doc_id] : res) doc_ids.push_back(doc_id);
        std::sort(doc_ids.begin(), doc_ids.end());
        EXPECT_EQ(doc_ids, (std::vector<unsigned long long>{0,1,2,3,4,5}));
    }

    // --- One engine, many queries ---

    TEST_F(QueryEndToEndTest, ReusedEngineMatchesFreshEnginesInOrder) {
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"cat",  {{0,1},{2,2},{4,1}}},
            {"dog",  {{1,1},{2,1}}},
            {"bird", {{3,1}}},
        });
        fs::path meta_path = build_index();

        auto [cat_fresh, cat_fresh_elapsed] = QueryEngine(meta_path).query({"cat"}, 3);
        auto [dog_fresh, dog_fresh_elapsed] = QueryEngine(meta_path).query({"dog"}, 3);
        auto [bird_fresh, bird_fresh_elapsed] = QueryEngine(meta_path).query({"bird"}, 3);

        QueryEngine engine(meta_path);
        auto [cat_res, cat_elapsed] = engine.query({"cat"}, 3);
        auto [dog_res, dog_elapsed] = engine.query({"dog"}, 3);
        auto [bird_res, bird_elapsed] = engine.query({"bird"}, 3);

        EXPECT_EQ(cat_res, cat_fresh) << "query 1 on a reused engine should match a fresh engine";
        EXPECT_EQ(dog_res, dog_fresh) << "query 2 must not see cursor state left by query 1";
        EXPECT_EQ(bird_res, bird_fresh) << "query 3 must not see cursor state left by queries 1-2";
        EXPECT_GE(cat_elapsed.count(), 0.0);
        EXPECT_GE(dog_elapsed.count(), 0.0);
        EXPECT_GE(bird_elapsed.count(), 0.0);
    }

    TEST_F(QueryEndToEndTest, ReusedEngineHandlesMixOfMatchingAndNonMatchingQueries) {
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"cat", {{0,1},{2,2},{4,1}}},
        });
        fs::path meta_path = build_index();

        QueryEngine engine(meta_path);
        EXPECT_EQ(engine.query({"cat"}, 3).first.size(), 3u) << "cat matches 3 documents";
        EXPECT_TRUE(engine.query({"nonexistent_term"}, 3).first.empty()) << "unindexed term must return no results";
        EXPECT_TRUE(engine.query({}, 3).first.empty()) << "empty query must return no results";
        EXPECT_EQ(engine.query({"cat"}, 3).first.size(), 3u) << "earlier empty queries must not affect a later one";
    }

    TEST_F(QueryEndToEndTest, ReusedEngineRespectsPerQueryK) {
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"cat", {{0,1},{2,2},{4,1}}},
        });
        fs::path meta_path = build_index();

        QueryEngine engine(meta_path);
        EXPECT_EQ(engine.query({"cat"}, 1).first.size(), 1u) << "first query's k=1 must cap its own result count";
        EXPECT_EQ(engine.query({"cat"}, 2).first.size(), 2u) << "second query's k=2 is independent of the first";
    }

    TEST_F(QueryEndToEndTest, QueryExhaustiveMatchesQuery) {
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2},{5,2},{6,5},{7,1}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"cat",  {{0,1},{2,2},{4,1},{6,3}}},
            {"dog",  {{1,1},{2,1},{5,2},{7,1}}},
            {"bird", {{3,1},{6,1}}},
        });
        // block_size=1 so pruning actually skips blocks.
        fs::path meta_path = build_index(1.2f, 0.75f, /*block_size=*/1);

        QueryEngine engine(meta_path);
        const std::vector<std::vector<std::string>> queries = {
            {"cat"}, {"cat", "dog"}, {"cat", "dog", "bird"}, {"bird", "nonexistent_term"}, {}
        };
        for (const auto& terms : queries) {
            for (int k : {1, 2, 3, 8}) {
                auto [pruned, pruned_elapsed] = engine.query(terms, k);
                auto [exhaustive, exhaustive_elapsed] = engine.query_exhaustive(terms, k);
                ASSERT_EQ(pruned.size(), exhaustive.size()) << "k=" << k;
                for (size_t i = 0; i < pruned.size(); i++) {
                    EXPECT_EQ(pruned[i].second, exhaustive[i].second) << "k=" << k << ", rank " << i;
                    EXPECT_FLOAT_EQ(pruned[i].first, exhaustive[i].first) << "k=" << k << ", rank " << i;
                }
            }
        }
    }

    TEST_F(QueryEndToEndTest, EngineLoadsIndexAfterFolderIsRenamed) {
        // metadata.bin keeps the build-time absolute folder path; the engine
        // must still open an index whose folder was renamed afterwards.
        write_doc_len_list({{0,3},{1,2},{2,4},{3,1},{4,2}});
        write_raw_block_multi(file_names::partial_block_file_name(0), {
            {"cat", {{0,1},{2,2},{4,1}}},
            {"dog", {{1,1},{2,1}}},
        });
        fs::path meta_path = build_index();
        auto [before, before_elapsed] = QueryEngine(meta_path).query({"cat", "dog"}, 4);

        fs::path renamed_dir = tmp_path / "renamed";
        fs::rename(merge_dir, renamed_dir);

        auto [after, after_elapsed] = QueryEngine(renamed_dir / file_names::METADATA_BIN).query({"cat", "dog"}, 4);
        EXPECT_FALSE(after.empty());
        EXPECT_EQ(after, before);
    }
}
