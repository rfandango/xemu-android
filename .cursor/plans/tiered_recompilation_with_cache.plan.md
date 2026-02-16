---
name: ""
overview: ""
todos: []
isProject: false
---

# Tiered TCG Recompilation with Persistent Code Cache

## Problem

TCG currently treats every translation block identically: the same quick-and-dirty
translation for a one-off initialization routine and for the inner loop of a game's
renderer.  On Android/ARM64, this means:

- **Hot loops** (physics, rendering, audio mixing) run suboptimal code forever
- **Flag computation overhead**: lazy EFLAGS evaluation costs 10-50+ host instructions
per flag read via `helper_cc_compute_all()` switch dispatch
- **Simple register allocator**: linear scan wastes ARM64's 31 GP registers
- **No instruction scheduling**: ops emitted in IR order, ignoring ARM64 pipeline
- **First-launch stutter**: thousands of blocks compiled on demand (partially solved
by the translation hints cache)

## Design: Two-Tier JIT with Persistent Hotness Data

```
                    ┌──────────────────────────────────┐
                    │         Persistent Cache          │
                    │  (tb_cache.bin per game)          │
                    │                                   │
                    │  [hint + exec_count + tier_level] │
                    └──────────┬───────────┬────────────┘
                               │ load      │ save
                    ┌──────────▼──────┐    │
     First exec ──►│   Tier 0 (fast) │────┤
                    │  Current TCG    │    │
                    │  ~1µs/block     │    │
                    └────────┬────────┘    │
                             │ exec_count  │
                             │ > threshold │
                    ┌────────▼────────┐    │
                    │  Tier 1 (opt)   │────┘
                    │  Enhanced JIT   │
                    │  ~10-50µs/block │
                    └─────────────────┘
```

### Tier 0 — Quick Translation (Current TCG)

What TCG does today:

1. x86 frontend → TCG IR
2. Single-pass `tcg_optimize()` (constant folding, copy propagation, algebraic)
3. `liveness_pass_0/1/2` (dead code, indirect lowering)
4. Linear-scan register allocation
5. Sequential ARM64 code emission

**Cost:** ~1µs per block. Good enough for cold code.

### Tier 1 — Optimized Re-translation

When a block is identified as hot (executed > N times), re-translate it with:


| Optimization                     | What it does                                                                                                                   | Estimated speedup         |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ | ------------------------- |
| **Dead flag elimination**        | Track which CC flags are actually *read* before the next CC-setting op; skip computing unused flags entirely                   | 15-30% on flag-heavy code |
| **Eager flag materialization**   | For hot CC patterns (e.g., `SUB → JCC`), compute only the needed flag bit instead of the full `helper_cc_compute_all` dispatch | 10-20%                    |
| **Extended register allocation** | Second-chance allocation: spill analysis + register preference propagation based on actual usage patterns in the block         | 5-15%                     |
| **ARM64 instruction scheduling** | Reorder independent ops to avoid pipeline stalls (dual-issue, load-use latency hiding)                                         | 5-10%                     |
| **Constant specialization**      | When a TB always executes with the same `cc_op` value, specialize the flag computation path                                    | 5-10%                     |
| **TB superblock formation**      | When TB A always chains to TB B (single successor), merge them for cross-block optimization                                    | 10-25% for hot traces     |


### Integration with Persistent Cache

The existing `tb_cache.bin` hint file is extended to store **execution counts**
and **tier levels** so that on second launch:

- Previously-hot blocks skip Tier 0 entirely and go straight to Tier 1
- The warm-up stutter is eliminated AND hot code runs optimized from the start

```
First Launch:
  Block X executes 1000 times → promoted to Tier 1 at runtime
  On exit: save {pc, flags, exec_count=1000, tier=1} to cache

Second Launch:
  Load cache → Block X has tier=1
  Pre-warm: translate Block X directly at Tier 1
  Block X runs optimized from first execution
```

## Key Data Structures

### Extended Translation Block

```c
/* In include/exec/translation-block.h, add to struct TranslationBlock */
#ifdef XBOX
    uint32_t exec_count;    /* Approximate execution count (saturating) */
    uint8_t  tier;          /* 0 = quick, 1 = optimized */
    uint8_t  tier_pad[3];   /* Alignment padding */
#endif
```

### Extended Cache Hint (v2)

```c
/* Extended hint with hotness data */
typedef struct TBCacheHintV2 {
    uint64_t pc;
    uint64_t cs_base;
    uint32_t flags;
    uint32_t cflags;
    uint64_t phys_pc;
    uint32_t exec_count;  /* How many times this block executed */
    uint8_t  tier;        /* Which tier it reached */
    uint8_t  pad[3];
} TBCacheHintV2;
```

