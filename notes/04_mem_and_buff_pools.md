# Buffer Pools — Memory Management

> [!NOTE]
> **Big idea:** The DBMS is fighting the OS for control of memory and
> I/O. The buffer pool is the DBMS's *own* page cache, and the
> sophistication of its replacement policy + I/O scheduling is one of
> the biggest things separating enterprise databases from
> lightweight open-source ones. This lecture covers advanced
> replacement policies (LRU-K, ARC), localization hints, dirty-page
> handling, and why the OS is an unreliable partner.

---

## LRU-K (1993)

Instead of remembering just the **last** access time (plain LRU),
LRU-K remembers the **last K** access times for each page.

> [!IMPORTANT]
> **Why it's better than LRU:** plain LRU is fooled by one-off scans
> — a single sequential scan touches every page once, pushing all
> "important" pages to the bottom of the LRU list. LRU-K (typically
> K=2) needs a page to be *re-accessed* before it's considered hot.

### Example query

```sql
SELECT * FROM A WHERE id IN (1, 31, 11, 41);
```

### Page history (K = 2)

| Page | Last access #1 | Last access #2 |
|---|---|---|
| `page0` | t=10 | t=20 |
| `page1` | t=15 | t=22 |
| `page2` | t=18 | (only one access) |

### Eviction rule

```
backward distance = current_time − k-th-last access
```

Evict the page with the **largest backward distance** (longest time
since its $K$th-last access). Use oldest single access time to break
ties (pages with fewer than $K$ accesses).

```
                          ┌────────────┐
                          │ Buffer Pool│
                          ├────────────┤
                          │   page0    │
                          │   page1    │
                          │   page2    │  ← victim has biggest
                          └────────────┘     backward distance
```

---

## MySQL: approximate LRU-K

MySQL uses a simpler variant: **one LRU linked list with two entry
points** — *young* and *old*.

```
       New pages
          │
          ▼
       ┌──────────────────────────────────────┐
       │       Old List       │    Young List │
       │  (probationary zone) │  (hot pages)  │
       └──────────────────────┴───────────────┘
          │                                  │
          ▼                                  ▼
       Eviction                           Promotion
       happens here                       on re-access
```

- New pages are always inserted at the **head of the old list**
- Pages get promoted to the **young list** only if they're accessed
  again while still in the old list
- This is approximate LRU-2: "needs at least two accesses to be hot"

> [!TIP]
> **Why this design wins on scans:** a one-off sequential scan touches
> pages once, pushing them into the old list — where they're evicted
> first without ever displacing genuinely hot pages on the young list.

---

## ARC — Adaptive Replacement Cache (2003)

Developed at **IBM Research**. Used in IBM DB2, PostgreSQL (briefly,
later rewritten), and ZFS.

> [!NOTE]
> **Key idea:** support both **recency** (LRU) and **frequency**
> (LFU) by maintaining two lists, and **adapt** the size of each
> based on workload access patterns.

### The four lists

| List | Purpose |
|---|---|
| **T1** (Recent) | Pages accessed *once* recently |
| **B1** (Recent Ghost) | History of pages recently evicted from T1 |
| **T2** (Frequent) | Pages accessed *at least twice* |
| **B2** (Frequent Ghost) | History of pages recently evicted from T2 |

Plus an adaptive **target size parameter `p`** that adjusts how much
to favor recency (T1) vs. frequency (T2).

### Ghost lists — the clever part

```
   T1 (in pool)  ─── evict ──►  B1 (ghost: just remembers IDs)

   T2 (in pool)  ─── evict ──►  B2 (ghost: just remembers IDs)
```

Ghost lists store **only page IDs**, no data — cheap. On a cache
miss, if the page is found in B1 or B2, ARC knows it just evicted
this page, so it **adjusts `p`** to grow whichever list (T1 or T2)
would have kept it.

### ARC lookup protocol

| Scenario | Action |
|---|---|
| Hit in T1 → move to T2 | Page is now "frequent" |
| Hit in T2 → stay in T2 (MRU) | Reinforce frequency |
| Miss, found in B1 | Increase `p` (favor recency) — bring page in |
| Miss, found in B2 | Decrease `p` (favor frequency) — bring page in |
| Miss, not in any ghost list | Standard cold miss |

> [!IMPORTANT]
> The ghost lists are what make ARC **adaptive**: by remembering what
> it *just* evicted, ARC can tell whether the workload is more
> recency-biased or frequency-biased and shift its allocation in
> response — *without* the DBA having to tune anything.

---

