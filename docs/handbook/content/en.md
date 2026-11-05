## 0 How to use this handbook

This handbook covers every level the ROADMAP completed: **L1 core matching**, **L2 performance engineering**, **L3′ market data and pipeline**, and **L4 order entry**. L5 and L6 are future plans (chapter 11).
Each chapter follows the same order: **what I built → why it is designed this way → how I know it is right**. The boxed paragraphs are written to be said out loud.

<div class="box say" markdown="1">
<span class="box-title">The 30-second version</span>
I built a deterministic limit order book and matching engine from scratch in C++20. On top of it I added a Nasdaq ITCH market-data feed, a multi-threaded pipeline, a strategy with pre-trade risk checks, and finally order entry over OUCH and SoupBinTCP to a single-threaded epoll exchange simulator.
The guiding rule was correctness first, then verifiability, then speed. Every layer is checked against a deliberately simple reference model, the multi-threaded and networked modes must match the single-threaded, in-process results bit for bit, and every performance number says where it was measured.
</div>

<div class="box say" markdown="1">
<span class="box-title">The 2-minute version (tell it in this order)</span>

1. **L1**: a price-time priority matching engine. Every check happens before anything is modified, and the event stream alone can rebuild the state. I ran {{n:soak_commands}} commands of differential testing against a reference model, and the journal and snapshots survive the process being killed.
2. **L2**: I wrote the measurement plan before measuring. Profiling showed the session layer copied its whole history on every commit. After switching to geometric growth, same-session alternating runs went from {{n:e1_before}} to {{n:e1_after}}, and I'm careful to call that a complexity bug fix, not "the engine got hundreds of times faster".
3. **L3′**: ITCH 5.0 decoding, order-by-order book reconstruction, and a lock-free SPSC queue between the decoder and book threads. The multi-threaded output is byte-identical to single-threaded replay. I validated it on full days of real Nasdaq data, and **the real data broke one of my rules** (chapter 5).
4. **L4**: order entry over OUCH 4.2 with SoupBinTCP sessions, reconnect handling and duplicate-order protection. In lockstep mode the network path matches the in-process simulation bit for bit. The software path to the first order takes {{n:t2t_inproc_first}}; what loopback adds depends on the machine. On the cloud VM it was mostly waking up the other thread, but when I re-measured on my own machine that didn't hold, so I kept both results.
</div>

Suggested reading: chapters 0 and 1 first; chapters 5, 9 and 10 the day before an interview. Numbers marked † are wall-clock numbers (throughput, latency, tick-to-trade); the page footer says what † currently means. One command in Appendix B re-measures them on your machine and regenerates this handbook.

## 1 The project on one page

### 1.1 What I built

| Level | Content | Evidence in one line |
|---|---|---|
| L1 core | types and validation, order book, matching engine, test infrastructure, replay / snapshot / recovery | {{n:l1_tests_per_preset}} tests per preset; {{n:soak_commands}} commands of differential soak; {{n:mutants_killed}} |
| L2 performance | measurement method, profiling, one hypothesis-driven experiment (E1), baseline switch, equivalence, memory | same-session alternating A/B; {{n:l2_equiv_inputs}} inputs byte-identical across both implementations |
| L3′ market data | ITCH 5.0 decoding, book reconstruction, feed state machine, SPSC pipeline, strategy and risk, market-wide mode | single-threaded == multi-threaded; real data: {{n:stock_days}} stock-days over {{n:days}} trading days, all Live |
| L4 order entry | OUCH 4.2, SoupBinTCP, order gateway, epoll exchange simulator, tick-to-trade | lockstep network mode bit-identical to in-process; free-run recordings replay bit for bit |

Across the repository: {{n:presets}} build presets (GCC / Clang × Debug / Release / ASan+UBSan) with {{n:tests_per_preset}} tests each and zero warnings, plus two TSan presets with {{n:tsan_tests}} concurrency tests each; decisions D-001 to D-0{{n:decisions_total}} are written down.

### 1.2 Overall architecture

{{fig:architecture|Figure 1. Overall architecture. The three bands are L3′, L4 and L1/L2; double borders are separate threads, dashed lines are the L4 network mode.}}

Data comes in at the top left. The decoder thread decodes ITCH frames from a file or a pipe and hands them over an SPSC queue to the book thread. The book thread rebuilds the order book, produces market events and passes them to the trading stage: strategy, then risk, then the exchange.
There are two exchanges: the in-process `SimExchange`, which wraps the frozen L1 matching engine, or the path through the order gateway, OUCH and SoupBinTCP over loopback TCP to a single-threaded epoll exchange simulator — which runs the very same `SimExchange` inside.

### 1.3 Three principles that run through the project