The file format version bumps from 1 to 2.  The loader auto-detects v1 files
and treats all blocks as tier=0, exec_count=0 (graceful upgrade).

### Tier 1 Optimization Context

```c
/* Per-TB context for Tier 1 recompilation */
typedef struct Tier1OptContext {
    /* Dead flag analysis results */
    uint32_t live_flags_at_exit;   /* Which CC flags are live at TB exit */
    uint32_t dead_flag_ops;        /* Bitmask of CC-setting ops whose results are dead */

    /* Register pressure analysis */
    uint16_t temp_live_ranges[TCG_MAX_TEMPS]; /* Live range length per temp */
    uint8_t  spill_weights[TCG_MAX_TEMPS];    /* Spill cost estimate */

    /* Superblock info (if applicable) */
    TranslationBlock *successor_tb;  /* Single-successor TB, if any */
    bool is_superblock;
} Tier1OptContext;
```

## Implementation Phases

### Phase 1: Execution Counting Infrastructure

**Effort:** Low | **Files:** `translation-block.h`, `cpu-exec.c`, `translate-all.c`

Add a lightweight execution counter to each TB.  The counter is **approximate**
(not atomic) to avoid overhead — racing on a saturating counter is harmless.

**In `cpu_exec_loop` (`accel/tcg/cpu-exec.c:1024`):**

```c
/* After cpu_loop_exec_tb returns successfully */
#ifdef XBOX
{
    uint32_t c = tb->exec_count;
    if (c < TB_TIER1_THRESHOLD * 2) {
        tb->exec_count = c + 1;  /* Saturating approximate count */
    }
}
#endif
```

**Threshold:** `TB_TIER1_THRESHOLD = 64` (configurable).  Xbox game hot loops
execute thousands of times; 64 is enough to distinguish hot from cold while
keeping promotion fast.

**Why not in-band counting (counter inside generated code)?**
In-band counting (decrement-and-branch in prologue) adds overhead to *every*
TB execution.  Out-of-band counting in the `cpu_exec_loop` only fires on
TB transitions (chained blocks skip it), which is acceptable because:

- Chained blocks that never exit are already fast
- Unchained blocks (loop entries, indirect jumps) are the promotion targets

### Phase 2: Tier 1 Promotion Mechanism

**Effort:** Medium | **Files:** `cpu-exec.c`, `translate-all.c`, `tb-cache-hints.c`

When `exec_count` crosses the threshold, queue the block for re-translation:

**In `cpu_exec_loop`, after the counting code:**

```c
#ifdef XBOX
if (tb->tier == 0 && tb->exec_count >= TB_TIER1_THRESHOLD) {
    tb_request_tier1_promotion(cpu, tb);
}
#endif
```

`**tb_request_tier1_promotion`:**

1. Invalidate the existing Tier 0 TB (marks `CF_INVALID`)
2. Re-translate with `cflags |= CF_TIER1` (new compile flag)
3. The new TB replaces the old one in the hash table
4. Chained jumps to the old TB are reset, and will re-link to the new one

```c
void tb_request_tier1_promotion(CPUState *cpu, TranslationBlock *tb)
{
    TCGTBCPUState s = {
        .pc      = tb->pc,
        .cs_base = tb->cs_base,
        .flags   = tb->flags,
        .cflags  = (tb->cflags & ~CF_COUNT_MASK) | CF_TIER1,
    };

    /* Invalidate the old Tier 0 TB */
    tb_phys_invalidate(tb, tb_page_addr0(tb));

    /* Re-translate at Tier 1 */
    mmap_lock();
    TranslationBlock *new_tb = tb_gen_code(cpu, s);
    mmap_unlock();

    if (new_tb) {
        new_tb->tier = 1;
        new_tb->exec_count = tb->exec_count;
    }
}
```

**New compile flag:**

```c
/* In translation-block.h */
#define CF_TIER1  0x00080000  /* Use Tier 1 optimized compilation */
```

### Phase 3: Dead Flag Elimination (Tier 1 Optimization #1)

**Effort:** High | **Files:** `tcg/optimize.c` (or new `tcg/tier1-opt.c`)

This is the single highest-impact Tier 1 optimization.  x86 sets EFLAGS on
almost every ALU instruction, but most of the time the flags are overwritten
before being read.

**Approach: backward dataflow analysis on CC state**

After the x86 frontend emits TCG IR, scan backward through the ops:

1. At each `brcond`/`setcond` that reads CC state: mark CC as **live**
2. At each op that writes CC state (`set_cc_op`, stores to `cc_dst`/`cc_src`):
  - If CC is **dead** at that point → eliminate the CC write
  - If CC is **live** → keep it, mark CC as **dead** (the write satisfies the read)
