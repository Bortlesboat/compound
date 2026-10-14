#include "gpu/sm86/spirv.h"

#include "log.h"
#include <string.h>

// -----------------------------------------------------------------------------
// Fixed register ids (see the table in spirv.h)
// -----------------------------------------------------------------------------

#define SM86_SPIRV_ID_VOID           1U
#define SM86_SPIRV_ID_BOOL           2U
#define SM86_SPIRV_ID_U32            3U
#define SM86_SPIRV_ID_CONST_ZERO     4U
#define SM86_SPIRV_ID_CONST_ONE      5U
#define SM86_SPIRV_ID_CONST_256      6U
#define SM86_SPIRV_ID_CONST_TRUE     7U
#define SM86_SPIRV_ID_CONST_FALSE    8U
#define SM86_SPIRV_ID_PTR_U32        9U
#define SM86_SPIRV_ID_ARRAY          10U
#define SM86_SPIRV_ID_CB_STRUCT      11U
#define SM86_SPIRV_ID_PTR_CB_STRUCT  12U
#define SM86_SPIRV_ID_UBO_BASE       13U
#define SM86_SPIRV_ID_FUNCTION_TYPE  29U
#define SM86_SPIRV_ID_MAIN           30U
#define SM86_SPIRV_ID_MAIN_LABEL     31U
#define SM86_SPIRV_ID_FIRST_DYNAMIC  32U
#define SM86_SPIRV_BOUND             65536U

#define SM86_SPIRV_EXECUTION_MODEL_GLCOMPUTE 5U
#define SM86_SPIRV_EXECUTION_MODE_LOCAL_SIZE 17U

// -----------------------------------------------------------------------------
// The fixed header
// -----------------------------------------------------------------------------
//
// The per-buffer annotations and variable declarations repeat sixteen times.
// Listing them with an X-macro keeps the repetition out of the source while the
// initialiser stays a compile-time constant: every expansion is a list of
// integer literals, so nothing is computed at run time.

#define SM86_SPIRV_UBO_DECORATIONS_(i)                                                  \
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_DECORATE, 4), (SM86_SPIRV_ID_UBO_BASE + (i)),  \
        SM86_SPIRV_DECORATION_BINDING, (i),                                             \
        SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_DECORATE, 4), (SM86_SPIRV_ID_UBO_BASE + (i)), \
        SM86_SPIRV_DECORATION_DESCRIPTOR_SET, 0U,

#define SM86_SPIRV_UBO_VARIABLE_(i)                                                     \
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_VARIABLE, 4), SM86_SPIRV_ID_PTR_CB_STRUCT,     \
        (SM86_SPIRV_ID_UBO_BASE + (i)), SM86_SPIRV_STORAGE_UNIFORM,

#define SM86_SPIRV_UBO_LIST_(X) \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)