- **Correctness, then verifiability, then performance.** No optimization is worth it if I can no longer prove the output didn't change, and a performance number only counts if it states its environment and method.
- **Determinism.** Prices are integer ticks; the risk token bucket refills on market time, not system time; the multi-threaded and networked modes must produce exactly the same bytes as single-threaded, in-process runs. Determinism is what makes cross-checking possible, and cross-checking is what makes correctness provable.
- **Measure before concluding, and write down what isn't verified.** Every wall-clock number says which machine it came from; the cloud finding "loopback is mostly wake-up cost" didn't reproduce on my own machine, so both results are kept; power-loss durability is "not verified"; the L1 engine still allocates when an order rests, so "zero allocation" is only claimed for the market-data hot path.

## 2 L1 core matching engine

### 2.1 Types and error handling

- **Prices are `int64` integer ticks** (D-001): floating point can't compare exactly and its sums aren't deterministic. **Quantities are signed `int64`** (D-003): with unsigned types a bug in `remaining − fill` silently wraps to a huge positive number; with signed types validation and UBSan catch it.
- **Bad input returns a reject code; bad configuration throws** (D-004). An invalid order is normal traffic, not an exception, so the hot path never throws or allocates; a bad configuration is a deployment error and fails at construction.
- **Configuration limits rule out overflow** (D-006): `max_live_orders × max_order_quantity ≤ INT64_MAX`, so no running sum needs a per-step overflow check — overflow is excluded when the engine is configured.

### 2.2 The order book (M2)

The baseline uses standard containers: a `std::map` of price levels per side, a `std::list` per level for FIFO order, and an `unordered_map` from order id to position (D-007). I built a baseline that is easy to prove correct first, and kept intrusive lists or object pools as L2 experiments rather than adding them up front.
The book **contains no matching logic**; it only stores resting orders in price-time priority. In tests, `find_invariant_violation()` re-derives every structural fact after every step: level totals, FIFO order, index consistency, never crossed.

### 2.3 The matching engine (M3)

- **Price-time priority**, trading at the maker's price; one order can sweep several levels.
- **One sequence clock** (D-020): every event, rejections included, takes the next sequence number, and a resting order's time priority is the sequence number of its acceptance. One clock gives both the total order of events and the queue order, so an auditor can read the queue straight off the event stream.
- **Two phases** (D-019): the prepare phase only reads — all validation, the sequence overflow check, reserving room for the events. The commit phase neither allocates nor throws; the only possible failure is allocating the resting remainder, and then the engine emits a terminating event before rethrowing (D-024), so the event stream is always complete.
- **When to reject with `BOOK_FULL`** (D-021): simulate the match first and reject only if a remainder would rest in a full book. One can prove that a rejected order would not have traded at all, so there is never a "half filled, then rejected" side effect.

### 2.4 Test infrastructure (M4)

The main evidence for correctness isn't hand-written cases but **differential testing**: a reference engine written to be obviously correct runs the same random command stream as the real engine, and every event and snapshot is compared.

- Soak matrix: {{n:soak_profiles}} workload profiles, {{n:soak_commands}} commands in total, with a full comparison every {{n:soak_check_every}} commands. The L1 gate asked for {{n:diff_gate}}.
- An event auditor independently checks conservation from the event stream (accepted = 2 × traded + resting + cancelled), summing in 128-bit integers so the auditor itself can't overflow.
- Fault injection: the global `operator new` is replaced so that the k-th allocation fails, covering every allocation point; in long runs {{n:fault_rate}} of the commands are armed and the run keeps going.
- Mutation testing: I deliberately broke the engine — "trade at the taker's price", "BOOK_FULL ignores freed slots" — to confirm the tests catch it: {{n:mutants_killed}}.
- Cross-compiler: GCC and Clang produce identical coverage, state and fault lines.

### 2.5 Replay, snapshots and recovery (M5)

- **Text format v1** (D-034): strict, canonical and versioned. It rejects extra spaces and leading zeros, so "byte-identical" means "semantically identical", which makes golden files possible ({{n:golden_scenarios}}).
- **The journal is one checksummed batch per command, and the events are the source of truth** (D-035): recovery folds events rather than re-executing commands, so it is exact even when the history contains allocation failures.
- **Torn tail versus corruption** (D-036): a half-written tail is dropped and truncated; a failed checksum with more data after it is corruption and blocks automatic recovery. Making {{n:journal_mods}} kinds of modification to every byte of the last batch gave {{n:journal_tail_corrupt}} corruption reports, {{n:journal_tail_dropped}} dropped batches and **zero** accepted modifications.
- **Commit protocol** (D-038): persist first, then acknowledge; a persistence failure "poisons" the session and only recovery from the journal can continue. A real process killed with `SIGKILL` and restarted ends in the same state as an uninterrupted run.