3. For `helper_cc_compute_all` calls: if only one specific flag bit is needed
  (e.g., ZF for `JE`), replace with a specialized helper

**Implementation:**

```c
/* New function called from tcg_gen_code when CF_TIER1 is set */
static void tier1_dead_flag_elimination(TCGContext *s)
{
    /* Backward pass: track liveness of cc_op, cc_dst, cc_src, cc_src2 globals */
    bool cc_op_live = true;   /* Conservative: assume live at exit */
    bool cc_dst_live = true;
    bool cc_src_live = true;

    TCGOp *op;
    QTAILQ_FOREACH_REVERSE(op, &s->ops, link) {
        /* If this op reads CC state, mark as live */
        if (op_reads_cc_state(op)) {
            cc_op_live = cc_dst_live = cc_src_live = true;
        }
        /* If this op writes CC state and it's dead, eliminate */
        if (op_writes_cc_state(op, CC_DST) && !cc_dst_live) {
            tcg_op_remove(s, op);
            continue;
        }
        if (op_writes_cc_state(op, CC_DST)) {
            cc_dst_live = false;  /* Write satisfies future reads */
        }
        /* ... similar for cc_op, cc_src ... */
    }
}
```

**Identifying CC state ops:**
The x86 frontend stores CC state via TCG `st` ops to known offsets in
`CPUX86State`:

- `offsetof(CPUX86State, cc_op)`  → `cc_op`
- `offsetof(CPUX86State, cc_dst)` → `cc_dst`
- `offsetof(CPUX86State, cc_src)` → `cc_src`
- `offsetof(CPUX86State, cc_src2)` → `cc_src2`

The Tier 1 pass identifies these by matching store targets against known offsets.

### Phase 4: Extended Register Allocation (Tier 1 Optimization #2)

**Effort:** High | **Files:** `tcg/tcg.c` (or new `tcg/tier1-regalloc.c`)

The current allocator is a single-pass linear scan that greedily assigns registers.
Tier 1 adds a **second-chance pass**:

1. **First pass (existing):** Quick linear scan, same as Tier 0
2. **Analysis pass:** After allocation, scan for:
  - Temps that were spilled but have short live ranges (should have been kept)
  - Temps that were kept in registers but aren't used for a long time (should be spilled)
  - Register-to-register moves that could be eliminated by better initial assignment
3. **Rewrite pass:** Re-allocate the worst offenders

This is **not** a full graph-coloring allocator (which would be too expensive).
Instead, it's a targeted improvement on the ~20% of temps where the linear scan
made poor choices.

**ARM64-specific opportunity:** With 31 GP registers, the current allocator often
leaves 5-10 registers unused.  The extended allocator can:

- Keep more TCG globals (`cpu_regs[]`, `cc_dst`, `cc_src`) pinned in registers
- Reduce memory traffic for the most frequently accessed state

### Phase 5: Instruction Scheduling (Tier 1 Optimization #3)

**Effort:** Medium | **Files:** `tcg/aarch64/tcg-target.c.inc` (or new file)

After register allocation, reorder independent instructions to:

- Hide load-use latency (ARM64 Cortex-A76+: 4 cycles for L1 hit)
- Maximize dual-issue (ARM64 can issue 2+ ops per cycle)
- Avoid flag-dependency stalls

**Approach: list scheduling within basic blocks**

```
Before scheduling:
  LDR X0, [X19, #cc_dst]    ; load cc_dst (4 cycle latency)
  CMP X0, #0                 ; STALL: waiting for X0
  B.EQ label

After scheduling:
  LDR X0, [X19, #cc_dst]    ; load cc_dst
  <other independent ops>    ; fill the latency gap
  CMP X0, #0                 ; X0 is ready
  B.EQ label
```

**Scope:** Only within basic blocks (between labels/branches).  Cross-block
scheduling is too complex for the benefit.

### Phase 6: TB Superblock Formation (Tier 1 Optimization #4)

**Effort:** Very High | **Files:** `translate-all.c`, `cpu-exec.c`

When TB A always chains to TB B (determined from execution profiling — A's
`jmp_dest[0]` is always B and `jmp_dest[1]` is never taken), merge them into
a single TB "superblock" for Tier 1 compilation:

- Cross-block register allocation (no spill/reload at A→B boundary)
- Dead flag elimination spans both blocks
- Larger scheduling window

**Tracking chain statistics:**

```c
#ifdef XBOX
    /* In TranslationBlock */
    uint32_t chain_count[2];  /* How many times each exit was taken */
#endif
```

Increment in `tb_add_jump()` or `cpu_exec_loop`.  When one exit dominates
(>95% of executions), the two TBs are candidates for superblock formation.

**Complexity warning:** Superblocks require:

- Merging two TBs' guest code ranges
- Handling page-crossing TBs
- Invalidating the superblock when either component is invalidated
- This is the highest-effort optimization and should be implemented last

