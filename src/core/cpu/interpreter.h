//! ARM64 (A64) interpreter.
//!
//! This is the fallback path beneath the JIT. Where `jit_execution` fills a miss
//! with a host-code block it runs the translated block; where it cannot -- because
//! the translator refused the block, or because the block is being invalidated --
//! it runs the guest instruction by instruction through `interpreter_run`. That is
//! why the entry points are shaped the way they are: `interpreter_probe` answers
//! "can this one be run here?", and `interpreter_run` runs a *budget* of them and
//! stops the moment it hits something it cannot run, rather than committing to a
//! whole block.
//!
//! # Supported instructions
//!
//! The set is deliberately a subset, and it is closed: an encoding outside it is
//! reported (`INTERPRETER_TRAP_UNDEFINED`) rather than guessed at, and
//! `interpreter_probe` answers `0` for it so the caller can fall back.
//!
//! - No-ops: `NOP`, and the `HINT` space (treated as `NOP`).
//! - Moves and immediates: `MOVZ`, `MOVN`, `MOVK`, `ADR`, `ADRP`.
//! - Add/subtract: `ADD`/`SUB`/`ADDS`/`SUBS`, immediate and shifted-register.
//! - Logical: `AND`/`ORR`/`EOR`/`ANDS`, immediate and shifted-register, including
//!   the `BIC`/`ORN`/`EON`/`BICS` forms and the `MOV`/`MVN`/`CMP`/`CMN`/`TST`
//!   aliases, which are those instructions with a fixed operand.
//! - Multiply/divide/shift: `MADD`, `MSUB`, `UDIV`, `SDIV`, `LSLV`, `LSRV`,
//!   `ASRV`.
//! - Conditional select: `CSEL`, `CSINC`, `CSINV`, `CSNEG`, and the `CSET`/`CSETM`
//!   aliases.
//! - Branches: `B`, `BL`, `B.cond`, `CBZ`/`CBNZ`, `TBZ`/`TBNZ`, `BR`, `BLR`,
//!   `RET`.
//! - Loads and stores: the unsigned-offset and literal forms of
//!   `LDR`/`LDRB`/`LDRH`/`LDRSB`/`LDRSH`/`LDRSW`/`STR`/`STRB`/`STRH`, and
//!   offset-form `LDP`/`STP`.
//! - Exceptions: `SVC`, `BRK`, `HLT` (reported, never simulated).
//!
//! Anything else -- notably floating point, SIMD, and the atomics -- is not
//! represented. That is not an oversight to be fixed by widening this file: the
//! architecture tooling runs those through the JIT, and a partial interpreter that
//! silently approximated them would be worse than one that refuses.
//!
//! # Register 31
//!
//! Register 31 is `SP` where the architecture uses a stack pointer (an add/subtract
//! immediate operand or a load/store base) and `ZR` everywhere else. The interpreter
//! reproduces that per instruction rather than storing two registers, and `SP` lives
//! in `x[31]` of the guest state.

#ifndef POUND_INTERPRETER_H
#define POUND_INTERPRETER_H

#include <stdbool.h>
#include <stdint.h>

#include "errors.h"
#include "guest_state.h"
#include "memory/guest_memory.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /// Why `interpreter_run` stopped before its budget ran out.
    ///
    /// `INTERPRETER_TRAP_NONE` is the value left in `interpreter_trap_t::kind` when
    /// the loop instead ran to the end of its budget, so a caller that ignores the
    /// return value cannot mistake "finished" for a trap it did not handle.
    ///
    /// `BRK` and `HLT` both report `INTERPRETER_TRAP_BREAKPOINT`: they are the two
    /// deliberate halts, and the distinction between them is the debugger's, not
    /// the interpreter's.
    typedef enum
    {
        INTERPRETER_TRAP_NONE = 0,
        INTERPRETER_TRAP_SVC   = 1,
        INTERPRETER_TRAP_BREAKPOINT,
        INTERPRETER_TRAP_UNDEFINED,
    } interpreter_trap_kind_t;

    /// What stopped the interpreter, when something did.
    typedef struct
    {
        interpreter_trap_kind_t kind;

        /// Address of the instruction that trapped. `guest_state_t::pc` is left at
        /// the same address, so resuming means re-running or stepping past it.
        uint64_t pc;

        /// The trapping instruction word.
        uint32_t instr;

        /// `SVC`/`BRK` immediate, or 0 for the other kinds.
        uint64_t imm;
    } interpreter_trap_t;

    /* ——— Public API ———————————————————————————————— */

    /// Runs up to `budget` guest instructions.
    ///
    /// Returns:
    ///
    /// - `POUND_SUCCESS` when the loop stopped normally: either `budget`
    ///   instructions retired, or one trapped. When it trapped, `trap_out` (if
    ///   non-NULL) holds why; when it did not, `trap_out->kind` is
    ///   `INTERPRETER_TRAP_NONE`. `*executed` always reports how many instructions
    ///   actually retired.
    /// - `POUND_ERROR_MEMORY_FAULT` when an instruction fetch, load or store had no
    ///   mapped host memory. `guest_state_t::pc` is left at the faulting
    ///   instruction and `*executed` counts what retired before it.
    /// - `POUND_ERROR_INVALID_ARGUMENT` when `state`, `memory` or `executed` is
    ///   NULL.
    ///
    /// `trap_out` may be NULL, in which case a trap still stops the loop and is
    /// simply not reported.
    error_t interpreter_run(guest_state_t *POUND_RESTRICT state,
                            guest_memory_t *POUND_RESTRICT memory,
                            uint32_t                        budget,
                            uint32_t *POUND_RESTRICT        executed,
                            interpreter_trap_t *POUND_RESTRICT trap_out);

    /// Reports whether `interpreter_run` will execute `instr`.
    ///
    /// Returns `1` when the encoding is one this file implements and `0` when it is
    /// not, so a caller unsure whether to run a block here or translate it can ask
    /// first. Traps count as decodable: `SVC` is something the interpreter handles,
    /// not something it refuses.
    int interpreter_probe(uint32_t instr);

#ifdef __cplusplus
}
#endif

#endif /* POUND_INTERPRETER_H */

/*** end of file ***/
