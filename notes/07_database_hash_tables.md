# Database Hash Tables

> [!NOTE]
> **Big idea:** The DBMS needs fast key-to-data lookups everywhere —
> for internal metadata, indexes, join algorithms, and more. Hash
> tables give O(1) average lookup, but the *constants* matter at
> database scale. This lecture covers hash functions, static schemes
> (linear probe, cuckoo), and dynamic schemes (chained, extendible,
> linear hashing) that let the table grow.

---

## Where this fits in the DBMS stack

```
   ┌────────────────────────────┐
   │     Query Planning         │
   ├────────────────────────────┤
   │     Operator Execution     │
   ├────────────────────────────┤
   │     Access Methods         │  ◄── HASH TABLES + TREES live here
   ├────────────────────────────┤
   │     Buffer Pool Manager    │
   ├────────────────────────────┤
   │     Disk Manager           │
   └────────────────────────────┘
```

We're now at the **access methods** layer — how the DBMS finds
specific data within pages, on top of the buffer pool.

| Data structure family | Property |
|---|---|
| **Hash tables** | Unordered, O(1) lookup |
| **Trees** | Ordered, O(log n) lookup, supports ranges |

---

## What the DBMS uses these for

| Use | Examples |
|---|---|
| **Internal metadata** | Catalogs, system tables, transaction state |
| **Core data storage** | (Sometimes — e.g. some hash-organized tables) |
| **Temporary data structures** | Hash joins, GROUP BY hash aggregation |
| **Table indexes** | Hash indexes, B+ tree indexes |

---

## Design decisions

| Decision | What it covers |
|---|---|
| **Data organization** | Layout in memory/pages; what metadata to store for efficient access |
| **Concurrency** | How multiple threads access safely (latches, lock-free schemes) |

---

# Hash tables

> [!IMPORTANT]
> **Definition:** an unordered associative array that maps keys to
> values. A **hash function** computes an offset into an array.

| Metric | Value |
|---|---|
| Space | O(n) |
| Avg lookup | O(1) |
| Worst lookup | O(n) |

> [!TIP]
> **The DBMS cares about constants, not just Big-O.** A "stupid" O(1)
> hash function taking 100 ns vs. a smart one taking 1 ns — over a
> billion lookups, that's the difference between 100 seconds and 1
> second. Choose carefully.

---

## The static hash table (idealized)

Allocate one giant array; find an entry by `hash(key) % N`.

```
   key  ──►  hash(key) % N  ──►  index
                                   │
                                   ▼
                  ┌──────────────────────────────┐
                  │ ... | value | ... | value |..│
                  └──────────────────────────────┘
                              the array
```

### Unrealistic assumptions (we'll fix these)

1. **Number of elements** is known and fixed in advance
2. Each **key is unique**
3. **Perfect hash function** — no collisions

Real-world databases violate all three. The rest of the lecture is
how to deal with that.

---

# Hash functions

> [!NOTE]
> For any input key, compute a one-way integer representation. **In a
> DBMS we care about only two properties: fast, and low collision
> rate.**

We **don't** want:
- Cryptographic strength (SHA-2, etc.) — wastes CPU on properties we
  don't need
- Reversibility — irrelevant for hash tables

### Common hash functions used in databases

| Year | Function | Notes |
|---|---|---|
| 1975 | **CRC-64** | From networking, for error detection |
| 2008 | **MurmurHash** | Fast, general-purpose |
| 2011 | **Google CityHash** | Tuned for short keys |
| 2012 | **Facebook XXHash** | From the `zstd` creator; very fast |
| 2014 | **Google FarmHash** | Newer CityHash with better collisions |
| 2019 | **rapidhash** | Fast, no architecture-specific instructions |

> [!TIP]
> Modern systems mostly use **XXHash** or **FarmHash**. Both are
> battle-tested in production at massive scale.

---

# Static hashing schemes

## Approach #1 — Linear probe hashing

> [!IMPORTANT]
> One giant table of fixed-length slots. On collision, scan forward
> for the next free slot.

### How insertion works

```
   Inserting C:  hash(C) % N = 0  (where A already lives)

   slot │ key │ value
   ─────┼─────┼─────
    0   │  A  │  ...    ← occupied → probe forward
    1   │  C  │  ...    ← landed here
    2   │     │
    3   │     │
   ...
```

Now insert D where `hash(D) % N = 0`:
```
    0   │  A  │  ...    ← occupied
    1   │  C  │  ...    ← occupied
    2   │  D  │  ...    ← landed here
    3   │     │
```

> [!NOTE]
> The good news: linear probing creates **sequential memory access**,
> not random. CPUs and disks love that — cache prefetchers can guess
> what comes next.

### Load factor

The fraction of slots that are full. When too high (typically > 0.7),
probing chains get long → performance degrades → **resize the table**
(double it, rehash everything).

### How key/value entries are stored

