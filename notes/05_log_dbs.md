# Log-Structured Storage

> [!NOTE]
> **Big idea:** Tuple-oriented storage (slotted pages) has fundamental
> problems with writes — fragmentation, useless I/O, random I/O. Two
> alternative philosophies fix this: **index-organized storage** (the
> index *is* the table), and **log-structured storage** (don't update
> in place — append a log and compact later). LSM-trees (RocksDB,
> Cassandra, LevelDB, modern NoSQL) all build on this.

---

## Recap & today's agenda

| Last class | Today |
|---|---|
| Buffer pool manager — where DBMS caches pages | Buffer pool optimizations |
|  | Tuple-oriented storage |
|  | Index-oriented storage |
|  | Log-structured storage |

---

# Buffer pool optimizations

Three sophistications layered on top of basic buffer pool:

1. Multiple buffer pools
2. Pre-fetching
3. Scan sharing

---

## Multiple buffer pools

> [!TIP]
> **Insight:** the buffer pool doesn't have to be one giant block of
> memory. You can break it into sub-pools, each with its own page
> table and replacement policy.

```
   ┌──────────────────────────┐
   │     Total memory         │
   │                          │
   │  ┌────────┐  ┌────────┐  │
   │  │ Pool A │  │ Pool B │  │  ← different policies,
   │  │  LRU   │  │  ARC   │  │     different lifetimes,
   │  └────────┘  └────────┘  │     different workloads
   │  ┌────────┐  ┌────────┐  │
   │  │ Pool C │  │ Pool D │  │
   │  │ ring   │  │ LRU-2  │  │
   │  └────────┘  └────────┘  │
   └──────────────────────────┘
```

Common partitionings: **per database, per table, per page type**.

Benefits:
- **Reduces latch contention** — fewer threads fighting for the same data structures
- **Improves locality** — keep related pages together

### Two approaches for picking which pool a page lives in

| Approach | How |
|---|---|
| **Object ID** | Embed an object ID in record IDs; maintain a mapping from objects → buffer pool |
| **Hashing** | Hash the page ID to pick the pool |

In either case, ensure a page lives in **exactly one** pool (or you create coherence problems).

> [!NOTE]
> Beneath all the pools is **one disk scheduler** that sees all
> requests, so it can still reorder and batch globally.

---

## Pre-fetching

The DBMS knows the query plan — so it knows what pages a query
*will* need next. Why wait?

> [!IMPORTANT]
> **`mmap` and the OS can pre-fetch sequentially**, but only that.
> They can't follow non-sequential access patterns. The DBMS can.

### Example: range scan via index

```sql
SELECT * FROM A WHERE val BETWEEN 100 AND 250;
```

```
                  ┌─── root ───┐
                  │            │
              ┌───┴───┐    ┌───┴───┐    ← inner index pages
              │       │    │       │
        ┌──┬──┬──┐  ┌──┬──┬──┐
        │L0│L1│L2│  │L3│L4│L5│      ← leaf pages
        └──┴──┴──┘  └──┴──┴──┘
              ▲   ▲   ▲   ▲
              │   │   │   │
            scan visits: 2, 3, 5, 6
            (not 0, 1, 4 — not in range)
```

The OS can't follow this — it'd just see "random page reads." The
DBMS knows it'll need leaf pages `2 → 3 → 5 → 6` and can pre-fetch
them.

---

## Scan sharing

> [!TIP]
> **Insight:** if two queries both need to scan a big table, let the
> second one *piggyback* on the first one's cursor instead of
> starting its own scan from scratch.

Also called **synchronized scans**.

### Example

```sql
-- Query 1 already running
SELECT SUM(val) FROM A;

-- Query 2 arrives mid-scan
SELECT AVG(val) FROM A;
```

```
   Page:   0   1   2   3   4   5   6   ...   N
                       ▲
                   Q1 here, scanning forward
                   
   Q2 arrives → attaches to Q1's cursor at page 3
              → both queries see pages 3..N together
              → Q2 then loops back to scan pages 0..2 to finish
```

Q2 only loads page 0–2 fresh; pages 3–N come "for free" while Q1 is
scanning them anyway. Two queries, ~one disk pass.

> [!NOTE]
> **Scan sharing vs. result caching:** result caching memoizes the
> *answer* to a query for reuse. Scan sharing reuses the *page I/O*
> mid-scan, even if the queries produce different results
> (different aggregations, different filters).

Supported in: **DB2, MSSQL, Teradata, PostgreSQL**. Relatively rare
overall.

---

# Storage layouts

So far we've been assuming **slotted pages** — let's recap, then
contrast with other approaches.

## Slotted pages (most common row-oriented layout)

