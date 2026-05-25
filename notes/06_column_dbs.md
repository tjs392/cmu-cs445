# Column Store Databases

> [!NOTE]
> **Big idea:** The way you lay out data on disk should match your
> workload. OLTP (small reads/writes) wants row storage; OLAP (huge
> scans, few columns) wants column storage. PAX gets the best of both
> by mixing them. On top of that, compression schemes tailored to
> columnar data can shrink storage and **speed up queries** — especially
> when the DBMS can operate on compressed data directly.

---

## Agenda

1. Database workloads (OLTP, OLAP, HTAP)
2. Storage models (NSM, DSM, PAX)
3. Data compression

> [!IMPORTANT]
> Column stores can take queries that would run for **days** on a row
> store and finish them in **minutes**. The rest of the lecture is
> *why*.

---

# Database workloads

| Workload | What it does | Example |
|---|---|---|
| **OLTP** (Transaction Processing) | Fast operations reading/updating *small* amounts of data | Adding an item to an Amazon cart |
| **OLAP** (Analytical Processing) | Complex queries reading *lots* of data to compute aggregates | "Items bought by 20-somethings in Pittsburgh when temp > 70°" |
| **HTAP** (Hybrid) | OLTP + OLAP on the same instance | The holy grail. Hard. Usually not the best at either. |

> [!TIP]
> **Where AI fits in:** AI workloads go past OLAP — they ask the
> *why* questions, not just the *what*. Think of them as an extension
> of OLAP.

### The workload landscape

```
   Complex   │
             │
             │              OLAP
             │           (analytics)
             │
   ───────── HTAP ───────────────────────
             │
             │   OLTP
             │  (transactions)
             │
   Simple    │
             └──────────────────────► Read-heavy
```

---

## Wikipedia example schema

```sql
CREATE TABLE useracct (
    userID    INT PRIMARY KEY,
    userName  VARCHAR UNIQUE,
    ...
);

CREATE TABLE pages (
    pageID  INT PRIMARY KEY,
    title   VARCHAR UNIQUE,
    latest  INT REFERENCES revisions(revID)
);

CREATE TABLE revisions (
    revID    INT PRIMARY KEY,
    userID   INT REFERENCES useracct(userID),
    pageID   INT REFERENCES pages(pageID),
    content  TEXT,
    updated  DATETIME
);
```

A user account table, an articles/pages table, and a revisions table
holding every new version of every page.

> [!NOTE]
> **Observation:** The relational model is great at describing data
> *logically* — but says **nothing about how to physically store the
> bits and bytes**. That's where storage models come in.

---

## OLTP queries — touch few tuples

```sql
-- Latest revision of one page
SELECT P.*, R.*
FROM   pages AS P
       INNER JOIN revisions AS R ON P.latest = R.revID
WHERE  P.pageID = ?;

-- Update user login info
UPDATE useracct
SET    lastLogin = NOW(),
       hostname  = ?
WHERE  userID = ?;

-- Insert a new revision
INSERT INTO revisions VALUES (?, ?, ...);
```

The bottom two touch **one tuple**. The top touches **two**. All
small.

---

## OLAP query — scans huge swaths

```sql
-- How many .gov visits per month?
SELECT COUNT(U.lastLogin),
       EXTRACT(month FROM U.lastLogin) AS month
FROM   useracct AS U
WHERE  U.hostname LIKE '%.gov'
GROUP BY EXTRACT(month FROM U.lastLogin);
```

This reads **the entire `useracct` table** but only needs two columns
(`hostname`, `lastLogin`). Most of what we load from disk is wasted.

---

# Storage models

| Model | AKA | What it does |
|---|---|---|
| **NSM** (N-ary Storage Model) | Row store | All attributes of a tuple stored together |
| **DSM** (Decomposition Storage Model) | Column store | All values of one attribute stored together |
| **PAX** (Partition Attributes Across) | Hybrid | Row groups, then columns within each group |

Most modern column-store systems actually use **PAX** (option 3).

---

## NSM — Row store

> [!NOTE]
> **All attributes of a single tuple are stored contiguously.** The
> next tuple doesn't start until the previous one ends.

### Physical layout (slotted page)

```
   ┌─────────────────────────────────────────┐
   │ header │ slot array →                   │
   ├────────┴────────────────────────────────┤
   │ [s0][s1][s2]                            │
   │                                          │
   │           (free space)                   │
   │                                          │
   │  ┌──────────────────────────────────┐   │
   │  │ tuple 2: |id|name|email|hostname| │   │
   │  ├──────────────────────────────────┤   │
   │  │ tuple 1: |id|name|email|hostname| │   │
   │  ├──────────────────────────────────┤   │
   │  │ tuple 0: |id|name|email|hostname| │   │
   │  └──────────────────────────────────┘   │
   └─────────────────────────────────────────┘
```