| Key/value size | How it's stored |
|---|---|
| **Fixed-length** | Inline within the hash table pages |
| **Variable-length** | Store data in a separate temp table; the hash table holds the hash key and a *record ID* pointing to the temp table entry |

### Deletes are tricky

Say we want to delete C:

```
    0   │  A  │  ...
    1   │  C  │  ...   ← delete this
    2   │  D  │  ...
```

If we just empty slot 1, then a later lookup of D would probe slot 0
(A, not a match), slot 1 (empty → stop searching) and **incorrectly
report D as missing**.

Two fixes:

| Approach | How it works |
|---|---|
| **Movement** | After deleting, rehash the keys following the gap until you hit an empty slot — pull them back if needed |
| **Tombstones** | Mark the slot as "logically deleted" (a bit map). Future searches skip past it; future inserts can reuse it. Needs periodic garbage collection. |

---

## Approach #2 — Cuckoo hashing

> [!IMPORTANT]
> Use **two (or more) hash functions** and check multiple candidate
> locations. Lookup is O(1) — only those few spots ever get checked.

### How insertion works

```
   PUT A:  h₁(A) = slot 2,  h₂(A) = slot 7
           Flip a coin or check which is empty → place in slot 2.

   PUT B:  h₁(B) = slot 2  (occupied by A),  h₂(B) = slot 5
           Slot 5 is empty → place there.

   PUT C:  h₁(C) = slot 2  (occupied),  h₂(C) = slot 5  (occupied)
           CONFLICT.
           
           Choose a victim (say B at slot 5), evict it.
           Place C in slot 5.
           Now B must rehash to its OTHER location:
             h₁(B) = slot 2  (now occupied by C)
           Evict A instead → A re-homes to slot 7 (h₂(A)).

   Final:
       slot 2 → C
       slot 5 → B
       slot 7 → A
```

The "cuckoo" name: like a cuckoo chick pushing other eggs out of the
nest. Each evicted key finds a new home by re-hashing.

> [!WARNING]
> If the chain of evictions becomes infinite (cycle), the table must
> be **resized** with new hash functions. This is rare with good
> functions, but possible.

### Why cuckoo is appealing

- **Lookup: always O(1)** — check just 2 slots (or k for k-cuckoo)
- **Delete: always O(1)** — same as lookup
- Inserts can be expensive (cascade of evictions), but bounded on
  average

---

# Dynamic hashing schemes

> [!NOTE]
> **The static schemes assumed you knew the table size up front.**
> Real DBMSes don't — tables grow without notice. Dynamic schemes
> let the hash table grow incrementally.

---

## Chained hashing

> [!IMPORTANT]
> Each slot points to a **linked list of buckets**. Collisions just
> get appended to the same chain.

```
   hash(key) % N

   slot │
   ─────┤
    0   │──► [A,X] ──► [B,Y] ──► [C,Z]
    1   │──► [D,W]
    2   │ (empty)
    3   │──► [E,V] ──► [F,U]
   ...
```

### Insertion / lookup

```
PUT A:
  → hash → slot 0
  → walk chain, find a free slot in a bucket
  → if all buckets full, allocate a new one and link it

GET A:
  → hash → slot 0
  → scan chain comparing keys until found
```

Simple, but chains can grow unboundedly → lookup degrades to O(n).
The next schemes fix this.

---

## Extendible hashing

> [!IMPORTANT]
> Chained hashing that **splits a bucket when it overflows** instead
> of letting the chain grow forever. Multiple slot pointers can point
> to the same bucket — splitting just remaps a subset of them.

```
   Use top 2 bits of hash:           Use top 3 bits when needed:

   00 │──┐                            000 │──┐
   01 │──┼──► Bucket X                001 │──┼──► Bucket X
   10 │──┘                            010 │──► Bucket Y
   11 │──► Bucket Y                   011 │──► Bucket Y
                                      100 │──► Bucket Z (new!)
                                      101 │──► Bucket Z
                                      110 │──► Bucket Y
                                      111 │──► Bucket Y
```

When a bucket overflows, **deepen the directory** (use one more bit
of the hash) for *just that bucket*'s entries. The other buckets keep
their shallower mappings.

**Net effect:** the table grows where it needs to, leaving the rest
alone.

---

## Linear hashing

> [!IMPORTANT]
> Maintain a **split pointer** that walks through buckets one at a
> time. Whenever *any* bucket overflows, split the bucket at the
> pointer (not the overflowing one!) and advance the pointer.

