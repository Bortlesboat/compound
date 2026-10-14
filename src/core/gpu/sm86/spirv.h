//! SPIR-V 1.0 lowering for the SM86 IR.
//!
//! This is the back half of the pipeline. It owns two things:
//!
//! 1. **A fixed module header** (`docs/SM86_TO_SPIRV_RULES.md` rule 6). Types,
//!    constants and the constant-buffer declarations are identical for every
//!    translated block, so they are a `static const uint32_t` array emitted once
//!    rather than rebuilt per block.
//! 2. **A body emitter** that walks a `sm86_ir_program_t` and appends one
//!    instruction per IR op with `*cursor++ = word` (rule 7): the write cursor
//!    is a register-resident pointer, not an index into memory that is
//!    recomputed per instruction.
//!
//! The module is a single `GLCompute` entry point with a straight-line body.
//! Control flow is not yet modelled (the IR is a single block), which is why
//! `BAR`/`WARPSYNC`/`MEMBAR` lower to nothing: with no other block to order
//! against, they have no effect on the emitted module.
//!
//! # Register ids
//!
//! The header pins these ids, and the body allocates from
//! `SM86_SPIRV_ID_FIRST_DYNAMIC` upward. Pinning them is what lets the header be
//! a compile-time constant: a value's SPIR-V id is its index offset by one, so
//! no id ever depends on another value.
//!
//! | Id  | Meaning                                   |
//! |-----|-------------------------------------------|
//! | 1   | `void`                                    |
//! | 2   | `bool`                                    |
//! | 3   | `uint`                                    |
//! | 4   | `uint` constant `0`                       |
//! | 5   | `uint` constant `1`                       |
//! | 6   | `uint` constant `256`                     |
//! | 7   | `bool` constant `true`                    |
//! | 8   | `bool` constant `false`                   |
//! | 9   | `ptr Uniform uint`                        |
//! | 10  | `uint[256]`                               |
//! | 11  | the constant-buffer struct               |
//! | 12  | `ptr Uniform <constant-buffer struct>`    |
//! | 13..28 | the sixteen `Uniform` buffer variables |
//! | 29  | `void()` function type                    |
//! | 30  | `main` function                           |
//! | 31  | `main`'s entry label                      |
//! | 32+ | body values                               |

#ifndef POUND_GPU_SM86_SPIRV_H
#define POUND_GPU_SM86_SPIRV_H