## Better policies: localization

> [!TIP]
> **Insight:** the DBMS knows *how* the query will access pages.
> Don't be a generic cache — use that knowledge.

**Example: sequential scan over a one-shot table.** Don't pollute the
global buffer pool — those pages won't be reused. Use a **side
buffer** (a *ring buffer*) just for this query.

### Postgres ring buffer

Postgres maintains a small **circular ring buffer** for big
sequential scans:

```
   ┌─────────────────────┐
   │   Ring buffer       │
   │                     │
   │  ┌───┬───┬───┬───┐  │
   │  │ A │ B │ C │ D │  │  ← scan reuses these slots
   │  └───┴───┴───┴───┘  │
   │       ↑             │
   │   reused for         │
   │   next page          │
   └─────────────────────┘
```

Super cheap to implement. The big buffer pool stays untouched by the
scan.

---

## Priority hints

The buffer pool manager can inspect **what's actually in the page**
to make smarter eviction choices.

### Example: B-tree index pages

```
              Root (page 0)        ← stay in cache!
                  │
       ┌──────────┼──────────┐
       │          │          │
   Inner pages (subtree roots)    ← also valuable
       │          │          │
   …leaves…    …leaves…   …leaves…   ← evictable
```

A B-tree's **root page** is touched by *every* query through the
index. Evicting it would be catastrophic. The buffer pool can be
hinted: "this is index page level 0 — protect it."

### Other useful hints

- **Transaction context**: if a query updates the same row many
  times, the DBMS can defer flushing dirty pages until the
  transaction commits
- **Query plan info**: "this scan won't repeat" → ring buffer
- **Data type info**: "log pages are critical; leaf pages are
  evictable"

> [!IMPORTANT]
> **This is one of the big enterprise vs. open-source separators.**
> Sophisticated buffer pool managers cost serious engineering effort
> — the kind enterprise DBMS vendors charge for.

---

## Dirty pages

When evicting a page, two paths:

| Path | When | Cost |
|---|---|---|
| **Fast** | Page is clean (not modified) | Just drop it |
| **Slow** | Page is dirty | Must write to disk first |

> [!WARNING]
> **The tradeoff:** evict a dirty page fast → repeated writes if the
> page is read again. Hold onto dirty pages → eviction slows down
> when memory is tight.

---

## Background writing

The DBMS runs a **background writer** that periodically walks the
page table and proactively writes dirty pages to disk.

```
   ┌─────────────────────────────────────┐
   │            Buffer pool              │
   │                                     │
   │  [P0*] [P1 ] [P2*] [P3 ] [P4*] ...  │   * = dirty
   └────────┬─────────────┬─────────────┘
            │             │
       Background writer flushes dirty pages
            │             │
            ▼             ▼
              ┌──────────────┐
              │     Disk     │
              └──────────────┘
```

After a successful flush, the DBMS can either:
- **Evict** the page, or
- **Clear the dirty flag** (page stays in cache, now clean)

> [!IMPORTANT]
> **Critical ordering constraint:** never write a dirty page to disk
> **before** its log records are written. Otherwise crash recovery is
> impossible. This is the **Write-Ahead Log (WAL) protocol** — covered
> in later lectures.

Also: balance background writing — too aggressive wastes I/O on
pages that'll get re-dirtied; too lazy and crash recovery has more
to redo.

---

## OS vs. DBMS: I/O scheduling

> [!NOTE]
> **The OS and hardware try to maximize disk bandwidth** by reordering
> and batching I/O requests. They do this well — but they don't know
> *which requests matter most*.

### What the OS knows

- Disk geometry, request queue, batching
- Process-level I/O priorities (`ionice`, etc.)

### What the OS doesn't know

- Thread-level priority within one process
- Which pages belong to a critical query vs. a background scan
- That index pages are more important than leaf pages
- Transaction state, query plan, user SLAs

---

## DBMS internal I/O scheduling

The DBMS keeps its own internal queue and reorders requests using
knowledge the OS lacks:

| Distinction the DBMS can make | Optimization |
|---|---|
| Sequential vs. random I/O | Sort random page IDs to convert to sequential |
| Critical path vs. background | Prioritize critical queries |
| Table vs. index vs. log vs. ephemeral data | Log is high priority |
| Per-transaction context | Batch related I/Os |
| User SLAs | Prioritize paid tiers |

> [!WARNING]
> The OS doesn't know any of this, and it'll **get in the way** of
> your beautifully-tuned DBMS scheduler if you let it.

---

## OS page cache — the duplicate problem