```
   ┌──────────────────────────────────────┐
   │ header │ slot array →                │
   ├────────┴─────────────────────────────┤
   │ [s0][s1][s2][s3]                     │
   │                                       │
   │            (unused space)             │
   │                                       │
   │                          tuple 3      │
   │                  tuple 2              │
   │              tuple 1                  │
   │      tuple 0                          │
   └──────────────────────────────────────┘
```

- Slot array grows from the front, tuples grow from the back
- Slot `i` holds the offset of tuple `i` inside the page
- Find a tuple → look at slot, jump to offset

### Record IDs

A way to physically locate a tuple in the database:

```
record_id = (file_id, page_id, slot_#)
```

- The page directory maps logical objects (tables, indexes) → files
- Each tuple gets a unique record ID for its physical location
- **Most DBMSes don't store record IDs in the tuple itself**
- **SQLite** is an exception — it uses `rowid` as the true primary key, stored as a hidden attribute

> [!WARNING]
> **Applications should never rely on record IDs meaning anything.**
> They can change as data moves — they're internal bookkeeping.

---

## Tuple-oriented storage: reads

Reads are straightforward:

1. Get a tuple by record ID
2. Look up the page in the page directory
3. Fetch the page from disk if not in buffer pool
4. Use the slot array to find the offset within the page

> [!NOTE]
> The DBMS relies on **indexes** to find tuples, because the table
> itself is **inherently unsorted** (heap files). The index gives you
> the record ID; you then do the lookup above.

---

## Index-organized storage

> [!IMPORTANT]
> **What if the index *is* the table?** Skip the separate
> index-then-table lookup — store the tuples *inside* the index's
> leaf nodes.

```
                  ┌─── root ───┐
                  │  (keys)    │
                  └──┬──────┬──┘
                     │      │
              ┌──────┘      └──────┐
              ▼                    ▼
        ┌────────────┐       ┌────────────┐
        │ Leaf page  │       │ Leaf page  │
        │ ┌──┬──┬──┐ │       │ ┌──┬──┬──┐ │
        │ │k0│k1│k2│ │  ...  │ │k7│k8│k9│ │   ← slot array
        │ └──┴──┴──┘ │       │ └──┴──┴──┘ │
        │            │       │            │
        │  tuple 2    │       │  tuple 9   │
        │  tuple 1    │       │  tuple 8   │
        │  tuple 0    │       │  tuple 7   │   ← tuples stored
        └────────────┘       └────────────┘     INSIDE the leaves
```

- Leaf nodes look like slotted pages internally — header maps keys → offsets, tuples live at the back
- Tuples are **sorted by key** within the page
- Lookup = traverse the tree → land on a leaf → binary search the offset array

### Who uses this?

| DBMS | Default? |
|---|---|
| **SQLite** | Yes — only option, this is how it organizes data |
| **Oracle / SQL Server** | Slotted pages by default, index-organized as an option |
| **MySQL (InnoDB)** | Index-organized on the primary key (a clustered index) |

Great for reads — one tree traversal, no separate index→table hop.
But it doesn't solve the **write** problems coming up next.

---

## Tuple-oriented storage: writes (the problem)

### Insert

1. Check page directory for a page with a free slot
2. Fetch page
3. Use slot array to find empty space that fits

### Update

1. Check page directory
2. Fetch page
3. Find offset via slot array
4. **If new data fits:** overwrite in place
5. **Otherwise:** mark old tuple deleted, insert new version on a *different* page

> [!WARNING]
> Updates are expensive. And they create three structural problems:

| Problem | What goes wrong |
|---|---|
| **Fragmentation** | Pages aren't fully utilized — gaps from deleted/moved tuples |
| **Useless disk I/O** | Must fetch a whole page (typically 4–8 KB) to change one tuple |
| **Random disk I/O** | Updating many tuples = scattered writes across many pages |

Plus: **what if the DBMS can't overwrite pages at all?** Some storage
media (early SSDs, append-only file systems, modern cloud storage)
*physically can't* update in place — they can only write new pages.

This is what motivates a totally different approach.

---

# Log-structured storage

> [!IMPORTANT]
> **The pivot:** instead of updating tuples in place inside pages,
> the DBMS maintains a **log** that records every change. Updates
> become **appends**. The log is periodically compacted to merge and
> discard outdated entries.

This is the foundation of **LSM-trees** (Log-Structured Merge trees),
used by RocksDB, Cassandra, HBase, LevelDB, ScyllaDB, and many
modern key-value stores.

### High-level flow

