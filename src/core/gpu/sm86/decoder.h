//! SM86 (NVIDIA Volta/Turing-class) SASS instruction decoder.
//!
//! A guest graphics shader arrives as a stream of 128-bit SASS instructions.
//! This module turns each raw instruction into a field-by-field
//! `sm86_decoded_instruction_t`, which the IR builder in `ir.h` consumes. It is
//! the front half of the SM86 -> SPIR-V pipeline described by
//! `docs/SM86_TO_SPIRV_RULES.md`.
//!
//! # What the decoder is, and is not
//!
//! The decoder is *structural*: it slices the fixed bit fields that the SM86
//! encoding documents (see the Mesa `nak/sm70_encode.rs` back end referenced
//! below) and resolves the 12-bit opcode through
//! `g_sm86_opcodes_bits_to_enum`. It does not judge whether the instruction is
//! translatable; that is the IR builder's job, and it reports
//! `POUND_ERROR_UNSUPPORTED_INSTRUCTION` rather than guessing.
//!
//! # Bit layout
//!
//! The 128-bit instruction is carried as `{low, high}` and read as four 32-bit
//! words: `word0 = low`, `word1 = low >> 32`, `word2 = high`,
//! `word3 = high >> 32`. `word0` holds the opcode, the source-form selector,
//! the predicate guard and the destination register; `word1` holds src1 or the
//! payload (immediate / constant-buffer reference); `word2` holds src2 and the
//! class-specific modifier bits; `word3` holds scheduling and barrier bits.
//!
//! # Two deviations from the reference decoder, and why
//!
//! 1. **LDC keeps its constant-buffer payload.** In the reference, the
//!    `SM86_CLASS_MEMORY_LOAD_STORE` arm overwrites the `payload` union with a
//!    sign-extended `memory_offset`, which for an `LDC` (form 5, a constant
//!    buffer read) destroys the `{binding, byte_offset}` pair that rule 9 of
//!    the translation rules needs. This decoder recognises `LDC` explicitly and
//!    decodes the constant-buffer reference from `word1` before the class
//!    switch, and the class switch does not touch the payload for `LDC`.
//! 2. **Form-stripped opcode fallback.** The low 12 bits of `word0` are the
//!    opcode *and* the 3-bit source-form selector; the reference table names the
//!    base encodings, so a lookup such as `0x810` (IADD3 with form 4) misses.
//!    When the exact lookup is `NOP` but the index is non-zero, the decoder
//!    retries with bits 9..11 cleared. This is a decode-time convenience, not a
//!    semantic guess: the instruction's identity is unchanged, only the form
//!    bits are masked off to find its name.

#ifndef POUND_GPU_SM86_DECODER_H
#define POUND_GPU_SM86_DECODER_H

