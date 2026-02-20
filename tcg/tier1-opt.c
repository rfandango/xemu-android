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

/*
 * Compute cc_defines_first: forward scan of IR to find which CC globals
 * are DEFINED before any USE.  Stored on the TB for cross-TB DFE.
 *
 * Returns a bitmask where bit i is set if cc_temps[i] is written
 * before being read in the TB's IR.
 */
uint8_t tier1_compute_cc_defines_first(TCGContext *s)
{
    Tier1DFEContext ctx = { .tcg = s };
    TCGOp *op;
    uint32_t defined = 0;
    uint32_t used = 0;

    if (!find_cc_globals(&ctx)) {
        return 0;
    }

    QTAILQ_FOREACH(op, &s->ops, link) {
        TCGOpcode opc = op->opc;
        const TCGOpDef *def;

        if (opc == INDEX_op_insn_start) {
            continue;
        }

        /* At calls or branches, stop -- subsequent code may not execute. */
        if (opc == INDEX_op_call || opc == INDEX_op_set_label) {
            break;
        }

        def = &tcg_op_defs[opc];
        if (def->flags & (TCG_OPF_BB_EXIT | TCG_OPF_BB_END |
                          TCG_OPF_COND_BRANCH)) {
            break;
        }

        int nb_oargs = def->nb_oargs;
        int nb_iargs = def->nb_iargs;

        /* Check reads first (order matters: read before write in same op). */
        for (int i = nb_oargs; i < nb_oargs + nb_iargs; i++) {
            TCGTemp *ts = arg_temp(op->args[i]);
            int idx = cc_global_index(&ctx, ts);
            if (idx >= 0) {
                used |= (1u << idx);
            }
        }

        /* Then check writes. */
        for (int i = 0; i < nb_oargs; i++) {
            TCGTemp *ts = arg_temp(op->args[i]);
            int idx = cc_global_index(&ctx, ts);
            if (idx >= 0 && !(used & (1u << idx))) {
                defined |= (1u << idx);
            }
        }
    }

    return (uint8_t)(defined & 0xf);
}

/*
 * Cross-TB dead flag elimination for tier1 TBs.
 *
 * For goto_tb exits with a known successor TB, check the successor's
 * cc_defines_first mask (pre-populated in s->succ_cc_defines[] by the
 * caller in translate-all.c).  If the successor defines a CC global
 * before using it, the predecessor's store of that global at exit is dead.
 *
 * This extends the existing intra-TB DFE by reducing the conservative
 * "all live" assumption at exit points.
 */
void tier1_cross_tb_dead_flag_elimination(TCGContext *s)
{
    Tier1DFEContext ctx = { .tcg = s, .num_eliminated = 0 };
    TCGOp *op, *op_prev;

    if (!find_cc_globals(&ctx)) {
        return;
    }

    if (s->succ_cc_defines[0] == 0 && s->succ_cc_defines[1] == 0) {
        return;
    }

    uint32_t cc_live = (1u << CC_GLOBAL_COUNT) - 1;

    QTAILQ_FOREACH_REVERSE_SAFE(op, &s->ops, link, op_prev) {
        TCGOpcode opc = op->opc;
        const TCGOpDef *def;

        if (opc == INDEX_op_set_label) {
            cc_live = (1u << CC_GLOBAL_COUNT) - 1;
            continue;
        }

        def = &tcg_op_defs[opc];

        if (def->flags & (TCG_OPF_BB_EXIT | TCG_OPF_BB_END |
                          TCG_OPF_COND_BRANCH)) {
            /*
             * For goto_tb, check if we know the successor.
             * The goto_tb index (0 or 1) is in args[0].
             */
            if (opc == INDEX_op_goto_tb) {
                int idx = op->args[0];
                if (idx < 2 && s->succ_cc_defines[idx]) {
                    cc_live = ((1u << CC_GLOBAL_COUNT) - 1)
                              & ~s->succ_cc_defines[idx];
                } else {
                    cc_live = (1u << CC_GLOBAL_COUNT) - 1;
                }
            } else {
                cc_live = (1u << CC_GLOBAL_COUNT) - 1;
            }
            continue;
        }

        if (opc == INDEX_op_call) {
            cc_live = (1u << CC_GLOBAL_COUNT) - 1;
            continue;
        }

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

        if (writes_cc && all_outputs_dead_cc && nb_oargs > 0 &&
            !(def->flags & TCG_OPF_SIDE_EFFECTS)) {
            tcg_op_remove(s, op);
            ctx.num_eliminated++;
            continue;
        }

        if (writes_cc) {
            for (int i = 0; i < nb_oargs; i++) {
                TCGTemp *ts = arg_temp(op->args[i]);
                int idx = cc_global_index(&ctx, ts);
                if (idx >= 0) {
                    cc_live &= ~(1u << idx);
                }
            }
        }

        for (int i = nb_oargs; i < nb_oargs + nb_iargs; i++) {
            TCGTemp *ts = arg_temp(op->args[i]);
            int idx = cc_global_index(&ctx, ts);
            if (idx >= 0) {
                cc_live |= (1u << idx);
            }
        }
    }
}

#endif /* XBOX */