All of `tuple 0`'s fields sit together. Reading one tuple = reading
one chunk.

### NSM on OLTP — the happy case

```sql
SELECT * FROM useracct WHERE userName = ? AND userPass = ?;
```

```
   Index (hash / B+ tree)
        │
        ▼
   record ID:  (page=42, slot=3)
        │
        ▼
   Page directory  ──►  Disk page  ──►  load into buffer pool
                                          │
                                          ▼
                              Tuple 3 — all fields right there ✓
```

One page load → entire tuple in hand. **Perfect.**

### NSM on OLAP — the disaster

```sql
SELECT COUNT(...) FROM useracct WHERE hostname LIKE '%.gov';
```

```
   Page 0:  [tup0: id|name|email|hostname|...]  → only need hostname
            [tup1: id|name|email|hostname|...]
            [tup2: id|name|email|hostname|...]
            ...
   Page 1:  [same story]
   Page 2:  [same story]
   ...
   Page N:  [same story]

   For every page: read ALL the fields, throw away all but `hostname`
```

> [!WARNING]
> Most of the I/O is **wasted**. You loaded 8 KB just to look at one
> 20-byte column.

### NSM summary

| ✅ Advantages | ❌ Disadvantages |
|---|---|
| Fast inserts, updates, deletes | Bad for scans over a subset of attributes |
| Good for queries needing the whole tuple | Terrible memory locality for analytics |
| Plays well with index-organized storage | Not ideal for compression (mixed value domains per page) |

---

## DSM — Column store

> [!IMPORTANT]
> **Flip the layout:** store one attribute's values for *all* tuples
> contiguously. Each column is its own array on disk.

```
   NSM (row store)                 DSM (column store)
   
   Page 0                          Page 0  (only `id`)
   ┌──────────────────┐            ┌──────────────────┐
   │ id|name|host|... │ tup 0      │  0 │  1 │  2 │... │
   │ id|name|host|... │ tup 1      └──────────────────┘
   │ id|name|host|... │ tup 2
   └──────────────────┘            Page 1  (only `name`)
                                   ┌──────────────────┐
                                   │ Alice│Bob│Carol... │
                                   └──────────────────┘

                                   Page 2  (only `hostname`)
                                   ┌──────────────────┐
                                   │a.com│b.gov│c.org... │
                                   └──────────────────┘
```

The DBMS is responsible for **stitching tuples back together** when
queries need multiple columns.

### Physical organization

```
   ┌──────────────────────────────────────┐
   │ header │ null bitmap                  │
   ├────────┴─────────────────────────────┤
   │ a₀ │ a₁ │ a₂ │ a₃ │ a₄ │ a₅ │ a₆ ... │
   └──────────────────────────────────────┘
       fixed-length values in a simple array
```

Notice: **one header per *column*** (not per tuple). Way less
metadata overhead. Way more compressible.

### DSM on OLAP — the happy case

Same `.gov` count query:

```
   Step 1: load the `hostname` column pages only
           ┌─────────────────────────────┐
           │ a.com | b.gov | c.org | ... │  ← scan this
           └─────────────────────────────┘
           filter where ends in `.gov`
           remember matching tuple positions

   Step 2: load the `lastLogin` column pages
           ┌─────────────────────────────┐
           │ t₀ | t₁ | t₂ | ... | tₙ      │
           └─────────────────────────────┘
           pull only the positions that matched
           group by month, count
```

**Zero wasted I/O.** Only the two columns we asked about are read.

---

## Tuple identification in DSM

How do you know which tuple a value belongs to?

| Approach | Description | Recommended? |
|---|---|---|
| **Fixed-length offsets** | Tuple `i`'s value is always at offset `i × size` — implicit positioning | ✅ Yes |
| **Embedded tuple IDs** | Store an explicit tuple ID with each value | ❌ Avoid |

> [!TIP]
> **Why avoid embedded IDs?** You'd waste space (every value carries
> a tuple number) *and* slow scans (every step has to check the ID).
> Fixed-length offsets give you tuple identity for free — position
> implies identity.

### Variable-length data?

Fixed offsets work great for ints, but what about strings?

> [!WARNING]
> Padding strings to a fixed length wastes a lot of space, especially
> for large attributes.

**Better:** use **dictionary compression** to convert variable-length
strings into fixed-length integer codes. (Covered in the compression
section below.)

