/*
 * Tier 1 Optimizations for the Two-Tier TCG JIT
 *
 * Dead flag elimination: backward dataflow analysis over CC state
 * globals (cc_op, cc_dst, cc_src, cc_src2) to remove writes whose
 * results are overwritten before being read.
 *
 * This pass runs only for TBs compiled with CF_TIER1 set, after
 * tcg_optimize() and before the liveness passes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "tcg/tcg.h"
#include "tcg/tcg-op-common.h"
#include "tcg-internal.h"
#include "exec/translation-block.h"

#ifdef XBOX

/*
 * We track liveness of four CC-related globals from the x86 frontend:
 *   cc_op, cc_dst, cc_src, cc_src2
 *
 * These are identified at pass start by scanning TCGContext->temps[]
 * for TEMP_GLOBAL entries whose name matches.
 */

#define CC_GLOBAL_OP    0
#define CC_GLOBAL_DST   1
#define CC_GLOBAL_SRC   2
#define CC_GLOBAL_SRC2  3
#define CC_GLOBAL_COUNT 4

typedef struct Tier1DFEContext {
    TCGContext *tcg;
    TCGTemp   *cc_temps[CC_GLOBAL_COUNT];
    int        num_eliminated;
} Tier1DFEContext;

/*
 * Find the CC global temps by matching names.
 * Returns true if all four were found.
 */
static bool find_cc_globals(Tier1DFEContext *ctx)
{
    TCGContext *s = ctx->tcg;
    int found = 0;

    memset(ctx->cc_temps, 0, sizeof(ctx->cc_temps));

    for (int i = 0; i < s->nb_globals; i++) {
        TCGTemp *ts = &s->temps[i];
        if (!ts->name) {
            continue;
        }
        if (strcmp(ts->name, "cc_op") == 0) {
            ctx->cc_temps[CC_GLOBAL_OP] = ts;
            found++;
        } else if (strcmp(ts->name, "cc_dst") == 0) {
            ctx->cc_temps[CC_GLOBAL_DST] = ts;
            found++;
        } else if (strcmp(ts->name, "cc_src") == 0) {
            ctx->cc_temps[CC_GLOBAL_SRC] = ts;
            found++;
        } else if (strcmp(ts->name, "cc_src2") == 0) {
            ctx->cc_temps[CC_GLOBAL_SRC2] = ts;
            found++;
        }
    }

    return found == CC_GLOBAL_COUNT;
}

/*
 * Check if a TCGTemp is one of the tracked CC globals.
 * Returns the index (0-3) or -1 if not.
 */
static inline int cc_global_index(const Tier1DFEContext *ctx, TCGTemp *ts)
{
    for (int i = 0; i < CC_GLOBAL_COUNT; i++) {
        if (ts == ctx->cc_temps[i]) {
            return i;
        }
    }
    return -1;
}

/*
 * Perform dead flag elimination on the TCG IR.
 *
 * Walk backward through the ops.  Track a liveness bitmask for the
 * four CC globals.  At TB exit points, conservatively mark all CC
 * globals live.  When we encounter a write to a dead CC global
 * (in a side-effect-free op), remove the op.  When we encounter a
 * read of a CC global, mark it live.  When we encounter a write
 * that satisfies a live read, mark the global dead.
 */
