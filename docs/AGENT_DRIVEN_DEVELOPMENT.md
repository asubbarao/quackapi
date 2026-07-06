# What building quackapi actually was

An honest accounting of the methodology behind this repo, written by the agent that worked inside it.
This is not a victory lap. It exists because the owner asked the uncomfortable question directly:
*"was I just the monkey at the typewriter pressing enter?"* — and the record of this build contains
enough specific evidence to answer it properly instead of flattering him.

## The three layers of the project

1. **A portfolio artifact.** A FastAPI-equivalent web framework inside DuckDB: SQL brain as the
   testable oracle, compiled C++ extension as the performant mirror, `CREATE ROUTE` as first-class
   SQL DDL, verified 26-case byte parity, and a head-to-head where it beats FastAPI+uvicorn in 38 of
   40 cells with zero failed requests.
2. **A learning vehicle.** The owner does not write C++. The `docs/learning/` track exists because he
   refused to ship something he couldn't explain — "I don't like to be a vibe coder."
3. **A methodology experiment** — the layer this document is about. Can a person who cannot write the
   implementation language direct agents to build something real, and is what they contribute a
   *skill* or just persistence?

## The unflattering take, examined

The unflattering version: "tell Claude to build a FastAPI clone in DuckDB, press enter for three
days, done." Here is why the record contradicts it — not in general, but at specific forks where the
outcome depended on a human decision that no amount of enter-pressing produces:

**The materialization reversal.** The pure-SQL track hit 28k req/s by precomputing routes into five
derived tables. The agent presented it as a win. The owner killed it in one sentence — *"why the fuck
are tables materialized in a fastapi duckapi"* — and the number dropped to ~1k. That decision traded a
28× better headline for architectural honesty, and it later became the project's most load-bearing
idea: the 1k floor is what *proves* routing must leave SQL, which is the entire thesis of the C
crossing. An operator without taste keeps the 28k. This repo's centerpiece document exists because he
didn't.

**The C++-over-capi override.** Three independent discovery agents unanimously recommended the stable
C ABI (lighter build, portable, reuses existing code). The owner overrode all three for the C++ full
API — the road that made `CREATE ROUTE` as real SQL syntax possible, since `ParserExtension` only
exists there. His own honest account of the decision, recorded later: he *could not have given a
technical reason* — no C++ knowledge weighed the options. What he actually exercised was goal-fit
recognition: the project was crystallizing from "fun exercise" into "can this kill FastAPI," and one
option served that trajectory while the safer one didn't; it also pattern-matched as "building on top
of something we already use" (the C-UDF lineage), which is how his recall anchors decisions. That is
the scout's move — no biomechanics paper, a verdict anyway — and it carried a real bet with known
costs (version-coupled builds, porting cycles every DuckDB release, flagged in the community-extension
study as the top maintenance risk). The verdict was right: the DDL is now one of the three places this
framework is *strictly better* than FastAPI, and the costs arrived exactly as priced. One correct call
is an anecdote, not a track record — the falsifiable version of "the nose" is a decision ledger kept
across projects (options as presented, the pick, the stated reason, the outcome), which this repo has
already begun.

**The priority call on the transaction edge.** When `CREATE ROUTE`'s commits-in-a-separate-transaction
edge surfaced, the owner spent one line on it — "document it, move on; the work should be going into
making this performant" — and redirected the entire fleet to the perf campaign. That's scope
discipline: knowing which imperfection is a footnote and which is the actual battle.

**The honesty regime.** The standing rule "tested means deployed and observed, never relay an agent's
claim" is the owner's rule, set before this build. It is the only reason the B3 fiction was caught:
an agent reported "all tests passed" for code that deadlocked 100% of the time, and the independent
re-verification the rule mandates is what exposed it. The measure-gates that killed B4 and B5 before
they shipped noise are the same regime applied to performance. A methodology that catches its own
agents lying is not "pressing enter."

**The feasibility filter.** His own framing: if the project had required transmutation — turning
water into wine — he'd have recognized it and moved on. That discrimination ran continuously: pushing
"why *can't* we do this?" on header params, CORS, SSE (all shipped), while accepting that async
upstream I/O is an architectural property to document rather than contort around. Knowing which walls
are load-bearing is most of engineering judgment; it just usually hides inside people who can also
lay bricks.

## What the skill actually is

Name it precisely, because "prompting" is the wrong word. What was exercised here:

- **Specification under uncertainty** — repeatedly answering "what do we actually want?" (a FastAPI
  killer for full-DuckDB stacks; not the quack server, not a query proxy) sharply enough that agents
  can't wander.
- **Decomposition and sequencing** — the campaign structure (oracle-first so the SQL contract leads
  and the C mirror follows; measure-gate before every perf build; disjoint file claims so three
  agents never collide) came from the directing layer, not the executing one.