### 2.6 How I know L1 is right

| Property | Evidence |
|---|---|
| matching semantics | at least one named test per rule: price priority, FIFO at a price, partial fills, multi-level sweeps, every cancel case, every reject reason, boundary values |
| agrees with the reference model | {{n:soak_commands}} commands of differential soak; every CI build also runs {{n:ci_diff}} |
| determinism | replaying twice, and GCC vs Clang builds, give identical event-stream and snapshot hashes; golden files match byte for byte |
| exception safety | fault injection at every allocation point; soak runs under ASan / UBSan / LSan |
| crash consistency | truncation at every byte, injected write failures, a real `kill -9` and restart |

## 3 L2 performance engineering

### 3.1 Method: write the plan, then measure

- The plan was fixed before any measurement (BENCHMARK §1–§6): data generation and validation stay outside the timed region; throughput and per-operation latency are measured separately; every repetition starts from the same state and checks the state hash.
- **Conclusions only from alternating A/B runs in the same session** (D-043). If the ranges of the processes overlap, the difference is noise; in this environment anything within {{n:noise_band}} can't be told apart. Numbers from two sessions once differed by {{n:session_drift}} with identical code — that is exactly why the rule exists.
- The author's WSL2 VM has no hardware performance counters, so cache and branch behaviour only comes from cachegrind simulation, always labelled as simulated.

On the {{n:engine_profiles}} workload profiles the engine handles millions to tens of millions of commands per second with p99 around {{n:engine_p99_range}}; on the mixed workload it does {{n:engine_mixed_tput}} with p50 / p99 of {{n:engine_mixed_p50}} / {{n:engine_mixed_p99}}.

### 3.2 What profiling showed

The allocator is the biggest single item, {{n:allocator_share}} of the engine's instructions. Pooling would mean changing M2's containers and move semantics, and the risk wasn't worth it inside L2, so it's a follow-up task (D-044). A glibc tuning probe that changes no code (P1) cut allocator instructions by {{n:p1_ir}}, but wall-clock time stayed within noise — **an honestly reported null result**.

### 3.3 E1: the story of a complexity bug

- **Symptom**: session throughput fell roughly as 1/n with stream length; {{n:perf_samples}} of `perf` samples sat in the copy loop of `submit`.
- **Hypothesis**: the commit log grew with an exact `reserve(size + 1)`, so every commit copied the whole history — O(n²) per session. The L1 event buffer had already been fixed for the same reason (D-022), but M5 didn't follow that rule.
- **Change**: when capacity runs out, `reserve(max(size + 1, 2 × capacity))`, a change of {{n:e1_loc}}. The reserve still happens before any effect, and nothing allocates after the commit point.
- **Result** (same session, {{n:e1_pairs}} alternating): non-durable session {{n:e1_before}} → {{n:e1_after}}; replay latency p50 {{n:e1_lat_before}} → {{n:e1_lat_after}}; the durable mode and the engine control stayed within noise.

<div class="box say" markdown="1">
<span class="box-title">How to say it in an interview</span>
The speed-up depends on the stream length, because the bug's cost grows with the square of it, so I don't say "{{n:e1_ratio}} faster". What I say is: the per-command cost of the session went from growing linearly with the number of committed commands to constant, and not a single line of the matching engine or the order book changed.
</div>

### 3.4 Baseline switch, equivalence and memory

- **The baseline stays available as a build option** (D-045): `LOB_BASELINE=ON` compiles a frozen copy of the baseline source file, `OFF` the optimized one, so the optimized code and its generated code are untouched.
- **Equivalence**: on {{n:l2_equiv_inputs}} inputs the two implementations produce byte-identical event streams, journals and snapshots.
- **Memory went the opposite way from the estimate**: by capacity the optimized version should use more; measured peak RSS was lower (mixed {{n:rss_mixed}}, deep_book {{n:rss_deep}}). Two mechanisms are verified — the baseline holds old and new blocks at once while growing, and reserved but untouched pages don't count towards RSS — but they don't explain the whole difference, and **I wrote it down as "still unexplained"**.
- One command reproduces the whole comparison: `scripts/l2_reproduce.sh` ({{n:l2_reproduce_minutes}}).

## 4 L3′ market data and pipeline

### 4.1 ITCH 5.0 decoding and book reconstruction

