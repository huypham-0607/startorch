#include "startorch/retrieval/posting_list.h"

#include <gtest/gtest.h>
#include <stdexcept>

TEST(PostingListTest, EmptyListHasNoDocument) {
    PostingList list;
    ASSERT_FALSE(list.has_document(5));
    ASSERT_EQ(list.size(), 0);
}

TEST(PostingListTest, AddDistinctDocuments) {
    PostingList list;
    list.add_document(3);
    list.add_document(7);
    list.add_document(7);
    list.add_document(9);

    ASSERT_EQ(list.size(), 3);
    ASSERT_EQ(list[0].doc_id, 3); ASSERT_EQ(list[0].freq, 1);
    ASSERT_EQ(list[1].doc_id, 7); ASSERT_EQ(list[1].freq, 2);
    ASSERT_EQ(list[2].doc_id, 9); ASSERT_EQ(list[2].freq, 1);
}

TEST(PostingListTest, HasDocumentChecksLastInserted) {
    // add_document assumes monotonically increasing doc_id input, so
    // has_document only ever needs to check the last-inserted entry.
    PostingList list;
    list.add_document(3);
    list.add_document(5);

    ASSERT_TRUE(list.has_document(5));
    ASSERT_FALSE(list.has_document(3));
}

TEST(PostingListTest, OperatorIndexThrowsOutOfRange) {
    PostingList list;
    list.add_document(1);

    ASSERT_NO_THROW(list[0]);
    ASSERT_THROW(list[1], std::out_of_range);
}

TEST(PostingListTest, AddDocumentWithExplicitFreqAccumulates) {
    PostingList list;
    list.add_document(1, 3);
    list.add_document(1, 2);
    list.add_document(2, 1);

    ASSERT_EQ(list.size(), 2);
    ASSERT_EQ(list[0].doc_id, 1); ASSERT_EQ(list[0].freq, 5);
    ASSERT_EQ(list[1].doc_id, 2); ASSERT_EQ(list[1].freq, 1);
}

TEST(PostingListTest, ClearEmptiesList) {
    PostingList list;
    list.add_document(1);
    list.add_document(2);
    ASSERT_EQ(list.size(), 2);

    list.clear();
    ASSERT_EQ(list.size(), 0);
    ASSERT_FALSE(list.has_document(2));
}

TEST(PostingListTest, SortOrdersByDocIdAndMergesRepeatedDocIds) {
    // Built from a stream not in doc_id order: doc 3 appears twice, but not
    // consecutively, so add_document can't merge it.
    PostingList list;
    list.add_document(7);
    list.add_document(3, 2);
    list.add_document(9);
    list.add_document(3);

    list.sort();

    ASSERT_EQ(list.size(), 3);
    ASSERT_EQ(list[0].doc_id, 3); ASSERT_EQ(list[0].freq, 3);
    ASSERT_EQ(list[1].doc_id, 7); ASSERT_EQ(list[1].freq, 1);
    ASSERT_EQ(list[2].doc_id, 9); ASSERT_EQ(list[2].freq, 1);
}

TEST(PostingListTest, SortLeavesSortedListUnchanged) {
    PostingList list;
    list.add_document(1);
    list.add_document(4, 5);
    list.add_document(8);

    list.sort();

    ASSERT_EQ(list.size(), 3);
    ASSERT_EQ(list[0].doc_id, 1); ASSERT_EQ(list[0].freq, 1);
    ASSERT_EQ(list[1].doc_id, 4); ASSERT_EQ(list[1].freq, 5);
    ASSERT_EQ(list[2].doc_id, 8); ASSERT_EQ(list[2].freq, 1);
}

TEST(PostingListTest, SortOnEmptyListIsNoop) {
    PostingList list;
    list.sort();
    ASSERT_EQ(list.size(), 0);
}

TEST(PostingItemTest, OperatorLessComparesDocIdThenFreq) {
    PostingItem a(5, 10);
    PostingItem b(5, 20);
    PostingItem c(6, 1);

    ASSERT_TRUE(a < b);
    ASSERT_FALSE(b < a);
    ASSERT_TRUE(b < c);
    ASSERT_FALSE(c < a);
}