static const uint32_t k_sm86_spirv_header[] = {
    // Module header: magic, version 1.0, generator 0, bound, schema 0.
    0x07230203U,
    0x00010000U,
    0x00000000U,
    SM86_SPIRV_BOUND,
    0x00000000U,

    // OpCapability Shader
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_CAPABILITY, 2),
    1U,
    // OpMemoryModel Logical GLSL450
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_MEMORY_MODEL, 3),
    0U,
    1U,
    // OpEntryPoint GLCompute %main "main"
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_ENTRY_POINT, 5),
    SM86_SPIRV_EXECUTION_MODEL_GLCOMPUTE,
    SM86_SPIRV_ID_MAIN,
    0x6E69616DU,
    0x00000000U,
    // OpExecutionMode %main LocalSize 32 1 1
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_EXECUTION_MODE, 6),
    SM86_SPIRV_ID_MAIN,
    SM86_SPIRV_EXECUTION_MODE_LOCAL_SIZE,
    32U,
    1U,
    1U,

    // Annotations: the constant-buffer block, its array stride, and one
    // binding/descriptor-set pair per reserved buffer.
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_DECORATE, 3),
    SM86_SPIRV_ID_CB_STRUCT,
    SM86_SPIRV_DECORATION_BLOCK,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_DECORATE, 4),
    SM86_SPIRV_ID_ARRAY,
    SM86_SPIRV_DECORATION_ARRAY_STRIDE,
    4U,
    SM86_SPIRV_UBO_LIST_(SM86_SPIRV_UBO_DECORATIONS_)

    // Types, constants and the constant-buffer variables.
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_TYPE_VOID, 2),
    SM86_SPIRV_ID_VOID,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_TYPE_BOOL, 2),
    SM86_SPIRV_ID_BOOL,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_TYPE_INT, 4),
    SM86_SPIRV_ID_U32,
    32U,
    0U,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_CONSTANT, 4),
    SM86_SPIRV_ID_U32,
    SM86_SPIRV_ID_CONST_ZERO,
    0U,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_CONSTANT, 4),
    SM86_SPIRV_ID_U32,
    SM86_SPIRV_ID_CONST_ONE,
    1U,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_CONSTANT, 4),
    SM86_SPIRV_ID_U32,
    SM86_SPIRV_ID_CONST_256,
    256U,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_CONSTANT_TRUE, 3),
    SM86_SPIRV_ID_BOOL,
    SM86_SPIRV_ID_CONST_TRUE,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_CONSTANT_FALSE, 3),
    SM86_SPIRV_ID_BOOL,
    SM86_SPIRV_ID_CONST_FALSE,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_TYPE_POINTER, 4),
    SM86_SPIRV_ID_PTR_U32,
    SM86_SPIRV_STORAGE_UNIFORM,
    SM86_SPIRV_ID_U32,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_TYPE_ARRAY, 4),
    SM86_SPIRV_ID_ARRAY,
    SM86_SPIRV_ID_U32,
    SM86_SPIRV_ID_CONST_256,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_TYPE_STRUCT, 3),
    SM86_SPIRV_ID_CB_STRUCT,
    SM86_SPIRV_ID_ARRAY,
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_TYPE_POINTER, 4),
    SM86_SPIRV_ID_PTR_CB_STRUCT,
    SM86_SPIRV_STORAGE_UNIFORM,
    SM86_SPIRV_ID_CB_STRUCT,
    SM86_SPIRV_UBO_LIST_(SM86_SPIRV_UBO_VARIABLE_)
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_TYPE_FUNCTION, 3),
    SM86_SPIRV_ID_FUNCTION_TYPE,
    SM86_SPIRV_ID_VOID,

    // OpFunction void %main None %void()
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_FUNCTION, 5),
    SM86_SPIRV_ID_VOID,
    SM86_SPIRV_ID_MAIN,
    0U,
    SM86_SPIRV_ID_FUNCTION_TYPE,
    // OpLabel %main
    SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_LABEL, 2),
    SM86_SPIRV_ID_MAIN_LABEL,
};

_Static_assert(0U == (sizeof(k_sm86_spirv_header) % sizeof(uint32_t)),
               "the SPIR-V header must be a whole number of words");

const uint32_t *
sm86_spirv_header_words(void)
{
    return k_sm86_spirv_header;
}

size_t
sm86_spirv_header_word_count(void)
{
    return sizeof(k_sm86_spirv_header) / sizeof(k_sm86_spirv_header[0]);
}

// -----------------------------------------------------------------------------
// The body emitter
// -----------------------------------------------------------------------------

typedef struct
{
    uint32_t *cursor;                       // Write cursor; advanced with `*cursor++`.
    uint32_t  next_id;                      // Next unallocated SPIR-V result id.
    uint32_t  value_ids[SM86_IR_MAX_VALUES]; // IR value -> SPIR-V id (0 = unset).
    bool      invalid;                      // Set when an operand id was missing.
} sm86_spirv_emitter_t;

/// Rule 7: the single write cursor, advanced by post-increment.
static void
emit_word(sm86_spirv_emitter_t *POUND_RESTRICT emitter, const uint32_t word)
{
    *emitter->cursor++ = word;
}

