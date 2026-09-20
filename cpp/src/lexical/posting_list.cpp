#include "startorch/lexical/posting_list.h"

#include <algorithm>
#include <stdexcept>

PostingItem::PostingItem(const unsigned long long _doc_id, const unsigned int _freq) : doc_id(_doc_id), freq(_freq) {}

const bool PostingItem::operator<(PostingItem other) const {
    if (doc_id == other.doc_id) {
        return freq < other.freq;
    }
    return doc_id < other.doc_id;
}

PostingList::PostingList() {
    list = std::vector<PostingItem>();
}

bool PostingList::has_document(const unsigned long long doc_id) const {
    return (!list.empty() && list.back().doc_id == doc_id);
}

void PostingList::add_document(const unsigned long long doc_id, const unsigned int freq) {
    // Only merges into the last entry, for constant time. A document's tokens
    // are contiguous in the token stream, so repeats of one term within one
    // document always land here; sort() handles a stream not in doc_id order.
    if (list.empty() || list.back().doc_id != doc_id) {
        list.push_back(PostingItem(doc_id, freq));
    }
    else {
        list.back().freq += freq;
    }
}

size_t PostingList::size() const {
    return list.size();
}

void PostingList::clear() {
    list.clear();
}

void PostingList::sort() {
    auto not_increasing = [](const PostingItem& a, const PostingItem& b) {
        return a.doc_id >= b.doc_id;
    };
    // Already strictly increasing (e.g. built from a doc_id-ordered stream).
    if (std::adjacent_find(list.begin(), list.end(), not_increasing) == list.end()) return;

    std::sort(list.begin(), list.end(), [](const PostingItem& a, const PostingItem& b) {
        return a.doc_id < b.doc_id;
    });

    size_t out = 0;
    for (size_t i = 0; i < list.size(); i++) {
        if (out > 0 && list[out - 1].doc_id == list[i].doc_id) {
            list[out - 1].freq += list[i].freq;
        }
        else {
            list[out++] = list[i];
        }
    }
    list.erase(list.begin() + out, list.end());
}

const PostingItem& PostingList::operator[] (size_t idx) const {
    if (idx >= list.size()) {
        throw std::out_of_range("Posting list index out of bounds.");
    }
    return list[idx];
}
