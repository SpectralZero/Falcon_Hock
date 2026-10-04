/*
 * umf_trampoline_builder.c — §BUILDER: Zydis-backed instruction relocator
 *
 * Steals the first instructions of a target function and copies them into
 * a trampoline so the original behaviour can still be invoked after the
 * target's entry has been overwritten with a jump to the hook.
 *
 * Position-dependent instructions must be rewritten because the trampoline
 * lives at a different address than the original code:
 *   - RIP-relative memory operands: the 32-bit displacement is rebased.
 *   - Relative branches/calls (JMP/CALL/Jcc rel8|rel32): re-encoded as
 *     64-bit-reachable absolute forms so the target is reached regardless
 *     of how far the trampoline landed from the original.
 *
 * Instructions with no near-reachable encoding (LOOP/LOOPcc, J*CXZ) are
 * refused — the hook simply fails to install (fail-loud, never corrupt).
 *
 * A src→dst instruction-boundary map is produced so that a thread caught
 * mid-prologue while the hook is being written can have its RIP relocated
 * into the equivalent point in the trampoline (see umf_hook_batch.c).
 */

#include "umf/umf.h"
#include <Zydis/Zydis.h>
#include <string.h>

/* Tail bytes of the slot reserved for UNWIND_INFO + RUNTIME_FUNCTION
 * (written later by umf_register_unwind_info). Worst case:
 *   align(3) + UNWIND_INFO(4) + align(3) + RUNTIME_FUNCTION(12) = 22 → 24. */
#define UMF_UNWIND_RESERVE 24

/* ── Absolute code emitters (always 64-bit reachable) ── */

static size_t emit_abs_jmp(uint8_t* dst, uint64_t target) {
    /* FF 25 00 00 00 00 | <qword target>  (jmp qword [rip+0]) */
    dst[0] = 0xFF; dst[1] = 0x25;
    uint32_t disp = 0;
    memcpy(dst + 2, &disp, 4);
    memcpy(dst + 6, &target, 8);
    return 14;
}

static size_t emit_abs_call(uint8_t* dst, uint64_t target) {
    /* call qword [rip+2]; jmp +8; <qword target>
     * The call pushes the address of the EB 08, so on return execution
     * resumes immediately after the embedded literal — i.e. in the
     * trampoline, exactly as a near call would. */
    dst[0] = 0xFF; dst[1] = 0x15;
    uint32_t disp = 2;
    memcpy(dst + 2, &disp, 4);
    dst[6] = 0xEB; dst[7] = 0x08;
    memcpy(dst + 8, &target, 8);
    return 16;
}

static size_t emit_abs_jcc(uint8_t* dst, uint8_t cc, uint64_t target) {
    /* j<!cc> +14 ; <14-byte abs jmp to target>
     * When the original condition is FALSE the inverted short branch
     * skips the absolute jmp; when TRUE it falls through and takes it. */
    dst[0] = (uint8_t)(0x70 | (cc ^ 1));  /* inverted short Jcc */
    dst[1] = 14;                          /* skip the abs jmp     */
    emit_abs_jmp(dst + 2, target);
    return 16;
}

/* Map a conditional-branch mnemonic to its 4-bit condition code (tttn),
 * or return -1 if it is not a widenable conditional branch. */
static int jcc_condition_code(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_JO:   return 0x0;
        case ZYDIS_MNEMONIC_JNO:  return 0x1;
        case ZYDIS_MNEMONIC_JB:   return 0x2;
        case ZYDIS_MNEMONIC_JNB:  return 0x3;
        case ZYDIS_MNEMONIC_JZ:   return 0x4;
        case ZYDIS_MNEMONIC_JNZ:  return 0x5;
        case ZYDIS_MNEMONIC_JBE:  return 0x6;
        case ZYDIS_MNEMONIC_JNBE: return 0x7;
        case ZYDIS_MNEMONIC_JS:   return 0x8;
        case ZYDIS_MNEMONIC_JNS:  return 0x9;
        case ZYDIS_MNEMONIC_JP:   return 0xA;
        case ZYDIS_MNEMONIC_JNP:  return 0xB;
        case ZYDIS_MNEMONIC_JL:   return 0xC;
        case ZYDIS_MNEMONIC_JNL:  return 0xD;
        case ZYDIS_MNEMONIC_JLE:  return 0xE;
        case ZYDIS_MNEMONIC_JNLE: return 0xF;
        default:                  return -1;
    }
}

/* ════════════════════════════════════════════════════════════════
 * umf_disassemble_prologue_length
 * ════════════════════════════════════════════════════════════════ */

size_t umf_disassemble_prologue_length(const uint8_t* code, size_t min_bytes) {
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder,
            ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        return 0;
    }

    size_t pos = 0;
    while (pos < min_bytes) {
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,
                code + pos, 16, &insn, ops))) {
            return 0;
        }
        pos += insn.length;
    }
    return pos;
}

/* ════════════════════════════════════════════════════════════════
 * umf_build_trampoline_code
 * ════════════════════════════════════════════════════════════════ */