void tier1_dead_flag_elimination(TCGContext *s)
{
    Tier1DFEContext ctx = { .tcg = s, .num_eliminated = 0 };
    TCGOp *op, *op_prev;

    if (!find_cc_globals(&ctx)) {
        return;
    }

    /*
     * Liveness bitmask: bit i is set if cc_temps[i] is live.
     * Start with all live (conservative for TB exit).
     */
    uint32_t cc_live = (1u << CC_GLOBAL_COUNT) - 1;

    QTAILQ_FOREACH_REVERSE_SAFE(op, &s->ops, link, op_prev) {
        TCGOpcode opc = op->opc;
        const TCGOpDef *def;

        /*
         * At basic block boundaries (branches, exits, labels),
         * conservatively assume all CC globals are live.
         */
        if (opc == INDEX_op_set_label) {
            cc_live = (1u << CC_GLOBAL_COUNT) - 1;
            continue;
        }

        def = &tcg_op_defs[opc];

        if (def->flags & (TCG_OPF_BB_EXIT | TCG_OPF_BB_END |
                          TCG_OPF_COND_BRANCH)) {
            cc_live = (1u << CC_GLOBAL_COUNT) - 1;
        }

        /*
         * For call ops, conservatively assume all CC globals are
         * both read and written (the helper might access cpu state).
         */
        if (opc == INDEX_op_call) {
            cc_live = (1u << CC_GLOBAL_COUNT) - 1;
            continue;
        }

        /*
         * Check if this op writes to any CC globals.
         * If the written CC global is dead and the op has no side effects,
         * we can remove the op (if it ONLY writes CC globals or dead temps).
         */
        int nb_oargs = def->nb_oargs;
        int nb_iargs = def->nb_iargs;
        bool writes_cc = false;
        bool all_outputs_dead_cc = true;
        uint32_t written_cc_mask = 0;

        for (int i = 0; i < nb_oargs; i++) {
            TCGTemp *ts = arg_temp(op->args[i]);
            int idx = cc_global_index(&ctx, ts);
            if (idx >= 0) {
                writes_cc = true;
                written_cc_mask |= (1u << idx);
                if (cc_live & (1u << idx)) {
                    all_outputs_dead_cc = false;
                }
            } else {
                all_outputs_dead_cc = false;
            }
        }

        /*
         * If this op only writes to dead CC globals and has no
         * side effects, remove it.
         */
        if (writes_cc && all_outputs_dead_cc && nb_oargs > 0 &&
            !(def->flags & TCG_OPF_SIDE_EFFECTS)) {
            tcg_op_remove(s, op);
            ctx.num_eliminated++;
            continue;
        }

        /*
         * Process writes: a write to a live CC global satisfies the
         * pending read, so mark it dead going backward.
         */
        if (writes_cc) {
            for (int i = 0; i < nb_oargs; i++) {
                TCGTemp *ts = arg_temp(op->args[i]);
                int idx = cc_global_index(&ctx, ts);
                if (idx >= 0) {
                    cc_live &= ~(1u << idx);
                }
            }
        }

        /*
         * Process reads: a read of a CC global makes it live.
         */
        for (int i = nb_oargs; i < nb_oargs + nb_iargs; i++) {
            TCGTemp *ts = arg_temp(op->args[i]);
            int idx = cc_global_index(&ctx, ts);
            if (idx >= 0) {
                cc_live |= (1u << idx);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Phase 4: Extended Register Allocation (Global Pinning)             */
/* ------------------------------------------------------------------ */

/*
 * For Tier 1 blocks, set register preferences on ops that write to
 * CC globals to favor callee-saved registers (X20-X27 on ARM64).
 * This guides the allocator to keep frequently accessed CC state
 * in registers rather than spilling to memory.
 *
 * This runs after the liveness passes have set initial output_pref,
 * and we OR in our preferences to augment rather than replace.
 */
void tier1_global_register_pinning(TCGContext *s)
{
    Tier1DFEContext ctx = { .tcg = s };

    if (!find_cc_globals(&ctx)) {
        return;
    }

    /*
     * Build a register set of callee-saved registers that we'd like
     * CC globals to live in.  X20-X27 are callee-saved on ARM64 and
     * are at the top of the allocation order.
     */
    TCGRegSet cc_pref = 0;
#if defined(__aarch64__) || defined(_M_ARM64)
    tcg_regset_set_reg(cc_pref, TCG_REG_X20);
    tcg_regset_set_reg(cc_pref, TCG_REG_X21);
    tcg_regset_set_reg(cc_pref, TCG_REG_X22);
    tcg_regset_set_reg(cc_pref, TCG_REG_X23);
#else
    /* On non-ARM64, use a generic preference of all allocatable regs. */
    cc_pref = MAKE_64BIT_MASK(0, TCG_TARGET_NB_REGS);
#endif

    TCGOp *op;
    QTAILQ_FOREACH(op, &s->ops, link) {
        TCGOpcode opc = op->opc;

        if (opc == INDEX_op_call || opc == INDEX_op_set_label) {
            continue;
        }

        const TCGOpDef *def = &tcg_op_defs[opc];
        int nb_oargs = def->nb_oargs;

        for (int i = 0; i < nb_oargs && i < 2; i++) {
            TCGTemp *ts = arg_temp(op->args[i]);
            int idx = cc_global_index(&ctx, ts);
            if (idx >= 0) {
                /* Augment existing preference with our callee-saved set. */
                op->output_pref[i] |= cc_pref;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Phase 5: Basic-Block Instruction Scheduling                        */
/* ------------------------------------------------------------------ */

/*
 * Simple list scheduling within basic blocks to hide load-use latency.
 *
 * Strategy: identify ops that perform memory loads (qemu_ld, loads
 * from globals) and try to move independent computation between a
 * load and its first consumer.  This is done by reordering TCG IR
 * ops within basic blocks *before* the register allocator / emitter.
 *
 * We use a simple priority scheme:
 *   - Memory loads get high priority (scheduled early)
 *   - Ops that consume recently-loaded values get low priority
 *     (scheduled later to fill the latency gap)
 *   - Everything else stays in order (stable sort)
 */

#define MAX_BB_OPS 512  /* Max ops per basic block we'll attempt to schedule */

typedef struct SchedOp {
    TCGOp *op;
    int     priority;     /* Lower = schedule earlier */
    int     depth;        /* Dependency depth for tie-breaking */
    bool    is_load;      /* True if this is a memory load op */
    bool    scheduled;
} SchedOp;

/*
 * Check if a TCG opcode is a memory load operation.
 */
static bool is_load_op(TCGOpcode opc)
{
    switch (opc) {
    case INDEX_op_qemu_ld:
    case INDEX_op_qemu_ld2:
        return true;
    default:
        return false;
    }
}

/*
 * Check if op B depends on op A (B reads a temp that A writes).
 */
static bool op_depends_on(TCGOp *a, TCGOp *b)
{
    if (a->opc == INDEX_op_call || b->opc == INDEX_op_call) {
        return true;
    }

    const TCGOpDef *def_a = &tcg_op_defs[a->opc];
    const TCGOpDef *def_b = &tcg_op_defs[b->opc];
    int a_oargs = def_a->nb_oargs;
    int b_oargs = def_b->nb_oargs;
    int b_iargs = def_b->nb_iargs;

    for (int i = 0; i < a_oargs; i++) {
        TCGTemp *a_out = arg_temp(a->args[i]);
        /* Check if B reads what A writes. */
        for (int j = b_oargs; j < b_oargs + b_iargs; j++) {
            if (arg_temp(b->args[j]) == a_out) {
                return true;
            }
        }
        /* Check if B also writes the same temp (WAW dependency). */
        for (int j = 0; j < b_oargs; j++) {
            if (arg_temp(b->args[j]) == a_out) {
                return true;
            }
        }
    }

    /* WAR: B writes something that A reads. */
    for (int i = 0; i < b_oargs; i++) {
        TCGTemp *b_out = arg_temp(b->args[i]);
        int a_iargs = def_a->nb_iargs;
        for (int j = a_oargs; j < a_oargs + a_iargs; j++) {
            if (arg_temp(a->args[j]) == b_out) {
                return true;
            }
        }
    }

    /* Side effect ordering. */
    if ((def_a->flags & TCG_OPF_SIDE_EFFECTS) &&
        (def_b->flags & TCG_OPF_SIDE_EFFECTS)) {
        return true;
    }

    return false;
}

/*
 * Schedule one basic block's worth of ops using list scheduling.
 * Ops are collected from the tail queue, reordered, and re-inserted.
 */
static void schedule_basic_block(TCGContext *s, SchedOp *bb, int bb_len)
{
    if (bb_len <= 2) {
        return;
    }

    /*
     * Assign priorities: loads get priority 0 (earliest),
     * ops that directly consume loads get priority 2 (delayed),
     * everything else gets priority 1.
     */
    for (int i = 0; i < bb_len; i++) {
        bb[i].priority = 1;
        bb[i].depth = i;
        bb[i].scheduled = false;

        if (bb[i].is_load) {
            bb[i].priority = 0;
        }
    }

    /* Mark consumers of loads as delayed (priority 2). */
    for (int i = 0; i < bb_len; i++) {
        if (!bb[i].is_load) {
            continue;
        }
        for (int j = i + 1; j < bb_len; j++) {
            if (op_depends_on(bb[i].op, bb[j].op)) {
                if (bb[j].priority < 2) {
                    bb[j].priority = 2;
                }
                break;
            }
        }
    }

    /*
     * List schedule: repeatedly pick the ready op with lowest priority
     * (and lowest depth for tie-breaking) that has all dependencies met.
     */
    /*
     * Find the op just before the first BB op so we can re-insert
     * after it.  We use the QTAILQ internal prev pointer.
     */
    TCGOp *insert_after = NULL;
    TCGOp *first_op = bb[0].op;
    TCGOp *tmp;
    QTAILQ_FOREACH(tmp, &s->ops, link) {
        if (tmp == first_op) {
            break;
        }
        insert_after = tmp;
    }

    /* Remove all BB ops from the list. */
    for (int i = 0; i < bb_len; i++) {
        QTAILQ_REMOVE(&s->ops, bb[i].op, link);
    }

    /* Re-insert in scheduled order. */
    for (int scheduled = 0; scheduled < bb_len; scheduled++) {
        int best = -1;
        int best_prio = INT_MAX;
        int best_depth = INT_MAX;

        for (int i = 0; i < bb_len; i++) {
            if (bb[i].scheduled) {
                continue;
            }

            /* Check all dependencies are satisfied. */
            bool ready = true;
            for (int j = 0; j < bb_len; j++) {
                if (j == i || bb[j].scheduled) {
                    continue;
                }
                if (op_depends_on(bb[j].op, bb[i].op) && !bb[j].scheduled) {
                    /*
                     * j must be scheduled before i, but j isn't yet.
                     * However, we only care about deps where j is BEFORE i
                     * in the original order.
                     */
                    if (bb[j].depth < bb[i].depth) {
                        ready = false;
                        break;
                    }
                }
            }

            if (!ready) {
                continue;
            }

            if (bb[i].priority < best_prio ||
                (bb[i].priority == best_prio && bb[i].depth < best_depth)) {
                best = i;
                best_prio = bb[i].priority;
                best_depth = bb[i].depth;
            }
        }

        if (best < 0) {
            /*
             * Safety: if we can't find a ready op, just emit the first
             * unscheduled one in original order (dependency cycle or
             * analysis limitation).
             */
            for (int i = 0; i < bb_len; i++) {
                if (!bb[i].scheduled) {
                    best = i;
                    break;
                }
            }
        }

        bb[best].scheduled = true;

        if (insert_after) {
            QTAILQ_INSERT_AFTER(&s->ops, insert_after, bb[best].op, link);
        } else {
            QTAILQ_INSERT_HEAD(&s->ops, bb[best].op, link);
        }
        insert_after = bb[best].op;
    }
}

/*
 * Tier 1 instruction scheduling pass.
 *
 * Walk the IR, identify basic blocks (sequences between labels and
 * branches), and apply list scheduling to each.
 */
void tier1_instruction_scheduling(TCGContext *s)
{
    SchedOp bb[MAX_BB_OPS];
    int bb_len = 0;
    TCGOp *op;

    QTAILQ_FOREACH(op, &s->ops, link) {
        TCGOpcode opc = op->opc;
        const TCGOpDef *def = &tcg_op_defs[opc];

        /*
         * Basic block boundaries: labels, branches, exits, calls.
         * Schedule the accumulated BB and start fresh.
         */
        if (opc == INDEX_op_set_label ||
            opc == INDEX_op_call ||
            opc == INDEX_op_insn_start ||
            (def->flags & (TCG_OPF_BB_EXIT | TCG_OPF_BB_END |
                           TCG_OPF_COND_BRANCH))) {
            if (bb_len > 0) {
                schedule_basic_block(s, bb, bb_len);
                bb_len = 0;
            }
            continue;
        }

        /* Skip non-data ops. */
        if (opc == INDEX_op_discard) {
            continue;
        }

        if (bb_len < MAX_BB_OPS) {
            bb[bb_len].op = op;
            bb[bb_len].is_load = is_load_op(opc);
            bb_len++;
        } else {
            /* BB too large; schedule what we have and skip the rest. */
            schedule_basic_block(s, bb, bb_len);
            bb_len = 0;
        }
    }

    /* Schedule any trailing BB. */
    if (bb_len > 0) {
        schedule_basic_block(s, bb, bb_len);
    }
}

#endif /* XBOX */