```
   Writes
     │
     ▼
   ┌─────────────┐
   │  MemTable   │    in memory (sorted, e.g. red-black tree)
   │             │
   └──────┬──────┘
          │ flush when full
          ▼
   ┌─────────────┐
   │  SSTable    │    on disk (sorted, immutable)
   └─────────────┘
          │
          │ periodically compact
          ▼
   (older, larger SSTables at lower levels)
```

---

## Writes: into the MemTable

The MemTable is an in-memory sorted structure (typically a balanced
tree). Writes just go here:

```
PUT(key101, a_1)    →  MemTable: { key101: a_1 }
PUT(key101, a_2)    →  MemTable: { key101: a_2 }   ← in-place update (it's in RAM)
PUT(key102, b_1)    →  MemTable: { key101: a_2, key102: b_1 }
```

Because it's in memory, updates to the same key just overwrite —
fast and free.

---

## Flush: MemTable → SSTable

When the MemTable fills up, walk the tree's leaves (in key order)
and write them out to a **Sorted String Table (SSTable)** on disk:

```
   MemTable (in memory)                  SSTable (on disk)
   ┌──────────────────┐                  ┌──────────────────┐
   │ key101 → a_2     │                  │ key100  →  z_1    │
   │ key102 → b_1     │   ──flush──►     │ key101  →  a_2    │
   │ key100 → z_1     │                  │ key102  →  b_1    │
   │ ...              │                  │ ...              │
   └──────────────────┘                  └──────────────────┘
                                          sorted by key,
                                          written sequentially
```

> [!TIP]
> Key property: **SSTables are immutable**. Once written, they're
> never modified. New data → new SSTable.

After flushing, the MemTable is reset; new writes start filling a
new one.

---

## Levels: how SSTables accumulate

SSTables organize into **levels**, with each lower level holding
older, larger files:

```
   Level 0:  [SST_a]  [SST_b]  [SST_c]   ← newest, recently flushed
                 │
                 │  compaction
                 ▼
   Level 1:  [    SST_d    ]              ← merged from L0
                 │
                 │  compaction
                 ▼
   Level 2:  [        SST_e        ]      ← older, larger
                 │
                 ▼
   Level 3:  [            SST_f            ]   ← oldest, largest
```

Each level is larger than the previous (often by 10×). When a level
fills, its SSTables get merged into a single sorted SSTable at the
next level down.

> [!QUESTION]
> **Why multiple levels — why not just merge within Level 0?**
> Read performance. A read may need to check every SSTable that
> could contain the key. Fewer levels with larger sorted files → faster
> lookups. Many small files at one level → slow.

---

## Reads: the search across all levels

```c
GET(key101);
```

1. **Check the MemTable** first — if the key is there, return.
2. Otherwise, **check SSTables level by level**, newest first (so we get the most recent value for the key).

> [!WARNING]
> Worst case: read has to touch *every* SSTable. That's terrible.

### Mitigation: summary metadata

Each SSTable gets a small in-memory **summary**:

| Field | What it does |
|---|---|
| **Min/max key** | Skip SSTables whose key range doesn't include your target |
| **Bloom filter** | Quickly tell "this key is definitely not here" without disk I/O |

```
   Looking up key=42

   Check summary table:
   ┌────────┬───────────┬───────────────┐
   │ SSTab  │ Key range │ Bloom filter  │
   ├────────┼───────────┼───────────────┤
   │ SST_a  │ 100–200   │ ✗ skip        │   ← range excludes
   │ SST_b  │   0–80    │ ✓ maybe       │   ← check it
   │ SST_c  │   1–50    │ ✗ filter says no │   ← bloom says not here
   │ SST_d  │   1–60    │ ✓ maybe       │   ← check it
   └────────┴───────────┴───────────────┘
```

Now only 2 SSTables need actual disk reads, not all of them.

> [!QUESTION]
> **What's in the filter — a range or a hash?**
> Could be either. Typically a **Bloom filter** (a probabilistic hash
> structure that says "definitely no" or "maybe yes" for key
> membership). The min/max key gives you the range filter for free.

---

## Deletes — also become appends

Deletion in an immutable-SSTable world isn't really deletion:

```
DELETE(key101)    →  MemTable: { key101: TOMBSTONE }
```

A **tombstone** — a marker that says "this key is dead." On reads,
when you hit a tombstone for a key, return "not found" (don't keep
searching deeper levels). Tombstones get garbage-collected during
compaction.

---

## Compaction — keeping the system from drowning

> [!IMPORTANT]
> **Without compaction, LSM-trees would grow forever**, reads would
> get slower, and disk usage would explode. Periodic compaction
> merges SSTables and discards stale data.

### Sort-merge across two SSTables

Assume order is newest → oldest. Walk two SSTables with cursors:

