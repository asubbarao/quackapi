# Dequeue claim race (not runnable in single-threaded SQLLogic)

**Where:** `src/quackapi_queue.cpp` — `DequeueInit` UPDATE…WHERE id=(SELECT…LIMIT 1) without re-checking claim predicates on the outer UPDATE.

**Repro (5 lines):**

```text
1. LOAD quackapi; CREATE QUEUE q; SELECT quackapi_enqueue('q','only-one');
2. CREATE ROUTE dq GET '/dq' AS SELECT id,payload FROM quackapi_dequeue('q',1);
3. SELECT * FROM quackapi_serve(PORT, worker_threads:=8);
4. Parallel: curl PORT/dq ×8
5. Observe: one 200 with the job; losers → 422 "Conflict on tuple deletion!" (should be empty 200 [])
```

**Observable wrong behaviour:** concurrent claimants raise TransactionContext conflicts (surfaced as 422) instead of a clean exclusive claim / empty result for losers.
