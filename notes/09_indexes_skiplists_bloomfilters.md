Indexes vs. Filters
- Index is a data structure that is used as a way to find the sets of records that we want based on some attribute within the table. For some key it gives us an exact location or at least a starting point for the data we want
    - Example: B+Tree

- A filter just tells us set membership - does this key exist?
    - Example: Bloom Filter

Today's Agenda
- Bloom Filters
- Skip Lists
- Tries / Radix Trees
- Inverted Indexes
- Vector Indexes



Bloom Filters
- Probabilistic data structure (bitmap) that answers set membership queries
    - false negatives never occur
    - false positives sometimes occur
- insert(x)
    - use k hash functions to set bits in the filter to 1
- lookup(x)
    - check whether the bits are 1 for each has functoip

Other Filters
- Counting Bloom Filte
    - Multiple levels of bitmaps which allows you to do deletes
    - Instead of storing single bits, store a count
- Cuckoo Filter
    - Kind of like cuckoo hash table, but store fingerprint instead of whole key
    - Supports dynamically adding and removing keys
- Succinct Range Filter
    - Compact version of a radix tree
    - Can do set membership and range membership queries


Observation
- the easiest wayt o imlement a dynamic order preserving index is to use a sorted linked list
    - O(n)

Skip Lists
- Multi leveled linked list where you add these extra pointers to skip nodes and jump to further locations
    - 1st level is sorted list of all keys
    - 2nd level links every other key
    - 3rd level links every fourth key
    - each level has 1/2 the keys of one below it
- Maintains keys in sorted order without requiring global rebalancing
    - approximate O(logn) search times
- Mostly for in memory data structures

Skip lists: Delete
- First logically remove a key from the index by setting a flag 
- Then physically remove the key once we know that no other thread is holding the reference.

Skip Lists
- Advantages
    - May use less memory than a B+Tree, if you do not include reverse pointers
    - Insertions and deletions do not require rebalancing
- Disadvtanges
    - Not disk/cache friendly because they do not optimize locality of references
    - Reverse serach in non-trivial

Observation
- The inner node keys in a B+Tree cannot tell you whether a key exists in the index
- This mean you could have one buffer pool page miss per level in the tree just to find out a key does not exist

Trie Index
- Instead of storing an entire key at every level, store a portion or a digit of the key
- As you traverse the tree, the path can be used to reconstruct the key you're looking for
- All operations have O(k) complexity where k is the length of the key

Tri Key Span
- Span of a trie level is the number of bits that each partial key/digit represents
- Determines fanout

Radix Tree
- Vertically compressed trie that compacts nodes with a single trie
- Can produce false positives