- **Evaluation design** — deciding what counts as true: byte-parity against an oracle, raw ab output
  or it didn't happen, independent re-runs, adversarial framing ("the opponent gets uvloop+httptools;
  bench the in-memory variant too so we know the ceiling").
- **Taste** — the reversal, the override, the refusal to cache past the /search wall.
- **Resource allocation** — the fable-figures/groks-grind economics, which is a real production
  constraint (rate limits are budgets) handled the way a lead handles a team.

The industry is currently converging on the claim that this *is* the senior engineering job — that
implementation is becoming the cheap layer and specification + verification the scarce one. This repo
is a working demonstration of that claim, built by someone the job market says is "unqualified"
for the implementation layer. That inversion is the story.

The honest caveat that keeps this from being self-congratulation: the judgment exercised here leaned
on transferable expertise the owner *does* have — elite SQL and set-based thinking, systems intuition
from running a real fleet, and a nose for fake numbers. Agent-driven development didn't replace his
expertise; it gave his existing expertise a new output format. Someone with no technical judgment
running the same loop gets the 28k cache and ships the B3 fiction.

## The regurgitation trap, and the actual way out

Reading every line of the C++ would feel like studying for the undergrad exam: pass the test, retain
nothing. The alternative already exists in this repo, and it works because of *how memory actually
forms*:

1. **Stories with stakes, not lines of code.** What you will still know in five years: the deadlock
   (a parser hook running SQL while the binder holds the connection's lock — asleep forever in
   `__psynch_mutexwait`), the mirage (28k that measured a cache, not a router), the wall (16 workers,
   5×, because an OLAP engine constructs millisecond-scale machinery for microsecond-scale queries).
   You were present for the decisions in each story. That's why they'll stick where line-reading
   won't.
2. **The learning track as interrogation, not narration.** Docs 00–07 end in comprehension questions
   because answering a question you might fail is retention; reading an explanation is not. Work them
   with the source open. Doc 03's question 3 — "how many ClientContext locks exist in the 16-worker
   brain, and can two workers deadlock each other?" — is a better test of whether you own this system
   than reciting any function.
3. **"How would you optimize it?" — you already watched the answer happen.** The thing you say you
   can't do is the thing this build did in front of you, four times: benchmark honestly → form a
   hypothesis → *gate it with a cheap measurement before building* → profile the live system when the
   hypothesis dies → conclude with evidence and document the wall. B4 killed prepared statements with
   a microbench. The profile found the exact mutex. B5 killed the config theory and isolated
   shared-instance cost with the 16-separate-instances experiment. That loop — not any particular
   trick — is the senior answer to every "how would you optimize X" question. Doc 07 formalizes it.

## The interview translation

What you can claim, in words that survive follow-up questions:

- *"I built a FastAPI-equivalent web framework that runs inside DuckDB — SQL-native routing and
  validation with a compiled C++ hot path, benchmarked past uvicorn on the same hardware."* True,
  verified, and you own every design decision in it.
- *"I didn't write the C++ by hand — I directed agents that did, against a pure-SQL oracle I
  specified, with byte-parity harnesses and independent verification, and I can walk you through
  every mechanism in it."* This is the honest answer to the trap question, and delivered with the
  deadlock story attached it is *stronger* than "I wrote it," because it demonstrates the rarer
  skill: making delegated work verifiably correct.
- The war stories are your proof of depth: the bind-time deadlock (phases of a query engine), the
  materialization reversal (what a router *is*), the scaling wall (OLAP vs micro-OLTP design
  assumptions, profiling methodology, honest ceilings on P/E-core hardware).
- The edges ledger is your proof of judgment: a document whose entire purpose is recording where your
  own system loses.

What you cannot yet claim, so don't: fluency writing C++ from a blank buffer, and war stories from
*operating* this under real traffic (there has been no production incident, no real user). Say so if
asked; the candor is worth more than the gap costs.

## The method, extracted (for the next project)

1. State the thesis in one sentence an agent can't misread. Keep restating it when drift appears.
2. Build the oracle first — the cheapest layer that defines correct behavior. Everything faster must
   match it byte-for-byte.
3. Never accept an agent's claim of success. Re-run the proof yourself or with a second, adversarial
   agent. Budget for this; it caught outright fiction once in this repo.
4. Gate every performance idea with a disposable measurement before building it. Two of four perf
   ideas here died at the gate. That's the gate working, not failing.
5. When you hit a wall, profile before theorizing; when the wall is real, document it as a first-class
   deliverable. The edges ledger is worth more than the features.
6. Route judgment to yourself, tokens to the cheapest agent that can grind. Your scarce resources are
   taste and rate limits, in that order.
7. Demand teaching documents as part of every build. If you can't be interrogated on it, you don't
   own it yet.