#include "attributes.h"
#include "errors.h"
#include "gpu/sm86/ir.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /// SPIR-V `OpCode` and enum values the emitter needs, spelled out so the
    /// module does not depend on the Khronos header. The numeric values are
    /// frozen by the SPIR-V specification.
    typedef enum
    {
        SM86_SPIRV_OP_UNDEF                  = 1,
        SM86_SPIRV_OP_MEMORY_MODEL           = 14,
        SM86_SPIRV_OP_ENTRY_POINT            = 15,
        SM86_SPIRV_OP_EXECUTION_MODE         = 16,
        SM86_SPIRV_OP_CAPABILITY             = 17,
        SM86_SPIRV_OP_TYPE_VOID              = 19,
        SM86_SPIRV_OP_TYPE_BOOL              = 20,
        SM86_SPIRV_OP_TYPE_INT               = 21,
        SM86_SPIRV_OP_TYPE_ARRAY             = 28,
        SM86_SPIRV_OP_TYPE_STRUCT            = 30,
        SM86_SPIRV_OP_TYPE_POINTER           = 32,
        SM86_SPIRV_OP_TYPE_FUNCTION          = 33,
        SM86_SPIRV_OP_CONSTANT_TRUE          = 41,
        SM86_SPIRV_OP_CONSTANT_FALSE         = 42,
        SM86_SPIRV_OP_CONSTANT               = 43,
        SM86_SPIRV_OP_FUNCTION               = 54,
        SM86_SPIRV_OP_FUNCTION_END           = 56,
        SM86_SPIRV_OP_VARIABLE               = 59,
        SM86_SPIRV_OP_LOAD                   = 61,
        SM86_SPIRV_OP_ACCESS_CHAIN           = 65,
        SM86_SPIRV_OP_DECORATE               = 71,
        SM86_SPIRV_OP_COPY_OBJECT            = 83,
        SM86_SPIRV_OP_IADD                   = 128,
        SM86_SPIRV_OP_IMUL                   = 132,
        SM86_SPIRV_OP_LOGICAL_NOT            = 168,
        SM86_SPIRV_OP_SELECT                 = 169,
        SM86_SPIRV_OP_I_EQUAL                = 170,
        SM86_SPIRV_OP_I_NOT_EQUAL            = 171,
        SM86_SPIRV_OP_U_GREATER_THAN         = 172,
        SM86_SPIRV_OP_S_GREATER_THAN         = 173,
        SM86_SPIRV_OP_U_GREATER_THAN_EQUAL   = 174,
        SM86_SPIRV_OP_S_GREATER_THAN_EQUAL   = 175,
        SM86_SPIRV_OP_U_LESS_THAN            = 176,
        SM86_SPIRV_OP_S_LESS_THAN            = 177,
        SM86_SPIRV_OP_U_LESS_THAN_EQUAL      = 178,
        SM86_SPIRV_OP_S_LESS_THAN_EQUAL      = 179,
        SM86_SPIRV_OP_LABEL                  = 248,
        SM86_SPIRV_OP_KILL                   = 252,
        SM86_SPIRV_OP_RETURN                 = 253,
    } sm86_spirv_opcode_t;

    /// One `(opcode, word_count)` pair packed into a first word.
    ///
    /// `word_count` includes the first word, exactly as SPIR-V encodes it.
    #define SM86_SPIRV_INSTRUCTION(opcode_, word_count_) \
        ((((uint32_t)(word_count_)) << 16) | ((uint32_t)(opcode_)))

    /// SPIR-V storage classes and decoration kinds the emitter uses.
    #define SM86_SPIRV_STORAGE_UNIFORM 2U
    #define SM86_SPIRV_DECORATION_BLOCK 2U
    #define SM86_SPIRV_DECORATION_ARRAY_STRIDE 6U
    #define SM86_SPIRV_DECORATION_BINDING 33U
    #define SM86_SPIRV_DECORATION_DESCRIPTOR_SET 34U

    /// Constant buffers one module declares. A shader may reference several CBs;
    /// the module reserves a fixed sixteen so a binding maps to a known variable
    /// id without a per-block allocation pass.
    #define SM86_SPIRV_UBO_BINDING_COUNT 16U

    /// The largest word count `sm86_spirv_emit` may produce for one block.
    ///
    /// The body is bounded by `SM86_IR_MAX_INSTRUCTIONS`. Most IR ops append a
    /// single instruction of at most six words (`OpSelect`), but an `LDC`
    /// synthesises three -- a constant, an `OpAccessChain` and an `OpLoad` --
    /// for fourteen words, so the bound is taken per IR op at that worst case.
    /// The constant term covers the header and the closing `OpFunctionEnd`.
    #define SM86_SPIRV_MAX_WORDS (SM86_IR_MAX_INSTRUCTIONS * 16U + 512U)

    /// The constant module header, as raw words.
    POUND_PURE const uint32_t *sm86_spirv_header_words(void);

    /// The number of words in `sm86_spirv_header_words`.
    POUND_PURE size_t sm86_spirv_header_word_count(void);

    /// Lowers `program` into `out_words`.
    ///
    /// Writes the module header followed by the block body and `OpFunctionEnd`.
    /// `out_capacity_words` must be at least `SM86_SPIRV_MAX_WORDS`; the call
    /// fails with `POUND_ERROR_TRANSLATION_FAILED` rather than overflowing if it
    /// is not. On success `*out_word_count` is the number of words written.
    ///
    /// Returns `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument, and
    /// `POUND_ERROR_TRANSLATION_FAILED` for a construct the module cannot
    /// represent (an out-of-range constant-buffer byte offset, or an `ISETP`
    /// comparison the emitter does not implement).
    error_t sm86_spirv_emit(const sm86_ir_program_t *POUND_RESTRICT program,
                            uint32_t *POUND_RESTRICT                out_words,
                            size_t                                  out_capacity_words,
                            size_t *POUND_RESTRICT                  out_word_count);

#ifdef __cplusplus
}
#endif

#endif // POUND_GPU_SM86_SPIRV_H

/*** end of file ***/