---

## DSM summary

| ✅ Advantages | ❌ Disadvantages |
|---|---|
| No wasted I/O — read exactly what you need | Slow point queries, inserts, updates, deletes |
| Better locality → cache-friendly for modern CPUs | Need to split/stitch tuples on write |
| Excellent compression (one value domain per page) | Multiple disk writes per logical row write |

> [!NOTE]
> **Key observation:** Column stores win when you have **many columns
> and queries touch few of them**. At some point during query
> execution, you may need to reassemble the original tuple — but you've
> already deferred that work past the I/O bottleneck.

---

## PAX — Partition Attributes Across (the hybrid)

> [!IMPORTANT]
> **The best of both:** get the *processing* benefits of columnar
> layout while keeping the *spatial locality* of row storage.

### Physical organization

```
   ┌─────────────────────────────────────────────┐
   │ File-level metadata directory (offsets to    │
   │ row groups). Stored in the FOOTER for         │
   │ immutable formats like Parquet/ORC.           │
   ├─────────────────────────────────────────────┤
   │                                              │
   │  ┌──── Row group 1 ─────────────────────┐   │
   │  │ metadata header (counts, mins, ...)   │   │
   │  │  ┌─ Column chunk: id ──────────┐     │   │
   │  │  │ [page] [page] [page]         │     │   │
   │  │  └─────────────────────────────┘     │   │
   │  │  ┌─ Column chunk: name ────────┐     │   │
   │  │  │ [page] [page]                │     │   │
   │  │  └─────────────────────────────┘     │   │
   │  │  ┌─ Column chunk: hostname ────┐     │   │
   │  │  │ [page] [page] [page] [page]  │     │   │
   │  │  └─────────────────────────────┘     │   │
   │  └─────────────────────────────────────┘   │
   │                                              │
   │  ┌──── Row group 2 ─────────────────────┐   │
   │  │ (same structure, next group of rows)  │   │
   │  └─────────────────────────────────────┘   │
   │                                              │
   │  ... more row groups ...                     │
   │                                              │
   └─────────────────────────────────────────────┘
```

**Two-level partitioning:**

1. **Horizontal:** split rows into "row groups" (e.g., 100K rows each)
2. **Vertical:** within each row group, store each column's values together

### Parquet's hierarchy

| Level | Contains |
|---|---|
| **File** | Row groups + footer metadata |
| **Row group** | Column chunks (one per column) |
| **Column chunk** | Pages of that one column's values |
| **Page** | Encoded values + page metadata (min, max, count) + repetition/definition levels |

> [!TIP]
> The page metadata (min, max, count) lets the query engine **skip
> entire pages** without reading their data — same idea as the SSTable
> summaries from the LSM lecture. "This page's max is 50, my query
> wants >100, skip it."

This is the format used by **Parquet, ORC, Apache Arrow** — basically
every modern analytical data format.

---

# Database compression

> [!NOTE]
> **Observation:** I/O is the main bottleneck. If you compress pages,
> you move *more useful data* per I/O. Trade-off: compression saves
> DRAM and disk *and* may speed up CPU work (less data to process) —
> but only if compression/decompression doesn't add more cost than it
> saves.

### Compression goals

| Goal | Why |
|---|---|
| **Fixed-length values** | So you can index/scan without parsing |
| **Postpone decompression as long as possible** | Ideal: operate on compressed data directly |
| **Lossless** | Databases can't lose user data; lossy must be done by the app |

### Compression granularity

| Level | Compresses | Notes |
|---|---|---|
| **Block** | A block of tuples for the same table | Coarse |
| **Tuple** | An entire tuple's contents | NSM only |
| **Attribute** | One attribute within a tuple | Can target multiple attributes (overflow) |
| **Column** | Many values for one attribute across tuples | **DSM only — the columnar sweet spot** |

---

## Naïve compression (general-purpose)

Just run gzip / Snappy / LZ4 over a chunk of bytes.

| Consideration | Detail |
|---|---|
| Computational overhead | CPU cost of compress + decompress |
| Compress vs. decompress speed | Often asymmetric (decompress faster) |

### Case study: MySQL InnoDB compression

```
   Buffer pool                        Database file
   ┌────────────────────┐             ┌────────────────────┐
   │ [Mod log]          │             │ [Mod log]          │
   │ [Compressed page]  │  ◄────────► │ [Compressed page]  │
   └────────────────────┘             └────────────────────┘
```

How writes work:
- The compressed page lives in the buffer pool
- Modifications go to a **mod log** (in clear text) appended next to the compressed page
- Often you can write the change to the mod log **without decompressing the page** ✓

