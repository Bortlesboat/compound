//! The intermediate representation the SM86 -> SPIR-V translator works on.
//!
//! This is the middle of the pipeline: `decoder.h` produces a
//! `sm86_decoded_instruction_t` per SASS instruction, this module turns a
//! decoded block into an SSA IR with def-use chains, and `spirv.h` lowers that
//! IR. The design follows `docs/SM86_TO_SPIRV_RULES.md` rule by rule:
//!
//! - **Rule 1 -- Structure of Arrays.** A `sm86_ir_program_t` is not an array of
//!   instructions; it is a set of parallel arrays, one per field. The builder
//!   walks them with cursors, so laying one down is a store at `cursor++` rather
//!   than a strided read-modify-write (see `docs/PROGRAMMING_RULES.md` rule 2).
//! - **Rule 2 -- Def-use chains built during Pass 1.** Every value carries the
//!   instruction that defines it and the head of a linked list of its uses.
//!   Both are filled as instructions are appended, never in a later sweep.
//! - **Rule 3 -- Predicates map to a boolean.** Predicate values have type
//!   `SM86_IR_TYPE_PREDICATE`, which lowers to SPIR-V `OpTypeBool`.
//! - **Rules 4 and 5 -- Predicated instructions are expanded in Pass 1, the
//!   unpredicated instruction first.** A guarded SASS instruction becomes the
//!   compute op (carrying no architectural write) followed by a
//!   `SM86_IR_OP_SELECT` that commits `predicate ? computed : previous` to the
//!   destination register.
//! - **Rule 10 -- UGPRs are ordinary thread-local registers.** A uniform form
//!   (`is_uniform`) does not change how an operand is read; uniform register
//!   indices share the GPR space. **Rule 11** follows: no subgroup broadcast is
//!   ever emitted for a uniform operand.
//!
//! # Values and registers
//!
//! The IR is in SSA form. Each instruction that produces a result owns one
//! *value id*; reads of that result refer to the id. The mapping from an SM86
//! GPR to its current value is local to `sm86_ir_build`, so the IR itself never
//! stores a register file. A read of a register that has not been written in the
//! block becomes `SM86_IR_OP_READ_REG`, which lowers to `OpUndef`: the incoming
//! value of an undefined guest register is genuinely undefined.

#ifndef POUND_GPU_SM86_IR_H
#define POUND_GPU_SM86_IR_H

#include "attributes.h"
#include "errors.h"
#include "gpu/sm86/decoder.h"
#include <stdbool.h>
#include <stdint.h>

/// Instructions one block may hold after predication expansion.
///
/// A decoded instruction expands to at most a handful of IR ops (an operand
/// materialisation, the compute, and a committing select), so 512 leaves ample
/// headroom for the largest straight-line shader block the translator accepts.
#define SM86_IR_MAX_INSTRUCTIONS 512U

/// SSA values one program may define. Two per instruction covers a compute plus
/// its committing select, with room to spare.
#define SM86_IR_MAX_VALUES 1024U

/// Def-use edges one program may record.
#define SM86_IR_MAX_USES 2048U

/// Sentinel for "this instruction defines no value" / "no register".
#define SM86_IR_NONE UINT32_MAX

/// Sentinel stored in `write_register` / `write_predicate` for "no write".
#define SM86_IR_NO_REGISTER 0xFFU

/// Operand slot indices used in the def-use lists.
typedef enum
{
    SM86_IR_SLOT_SOURCE0 = 0,
    SM86_IR_SLOT_SOURCE1 = 1,
    SM86_IR_SLOT_SOURCE2 = 2,
    SM86_IR_SLOT_PREDICATE = 3,
} sm86_ir_slot_t;

/// The type of a value or result. Integer and boolean only: the supported
/// instruction set is integer ALU, constant-buffer loads and comparisons, so no
/// floating-point type is ever produced.
typedef enum
{
    SM86_IR_TYPE_VOID = 0,
    SM86_IR_TYPE_U32,
    SM86_IR_TYPE_PREDICATE, // one-bit boolean
} sm86_ir_value_type_t;

/// The operations the IR models. This is deliberately a small, closed set: the
/// translator either produces one of these or refuses the instruction.
typedef enum
{
    /// No operation. Present so index 0 is a defined value.
    SM86_IR_OP_NOP = 0,

    /// `result = immediate` (the immediate's bit pattern, as `u32`).
    SM86_IR_OP_CONST_U32,

    /// `result = source0` (`OpCopyObject`).
    SM86_IR_OP_COPY,

    /// `result = <undefined value>` (`OpUndef`), reading an unwritten register.
    SM86_IR_OP_READ_REG,

    /// `result = source0 + source1 + source2` (`OpIAdd` twice).
    SM86_IR_OP_IADD3,

    /// `result = source0 * source1 + source2` (`OpIMul`, `OpIAdd`).
    SM86_IR_OP_IMAD,

    /// `result = (source0 <cmp> source1)`, a boolean. `cmp_operator` and
    /// `cmp_signed` select the comparison.
    SM86_IR_OP_ISETP,

    /// `result = constant_buffer[binding].u32[byte_offset / 4]`.
    SM86_IR_OP_LDC,

    /// `result = predicate ? source0 : source1` (`OpSelect`).
    SM86_IR_OP_SELECT,

    /// `result = !source0` on a boolean (`OpLogicalNot`).
    SM86_IR_OP_NOT,

    /// `OpReturn`.
    SM86_IR_OP_EXIT,

    /// `OpKill`.
    SM86_IR_OP_KILL,

    /// A scheduling barrier with no SPIR-V counterpart in a single block. See
    /// `ir.c` for why it lowers to nothing.
    SM86_IR_OP_BARRIER,

    SM86_IR_OP_MAX,
} sm86_ir_opcode_t;