- The decoder handles {{n:itch_types}} message types (system events, directory, adds, executions, cancels, deletes, replaces, trading actions, cross trades and so on) and skips the other spec types at their exact length. It's zero-copy, and bad input returns an error code instead of crashing.
- **Why not reuse the L1 book** (D-046)? The L1 book can only fill the front of the queue, has no partial cancel and requires increasing sequence numbers. ITCH is the exchange telling you which order traded, and that can be any order. The reconstruction just applies what the exchange says; it never matches.
- `md::Book` **validates before it mutates**: a rejected message leaves the book exactly as it was, so I can assert precisely that a rejection has no side effects.
- Input is read through a fixed buffer instead of `mmap`. Peak memory depends only on configuration — {{n:replay_rss}} for inputs from {{n:replay_inputs}} — and it can read from a pipe, so a file can be downloaded, decompressed and replayed in one stream.

### 4.2 The FeedHandler state machine

{{fig:feed|Figure 2. One FeedHandler per instrument. When not Live it publishes no book updates; the resume grace (dashed box) is a sub-state of Live, see chapter 5.}}

Gaps are detected by **sequence numbers**, not timestamps: quiet periods are normal, so timestamps only have to be non-decreasing. Any input that can't be explained puts that instrument into Stale, and while Stale the downstream only sees state events — it never trades on a book that might be wrong.

### 4.3 SPSC and the pipeline: multi-threaded but still deterministic

- A single-producer single-consumer ring: indices are 64-bit counters that never wrap, so full and empty need no wasted slot; the producer's and consumer's indices live on separate cache lines; the producer publishes `tail` with release and the consumer reads it with acquire, so seeing a new tail guarantees seeing the whole element.
- **Lossless backpressure** (D-048): when the queue is full the producer spins; nothing is ever dropped.
- **Determinism**: the decode function and the book-thread object are the same code in single-threaded and pipelined mode, and both modes produce byte-identical event streams, books and trading digests (tested under GCC, Clang and TSan).

### 4.4 Latency waterfall and zero allocation

- Every hop is timestamped with the TSC, calibrated at start-up; the cost of reading the clock twice is measured and subtracted from every sample (D-049). Decode hop p50 / p99 {{n:decode_p50}} / {{n:decode_p99}}, book hop {{n:book_p50}} / {{n:book_p99}}, single-threaded replay {{n:itch_single_tput}}.
- **In saturated mode the "queue hop" is waiting time, not handoff time** ({{n:queue_saturated}} = queue capacity × per-message processing time). To see the cross-core handoff you have to look at the paced mode: p50 {{n:handoff_p50}}, with the p99 varying {{n:handoff_spread}} between repetitions — virtualization noise, so it's only an upper bound.
- Zero allocation on the hot path: a dedicated test replaces the global `operator new`, each thread arms it at the start of its hot loop, and any allocation fails the test; a deliberately allocating negative control proves it catches them.

### 4.5 Strategy, risk and simulated fills

- The strategy interface uses CRTP for static dispatch. Against a virtual-function version I measured {{n:crtp_ns}} vs {{n:virtual_ns}} ns — **no measurable difference**, and I don't claim a gain I couldn't measure.
- Pre-trade risk checks run in a fixed order: kill switch, trading phase, reference price, fat-finger quantity, fat-finger notional, price band, max position, token bucket; one check takes {{n:risk_check_ns}}. Cancels are never blocked, and the kill switch is sticky. The token bucket refills on **market time**, so risk is a pure function of the input.
- The simulated exchange sends approved orders into the **unchanged L1 matching engine**; market executions reported by ITCH become synthetic aggressive orders that trade against our resting orders. Two fill models run side by side — optimistic (we are at the front of the queue) and conservative (we join the back of the visible queue) — and for the same order flow they bound reality from above and below (D-053).

### 4.6 The CI performance gate counts instructions, not time

On shared CI machines wall-clock time swings by tens of percent; cachegrind's instruction count for a fixed input is exactly reproducible. The gate covers three workloads — decode / book / trading at {{n:icount_decode}} / {{n:icount_book}} / {{n:icount_trading}} instructions — and fails beyond {{n:icount_tolerance}}; it refuses to compare if the compiler or valgrind version differs (D-051). It catches "doing more work", not cache misses, which is why wall-clock benchmarks only run locally.

## 5 Key story: the resume grace (D-054 / D-055)

### 5.1 Trading-phase awareness (D-054)

A real exchange's book can **legitimately be locked or crossed** during auctions, halts, LULD pauses and pre- and post-market. The rule: enforce "never crossed" only in regular hours (System Event 'Q' to 'M') while the stock is trading ('T'). Elsewhere a crossed book is allowed, counted and flagged; the strategy stops quoting, and risk rejects new orders without tripping the kill switch.

### 5.2 Real data broke the rule

I replayed SXTC, which paused twice under LULD on 2019-01-30, from a full day of Nasdaq data. The D-054 rule marked it Stale at the exact moment trading resumed — **it called a correct book wrong**.