How reads work:
- If the read can be served from the mod log → great, no decompression
- Otherwise: decompress page → apply mod log → use → re-compress
  (expensive)

> [!QUESTION]
> **Can the compressed page only compress the part you need?**
> Only **tuple-level** compression schemes support incremental
> decompression. General-purpose algorithms (gzip, Snappy) compress
> the whole block as one unit — you can't decompress a slice.

> [!WARNING]
> **The fundamental problem with naïve compression:** the algorithm
> is an **opaque box** to the DBMS. The DBMS has no idea what the
> compressed bytes mean — only the decompression routine can recover
> the original values. The compression scheme also doesn't know what
> the data *means* — same blind-spot problem as the OS not knowing
> about transactions.

### The dream

Operate on **compressed data directly**, without decompressing first.
If your query is "find all rows where city = 'Pittsburgh'", and
'Pittsburgh' has dictionary code 42, you can scan for `42` in the
compressed column — no decompression needed.

That's where **columnar compression schemes** shine — they're
*designed* to be queryable while compressed.

---

# Columnar compression schemes

The big six:

1. Run-length encoding
2. Bit packing
3. Bitmap encoding
4. Delta encoding
5. Incremental encoding
6. Dictionary encoding

---

## Run-length encoding (RLE)

Store repeated values as `(value, start, count)` triples.

Tip: **sort the column first** if you can — turns scattered repeats
into long runs.

```
   Original column (isDead)         After sorting        RLE
   ┌──────────────────┐             ┌──────────────┐    ┌────────────────────┐
   │ id │ isDead      │             │ id │ isDead  │    │ (false, pos=0, n=5)│
   ├────┼─────────────┤             ├────┼─────────┤    │ (true,  pos=5, n=4)│
   │ 1  │ false       │             │ 1  │ false   │    └────────────────────┘
   │ 2  │ true        │             │ 4  │ false   │
   │ 3  │ false       │             │ 3  │ false   │    9 booleans → 2 tuples
   │ 4  │ false       │             │ 6  │ false   │
   │ 5  │ true        │             │ 8  │ false   │
   │ 6  │ false       │             │ 2  │ true    │
   │ 7  │ true        │             │ 5  │ true    │
   │ 8  │ false       │             │ 7  │ true    │
   │ 9  │ true        │             │ 9  │ true    │
   └────┴─────────────┘             └────┴─────────┘
```

> [!WARNING]
> Sorting helps RLE a lot — but sorting the *whole column* may
> conflict with other access patterns or break correspondence with
> other columns. Real systems handle this carefully.

---

## Bit packing

If your values are `int32` but actually fit in a smaller range, just
use fewer bits.

```
   Original (int32, 32 bits each):
   ┌────────────────────────────────────────┐
   │ 00000000 00000000 00000000 00010111 │ = 23
   │ 00000000 00000000 00000000 01000010 │ = 66
   │ 00000000 00000000 00000000 00000101 │ = 5
   └────────────────────────────────────────┘
   96 bits

   Bit-packed (8 bits each, since max = 66 < 256):
   ┌─────────┬─────────┬─────────┐
   │ 00010111│ 01000010│ 00000101│ = 23, 66, 5
   └─────────┴─────────┴─────────┘
   24 bits — 4× smaller
```

### Patching / mostly encoding

What if values are **mostly** small but some are large?

```
   Values: 23, 66, 5, 99999, 12, 88, 41, 999, 7, ...

   Bit-packed in 8 bits where possible:
   ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
   │  23  │  66  │   5  │PATCH │  12  │  88  │  41  │PATCH │
   └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
                          │                            │
                          └──────────► patch table ◄───┘
                                       ┌────────────────────┐
                                       │ position 3: 99999  │
                                       │ position 7: 999    │
                                       └────────────────────┘
```

A special sentinel ("patch this") means: "look up the real value in
the patch table." You get the size win for the common case, fall back
to wider storage only for outliers.

---

## Bitmap encoding

For columns with **low cardinality** (few distinct values), store one
bitmap per unique value. The *i*-th bit corresponds to the *i*-th
tuple.

