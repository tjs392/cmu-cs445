# B+ Trees: The Best Data Structure in the World

> [!NOTE]
> A B+ tree is a **self-balancing, ordered, m-way search tree** that keeps everything sorted in a linked list of leaf pages while using a shallow tree of "guide" nodes to jump into that list fast. It's the workhorse index of basically every disk-based DBMS because it does point lookups, range scans, *and* prefix lookups — all in `O(log_m n)` — while turning random I/O into sequential I/O. The whole second half of the lecture is just clever choices about how to lay one out on real storage.

---

## Recap — dynamic hashing (last class)

Hash tables are core DBMS data structures used all over the system. Last time split them into **static vs. dynamic** hashing schemes; today's recap is the two *dynamic* ones, which grow gracefully instead of forcing a full rehash.

> [!TIP]
> The whole point of *dynamic* hashing: a chaining-style scheme splits buckets **incrementally** as they overflow, instead of letting an overflow linked list grow forever (which degrades every lookup on that bucket).

### Extendible hashing

A directory of pointers sits in front of the buckets. Each bucket carries a **local depth**; the directory has a **global depth**. You hash a key, take the top *global-depth* bits, and follow the directory pointer. When a bucket overflows you split *just that bucket* — and only double the directory when you have to:

```
   BEFORE   global depth = 1
   Directory            Buckets
   ┌───┐
   │ 0 │ ─────────────▶ ┌────────────────┐  local depth 1
   │ 1 │ ─────────────▶ │  ●●●  (FULL!)  │  local depth 1
   └───┘                └────────────────┘
        bucket "1" overflows, and local == global depth
                          │
                          ▼  must DOUBLE the directory, then split
   AFTER    global depth = 2
   Directory            Buckets
   ┌────┐
   │ 00 │ ────────────▶ ┌────────────────┐  local depth 1
   │ 01 │ ────────────▶ │ (unchanged)    │
   ├────┤               └────────────────┘
   │ 10 │ ────────────▶ ┌────────────────┐  local depth 2  ┐ split out of
   │ 11 │ ────────────▶ ┌────────────────┐  local depth 2  ┘ the old bucket
   └────┘
```

> [!NOTE]
> **Rule of thumb:** if the overflowing bucket's *local depth < global depth*, just split the bucket and repoint two directory slots — cheap. If *local depth == global depth*, you first have to **double the directory** (global depth +1), then split.

### Linear hashing

Instead of a directory, keep a single **split pointer** that marches across the buckets. Here's the twist that trips everyone up: **the bucket you split is the one at the pointer — not the one that overflowed.** The overflowing bucket just gets a temporary overflow page; the pointer-bucket is what actually gets cleanly redistributed.

```
   split pointer ──┐
                   ▼
   ┌─────┬─────┬─────┬─────┬─────┐      ┌─────┐
   │ B0  │ B1  │ B2  │ B3  │ B4  │ ───▶ │ Bnew│  ← appended on each split
   └─────┴─────┴─────┴─────┴─────┘      └─────┘
   └── already split ──┘  └─ not yet split ─┘
        use h_{i+1}            use h_i
       ( key mod 2N )        ( key mod N )
```

- **Any** bucket overflows → add an overflow page to it, **split the bucket at the pointer** (rehashing its keys with the finer `h_{i+1}` into the original + a newly appended bucket), then **advance the pointer**.
- Buckets *before* the pointer have already been split, so they're addressed with the higher-resolution hash `h_{i+1}` (mod 2N); buckets *at or after* it still use `h_i` (mod N).
- When the pointer walks off the end, a round finishes: reset it to the front and bump `i`.

<details>
<summary>Why the "split the pointer bucket, not the overflowing one" rule works</summary>

It decouples *when* you split from *which* bucket triggered it, so splits happen in a fixed, predictable order across the whole table. Over a full round every bucket gets split exactly once and the table cleanly doubles — no directory needed. The overflowing bucket's temporary overflow page is fine because the pointer will reach and clean it up soon enough.

</details>

---

## Today's agenda

1. 🌳 B+ Tree overview
2. ⚙️ Design choices
3. 🚀 Optimizations

---

## 🌳 Part 1 — B+ Tree overview

### Why B+ trees beat hash tables for indexing

The B+ tree is **the most common indexing data structure**, and the reason is flexibility:

| Query type | Hash table | B+ tree |
|---|---|---|
| **Point query** (`key = x`) | ✅ | ✅ |
| **Range scan** (`x < key < y`) | ❌ (no ordering) | ✅ (sorted leaves) |
| **Partial / prefix key** on a multi-column key | ❌ (need *all* columns) | ✅ (leftmost prefix) |

> [!IMPORTANT]
> A hash table can only do point lookups — there's no ordering to walk, and on a composite key you can't look anything up unless you supply *every* column. B+ trees keep keys sorted, so range scans and prefix lookups fall out for free. That flexibility is why they dominate.

### The B-tree family

There's a whole family, and the names matter mostly so you know which one you're reading about:

| Variant | One-line idea |
|---|---|
| **B-Tree** | the 1971 original; stores values in *every* node |
| **B+Tree** | values only in leaves; inner nodes are pure guides |
| **B\*Tree** | keeps nodes fuller before splitting |
| **B^link-Tree** | adds sibling pointers + high keys for safe concurrent access |
| **B^ε-Tree** | buffers writes in inner nodes (a.k.a. fractal tree) |

> [!NOTE]
> The variant in play today is the **B^link-tree** — that's why the node layout below has *sibling pointers* and a *high key*. Those two features are exactly what let multiple threads traverse and modify the tree safely.

### What a B+ tree actually is

- A **self-balancing, ordered, m-way tree** for search, sequential access, insertion, and deletion in **`O(log_m n)`**, where `m` is the **fanout**.
- **Perfectly balanced** — every leaf is at the same depth.
- Every node except the root is **at least half full**: `⌈m/2⌉ − 1 ≤ #keys ≤ m − 1`.
- Every inner node with `k` keys has exactly `k + 1` non-null children.
- Optimized for **reading/writing large data blocks**.

> [!IMPORTANT]
> The performance magic: because fanout is huge, the tree is only ~4–5 levels deep even for billions of keys, and a leaf scan walks a *sorted linked list* — so what would be random access becomes **sequential** I/O. Block storage loves that.

### A 3-way tree (m = 3), drawn out

With order `m = 3`, each node holds up to 2 keys and 3 children. Inner nodes **guide**; only leaves hold real key→value pairs; leaves are chained by **sibling pointers**:

```
                     ┌──────────────┐
                     │   13  │  17   │           ← inner node: separator keys
                     └──┬────┬────┬──┘              (the "guides")
            < 13 ───────┘    │    └─────── ≥ 17
                             │  13 ≤ x < 17
            ▼                ▼              ▼
       ┌─────────┐     ┌──────────┐   ┌──────────┐
       │ 5  │ 9  │     │ 13 │ 15  │   │ 17 │ 24  │   ← leaf nodes: key → value
       └─────────┘     └──────────┘   └──────────┘
            ◀───────────────────────────────────▶
                    sibling pointers (a sorted linked list)
```

At each inner key you decide **left or right** and descend; the inner nodes interleave separator keys with child pointers, and the leaves at the bottom are the only place actual values live.

### The core insight: a smarter way into a linked list

> [!TIP]
> Strip it down and a B+ tree is **a way to jump into a sorted linked list more efficiently than a binary search**. The leaves *are* the linked list; the tree on top just lets you land near your target in `O(log_m n)` hops instead of scanning from the front.

```
   Without an index — linear scan from the front:
   [k1]→[k2]→[k3]→ … → [TARGET] → …                 up to O(n) hops  😖

   With a B+ tree — descend the guides, land near the target:
                       root
                     /  │  \
                    ▼   ▼   ▼      ← only O(log_m n) hops down
   [..]→[..]→[..]→[TARGET]→[..]→[..]    then scan siblings for a range
```

For a **range scan** you descend root → leaf once, then **walk the sibling pointers** across the leaves to sweep up every matching tuple — very nice. With a billion or trillion keys you skip the linear scan entirely.

### Inside a node

Nodes are **arrays of key/value pairs** plus a little header:

```
   ┌──────────────────────────────────────────────────────────┐
   │ HEADER │ level │ #slots │ prev │ next │ high key           │
   ├──────────────────────────────────────────────────────────┤
   │  KEYS  │  k1  │  k2  │  k3  │ …                            │ ← sorted
   ├────────┼──────┼──────┼──────┼─                             │
   │ VALUES │  v1  │  v2  │  v3  │ …                            │
   └──────────────────────────────────────────────────────────┘
```

