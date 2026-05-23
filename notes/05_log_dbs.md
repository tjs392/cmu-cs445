# Log Structured Storage

Last Class:
Buffer pool manager as the location of where the DBMS stores copies of database pages it retrieves from no volatile storage

Today's Agenda:
Buffer Pool Optimizations
Tuple-Oriented Storage
Index-Oriented Sotrage
Log Structured Storage


Buffer Pool OPtimizations:
- Multiple buffer Pools
- Pre fetching
- Scan sharing

Multiple Buffer Pools:
- THE DBMS doesnt have to be just a single block of memory, you can break it up into subarrays and have a page table per subarray and have different policies/agorithms for each buffer pool

- One per db, maybe one per table, one per page type, who knows
- Partitioning memory across multiple pools helps reduce latch contention and improve locality.

Approach #1: Object ID
- Embed an object id in record ids and then maintain a mapping from objects to specific buffer pools
- Say you have 2 buffer pools, you can pick apart the id, and then put the record in a certain buffer pool. And make sure that you only have a certain page in one buffer pool at a time.

Approach #2: Hashing
- Hash the page id to select which buffer pool to access

There is a disk scheduler underneath, so that the disk scheduler can see all the requests, sort things, and do these in a good manner

Pre Fecthing:
- The DBMS can also prefetch pages based on a query plan
- If we see a scan occuring on data, it's scanning through the disk pages, we can recognize that we have this sequential access pattern, why not just start prefetching stuff from the disk page into the buffer pool so we dont stall on wait.
- mmpa can do this, os can do this, buuuut it can only prefetch sequentially, nothing more sophisticated

SELECT * FROM A WHERE val BETWEEN 100 AND 250
Assume some tree data structure forthe index-pages.
So on the leaf nodes you have the key order.

Assume you're scanning index-page0 -> index-pageN
Scanning across the leaf nodes, you know you'll need like page 2-> page3 -> page5 -> page 6. not 0 1 and 4, because the leaf nodes are scanning like sequentially (better with a visualization)

The os can't do this, but a DBMS can do this.

Scan Sharing
- allow multiple queries to attach a single cursor that scans a table
- say you have some queries that scan multiple tables, allow the queries to piggyback and ride along the first query - see all the stuff it sees, etc.
- synchronized scans
- this is different rom result caching result caching is like run a query, produce an output, if the same query turns up - do an output
- This is pretty rare

Examples:
- supported in DB2, MSSQL, TERADATA, POSTGRES

Scan Sharing
SELECT SUM(val) FROM A

Say one query is scanning the disk pages into the buffer pool, another query shows up says it wants to do an entire scan on the table, but maybe a different aggregation. just piggyback Q2 on Q1 with the data that it's going for.

so say Q1 go to page 3 -> page n, piggybacking on Q1. and then it goes to the beginning -> page 3 non inclusive here ();

Simple.


Previously:
We presented a disk oriented architecture where the DBMS assumes that the primary storage location of the database is on non voltaile disk

We then discussed a page oriented storage scheme for organizing tuples across pages

Going to be looking at index organized storage and log structured storage today

Slotted Pages:
- most common layout for row oriented dbs
- take any tuple, just fit it into the page which a header and a slot array at the beginning, tuples starting at the end
- the slot array records offsets inside the page that tells how to find the beginning point for each tuple

Record IDS
- Way to physically find a tuple within the database
- Page Directory stores the File Id for some object, 
- The DBMS assigns each logical tuple a unique record identifer that represents its physical location in the DB
    - example: file id page id slot #
    - most DBMSs do not stores ids in tuple
    - SQLite uses rowid as the true primary key and stores them as a hidden attribute
Applications should never rely on these ids to mean anything


Tuple oriented Storage: Reads
- reads are pretty simple
    - just get an existing tuple using its record id
    - check page directory to find the page
    - retrieve the page from disk if not in mem
    - find offset in page using slot array
- The DB'S relies on idexes to find individual tuples because the tables are inherently unsorted.

What if we didnt have to do a separate lookpu in the index and the table heap?

Index Organized Storage
- Use the index as the storage itself
- Stil use page layout that looks like a slotted page
- tuples are typically sorted in page based key

- in the leaf nodes, instead of storing record id, just store the tuple itself (B+ tree, skip list, trie, whatever)
- within the leaf nodes, it looks a lot like the slot array
- in the header, have this key -> offsetarray mapping that are offsts within the page for the tuple that you want
- now for the lookups, scan down through the inner nodes of the tree, land on leaf node, then do binary search into offset array to find the tuple thats in the page.

- SQLLite famously gives this to you. It's how they organize the data in their system
- With oracle and SQLServer, default is slotted page, but you can do index organized storage

