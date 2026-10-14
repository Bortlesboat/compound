//! Entry point for the SM86 -> SPIR-V translator.
//!
//! This ties the three stages together -- `decoder.h`, `ir.h`, `spirv.h` -- and
//! is the only symbol an embedding emulator needs to call. The intermediate
//! stages stay public because the tests assert each rule against the stage it
//! belongs to (the IR for rules 1-5, the emitter for rules 6-11); a caller that
//! only wants a module uses `sm86_translate`.
//!
//! The translator is deliberately total on its inputs: any byte sequence either
//! yields a module or a typed error with a log record. It never writes a partial
//! module, and it never guesses at an encoding it does not implement.

#ifndef POUND_GPU_SM86_TRANSLATE_H
#define POUND_GPU_SM86_TRANSLATE_H

#include "attributes.h"
#include "errors.h"
#include "gpu/sm86/decoder.h"
#include "gpu/sm86/ir.h"
#include "gpu/sm86/spirv.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /// What one translation produced. Useful for tests and for sizing a cache
    /// entry without re-deriving the counts from the module.
    typedef struct
    {
        uint32_t instruction_count;   // SM86 instructions consumed
        uint32_t ir_instruction_count; // IR ops after predication expansion
        uint32_t ir_value_count;      // SSA values defined
        uint32_t ir_use_count;        // def-use edges recorded
        uint32_t spirv_word_count;    // words written, header included
    } sm86_translation_stats_t;

    /// Translates `instruction_count` raw SM86 instructions into a SPIR-V
    /// module.
    ///
    /// The output buffer must hold `SM86_SPIRV_MAX_WORDS`; the call fails with
    /// `POUND_ERROR_TRANSLATION_FAILED` otherwise rather than writing past the
    /// end. `out_stats` may be NULL.
    ///
    /// Returns `POUND_SUCCESS`, `POUND_ERROR_INVALID_ARGUMENT`,
    /// `POUND_ERROR_UNSUPPORTED_INSTRUCTION` or `POUND_ERROR_TRANSLATION_FAILED`.
    error_t sm86_translate(const sm86_raw_instruction_t *POUND_RESTRICT code,
                           uint32_t                                     instruction_count,
                           uint32_t *POUND_RESTRICT                     out_words,
                           size_t                                       out_capacity_words,
                           size_t *POUND_RESTRICT                       out_word_count,
                           sm86_translation_stats_t *POUND_RESTRICT     out_stats);

#ifdef __cplusplus
}
#endif

#endif // POUND_GPU_SM86_TRANSLATE_H

/*** end of file ***/