```
   SSTable_new (newer)        SSTable_old (older)
   ┌──────────────┐            ┌──────────────┐
   │ key100 → v_1 │ ◄─cursor   │ key101 → w_1 │ ◄─cursor
   │ key101 → v_2 │            │ key103 → w_2 │
   │ key105 → v_3 │            │ key106 → w_3 │
   └──────────────┘            └──────────────┘

   Output:
   ┌──────────────┐
   │ key100 → v_1 │   ← from newer
   │ key101 → v_2 │   ← from newer (older's key101=w_1 discarded)
   │ key103 → w_2 │   ← only in older
   │ key105 → v_3 │   ← from newer
   │ key106 → w_3 │   ← only in older
   └──────────────┘
```

For each pair of cursors:
- Lesser key wins → write it, advance that cursor
- Equal keys → keep the newer, discard the older, advance both

This is **leveled compaction**. There are other strategies.

---

## Compaction strategies

### Strategy 1: Leveled compaction

- Data organized into **levels** with an SSTable size limit per level
- SSTables at the same level have **non-overlapping** key ranges (except Level 0)
- **Level 0** = recently-flushed SSTables, *may overlap*
- Compaction merges a file from level $N$ into level $N+1$

> [!WARNING]
> **The cost:** when a Level 1 (say, 10 GB) absorbs a new file, you
> may have to **rewrite the entire 10 GB** to maintain sorted,
> non-overlapping ranges. Write amplification can be significant.

### Strategy 2: Universal compaction

- All SSTables in a **single level**, no hierarchy
- Compact when too many overlap, or size thresholds are exceeded
- **Faster writes** (less rewriting), **slower reads** (more SSTables to search)
- Best for **insert-heavy** workloads and time-series queries

| Strategy | Optimizes for | Trade-off |
|---|---|---|
| Leveled | Read speed | Higher write amplification |
| Universal | Write speed | Slower reads, more space overhead |

> [!QUESTION]
> **What triggers compaction?**
> Implementation-dependent — file count, level size, age, time since
> last compaction, or some combination. Each system tunes differently.

---

## Discussion

Log-structured storage is **far more common today** than in past
decades. The main driver:

> [!NOTE]
> **RocksDB** (Facebook's open-source LSM engine, forked from
> LevelDB) became the storage layer for an enormous number of
> systems: Cassandra-like databases, MyRocks (MySQL), TiKV,
> CockroachDB, YugabyteDB, ScyllaDB, and many more.

### Downsides

| Downside | Why |
|---|---|
| **Write amplification** | One logical write triggers many physical writes (memtable flush, compaction rewrites) |
| **Compaction overhead** | Costly background work; CPU + I/O + scheduling complexity |
| **Read amplification** | A read may consult MemTable + multiple SSTables + Bloom filters |
| **Space amplification** | Old versions stick around until compaction runs |

These are the classic **RUM (Read–Update–Memory) trade-offs** —
LSMs trade *read* and *space* amplification for *write* friendliness.
B-trees go the opposite way.

---

## Key takeaways

1. **Multiple buffer pools** reduce contention and enable
   per-workload tuning.
2. **Pre-fetching** lets the DBMS anticipate non-sequential access
   the OS can't see.
3. **Scan sharing** lets multiple queries piggyback on one cursor.
4. **Slotted pages + heap files** are the classic layout but suffer
   fragmentation, useless I/O, and random I/O on writes.
5. **Index-organized storage** stores tuples *in* index leaves —
   great for reads, no help with writes (SQLite, MySQL InnoDB).
6. **Log-structured storage** turns updates into appends:
   MemTable → SSTable (immutable, sorted) → levels of SSTables,
   compacted over time.
7. **Reads** check MemTable then SSTables newest-to-oldest, using
   per-SSTable summaries (min/max key, Bloom filter) to skip files.
8. **Deletes** are tombstones — also appends; cleaned up during
   compaction.
9. **Compaction strategies** (leveled vs. universal) trade read
   speed for write amplification.
10. **LSM trees** dominate modern KV storage thanks to RocksDB and
    cloud-friendly append-only workloads.

---

## Confusions to revisit

- [ ] Exactly how Bloom filters work (hash functions, false-positive math)
- [ ] How write-ahead logging coexists with the MemTable (durability before flush)
- [ ] When MyRocks vs. InnoDB wins (workload patterns)
- [ ] Tiered vs. leveled compaction in more detail — the size-amp math
- [ ] How range scans work on an LSM (merging multiple SSTables on the fly)
- [ ] RUM trade-offs framework: when is each layout *optimal*?

---

🔗 **Links:** [[buffer-pools-memory-management]] · [[btree-indexes]] · [[wal-logging]]