- **Inner node** values = pointers to other nodes.
- **Leaf node** values = record IDs / tuples / pointers to the data.
- Because keys are sorted, you can **binary-search the key array**, or use the **high key** to decide whether the value you want could even be in this node — handy metadata, very nice.

### Leaf node values: two approaches

| | Approach #1 — Record IDs | Approach #2 — Tuple Data |
|---|---|---|
| **What's stored** | a pointer to the tuple's location | the tuple contents themselves |
| **Style** | most common implementation | index-organized storage |
| **Primary-key index** | — | leaf stores the full tuple |
| **Secondary index** | — | leaf stores the tuple's **primary key** as its value |

### B-Tree vs. B+Tree

| | B-Tree (1971) | B+Tree |
|---|---|---|
| **Where values live** | in *all* nodes (even inner) | **leaves only**; inner nodes just guide |
| **Space** | more efficient — each key appears once | each key may repeat as a separator |
| **Range scan** | needs in-order traversal **up and down** → random I/O, bad for the CPU | **sequential scan** along the leaf linked list → great for the CPU |

> [!IMPORTANT]
> The B-tree wins on raw space (no duplicated separator keys), but the B+ tree wins where it counts for a DBMS: range scans are a flat sequential walk of the leaves instead of a random-I/O DFS bouncing between levels.

### Insert

```
1. Find the correct leaf L.
2. Insert the entry into L in sorted order.
3. If L has room → done.
4. Else split L into L and a new node L₂:
     · redistribute entries evenly
     · COPY UP the middle key
     · insert an index entry pointing to L₂ into L's parent
5. To split an INNER node: redistribute evenly, but PUSH UP the middle key.
```

> [!WARNING]
> **Copy-up vs. push-up is the #1 thing people get wrong.** A *leaf* split **copies** the middle key up (the key still exists in the leaf, because that's where values live). An *inner* split **pushes** the middle key up (it *leaves* the node entirely — inner keys are just guides, so no value is lost).

```
   LEAF SPLIT → COPY UP                 INNER SPLIT → PUSH UP
   middle key STAYS + a copy moves up   middle key MOVES up, gone below

          [ … 17 … ]                          [ … 17 … ]
              ▲ copy                              ▲ move
              │                                   │
     [13│15│17│19]  split             [13│15│17│19]  split
          ╱     ╲                          ╱     ╲
     [13│15]  [17│19]                  [13│15]  [19│…]
     (17 still present below)          (17 is GONE from the children)
```

### Delete

```
1. From the root, find the leaf L where the entry belongs.
2. Remove the entry.
3. If L is still at least half full → done.
4. If L now has only ⌈m/2⌉ − 1 entries (underflow):
     · try to REDISTRIBUTE — borrow from a sibling
     · if borrowing fails → MERGE L with a sibling
5. If a merge happened, delete the separator entry (pointing to L) from the parent.
```

### Composite indexes

A **composite index** has a key made of two or more attributes:

```sql
CREATE INDEX my_idx ON my_table (a, b DESC, c NULLS FIRST);
```

> [!TIP]
> The DBMS can use a composite B+ tree index whenever the query supplies a **prefix** of the key — the *leftmost* attributes. With `(a, b, c)` a query on `a`, or `a AND b`, or `a AND b AND c` can use it; a query on `b` alone (skipping `a`) generally can't, because the leaves are sorted by `a` first.

### Duplicate keys

| | Approach #1 — Append Record ID | Approach #2 — Overflow leaf nodes |
|---|---|---|
| **Idea** | append the tuple's unique record ID to the key so every key is unique | let leaves spill into overflow nodes holding the dupes |
| **Partial lookup** | still works (the original key is a usable prefix) | — |
| **Verdict** | preferred | 🚫 **don't do this one** |

> [!WARNING]
> Avoid Approach #2 (overflow nodes). Overflow chains reintroduce exactly the unbounded-linked-list problem that dynamic hashing was invented to escape — they wreck the clean, balanced, sorted structure that makes the tree fast.

### Index scan page sorting

> [!NOTE]
> Pulling tuples in the order they appear in a **non-clustered** index is inefficient: you bounce around the heap and re-read the same pages over and over. The fix is to first gather **all** the tuples the query needs, then **sort them by page ID** before fetching — so each page is read once, in order.

---

## ⚙️ Part 2 — Design choices

