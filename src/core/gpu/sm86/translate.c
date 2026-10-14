#include "gpu/sm86/translate.h"

#include "log.h"

error_t
sm86_translate(const sm86_raw_instruction_t *POUND_RESTRICT code,
               const uint32_t                               instruction_count,
               uint32_t *POUND_RESTRICT                     out_words,
               const size_t                                 out_capacity_words,
               size_t *POUND_RESTRICT                       out_word_count,
               sm86_translation_stats_t *POUND_RESTRICT     out_stats)
{
    if ((NULL == code) || (NULL == out_words) || (NULL == out_word_count))
    {
        POUND_LOG_ERROR(&thread_logger, "sm86_translate requires non-NULL code, output and count.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (NULL != out_stats)
    {
        *out_stats = (sm86_translation_stats_t){0};
        out_stats->instruction_count = instruction_count;
    }

    // The IR is roughly 44 KiB of fixed arrays. It is a translation-time scratch
    // area, not per-shader state, and building it on the stack keeps the
    // translator free of an allocator dependency.
    sm86_ir_program_t program;
    const error_t build_status = sm86_ir_build(&program, code, instruction_count);

    if (POUND_SUCCESS != build_status)
    {
        return build_status;
    }

    const error_t emit_status = sm86_spirv_emit(&program, out_words, out_capacity_words, out_word_count);

    if (POUND_SUCCESS != emit_status)
    {
        return emit_status;
    }

    if (NULL != out_stats)
    {
        out_stats->ir_instruction_count = program.count;
        out_stats->ir_value_count       = program.value_count;
        out_stats->ir_use_count         = program.use_count;
        out_stats->spirv_word_count     = (uint32_t)*out_word_count;
    }

    return POUND_SUCCESS;
}

/*** end of file ***/