bool umf_build_trampoline_code(void* target, UmfTrampolineSlot* slot,
                               size_t min_steal, size_t* out_steal,
                               UmfRelocMap* out_map) {
    if (!target || !slot || min_steal == 0) return false;

    const uint8_t* src = (const uint8_t*)target;
    uint8_t*       dst = slot->code;
    const size_t   code_budget = UMF_TRAMPOLINE_SLOT_SIZE - UMF_UNWIND_RESERVE;

    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder,
            ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        return false;
    }

    size_t src_pos = 0;   /* bytes consumed from the target             */
    size_t dst_pos = 0;   /* bytes emitted into the trampoline          */
    int    map_count = 0;

    if (out_map) out_map->count = 0;

    while (src_pos < min_steal) {
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,
                src + src_pos, 16, &insn, ops))) {
            UMF_ERROR("Prologue decode failed at +%zu", src_pos);
            return false;
        }

        if (map_count >= UMF_MAX_PROLOGUE_INSNS) {
            UMF_ERROR("Prologue exceeds %d instructions", UMF_MAX_PROLOGUE_INSNS);
            return false;
        }
        if (out_map) {
            out_map->src_off[map_count] = (uint8_t)src_pos;
            out_map->dst_off[map_count] = (uint8_t)dst_pos;
        }
        map_count++;

        const uint64_t src_rip = (uint64_t)(uintptr_t)(src + src_pos);

        /* Classify the position-dependent part, if any. */
        const ZydisDecodedOperand* rip_mem = NULL;
        const ZydisDecodedOperand* rel_imm = NULL;
        if (insn.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
            for (int i = 0; i < insn.operand_count; i++) {
                const ZydisDecodedOperand* op = &ops[i];
                if (op->type == ZYDIS_OPERAND_TYPE_MEMORY &&
                    op->mem.base == ZYDIS_REGISTER_RIP) {
                    rip_mem = op;
                } else if (op->type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                           op->imm.is_relative) {
                    rel_imm = op;
                }
            }
        }

        if (rip_mem) {
            /* RIP-relative memory operand → copy verbatim, rebase disp. */
            if (dst_pos + insn.length > code_budget) goto overflow;
            memcpy(dst + dst_pos, src + src_pos, insn.length);

            if (insn.raw.disp.size != 32) {
                UMF_ERROR("Unsupported RIP-relative disp size %u in prologue",
                          insn.raw.disp.size);
                return false;
            }
            uint64_t abs = 0;
            ZydisCalcAbsoluteAddress(&insn, rip_mem, src_rip, &abs);
            uint64_t new_rip =
                (uint64_t)(uintptr_t)(dst + dst_pos) + insn.length;
            int64_t new_disp = (int64_t)abs - (int64_t)new_rip;
            if (new_disp < INT32_MIN || new_disp > INT32_MAX) {
                UMF_ERROR("RIP-relative operand unreachable after relocation");
                return false;
            }
            int32_t d32 = (int32_t)new_disp;
            memcpy(dst + dst_pos + insn.raw.disp.offset, &d32, 4);
            dst_pos += insn.length;
        }
        else if (rel_imm) {
            /* Relative branch/call → re-encode as absolute. */
            uint64_t abs = 0;
            ZydisCalcAbsoluteAddress(&insn, rel_imm, src_rip, &abs);

            if (insn.mnemonic == ZYDIS_MNEMONIC_JMP) {
                if (dst_pos + 14 > code_budget) goto overflow;
                dst_pos += emit_abs_jmp(dst + dst_pos, abs);
            } else if (insn.mnemonic == ZYDIS_MNEMONIC_CALL) {
                if (dst_pos + 16 > code_budget) goto overflow;
                dst_pos += emit_abs_call(dst + dst_pos, abs);
            } else {
                int cc = jcc_condition_code(insn.mnemonic);
                if (cc < 0) {
                    UMF_ERROR("Un-relocatable branch '%s' in prologue",
                              ZydisMnemonicGetString(insn.mnemonic));
                    return false;
                }
                if (dst_pos + 16 > code_budget) goto overflow;
                dst_pos += emit_abs_jcc(dst + dst_pos, (uint8_t)cc, abs);
            }
        }
        else {
            /* Position-independent → copy verbatim. */
            if (dst_pos + insn.length > code_budget) goto overflow;
            memcpy(dst + dst_pos, src + src_pos, insn.length);
            dst_pos += insn.length;
        }

        src_pos += insn.length;
    }

    /* Append the jump back to the continuation (target + stolen bytes). */
    if (dst_pos + 14 > code_budget) goto overflow;
    dst_pos += emit_abs_jmp(dst + dst_pos,
                            (uint64_t)(uintptr_t)(src + src_pos));

    slot->used_size = dst_pos;
    if (out_map)   out_map->count = map_count;
    if (out_steal) *out_steal = src_pos;
    return true;

overflow:
    UMF_ERROR("Trampoline code budget exceeded (%zu bytes) for %p",
              code_budget, target);
    return false;
}
