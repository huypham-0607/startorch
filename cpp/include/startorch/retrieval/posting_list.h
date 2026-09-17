#ifndef POSTING_LIST_H
#define POSTING_LIST_H

#include <vector>

struct PostingItem {
    unsigned long long doc_id;
    unsigned int freq;

    PostingItem(const unsigned long long _doc_id, const unsigned int _freq);

    const bool operator<(PostingItem other) const;
};

class PostingList {
private:
    std::vector<PostingItem> list;
public:
    PostingList();

    bool has_document(const unsigned long long doc_id) const;

    void add_document(const unsigned long long doc_id, const unsigned int freq = 1);

    size_t size() const;

    const PostingItem& operator[] (size_t idx) const;

    void clear();

    // Order entries by doc_id, merging entries that share a doc_id by summing
    // their freq. add_document only merges into the last entry, so a list
    // built from a stream that isn't in doc_id order needs this before its
    // doc_id gaps are encoded.
    void sort();
};

#endif