The four knobs every B+ tree implementation has to set:

| Knob | The question it answers |
|---|---|
| **Node size** | how big should a node/page be? |
| **Merge threshold** | when (if ever) do we merge underfull nodes? |
| **Variable-length keys** | how do we store keys that aren't fixed width? |
| **Intra-node search** | how do we find a key *within* a node? |

### Node size

> [!IMPORTANT]
> **The slower the storage device, the larger the optimal node size.** Slow devices have high per-access cost, so you amortize it over a bigger block.

| Device | ~Optimal node size |
|---|---|
| HDD | ~1 MB |
| SSD | ~10 KB |
| In-memory | ~512 B |

Optimal size also shifts with the **workload** — leaf-node *scans* (which want big nodes) pull a different direction than root-to-leaf *traversals* (which want small, cache-friendly nodes).

### Merge threshold

> [!TIP]
> Many DBMSs **don't** merge a node the instant it hits half-full. The average occupancy of B+ tree nodes is about **69%**, and *delaying* merges cuts down on reorganization churn. Sometimes it's even better to let underfilled nodes sit and just **periodically rebuild the whole tree**.

This is why PostgreSQL describes its B+ tree as a **non-balanced B+ tree** — it tolerates underfull nodes rather than rebalancing eagerly.

### Variable-length keys

| Approach | How | Note |
|---|---|---|
| **1. Pointers** | store keys as pointers to the tuple's attribute | a.k.a. **T-Trees** (in-memory DBMSs) |
| **2. Variable-length nodes** | let each node's size vary | needs careful memory management |
| **3. Padding** | pad every key to the type's max length | simple, but wastes space |
| **4. Key map / indirection** | embed an array of pointers mapping to the key/value list inside the node | compact, sorted indirection |

### Intra-node search

| Approach | How |
|---|---|
| **1. Linear** | scan keys start→end; **vectorize with SIMD** to compare many at once |
| **2. Binary** | the straightforward `O(log)` search on the sorted key array |
| **3. Interpolation** | *approximate* the key's position from the known key distribution, then probe |

---

## 🚀 Part 3 — Optimizations

### Pointer swizzling

> [!IMPORTANT]
> **Super common.** Nodes normally reference each other by **page ID**, so every traversal step does a lookup to translate page ID → memory address. If a page is already **pinned in the buffer pool**, you can store the **raw pointer** in place of the page ID — skipping the address translation entirely on the hot path.

```
   Un-swizzled:  node ──[page id 42]──▶ buffer-pool lookup ──▶ memory addr ──▶ child
   Swizzled:     node ──[raw pointer]────────────────────────────────────────▶ child
                          (valid only while the page stays pinned)
```

### Write-optimized B+ trees (fractal / B^ε trees)

> [!NOTE]
> **Observation:** modifying a B+ tree is expensive whenever it forces node **splits/merges**. So it'd be nice to *delay* and *batch* them instead of paying per-update.

The trick: instead of applying each update immediately, **buffer the changes in log buffers at the inner nodes**. Updates **cascade down incrementally** only when a buffer fills up. This is the idea behind **fractal trees / B^ε trees** — you trade a little read overhead for far cheaper, batched writes.

```
                 ┌─────────────────────────┐
                 │ inner node               │
                 │  guides │ [ write buffer ]│ ← updates land here first
                 └────────────┬─────────────┘
                              │ flush only when the buffer is full
                              ▼
                 ┌─────────────────────────┐
                 │ child node (+ its buffer)│ ← changes cascade down a level
                 └─────────────────────────┘
```

---

## Key takeaways

1. A B+ tree is a **balanced, ordered, m-way tree**: inner nodes are guides, **values live only in leaves**, and leaves form a **sorted linked list** via sibling pointers.
2. It beats a hash table because it handles **point, range, *and* prefix** queries — hashing only does point lookups.
3. Operations are **`O(log_m n)`**, and huge fanout keeps real trees ~4–5 levels deep, turning random access into **sequential I/O**.
4. At heart it's just a **fast on-ramp into a sorted linked list** — descend the guides, then walk siblings for ranges.
5. **B+Tree vs B-Tree:** B-trees are more space-efficient (no repeated keys) but B+Tree range scans are a sequential leaf walk instead of an up-and-down DFS.
6. **Insert splits copy-up at leaves, push-up at inner nodes** — the single most-missed detail.
7. **Deletes** that underflow first try to **borrow** from a sibling, then **merge**, fixing up the parent separator.
8. **Composite indexes** are usable only on a **leftmost prefix** of the key.
9. For **duplicate keys**, append the record ID; **don't** use overflow nodes.
10. Design knobs — **node size scales with device slowness** (HDD 1MB / SSD 10KB / memory 512B), merges are often **delayed** (~69% occupancy), and optimizations like **pointer swizzling** and **write-buffering (B^ε)** shave real cost.