{{fig:grace|Figure 3. SXTC's first resume (schematic). The reopening cross (Q 'H') and the resume (H 'T') arrive at the same instant; the executions of the orders in the cross arrive afterwards.}}

The reason is arrival order. During the pause, {{n:sxtc_pause}}, the book crossed. On resume the exchange runs the cross and announces trading at the same moment, but the executions of the individual orders in that cross arrive {{n:sxtc_resolve_1}} later ({{n:sxtc_resolve_2}} on the second resume). In between, the book looks crossed.

### 5.3 The grace rule and its limits

If the book is still crossed when strict hours begin, the feed doesn't go Stale immediately; it enters a **resume grace**:

- during the grace the book stays non-strict, events are flagged and **nothing is tradable**;
- the book is clean **and** a Cross Trade for that stock has been seen (evidence of a reopening cross) → strict mode begins;
- not resolved within {{n:grace_time}} or {{n:grace_msgs}} of that stock → Stale;
- a new pause during the grace cancels it, and the next resume needs fresh evidence.

<div class="box" markdown="1">
<span class="box-title">Follow-up: couldn't the grace hide a real error?</span>
Three constraints: it needs a Cross Trade as evidence, so a cross without one still goes Stale after a second; it has two limits, time and message count; and nothing trades during the grace. The limits leave {{n:grace_margin}} of margin over the measured {{n:grace_observed}}, and all {{n:graces_total}} graces over {{n:days}} trading days resolved within them. I haven't recorded how long each one took, though, so it's still a parameter to revisit with more data.
</div>

### 5.4 Evidence

- {{n:grace_tests}} named tests, including a full hand-written event stream in SXTC's real order, "exactly at the limit doesn't time out, one nanosecond more does", a new pause during the grace, and a Cross Trade for another stock not counting.
- The second replay: SXTC stays Live all day, both graces resolved by the executions, and single-threaded and pipelined modes give the same hashes.
- The instruction-count gate stopped the first implementation: checking the timeout inline on the hot path cost the book workload {{n:grace_icount_inline}}; moving it into a non-inlined cold-path function brought it to {{n:grace_icount_cold}}.

<div class="box say" markdown="1">
<span class="box-title">How to tell it (about a minute)</span>
From the spec I wrote a reasonable-looking rule: during regular hours the book may never be crossed. Then I replayed real data, and a stock coming back from a pause was flagged as broken at the exact moment it resumed. Instead of just loosening the rule, I looked at the arrival order: the reopening cross and the resume arrive together, and the executions arrive {{n:grace_observed}} later. So I designed a bounded grace — it requires evidence of the cross, it has a time and a message limit, and nothing trades during it — and validated it on {{n:days}} full trading days: {{n:stock_days}} stock-days, not one false alarm. It's my favourite example of measuring first, then making the rule, and writing down why.
</div>

## 6 Market-wide mode and real data

### 6.1 Market-wide order books (D-056)

- One FeedHandler per stock, all stocks sharing one order pool (order references are unique across the market); stocks are sharded across N worker threads by stock locate modulo N.
- **Why modulo instead of balancing by load?** With modulo, which thread owns which stock depends only on the input, so results are reproducible and comparable. Moving stocks around at run time would depend on scheduling and make determinism and testing much harder.
- The summary hash is the sum of per-stock digests, independent of order, so single-threaded and N-shard runs compare directly.

### 6.2 Results on real data (Nasdaq 2019-01-30, full day, streamed, never stored)

| Item | Result |
|---|---|
| instruments / final state | {{n:instruments}}, all Live, none Stale |
| peak live orders | {{n:peak_orders}} |
| single-threaded vs 4 shards | per-stock hash files byte-identical, summary hash `{{n:market_hash}}` |
| throughput | single-threaded {{n:mkt_single}}, 4 shards {{n:mkt_n4}} (more threads than vCPUs, so not linear) |
| multi-day | {{n:stock_days}} stock-days over {{n:days}} trading days, all Live; all {{n:graces_total}} resume graces resolved |

A single-stock replay (AAPL): {{n:aapl_size}}, {{n:aapl_frames}} frames, peak memory {{n:aapl_rss}}, no Stale.

### 6.3 Zero allocation in market-wide mode (D-058)

In market-wide mode stocks are created when their directory message arrives, and level arrays grow on demand — both allocate. The fix: one pass writes a profile of each stock's peak number of price levels (the largest is {{n:level_peak_max}}); before a replay every stock is pre-created with peak × {{n:profile_margin}} levels and **capped** there. A stock that exceeds it goes Stale; nothing allocates. That takes {{n:profile_memory}}, against {{n:uniform_memory}} if every stock reserved the maximum. The cost is written down: the profile comes from an earlier day, so a stock might exceed it, and the result is a visible Stale rather than a hidden allocation.

## 7 L4 order entry and tick-to-trade

### 7.1 Layering: OUCH and SoupBinTCP

- **SoupBinTCP** only makes sure every message the exchange sends reaches the client, in order and without loss: login, implicit sequence numbers, replay from "next expected sequence" after a reconnect, a {{n:soup_heartbeat}} heartbeat, a {{n:soup_timeout}} timeout, end of session. It does **not** guarantee the client's messages.
- **OUCH 4.2** carries the order semantics, and every inbound message can be **safely resent**: the exchange ignores a duplicate token, and a cancel states the size you want left, not a decrement. Because inbound messages are idempotent, the session only needs to guarantee outbound delivery — that is the pivot of the whole recovery design.
- Both layers are **sans-IO** state machines: bytes and the current time go in; bytes to send, events and the next deadline come out. Every transition is tested with hand-written bytes and a fake clock, with no network and no sleeping.

### 7.2 The order gateway

- Three ids: the strategy's `client_id`; the OUCH token, unique on the wire (a replace gets a new one); and the exchange's order reference number. A token is the date, a per-start instance number and a counter; the instance number is persisted, so a restarted process never reuses a token that day.
- A per-order state machine: PendingNew → Live → Filled / Canceled / Rejected, and fills can still arrive after a cancel was sent, because the two race on the wire. Any report the gateway can't explain — unknown token, overfill, a message after a final state — marks it inconsistent, which trips the kill switch and cancels everything. **I'd rather stop trading than guess the position.**
- Reconnect: pending cancels and replaces are resent unchanged, since they're idempotent; new orders are not resent by default, since they were decided on stale market data. The spec gives no high-water mark for "the replay is over", so I use the spec's own signal: the server only sends a heartbeat after {{n:soup_heartbeat}} of silence, so the first heartbeat after login means the replay is complete.

### 7.3 The exchange simulator

One thread on epoll. The exchange **keeps its own copy of the market book**, because the conservative fill model needs the queue the exchange sees; matching is still the unchanged L1 engine. It implements the spec's four outcomes of a replace, partial cancels, exchange-side throttling (excess new orders and replaces get the spec's reject reason 'x'; **cancels are never throttled**) and cancel-on-disconnect (the cancels are sequenced, so they show up in the replay after reconnecting).

### 7.4 How I know the network mode is right

When replies arrive depends on thread scheduling, so the same input can interleave differently. I proved two separate things:

{{fig:lockstep|Figure 4. Lockstep mode. The barrier counts inbound messages sent on the current connection; the reply carries the outbound sequence watermark.}}

- **Lockstep** proves the network stack is transparent: after every step we wait for quiescence, so replies are delivered in exactly the in-process order. The bar is that orders, fills and position hashes are **bit-identical** to in-process — for both fill models, single-threaded and with the multi-threaded pipeline.
- **Free-run** doesn't compare hashes; it checks invariants — exposure within the position limit on both sides, no duplicate token, legal state transitions, and after draining, position, cash, open orders and sequence numbers reconcile. Both sides also **record the interleaving they actually saw, and replaying the recording offline must reproduce it bit for bit**. All of this holds with {{n:drops}} injected disconnects and exchange throttling.
- The tests were tested too: removing the lockstep wait makes both lockstep tests fail, and dropping one recorded delivery makes the replay test fail.

### 7.5 Tick-to-trade

Definition: from the start of decoding the ITCH frame to the OUCH bytes being handed to the kernel (`send()` returns), counting only frames that produced an order, with the clock-read overhead subtracted. **Send first**: market data is forwarded and replies are read only after the frame's orders are out, so no other system call sits inside the window.

{{fig:t2t|Figure 5. The same loopback measurement on two machines, p50. White: the exchange thread sleeps in epoll_wait; hatched: it busy-polls. Busy-polling helps a lot on the cloud VM and not at all locally.}}

- In-process, tick → first message of the frame encoded: {{n:t2t_inproc_first}} — the software path itself.
- On my machine, over loopback: tick → first message `send()` returned in {{n:t2t_loop_first}}, or {{n:t2t_loop_first_busy}} with a busy-polling exchange thread; one `send()` costs {{n:send_cost}}; client `send()` → exchange decoded takes {{n:t2t_wire}}; round trip {{n:t2t_rtt}} / {{n:t2t_rtt_busy}}.
- **Same experiment, two machines, two conclusions.** On the cloud VM busy-polling cut the first message from {{n:t2t_loop_first_cloud}} to {{n:t2t_loop_first_busy_cloud}} and the round trip from {{n:t2t_rtt_cloud}} to {{n:t2t_rtt_busy_cloud}}, so I concluded that loopback cost is mostly waking the other thread. On my own machine busy-polling changes nothing, so that conclusion **only holds for the cloud VM**. The method behind it still stands: controlled variables — move everything that isn't on the order path out of the window (reading replies, the eventfd write that forwards market data), use "new order vs first message of the frame" to estimate one `send()`, then test the remaining gap with busy-polling.
- **Why the local round trip is so slow isn't explained yet.** I know it isn't cross-core clock skew (the round trip is timed on the client's own core), it isn't the exchange thread sleeping (busy-polling doesn't change it), and it isn't the software path. The next step is a minimal loopback TCP ping-pong on the same machine, to separate the platform's own loopback latency from my code.
- These aren't wire-to-wire numbers: no NIC, no switch, no real gateway, no kernel bypass. I only compare them with my own previous version.

## 8 Engineering method

- **CI**: {{n:presets}} presets with zero warnings, two TSan presets and the cachegrind instruction-count gate; `scripts/check.sh` is the single entry point.
- **Negative controls**: in every sub-stage I deliberately broke the code and confirmed the tests failed, then restored it. Once a negative control reported "not caught"; it turned out the mutant hadn't compiled because of `-Werror`, so the old binary was being tested. Since then I always confirm the build succeeded first.
- **Decision records**: all {{n:decisions_total}} decisions state the options, the choice, the cost and when to revisit; deviations from a design (for example, a second login for the same account now takes over instead of being rejected) are recorded too.
- **Number discipline**: every number states its environment and source; in this handbook they all come from `numbers.yaml`, and wall-clock numbers carry †.
- **AI collaboration**: AI took part in design and implementation, and `AI_USAGE.md` records the division of work and the author's decisions stage by stage. If asked, I say so plainly and point out that verification rests on tests and reproducible evidence.

## 9 Limitations and unverified items (with how to answer)

| Limitation / not verified | How to answer |
|---|---|
| wall-clock numbers come from two machines (my WSL2 and a cloud VM); some were only measured in the cloud | "Every number says which machine and date it's from, Appendix A has the sources, and one command re-measures. Re-measuring overturned one of my cloud conclusions, so I don't carry a finding from one machine to another, and I never attribute a change using numbers from different sessions." |
| the slow local loopback round trip is unexplained | "I know it isn't clock skew and it isn't my software path. Next I'd measure the platform's own loopback with a minimal ping-pong; until then I don't make up an explanation." |
| loopback is not wire-to-wire: no NIC, switch or kernel bypass | "Loopback shows the cost of connecting a user-space program to a kernel socket. Real wire-to-wire needs hardware timestamps on a switch mirror port and PTP on both ends. I only compare against my own previous version." |
| durability under power loss or an OS crash is not verified | "I tested process kills, truncation at every byte and failed writes. Power loss depends on the file system implementing fsync and rename correctly; I couldn't test that, so I don't claim it." |
| the L1 engine still allocates when an order rests | "The zero-allocation guarantee covers the market-data hot path. Pooling the engine means changing the frozen L1 code; the risk wasn't worth it, so it's a recorded follow-up." |
| the fill models don't simulate our own market impact | "Optimistic and conservative give upper and lower bounds for the same order flow. With a closed-loop strategy the totals are no longer ordered — tests found that, and it's in the decision record." |
| the resume-grace limits need more data | "So far {{n:graces_total}} graces over {{n:days}} days all resolved. I haven't recorded the distribution of how long each took, so the limit is a value with margin, not a conclusion." |
| a restarted process doesn't take over the previous process's live orders | "Tokens never repeat across restarts, that's guaranteed. Taking over old orders needs recognising the old instance by token prefix — it's the clear next step." |
| the memory difference after E1 isn't fully explained | "Two mechanisms are verified, but they don't account for all of it. I wrote it down as 'still unexplained' instead of inventing a story." |

## 10 Selected interview questions

**Q1 Why determinism, and what does it cost?** It makes results for the same input comparable: differential tests, single vs multi-threaded, in-process vs network all depend on it. The cost is that I can't use system time or rely on scheduling order — risk refills on market time, and threads only exchange ordered data through SPSC queues.

**Q2 Why those memory orders in the SPSC queue?** The producer writes the element, then publishes `tail` with release; the consumer reads `tail` with acquire, so seeing the new tail means seeing the whole element. Each side reads its own index relaxed. No third party observes the relative order of the two indices, so seq_cst isn't needed.

**Q3 How can multi-threaded output be byte-identical to single-threaded?** Both modes run the same decode function and the same book-thread object; the queue is lossless and ordered; state is only ever modified on one thread. Tests compare event-stream and snapshot hashes under GCC, Clang and TSan.

**Q4 How do you know the matching engine is correct?** Differential testing against a naive reference model on random command streams ({{n:soak_commands}} commands), an event auditor that checks conservation independently, mutation testing to show the tests catch real bugs, and fault injection at every allocation point.

**Q5 What's the best performance fix you made?** E1 — not "the engine got faster" but an O(n²) complexity bug: located by profiling, a written hypothesis, verified by same-session alternating A/B, with an honest note that the ratio depends on stream length. The other one worth telling is a null result: allocator tuning showed no measurable wall-clock gain, so I didn't adopt it.

**Q6 Why does CI gate performance on instruction counts?** Wall-clock time on shared machines swings by tens of percent, while instruction counts for a fixed input are exactly reproducible. It catches "more work" regressions, not cache effects, so wall-clock benchmarks run locally. The gate did stop one real regression: the grace timeout check inlined on the hot path.

**Q7 Tell me about real data contradicting an assumption.** Chapter 5: at a resume the cross and the resume arrive together and the executions {{n:grace_observed}} later. The rule called a correct book wrong, so I designed a resume grace with evidence, limits and no trading, and validated it on {{n:stock_days}} stock-days without a single false alarm.

**Q8 Why shard by stock locate modulo N?** Ownership depends only on the input, so runs are reproducible and comparable; moving stocks at run time depends on scheduling. The cost is possible imbalance — if hot stocks cluster, I'd build a fixed mapping offline from historical volume, which stays deterministic.

**Q9 After a reconnect, how do you know which orders reached the exchange?** Log in with the old session and the next expected sequence number, and the exchange replays everything sent while you were gone. Cancels and replaces are resent unchanged because they're idempotent; new orders aren't by default. The first heartbeat after login means the replay is over, so a new order still unanswered at that point never arrived.

**Q10 With a network in the middle, timing is nondeterministic — how do you prove correctness?** Lockstep proves the network stack is transparent: bit-identical to in-process, using the outbound sequence watermark as a barrier, because a superfluous cancel gets no reply at all. Free-run checks invariants and records both sides' interleaving, and the recording replays bit for bit — so a failure can be brought back and stepped through.

**Q11 What's your tick-to-trade, and what does it tell you?** Definition first, then numbers: on my machine {{n:t2t_inproc_first}} in-process, {{n:t2t_loop_first}} on loopback, {{n:t2t_rtt}} round trip. The interesting part is that it overturned my own conclusion: on the cloud VM busy-polling cut loopback from {{n:t2t_loop_first_cloud}} to {{n:t2t_loop_first_busy_cloud}}, while locally it made no difference — so "it's the wake-up" was a fact about that machine, not a general one. It isn't wire-to-wire, so I only compare it with myself.

**Q12 What would you do differently?** Bring real data in earlier — rules should be tested against data first; set up controlled-variable measurements like "new order vs first message" from the start; and the next steps are pooling the book's nodes and taking over a previous instance's live orders.

## 11 Future plans: L5 and L6 (not implemented)

The project scope ends at L4 (D-065). These two are recorded only as future directions:

- **L5 UDP multicast with A/B line arbitration**: the exchange sends the same MoldUDP64 sequenced stream on two independent paths; the receiver deduplicates by message sequence number, takes whichever copy arrives first and delivers strictly in order. When both lines miss a message it waits a bounded window, then asks a retransmission server for the smallest range; if that fails too, the existing recovery state machine (Recovering → Stale) takes over instead of a second recovery mechanism. The bar: for any recoverable combination of faults, the message sequence handed to the pipeline is byte-identical to reading the file directly. There's a design draft (`docs/design/L5_DESIGN.md`, not implemented).
- **L6 microstructure signal research**: export features from the reconstructed book (columnar files tied to the book hash), test them in and out of sample, and report without claiming trading profits. No design yet.

## A Wall-clock numbers at a glance

These are all numbers marked † in this handbook, where they were measured and where they come from; the sources of all other numbers are in `docs/handbook/numbers.yaml`.

{{table:wallclock}}

## B Re-measure locally and regenerate

`remeasure.sh` builds Release, runs `lob_bench`, `lob_itch_bench` and `lob_t2t` on pinned cores, writes the medians, date and machine into `numbers.yaml` and rebuilds; the † footer follows (Appendix A). Run from the repository root; the first time, `sudo apt install python3-venv fonts-noto-cjk fonts-dejavu-core poppler-utils valgrind`:

```
docs/handbook/remeasure.sh                 # measure, rebuild
docs/handbook/remeasure.sh --itch FILE     # + market-wide
docs/handbook/remeasure.sh --l2            # + L2 (~73 min)
docs/handbook/remeasure.sh --from-log LOG  # hand-run log
docs/handbook/build.sh                     # rebuild only
```