```
   Split pointer
        │
        ▼
   ┌────┐    ┌────┐    ┌────┐    ┌────┐
   │ B0 │    │ B1 │    │ B2 │    │ B3 │
   └────┘    └────┘    └────┘    └────┘

   Some bucket (say B2) overflows  ──►  use overflow chain temporarily
                                        AND split B0 (where pointer is)

   After split:
                   Split pointer
                        │
                        ▼
   ┌────┐    ┌────┐    ┌────┐    ┌────┐    ┌────┐
   │ B0a│    │ B1 │    │ B2 │    │ B3 │    │ B0b│  ← new bucket
   └────┘    └────┘    └────┘    └────┘    └────┘
   (split into B0a and B0b based on hash function rehash;
    pointer moves to B1 next.)

   Eventually pointer wraps around — by then everything has been
   split once, and we switch to a deeper hash function.
```

**The trick:** by always splitting the bucket the pointer is on
(not the overflowing one), you get **uniform amortized work**, even
though overflows can happen anywhere. Splits are predictable; the
pointer just sweeps around the table over time.

> [!NOTE]
> Linear hashing is what some older DBMSes (e.g. Berkeley DB) use.
> It's predictable and simple to implement, but more complex schemes
> like extendible hashing usually win in practice.

---

## When dynamic schemes are used in DBMSes

| Scheme | Used in |
|---|---|
| Linear probing | Many in-memory hash join implementations |
| Cuckoo | Some high-performance KV stores (e.g. memcached variants) |
| Chained | Common for hash indexes in disk-based DBMSes |
| Extendible | PostgreSQL's hash indexes (historically) |
| Linear hashing | Berkeley DB, older systems |

---

## Key takeaways

1. **Hash tables = O(1) average lookups**, but the *constant matters*
   at DB scale — pick a fast hash function (XXHash, FarmHash, etc.).
2. **Cryptographic hashes are not for hash tables** — wrong tradeoff.
3. **Linear probing** is simple and cache-friendly; tombstones or
   movement are needed to handle deletes correctly.
4. **Cuckoo hashing** gives guaranteed O(1) lookup at the cost of
   harder inserts; great when reads dominate.
5. **Dynamic schemes** (chained, extendible, linear hashing) let the
   table grow without knowing the size in advance — necessary for any
   real DBMS workload.
6. **Extendible hashing** grows only where needed (more bits of the
   hash for overflowing buckets); **linear hashing** sweeps a split
   pointer for predictable amortized work.

---

## Things to revisit (with quick refreshers)

### Why does a tombstone require periodic garbage collection?
Tombstones occupy slots but hold no real data — over time, deletes
accumulate and chains get clogged with tombstones, slowing lookups.
The DBMS periodically rebuilds (or partially rebuilds) the table to
reclaim those slots. Similar to how LSM trees compact.

### Why can cuckoo hashing's eviction chain loop forever?
Two keys can have *the same pair* of hash locations. If you keep
evicting them between two slots, neither finds a new home. After a
bounded number of evictions, the table must be **rehashed with new
hash functions**, or **resized**, to break the cycle. In practice
this is rare with good hash functions.

### How does extendible hashing decide which bits of the hash to use?
The directory has a **global depth** (how many bits the whole
directory uses) and each bucket has a **local depth** (how many bits
*this bucket* needs). When a bucket overflows, increment its local
depth and split it. If local depth would exceed global, double the
directory (global depth + 1). Most buckets keep their shallow mapping
— only the hot ones grow.

### What's the difference between extendible and linear hashing?
Extendible: split the **overflowing** bucket immediately, doubling
the directory if needed. Linear: split the bucket the **pointer** is
on (which is *not* necessarily the overflowing one), and use overflow
chains temporarily. Extendible wastes space on the directory but
keeps lookups fast; linear keeps the directory small but lookups may
need to follow overflow chains.

### When does a hash join use one of these vs. another?
Hash joins typically use **simple linear probing or chained
hashing** in memory — speed matters more than memory efficiency,
since the hash table only lives for the duration of the join.
Persistent hash *indexes* on disk are where dynamic schemes (chained,
extendible, linear) come in, because the data on disk needs to grow
with the table.

### Why not just always use B+ trees?
Hash tables don't support **range queries** (`WHERE x BETWEEN 10 AND
20`). B+ trees do, and they're nearly as fast for point queries.
That's why most DBMSes default to B+ tree indexes and use hash
indexes only when range queries don't matter. Hash indexes are
*great* for things like primary key equality lookups, in-memory
joins, and aggregation grouping — but they're niche compared to B+
trees.

---

## Other confusions worth revisiting later

- [ ] Concurrency — how to make any of these schemes thread-safe
      with low latch contention
- [ ] Page-aware variants — how chained hashing maps onto disk pages
      (overflow pages, half-full bucket pages, etc.)
- [ ] Robin Hood hashing — a linear-probing variant that minimizes
      probe variance
- [ ] How hash join's "build" and "probe" phases use these structures
      *(→ join algorithms lecture)*
- [ ] Why PostgreSQL's hash indexes were unsafe pre-10 (crash recovery
      issues) and how they got fixed

---

🔗 **Links:** [[buffer-pools-memory-management]] · [[btree-indexes]] · [[join-algorithms]]