---

## Things to revisit (with quick refreshers)

**Why can a B+ tree do range scans when a hash table can't?**
A hash function deliberately scatters keys so that adjacent keys land in unrelated buckets — great for spreading load, useless for "give me everything between 10 and 50." A B+ tree instead keeps keys *sorted* and chains the leaves into a linked list, so once you've found the low end of a range you just walk sibling pointers until you pass the high end. Same reason it handles prefixes: sorted order means all keys starting with `a` sit contiguously. Ordering is the whole feature.

**What's really going on with copy-up vs. push-up?**
When a *leaf* overflows and splits, the middle key has to stay in a leaf because leaves are where actual values live — so you *copy* it upward as a new separator while keeping the original below. When an *inner* node splits, its keys are pure routing guides with no values attached, so the middle key can be *pushed* up entirely and removed from the children — there's nothing to lose. The mnemonic: leaves copy (because they own data), inner nodes push (because they're just signposts).

**Why does the ideal node size depend on the storage device?**
It's all about amortizing the cost of an access. A spinning disk pays a huge fixed penalty (seek + rotation) per read, so you want to haul back a big ~1MB block and get a lot of useful keys per trip. An SSD is far faster, so the sweet spot drops to ~10KB. In memory there's essentially no per-access penalty and you instead care about cache lines and pointer-chasing, so nodes shrink to ~512B. Slower medium → bigger node, to make each expensive access count.

**What does "the query must provide a prefix" mean for composite indexes?**
A composite index on `(a, b, c)` sorts the leaves by `a`, then `b` within equal `a`, then `c`. So filtering on `a`, or `a AND b`, or all three lets you descend straight to the relevant contiguous slice. But filtering on `b` alone gives the index nothing to anchor on — matching rows are scattered across every value of `a` — so the optimizer usually can't use it. Always order composite-key columns with the most commonly-filtered (and equality-filtered) attribute leftmost.

**Why would a DBMS run nodes at ~69% full instead of merging aggressively?**
Because merging is itself expensive structural work, and eagerly merging the moment a node dips below half-full causes thrashing when inserts and deletes alternate around the threshold. Letting nodes ride a bit under-full avoids that churn, and you can reclaim the slack in bulk with a periodic full rebuild instead of constant incremental reorg. PostgreSQL leans into this so hard that it calls its index a "non-balanced" B+ tree — it simply tolerates the looseness.

**What is pointer swizzling and why is it worth it?**
Normally a node points to its children by page ID, and following it means a buffer-pool lookup to translate that ID into the page's current memory address — a small cost, but paid on *every* hop of *every* traversal. If the child page is already pinned in memory, you can overwrite the stored page ID with the raw memory pointer ("swizzle" it) and jump directly next time. The catch is bookkeeping: the moment that page could be evicted you have to un-swizzle back to the page ID, or you'd be dereferencing a stale address.

---

## Other confusions worth revisiting later

- [ ] **B^link-tree concurrency / latching** — *how* the high key + sibling pointers actually let many threads traverse and split the tree safely (the reason we picked this variant).
- [ ] **Extendible vs. linear hashing mechanics** — the recap moved fast; the directory-doubling and split-pointer rules each deserve a careful worked example.
- [ ] **Fractal / B^ε trees in depth** — the read/write trade-off of buffering updates in inner nodes, and how it compares to LSM trees.
- [ ] **Clustered vs. non-clustered indexes** — tie the "sort by page ID" trick and index-organized storage back to physical layout.
- [ ] **Buffer pool, pinning, and eviction** — the prerequisite that makes pointer swizzling safe (and dangerous).
- [ ] **SIMD intra-node search** — how vectorized comparison actually speeds up the linear scan inside a node.

---

🔗 **Links:** [[hashing-static-and-dynamic]] · [[extendible-and-linear-hashing]] · [[index-concurrency-control]] · [[buffer-pool-and-memory-management]]