```
   Original                    Bitmap encoding
   ┌────┬─────────┐            ┌────────┬───┬───┐
   │ id │ isDead  │            │ row #  │ y │ n │
   ├────┼─────────┤            ├────────┼───┼───┤
   │ 1  │ y       │            │   1    │ 1 │ 0 │
   │ 2  │ y       │            │   2    │ 1 │ 0 │
   │ 3  │ n       │            │   3    │ 0 │ 1 │
   │ 4  │ y       │            │   4    │ 1 │ 0 │
   │ 5  │ n       │            │   5    │ 0 │ 1 │
   │ 6  │ y       │            │   6    │ 1 │ 0 │
   │ 7  │ n       │            │   7    │ 0 │ 1 │
   │ 8  │ y       │            │   8    │ 1 │ 0 │
   └────┴─────────┘            └────────┴───┴───┘
   8 booleans                  Two bitmaps:
                                y: 11010101 (8 bits)
                                n: 00101010 (8 bits)
                                Total: 16 bits — half the storage
                                of a naive bool[8] (assuming 1 byte/bool)
```

> [!TIP]
> Bitmaps shine when you have very few distinct values (gender,
> boolean flags, status codes) and lots of rows. They scale poorly
> when the cardinality is high.

---

## Delta encoding

Store the **differences** between consecutive (or referenced) values
instead of absolute values.

```
   Original (timestamps):
   1735689600, 1735689601, 1735689605, 1735689610, 1735689611, ...
   (~31-bit values, repeating)

   Delta encoded (from previous value):
   1735689600, +1, +4, +5, +1, ...
   (only the first is full-width; deltas are tiny)
```

Great for monotonically-increasing time-series, sensor readings,
sequential IDs.

---

## Dictionary compression

> [!IMPORTANT]
> **The most widely-used compression scheme in DBMSes.** Replace
> repeated values with small integer codes; keep a dictionary mapping
> codes → originals.

```
   Original column (hostnames)        Dictionary
   ┌──────────────────────┐          ┌──────┬─────────────────────┐
   │ a.com                │          │ code │ value               │
   │ b.gov                │          ├──────┼─────────────────────┤
   │ a.com                │          │  0   │ a.com               │
   │ c.org                │          │  1   │ b.gov               │
   │ b.gov                │          │  2   │ c.org               │
   │ a.com                │          └──────┴─────────────────────┘
   │ c.org                │
   │ a.com                │          Encoded column (compact ints)
   └──────────────────────┘          ┌─────────────┐
                                     │ 0 1 0 2 1 0 2 0 │
                                     └─────────────┘
   8 strings (~6 bytes each = 48 B)  → 8 small ints (~2 bits each)
```

### Why dictionaries are powerful

- **One code per attribute value** — perfect for low/medium
  cardinality columns
- Fast point lookups: get the code, scan for the code (no string
  comparisons)
- Range queries: works if the dictionary is **sorted** (codes
  preserve order, so `code < 5` finds all values lexicographically <
  the 5th value)
- Stay compressed through query execution — compare `2 == 2` instead
  of `"a.com" == "a.com"`

> [!NOTE]
> **The ideal dictionary scheme** supports fast encoding *and*
> decoding, for both point and range queries. Sorted dictionaries
> based on the values get range queries for free.

---

## Key takeaways

1. **Workload determines storage.** OLTP = row store; OLAP = column
   store; HTAP = hard.
2. **NSM (rows)** stores all attributes of a tuple together — great
   for whole-tuple access, terrible for wide scans.
3. **DSM (columns)** stores all values of one attribute together —
   great for scans of few columns, slow for tuple-level operations.
4. **PAX** partitions horizontally (row groups) then vertically
   (columns within a group) — the format behind **Parquet, ORC,
   Arrow** and modern analytical engines.
5. **Compression goals:** fixed-length, postpone decompression,
   lossless.
6. **Naïve compression** (gzip, Snappy) treats data as opaque — the
   DBMS can't operate on it without decompressing first.
7. **Columnar compression schemes** (RLE, bit packing, bitmap,
   delta, dictionary) work *with* the DBMS — many allow querying
   without decompression.
8. **Dictionary compression** is the most widely used and bridges
   variable-length data (strings) into fixed-length space (codes).
9. The dream: **operate on compressed data directly** — pushdown
   query predicates into the compressed representation.

---

## Confusions to revisit

- [ ] How sort order interacts with row identity across multiple
      columns in DSM
- [ ] How updates work in a strict column store (vs. PAX's
      immutability assumption)
- [ ] Repetition/definition levels in Parquet (for nested data)
- [ ] When bitmap indexes are stored compressed (RLE-compressed
      bitmaps are common — "compressed bitmap indexes")
- [ ] Vectorized execution — *how* column stores process compressed
      data efficiently in batches
- [ ] HTAP architectures — how systems like SingleStore, TiDB,
      SAP HANA actually pull it off

---

🔗 **Links:** [[buffer-pools-memory-management]] · [[log-structured-storage]] · [[query-execution]]