When you call `read()` on a normal file, the bytes go through:

```
   read()
     │
     ▼
   ┌─────────────────────┐
   │   OS page cache     │   ← cached here
   └──────────┬──────────┘
              │
              ▼
   ┌─────────────────────┐
   │     File system     │
   └──────────┬──────────┘
              │
              ▼
   ┌─────────────────────┐
   │        Disk         │
   └─────────────────────┘
```

If the DBMS also has its **own** buffer pool, you now have **two
copies** of every page in memory — one in the OS page cache, one in
the DBMS buffer pool. Wasteful.

### Direct I/O — bypassing the OS cache

```c
open("data.db", O_DIRECT | O_RDWR, ...);
```

`O_DIRECT` tells the OS: *"don't cache this — read straight from
disk."*

```
   read()
     │
     │     (skips OS page cache)
     ▼
   ┌─────────────────────┐
   │        Disk         │
   └─────────────────────┘
```

It's a **POSIX command, no kernel changes needed**. Most serious
DBMSes use it.

> [!QUESTION]
> **Which major DBMS doesn't use direct I/O?**
> **PostgreSQL.** Something in its design genuinely relies on the OS
> page cache, so Postgres deliberately *uses* the double-buffering
> tradeoff.

You can even have the DB talk **directly to the hardware** (bypass
the file system entirely), though this requires more engineering.

---

## fsync — and why the OS is your enemy

### What happens when the DBMS writes?

| Call | Effect |
|---|---|
| `fwrite` / `write` | Data goes to **kernel buffer** — *not* on disk yet |
| `fsync` | **Blocks** until kernel confirms data is flushed to disk |

`fsync` is how the DBMS knows its data has actually hit persistent
storage. Critical for durability.

### What if `fsync` fails?

> [!WARNING]
> Here's where it gets ugly. On Linux:
>
> 1. `fsync` fails (e.g., disk error, hardware issue)
> 2. **Linux marks the dirty pages as clean anyway**
> 3. DBMS retries `fsync`
> 4. Linux says "✅ flush successful!" — because from its POV there
>    are no dirty pages
> 5. **The DBMS assumes the data is persisted. It isn't.**
>
> The OS just lied to your database.

This caused a major incident in PostgreSQL ("**fsyncgate**", 2018).
For years, PostgreSQL had been treating `fsync` errors as recoverable
— retrying after a failure. On Linux, this could cause **silent data
loss**: a failed write would be forgotten, the retry would succeed,
and the database would believe data was durable that had actually
been thrown away.

The fix (after community uproar): on `fsync` failure, **PostgreSQL
panics and crashes the database**, forcing crash recovery from the
WAL. Better to halt loudly than corrupt silently. Other databases
audited their code afterward; many had the same bug.

> [!IMPORTANT]
> **The lesson:** the OS is not your friend. It optimizes for the
> general case (most apps don't care about fsync errors). A DBMS does
> care — extremely — and has to defend against the OS misleading it.

---

## Key takeaways

1. **LRU-K and MySQL's young/old list** beat plain LRU by needing
   multiple accesses before a page is considered hot — protects
   against scan pollution.
2. **ARC** uses ghost lists to remember recent evictions and
   *adapts* between recency and frequency without manual tuning.
3. **Localization**: use side buffers (ring buffers) for one-off
   scans so they don't pollute the main pool.
4. **Priority hints** based on page type (index root, log, leaf)
   make the buffer pool *workload-aware*.
5. **Dirty page handling** trades eviction speed against avoiding
   unnecessary writes. Background writers smooth this out — but
   must respect WAL ordering.
6. **The DBMS does its own I/O scheduling** because the OS lacks
   the context (sequential vs. random, critical path, transaction
   info).
7. **Direct I/O** bypasses the OS page cache to avoid double
   buffering. Most DBMSes use it; Postgres doesn't.
8. **The OS lies about fsync failures** on Linux. The DBMS must
   defend itself — usually by panicking on failure rather than
   trusting a retry.

---

## Confusions to revisit

- [ ] How exactly the `p` parameter in ARC is adjusted on each hit/miss
- [ ] What "page table" means inside a DBMS (vs. OS page tables)
- [ ] WAL ordering rules — what must happen before what
- [ ] When/how O_DIRECT alignment requirements bite you
- [ ] Postgres's specific dependency on the OS page cache — why exactly?
- [ ] The full fsyncgate story — what other systems had the same bug?

---

🔗 **Links:** [[buffer-pools-intro]] · [[wal-logging]] · [[crash-recovery]]