#include "attributes.h"
#include "gpu/sm86/generated/opcodes.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /// A raw 128-bit SM86 SASS instruction, low word first.
    typedef struct
    {
        uint64_t low;
        uint64_t high;
    } sm86_raw_instruction_t;

    /// The predicate value that means "no predicate".
    ///
    /// Predicate registers are `P0..P6`; index 7 (`PT`) is the always-true
    /// sentinel the encoding uses for an unpredicated instruction.
    #define SM86_PREDICATE_NONE 7U

    /// SM86 SASS encoding, as documented by the Mesa `nak/sm70_encode.rs` back end
    /// (<https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/nouveau/compiler/nak/sm70_encode.rs>).
    ///
    /// The struct is a decoded *view*; it is filled by `sm86_decode` and copied
    /// out immediately, so its in-memory layout is an implementation detail
    /// rather than an ABI. The `form` field's documented values are the src1/src2
    /// selectors listed inline.
    typedef struct
    {
        uint16_t opcode;

        /// The 12-bit opcode field exactly as encoded, including the source-form
        /// selector bits. `opcode` names the instruction; this preserves the raw
        /// value so a diagnostic can report what was actually seen.
        uint16_t raw_opcode;

        /// Destination GPR. For `ISETP` this names the predicate register written.
        uint8_t destination_register;

        /// Source 0 GPR.
        uint8_t source0_register;

        /// Source 1 GPR, when `form` selects a register.
        uint8_t source1_register;

        /// Source 2 GPR.
        uint8_t source2_register;

        /// Guard register and its polarity. `SM86_PREDICATE_NONE` means no guard.
        uint8_t predicate_register : 3;
        uint8_t predicate_not : 1;

        /// Src1 selector:
        ///     1 = Reg
        ///     6 = UReg
        ///     4 = Imm32
        ///     5 = CBuf
        ///
        /// Src2 selector:
        ///     7 = UReg
        ///     2 = Imm32
        ///     3 = CBuf
        uint8_t form : 4;

        uint8_t source0_neg : 1;   // -src0
        uint8_t source0_abs : 1;   // |src0|
        uint8_t source1_neg : 1;   // -src1
        uint8_t source1_abs : 1;   // |src1|
        uint8_t saturate : 1;      // .SAT (clamp to 0.0 - 1.0)
        uint8_t ftz : 1;           // .FTZ (flush to zero)
        uint8_t is_uniform : 1;    // Uniform ALU (UGPR)
        uint8_t extended_math : 1; // .X

        uint16_t padding : 10;
        uint8_t  unknown_opcode : 1; // True when the 12-bit opcode names no known instruction.
        uint8_t  cmp_type : 1;

        uint8_t  delay_cycles : 4;  // Cycles to stall before executing.
        uint8_t  yield_flag : 1;    // True if the warp can yield to the scheduler.
        uint8_t  read_barrier : 3;  // Wait on read barrier index.
        uint8_t  write_barrier : 3; // Set write barrier index.

        uint16_t cmp_operator : 3;
        uint16_t bool_operator : 2;

        uint16_t accumulator_predicate : 3;
        uint16_t accumulator_predicate_not : 1;

        /// An instruction cannot use an immediate and a constant buffer at the
        /// same time, so the two payloads share storage.
        union
        {
            /// `form == 4`.
            int32_t immediate_value;

            /// 24-bit sign-extended offset for LD/ST.
            int32_t memory_offset;

            /// `form == 5`, and always for `LDC`.
            struct
            {
                uint16_t byte_offset;
                uint8_t  binding_index;
                uint8_t  padding;
            } constant_buffer;
        } payload;
    } sm86_decoded_instruction_t;

    /// The broad family an opcode belongs to, used to select which modifier bits
    /// the decoder reads. Kept as a plain C11 enum: fixed underlying types are
    /// C23, and this tree is C11.
    typedef enum
    {
        SM86_CLASS_UNKNOWN = 0,
        SM86_CLASS_FLOAT_ALU,
        SM86_CLASS_INT_ALU,
        SM86_CLASS_HALF_FLOAT_ALU,
        SM86_CLASS_MEMORY_LOAD_STORE,
        SM86_CLASS_TEXTURE_FETCH,
        SM86_CLASS_SURFACE_ATOMIC,
        SM86_CLASS_CONTROL_FLOW,
        SM86_CLASS_SYNC_AND_YIELD,
    } sm86_instruction_class_t;

    /// What the IR builder needs to know about an opcode before reading its
    /// operand fields. `instruction_class` is stored as a `uint8_t` so the whole
    /// record stays 4 bytes and a large table stays cache-friendly.
    typedef struct
    {
        uint8_t instruction_class; // sm86_instruction_class_t
        uint8_t has_destination;
        uint8_t num_inputs;
        uint8_t is_memory_instruction;
    } sm86_instruction_metadata_t;

    _Static_assert(4 == sizeof(sm86_instruction_metadata_t), "metadata record must stay 4 bytes");

    /// Opcode -> metadata, indexed by `sm86_opcode_t`.
    extern const sm86_instruction_metadata_t g_sm86_opcode_metadata[SM86_OPCODE_MAX_INSTRUCTIONS];

    /// Decodes one raw instruction into `out_instruction`.
    ///
    /// Never fails: an opcode the table does not name decodes to
    /// `SM86_OPCODE_NOP` with `SM86_CLASS_UNKNOWN` metadata, which the IR builder
    /// then rejects. Both pointers must be non-NULL.
    POUND_HOT void sm86_decode(const sm86_raw_instruction_t *POUND_RESTRICT raw_instruction,
                               sm86_decoded_instruction_t   *POUND_RESTRICT out_instruction);

    /// Decodes `instruction_count` contiguous raw instructions.
    POUND_HOT void sm86_decode_block(const sm86_raw_instruction_t *POUND_RESTRICT raw_instructions,
                                     sm86_decoded_instruction_t   *POUND_RESTRICT out_decoded_instructions,
                                     uint32_t                      instruction_count);

    /// Human-readable name for `opcode`, or `"UNKNOWN"`.
    POUND_PURE const char *sm86_opcode_name(sm86_opcode_t opcode);

#ifdef __cplusplus
}
#endif

#endif // POUND_GPU_SM86_DECODER_H

/*** end of file ***/