### Phase 7: Persistent Cache v2 Integration

**Effort:** Medium | **Files:** `tb-cache-hints.h`, `tb-cache-hints.c`, `xemu_android.cpp`

**Extended file format (v2):**

```
[Header: magic, version=2, game_hash, hint_count]
[HintV2 0: pc, cs_base, flags, cflags, phys_pc, exec_count, tier]
[HintV2 1: ...]
...
```

**Recording changes:**

- `tb_cache_record_hint()` now also saves `exec_count` and `tier`
- On dedup collision, update `exec_count` to max of old and new

**Pre-warming changes:**

- Hints with `tier == 1` are pre-translated with `CF_TIER1`
- Hints with `tier == 0` are pre-translated as before
- Sorting: translate Tier 1 hints first (they're the hot blocks)

**Backward compatibility:**

- v2 loader reads v1 files (treats all as tier=0, exec_count=0)
- v1 loader rejects v2 files (version mismatch → cold start, harmless)

## Compilation Pipeline Summary

```
                Tier 0 (CF_TIER1 not set)          Tier 1 (CF_TIER1 set)
                ─────────────────────────          ──────────────────────
Frontend:       x86 → TCG IR                       x86 → TCG IR
                      │                                  │
Optimize:       tcg_optimize() [single pass]       tcg_optimize() [single pass]
                      │                                  │
                      │                            tier1_dead_flag_elimination()
                      │                                  │
Liveness:       liveness_pass_0/1/2                liveness_pass_0/1/2
                      │                                  │
RegAlloc:       linear scan                        linear scan + second-chance
                      │                                  │
CodeGen:        sequential ARM64 emission          ARM64 emission + scheduling
                      │                                  │
                ◄── generated code ──►              ◄── generated code ──►
```

## Implementation Order & Dependencies

```
Phase 1: Execution Counting ──────────────────► (no dependencies)
    │
Phase 2: Promotion Mechanism ─────────────────► (depends on Phase 1)
    │
    ├── Phase 3: Dead Flag Elimination ───────► (depends on Phase 2)
    │
    ├── Phase 4: Extended Register Alloc ─────► (depends on Phase 2)
    │
    ├── Phase 5: Instruction Scheduling ──────► (depends on Phase 2)
    │
    └── Phase 6: Superblock Formation ────────► (depends on Phases 3+4+5)
    │
Phase 7: Cache v2 Integration ───────────────► (depends on Phases 1+2)
```

**Recommended implementation order:**

1. Phase 1 (counting) + Phase 7 (cache v2) — foundation
2. Phase 2 (promotion) — the tier switching works
3. Phase 3 (dead flags) — highest single-optimization impact
4. Phase 5 (scheduling) — medium effort, decent payoff
5. Phase 4 (regalloc) — higher effort, moderate payoff
6. Phase 6 (superblocks) — very high effort, do last

## Estimated Impact


| Phase                   | CPU-bound speedup               | Effort    |
| ----------------------- | ------------------------------- | --------- |
| Execution counting      | 0% (infrastructure)             | 1-2 days  |
| Promotion mechanism     | 0% (infrastructure)             | 2-3 days  |
| Dead flag elimination   | 15-30% on hot code              | 5-7 days  |
| Extended register alloc | 5-15% on hot code               | 5-7 days  |
| Instruction scheduling  | 5-10% on hot code               | 3-5 days  |
| Superblock formation    | 10-25% on hot traces            | 7-10 days |
| Cache v2 integration    | Eliminates re-promotion stutter | 1-2 days  |


**Combined estimate:** 30-60% improvement on CPU-bound game code (hot loops),
with the benefit persisting across launches via the cache.

**Note:** The Xbox's Pentium III runs at 733 MHz.  On a modern Snapdragon 8 Gen 2
at ~3 GHz, TCG needs roughly 5-15x overhead to keep up.  Current Tier 0 overhead
is estimated at 10-20x.  With Tier 1 optimizations bringing hot code down to
5-10x, most games should run at full speed.

## Risk Mitigation

- **Correctness:** Each Tier 1 optimization can be individually disabled via
compile flags.  If a game breaks, bisect by disabling optimizations.
- **Code size:** Tier 1 code may be larger (unrolled, specialized).  The 64K
hint cap prevents unbounded growth.
- **Promotion storm:** If many blocks cross the threshold simultaneously,
promotion could cause a stutter.  Mitigate by rate-limiting promotions
(max N per frame) or deferring to idle time.
- **Invalidation:** When a Tier 1 TB is invalidated (e.g., SMC), it falls back
to Tier 0 on next translation.  The persistent cache remembers it was hot,
so next launch it goes straight to Tier 1 again.