/// A whole translated block, as parallel arrays.
///
/// All arrays have `count` live entries unless noted. The `definition`,
/// `use_head`, `use_next`, `use_instruction` and `use_slot` arrays carry the
/// def-use chains: `definition[v]` is the instruction that defined value `v`,
/// and `use_head[v]` is the index of the most recent use of `v`, chained through
/// `use_next`. A use records which instruction read the value and through which
/// operand slot.
typedef struct
{
    // --- Instruction streams (Structure of Arrays) ---
    uint8_t  opcode[SM86_IR_MAX_INSTRUCTIONS];
    uint8_t  type[SM86_IR_MAX_INSTRUCTIONS];       // sm86_ir_value_type_t of the result
    uint32_t result[SM86_IR_MAX_INSTRUCTIONS];     // SSA value defined, or SM86_IR_NONE
    uint32_t source0[SM86_IR_MAX_INSTRUCTIONS];
    uint32_t source1[SM86_IR_MAX_INSTRUCTIONS];
    uint32_t source2[SM86_IR_MAX_INSTRUCTIONS];
    int32_t  immediate[SM86_IR_MAX_INSTRUCTIONS];
    uint8_t  has_immediate[SM86_IR_MAX_INSTRUCTIONS];

    /// The GPR this instruction commits to, or `SM86_IR_NO_REGISTER`. For a
    /// predicated instruction only the committing `SELECT` carries it.
    uint8_t write_register[SM86_IR_MAX_INSTRUCTIONS];

    /// The predicate this instruction commits to, or `SM86_IR_NO_REGISTER`.
    uint8_t write_predicate[SM86_IR_MAX_INSTRUCTIONS];

    /// `sm86_ir_opcode_t == SM86_IR_OP_ISETP`: the comparison operator and
    /// signedness.
    uint8_t isetp_operator[SM86_IR_MAX_INSTRUCTIONS];
    uint8_t isetp_signed[SM86_IR_MAX_INSTRUCTIONS];

    /// `sm86_ir_opcode_t == SM86_IR_OP_LDC`: the constant buffer binding and
    /// byte offset.
    uint8_t  constant_buffer[SM86_IR_MAX_INSTRUCTIONS];
    int32_t  byte_offset[SM86_IR_MAX_INSTRUCTIONS];

    /// Index of the SM86 instruction that produced this IR op, for diagnostics.
    uint32_t source_instruction[SM86_IR_MAX_INSTRUCTIONS];

    // --- Def-use chains (Rule 2) ---
    uint32_t definition[SM86_IR_MAX_VALUES];  // defining instruction, or SM86_IR_NONE
    uint32_t use_head[SM86_IR_MAX_VALUES];    // newest use index, or SM86_IR_NONE
    uint32_t use_next[SM86_IR_MAX_USES];      // next use in the chain
    uint32_t use_instruction[SM86_IR_MAX_USES];
    uint8_t  use_slot[SM86_IR_MAX_USES];      // sm86_ir_slot_t

    uint32_t value_count;
    uint32_t use_count;
    uint32_t count;
} sm86_ir_program_t;

/// Clears `program` to an empty state.
void sm86_ir_init(sm86_ir_program_t *POUND_RESTRICT program);

/// Builds the IR for a decoded block.
///
/// `code` is `instruction_count` raw SM86 instructions, decoded internally.
/// Returns `POUND_SUCCESS`, or a typed error with a log record:
///
/// - `POUND_ERROR_INVALID_ARGUMENT` for a NULL program or code pointer.
/// - `POUND_ERROR_UNSUPPORTED_INSTRUCTION` for an opcode outside the supported
///   set (which is the integer ALU subset, `LDC`, `EXIT`/`KILL`, and the
///   no-op sync instructions; see `ir.c`).
/// - `POUND_ERROR_TRANSLATION_FAILED` when the block does not fit the fixed IR
///   capacity.
///
/// On failure the program is left empty and must not be emitted.
error_t sm86_ir_build(sm86_ir_program_t *POUND_RESTRICT       program,
                      const sm86_raw_instruction_t *POUND_RESTRICT code,
                      uint32_t                                     instruction_count);

#ifdef __cplusplus
}
#endif

#endif // POUND_GPU_SM86_IR_H

/*** end of file ***/