static uint32_t
emit_new_id(sm86_spirv_emitter_t *POUND_RESTRICT emitter)
{
    return emitter->next_id++;
}

/// The SPIR-V type id for an IR value type.
static uint32_t
spirv_type_of(const uint8_t ir_type)
{
    return (SM86_IR_TYPE_PREDICATE == ir_type) ? SM86_SPIRV_ID_BOOL : SM86_SPIRV_ID_U32;
}

/// Resolves an IR value to its SPIR-V id.
///
/// Records a translation failure instead of returning the unset sentinel `0`,
/// which would otherwise be emitted as a reference to the reserved id 0 and
/// produce a module that fails validation far from its cause.
static uint32_t
operand_id(sm86_spirv_emitter_t *POUND_RESTRICT emitter, const uint32_t value)
{
    if ((SM86_IR_NONE == value) || (value >= SM86_IR_MAX_VALUES) || (0U == emitter->value_ids[value]))
    {
        emitter->invalid = true;
        return 0U;
    }

    return emitter->value_ids[value];
}

static void
bind_result(sm86_spirv_emitter_t *POUND_RESTRICT        emitter,
            const sm86_ir_program_t *POUND_RESTRICT     program,
            const uint32_t                              index,
            const uint32_t                              result_id)
{
    if (SM86_IR_NONE != program->result[index])
    {
        emitter->value_ids[program->result[index]] = result_id;
    }
}

/// Maps an `ISETP` comparison to its SPIR-V opcode, or `0` when unsupported.
static uint32_t
spirv_compare_opcode(const uint8_t cmp_operator, const uint8_t is_signed)
{
    const bool is_signed_compare = (0U != is_signed);

    switch (cmp_operator)
    {
        case 1U: // LT
            return is_signed_compare ? SM86_SPIRV_OP_S_LESS_THAN : SM86_SPIRV_OP_U_LESS_THAN;

        case 2U: // EQ
            return SM86_SPIRV_OP_I_EQUAL;

        case 3U: // LE
            return is_signed_compare ? SM86_SPIRV_OP_S_LESS_THAN_EQUAL : SM86_SPIRV_OP_U_LESS_THAN_EQUAL;

        case 4U: // GT
            return is_signed_compare ? SM86_SPIRV_OP_S_GREATER_THAN : SM86_SPIRV_OP_U_GREATER_THAN;

        case 5U: // NE
            return SM86_SPIRV_OP_I_NOT_EQUAL;

        case 6U: // GE
            return is_signed_compare ? SM86_SPIRV_OP_S_GREATER_THAN_EQUAL
                                     : SM86_SPIRV_OP_U_GREATER_THAN_EQUAL;

        default:
            return 0U;
    }
}

