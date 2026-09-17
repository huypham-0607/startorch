# Refractor scratch

## Expose python binding to a single query class

Erase all query entries & query batch entries, and replace them with a unified query() & query_exhaustive() methods. These two returns tuple of query_result & execution time. Batch queries call sites will have a loop to repeatedly call each query instead. Fix all call sites to reflect this change.

## Tokenizer memory fix

Go with Option 3: keep file order and fix order in C++.

DO NOT SORT AT PYTHON LEVEL. Fetch rows in batches. Look up mapped ids per batch with numpy's searchsorted on the sorted raw ids from the lookup file, which needs about 2.8 GB. PREFERRABLY, THIS SHOULD HAVE NO DISK SPILL. Confirm batch fetching without sort does not cause spilling.

For python token stream generation, instead of struct.pack use a more efficient method for IO (buffering, using efficient IO lib, etc...) if you can.

The C++ build would then have to sort each partial block before writing it and compute doc lengths by id instead of by runs. That avoids every global sort, but the changes spread across Python, C++, and the doc-length file format, so I wouldn't start here.

Verify this is possible in C++. On merge_inverted_index, each individual posting list should be sorted in the build_posting_list function. In construct_doc_len_list, build a full doc_len_list, and then sort the aggregated list afterwards, and then output everything into output file.

## Profile spelling issue

Convert everything into middle dash (full-en). Ensure that no path references break (both in source code and CLI, benchmarks, etc...).

## Duplicated logic inside the engine

The advance_one_including and advance_one_exlcuding advance functions share their block search. The two single-cursor advance functions differ only by < versus <=. The pruned and exhaustive query methods share setup and teardown. Combine into one and let the call site pass in the compare function cmp() (or an operator, whichever is cleaner). Add one line comment noting down the difference between the two call sites. Ensure all tests passed afterwards.

## **"Open an index" is written twice,**
In the engine constructor and in the term-df reader. The old bug that read block metadata from a directory path lived in exactly this sequence. Fix: one loader, reside in merge_inverted_blocks.cpp. Ensure logic of both these call sites are THE EXACT SAME.


## IMPORTANT NOTE

Before executing the refractor, validate the feasibility of every single action item, outline a short, 2,3 bullet points summary on executing plan for each action item. DO NOT EXECUTE yet.

After executing the plan, update refractor.md to reflect the update.