So this solves the reads problems, but what about writes?

Tuple Oriented Storage: Writes
Insert a new tuple
- check page dir with a free slot 
- retrieve the page from disk
- check slot array to find empty space in page that will fit

Update an existing tuple using its record id:
- Check page dir
- Retreiev page from disk
- Find offset in page using slot array
- If new datafits, overwrite existing data
    - otherwise, mark existing tuple as deleted and insert new version in a different page 
This update is expensive

Problem 1: Fragmentation
- pages are not fully utilized (unusable space, empty slots)
Problem 2: Useless Disk IO
- DBMS must fetch entire page to update one tuple 
Problem 3: Random Disk I/O
- Worst case scenario when updating multiple tuples is that each tuple is on a separate page
Also, what if the DBMS cannot overwrite data in pages and could only cretae new pages?

Because of these issues, we come up with log Structured Storage
Instead of storing tuples in pages and updating them in place, the DBMS maintains a log that records changes to tuples.
- Each log entry represents a tuple PUT/DElete operation
The DBMS applies changes to an in memory data structure (MemTable) and then writes out the changes sequentially to disk as sorted string tables (SSTables).
A bit confused by how this works...

Log Structured Storage
Mem table in memory
PUT(key101, a_1) -> add thing in mem table
PUT(key101, a_2) -> because its in memory, you can do an in place update of this key
So this mem table look slike.... (make some type of table)

Once this me table gets full, you get a scan of the leaf table of the mem table (mem table is a tree), the leaves are the values.
Then put these into a SSTable. THink of this just like a log, so it'll do like the changes. Like the sstable holds PUT(key101, a_2), PUT(key102, B_1)... etc.
So once it's in the sstable initialized with the data of the mem table, you store the data from the lowest key to the highest key. Then you write this out to disk (non volatile storage) do a flush, make sure its there.
Then populate a new mem table, do this over again and do a new sstable and it'll be sroted from newest to odlest. So like level #0 has SSTables written out newest to oldest.
At some point one level gets full, and you combine sstables into a new sstable. the idea is as you keep going down a level, they get bigger and bigger in memory? (put some visualization in here)
Q: Why multiple levels why not just merge level 0 within itself?
So that is how you do puts and deletes
What about reads?
Reads:
GET(key101)
- Check memtable, if it's in there, great -> return. The value is typically the tuple itself
- If the key is not in the memtable, then need to check each sstable at different levels.... that sucks
- So the way you deal with this is maintain a summary table in memory which provides some metadata about the files
    - min/max key per sstable
    - key filter per level
- check summary table and can check if the key exists in the bottom level, but also at the higher level, want to check the highest level because they're sorted by timestamp, so want the highest one
Q: What is the filter in the summary table?
A: Could be a range filter, could be a lookup table filter like does this key exist, etc...  
If you want to delete something, just put the delete key record into the mem table and everyone thats sees that delete key record comes along
When do merges get merged?
Log Structured Compaction
Periodically compact data files to reduce wasted space and speed up reads.
- keep latest values for each key using a sort merge algorithm

Say you have two SSTables and you want to merge them
Assume the order is from newest to oldest.

Do a sort merge
Wherever the cursor is pointing at, look at the key you're pointing at. Say first one has key100 and the other has key101. Put the first one in the new sstable because it is the lesser key. Then move the first one (its newest). Now say the first cursor now have key101, and the second also has key101. Then put the first one in, but ignore the second one because theyre the same key and the sstable is the newer one.
This is level compaction, but there are different strategies

Log Structured Compaction Strategies:
Approach 1: Leveled Compaction
- Data is organize into levels with sstable size limit per level
- SSTables in a level are no overlapping on key ranges
- Level 0 contains sstables recently flushed from memory and contain overlapping ranges
- Compactions merge a file from a level into the next lower level
So maybe level 0 is just purely newest to oldest, but when you do the merge compactoins, then it sorts and helps with reading a lot better
So you may actually have to recreate all the files again bvecause level 1 needs to be sorted. 
Say you have 10 GB in level 1, you need to read 10 GB in and then do the sorting and then write it back out
Q: What triggers compaction?
A: Maybe size of files, size of levels, it's different for each system.

Approach #1: Universal Compaction
- SSTables reside in a single universal level, no hierarchy
- DBMS Triggers compaction when too many sstables overlap in key ranges or exceed size thresholds
- Better for insert heavy workloads and time oriented queries
- Makes writes faster, but reads potentially slower

Discussion:
Log structured storage managers are more ocmmon today than in previous decades
- Partly due to the proliferaiton of RocksDB

What are some downsides of this approach?
- Write amplification
- Compaction is expensive