static error_t
emit_instruction(sm86_spirv_emitter_t *POUND_RESTRICT  emitter,
                 const sm86_ir_program_t *POUND_RESTRICT program,
                 const uint32_t                          index)
{
    switch ((sm86_ir_opcode_t)program->opcode[index])
    {
        case SM86_IR_OP_NOP:
        case SM86_IR_OP_BARRIER:
            // Nothing to emit; see spirv.h.
            return POUND_SUCCESS;

        case SM86_IR_OP_CONST_U32:
        {
            const uint32_t result = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_CONSTANT, 4));
            emit_word(emitter, SM86_SPIRV_ID_U32);
            emit_word(emitter, result);
            emit_word(emitter, (uint32_t)program->immediate[index]);
            bind_result(emitter, program, index, result);
            return POUND_SUCCESS;
        }

        case SM86_IR_OP_COPY:
        {
            const uint32_t result = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_COPY_OBJECT, 4));
            emit_word(emitter, SM86_SPIRV_ID_U32);
            emit_word(emitter, result);
            emit_word(emitter, operand_id(emitter, program->source0[index]));
            bind_result(emitter, program, index, result);
            return POUND_SUCCESS;
        }

        case SM86_IR_OP_READ_REG:
        {
            const uint32_t result = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_UNDEF, 3));
            emit_word(emitter, spirv_type_of(program->type[index]));
            emit_word(emitter, result);
            bind_result(emitter, program, index, result);
            return POUND_SUCCESS;
        }

        case SM86_IR_OP_IADD3:
        {
            const uint32_t source0 = operand_id(emitter, program->source0[index]);
            const uint32_t source1 = operand_id(emitter, program->source1[index]);
            const uint32_t source2 = operand_id(emitter, program->source2[index]);

            const uint32_t partial = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_IADD, 5));
            emit_word(emitter, SM86_SPIRV_ID_U32);
            emit_word(emitter, partial);
            emit_word(emitter, source0);
            emit_word(emitter, source1);

            const uint32_t result = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_IADD, 5));
            emit_word(emitter, SM86_SPIRV_ID_U32);
            emit_word(emitter, result);
            emit_word(emitter, partial);
            emit_word(emitter, source2);

            bind_result(emitter, program, index, result);
            return POUND_SUCCESS;
        }

        case SM86_IR_OP_IMAD:
        {
            const uint32_t source0 = operand_id(emitter, program->source0[index]);
            const uint32_t source1 = operand_id(emitter, program->source1[index]);
            const uint32_t source2 = operand_id(emitter, program->source2[index]);

            const uint32_t product = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_IMUL, 5));
            emit_word(emitter, SM86_SPIRV_ID_U32);
            emit_word(emitter, product);
            emit_word(emitter, source0);
            emit_word(emitter, source1);

            const uint32_t result = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_IADD, 5));
            emit_word(emitter, SM86_SPIRV_ID_U32);
            emit_word(emitter, result);
            emit_word(emitter, product);
            emit_word(emitter, source2);

            bind_result(emitter, program, index, result);
            return POUND_SUCCESS;
        }

        case SM86_IR_OP_ISETP:
        {
            const uint32_t compare
                = spirv_compare_opcode(program->isetp_operator[index], program->isetp_signed[index]);

            if (0U == compare)
            {
                POUND_LOG_ERROR(&thread_logger,
                                "ISETP comparison operator %u is not supported.",
                                program->isetp_operator[index]);
                return POUND_ERROR_TRANSLATION_FAILED;
            }

            const uint32_t result = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(compare, 5));
            emit_word(emitter, SM86_SPIRV_ID_BOOL);
            emit_word(emitter, result);
            emit_word(emitter, operand_id(emitter, program->source0[index]));
            emit_word(emitter, operand_id(emitter, program->source1[index]));
            bind_result(emitter, program, index, result);
            return POUND_SUCCESS;
        }

        case SM86_IR_OP_LDC:
        {
            if (program->constant_buffer[index] >= SM86_SPIRV_UBO_BINDING_COUNT)
            {
                POUND_LOG_ERROR(&thread_logger,
                                "LDC references constant buffer %u, but only %u are declared.",
                                program->constant_buffer[index],
                                SM86_SPIRV_UBO_BINDING_COUNT);
                return POUND_ERROR_TRANSLATION_FAILED;
            }

            if ((0 != (program->byte_offset[index] % 4)) || (program->byte_offset[index] < 0))
            {
                POUND_LOG_ERROR(&thread_logger,
                                "LDC byte offset %d is not a multiple of 4.",
                                program->byte_offset[index]);
                return POUND_ERROR_TRANSLATION_FAILED;
            }

            const uint32_t element = (uint32_t)(program->byte_offset[index] / 4);
            const uint32_t array_length = 256U;

            if (element >= array_length)
            {
                POUND_LOG_ERROR(&thread_logger,
                                "LDC byte offset %d is outside the %u-byte constant buffer.",
                                program->byte_offset[index],
                                array_length * 4U);
                return POUND_ERROR_TRANSLATION_FAILED;
            }

            // Index constant, then the access chain into
            // cb[binding].u32[0][element], then the load.
            const uint32_t index_constant = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_CONSTANT, 4));
            emit_word(emitter, SM86_SPIRV_ID_U32);
            emit_word(emitter, index_constant);
            emit_word(emitter, element);

            const uint32_t pointer = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_ACCESS_CHAIN, 6));
            emit_word(emitter, SM86_SPIRV_ID_PTR_U32);
            emit_word(emitter, pointer);
            emit_word(emitter, SM86_SPIRV_ID_UBO_BASE + program->constant_buffer[index]);
            emit_word(emitter, SM86_SPIRV_ID_CONST_ZERO); // struct member 0
            emit_word(emitter, index_constant);           // array element

            const uint32_t result = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_LOAD, 4));
            emit_word(emitter, SM86_SPIRV_ID_U32);
            emit_word(emitter, result);
            emit_word(emitter, pointer);

            bind_result(emitter, program, index, result);
            return POUND_SUCCESS;
        }

        case SM86_IR_OP_SELECT:
        {
            const uint32_t result = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_SELECT, 6));
            emit_word(emitter, spirv_type_of(program->type[index]));
            emit_word(emitter, result);
            emit_word(emitter, operand_id(emitter, program->source0[index]));
            emit_word(emitter, operand_id(emitter, program->source1[index]));
            emit_word(emitter, operand_id(emitter, program->source2[index]));
            bind_result(emitter, program, index, result);
            return POUND_SUCCESS;
        }

        case SM86_IR_OP_NOT:
        {
            const uint32_t result = emit_new_id(emitter);
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_LOGICAL_NOT, 4));
            emit_word(emitter, SM86_SPIRV_ID_BOOL);
            emit_word(emitter, result);
            emit_word(emitter, operand_id(emitter, program->source0[index]));
            bind_result(emitter, program, index, result);
            return POUND_SUCCESS;
        }

        case SM86_IR_OP_EXIT:
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_RETURN, 1));
            return POUND_SUCCESS;

        case SM86_IR_OP_KILL:
            emit_word(emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_KILL, 1));
            return POUND_SUCCESS;

        case SM86_IR_OP_MAX:
        default:
            POUND_LOG_ERROR(&thread_logger, "Unknown IR opcode %u.", program->opcode[index]);
            return POUND_ERROR_TRANSLATION_FAILED;
    }
}

error_t
sm86_spirv_emit(const sm86_ir_program_t *POUND_RESTRICT program,
                uint32_t *POUND_RESTRICT                out_words,
                const size_t                            out_capacity_words,
                size_t *POUND_RESTRICT                  out_word_count)
{
    if ((NULL == program) || (NULL == out_words) || (NULL == out_word_count))
    {
        POUND_LOG_ERROR(&thread_logger, "sm86_spirv_emit requires non-NULL arguments.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (out_capacity_words < SM86_SPIRV_MAX_WORDS)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The output buffer holds %zu words, but a module needs up to %u.",
                        out_capacity_words,
                        (unsigned)SM86_SPIRV_MAX_WORDS);
        return POUND_ERROR_TRANSLATION_FAILED;
    }

    const uint32_t *POUND_RESTRICT header       = sm86_spirv_header_words();
    const size_t                   header_words = sm86_spirv_header_word_count();

    memcpy(out_words, header, header_words * sizeof(uint32_t));

    sm86_spirv_emitter_t emitter = {0};
    emitter.cursor               = out_words + header_words;
    emitter.next_id              = SM86_SPIRV_ID_FIRST_DYNAMIC;

    for (uint32_t i = 0; i < program->count; ++i)
    {
        const error_t status = emit_instruction(&emitter, program, i);

        if (POUND_SUCCESS != status)
        {
            return status;
        }
    }

    if (emitter.invalid)
    {
        POUND_LOG_ERROR(&thread_logger, "An IR value was used before it was defined.");
        return POUND_ERROR_TRANSLATION_FAILED;
    }

    emit_word(&emitter, SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_FUNCTION_END, 1));

    *out_word_count = (size_t)(emitter.cursor - out_words);
    return POUND_SUCCESS;
}

/*** end of file ***/
