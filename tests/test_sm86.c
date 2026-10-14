//! Tests for the SM86 -> SPIR-V translator.
//!
//! The module implements eleven rules (`docs/SM86_TO_SPIRV_RULES.md`), and the
//! cases below are grouped by the stage that owns each one: the decoder cases
//! pin the field extraction the rules depend on, the IR cases cover rules 1-5
//! (Structure of Arrays, def-use, predicates, predication expansion), and the
//! SPIR-V cases cover rules 6-11 (static header, pointer-increment emission, no
//! inline declarations, constant buffers, UGPRs, no broadcasts).
//!
//! Instructions are built field by field rather than read from a captured
//! binary, so a change to the decoder's bit positions is caught here rather than
//! only when a real shader is translated.

#include "pound_test.h"

#include "gpu/sm86/decoder.h"
#include "gpu/sm86/ir.h"
#include "gpu/sm86/spirv.h"
#include "gpu/sm86/translate.h"
#include "log.h"

#include <stddef.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Rule 1 pins the data layout at compile time: the IR stores one array per
// field, so a field's element type is the field's type, not a struct.
// -----------------------------------------------------------------------------

_Static_assert(sizeof(((sm86_ir_program_t *)0)->opcode[0]) == 1,
               "opcode is a byte stream, one per instruction");
_Static_assert(sizeof(((sm86_ir_program_t *)0)->result[0]) == 4,
               "result is an id stream, one per instruction");
_Static_assert(sizeof(((sm86_ir_program_t *)0)->write_register[0]) == 1,
               "write_register is a register stream, one per instruction");

// -----------------------------------------------------------------------------
// Opcodes under test. Each is a 12-bit field; the source-form selector in bits
// 9..11 is supplied separately by the encoders below.
// -----------------------------------------------------------------------------

#define SM86_TEST_MOV     0x002U
#define SM86_TEST_ISETP   0x00CU
#define SM86_TEST_IADD3   0x010U
#define SM86_TEST_IMAD    0x024U
#define SM86_TEST_FADD    0x021U // Supported by the decoder, not by the IR.
#define SM86_TEST_BAR     0x31DU
#define SM86_TEST_WARPSYNC 0x148U
#define SM86_TEST_MEMBAR  0x992U
#define SM86_TEST_EXIT    0x94DU
#define SM86_TEST_KILL    0x95BU
#define SM86_TEST_LDC     0x582U

// -----------------------------------------------------------------------------
// Encoding helpers
// -----------------------------------------------------------------------------

static uint32_t
make_word0(const uint32_t opcode_bits,
           const uint8_t  form,
           const uint8_t  predicate,
           const uint8_t  predicate_not,
           const uint8_t  destination,
           const uint8_t  source0)
{
    return (opcode_bits & 0x0FFFU) | ((uint32_t)(form & 0x07U) << 9)
           | ((uint32_t)(predicate & 0x07U) << 12) | ((uint32_t)(predicate_not & 0x01U) << 15)
           | ((uint32_t)destination << 16) | ((uint32_t)source0 << 24);
}

static sm86_raw_instruction_t
make_instruction(const uint32_t word0, const uint32_t word1, const uint32_t word2)
{
    sm86_raw_instruction_t instruction;
    instruction.low  = (uint64_t)word0 | ((uint64_t)word1 << 32);
    instruction.high = (uint64_t)word2;
    return instruction;
}

/// A register-form ALU instruction: `dst, src0, src1` with `src2` in `word2`'s
/// low byte.
static sm86_raw_instruction_t
make_alu(const uint32_t opcode_bits,
         const uint8_t  destination,
         const uint8_t  source0,
         const uint8_t  source1,
         const uint32_t word2)
{
    const uint32_t word0 = make_word0(opcode_bits, 1U, SM86_PREDICATE_NONE, 0U, destination, source0);
    return make_instruction(word0, (uint32_t)source1, word2);
}

/// An immediate-form ALU instruction: src1 is `immediate`.
static sm86_raw_instruction_t
make_alu_immediate(const uint32_t opcode_bits,
                   const uint8_t  destination,
                   const uint8_t  source0,
                   const uint8_t  source2,
                   const int32_t  immediate)
{
    const uint32_t word0 = make_word0(opcode_bits, 4U, SM86_PREDICATE_NONE, 0U, destination, source0);
    return make_instruction(word0, (uint32_t)immediate, (uint32_t)source2);
}

/// A predicated register-form ALU instruction.
static sm86_raw_instruction_t
make_predicated_alu(const uint32_t opcode_bits,
                    const uint8_t  destination,
                    const uint8_t  source0,
                    const uint8_t  source1,
                    const uint8_t  predicate,
                    const uint8_t  predicate_not)
{
    const uint32_t word0 = make_word0(opcode_bits, 1U, predicate, predicate_not, destination, source0);
    return make_instruction(word0, (uint32_t)source1, 0U);
}

static sm86_raw_instruction_t
make_isetp(const uint8_t predicate_destination,
           const uint8_t source0,
           const uint8_t source1,
           const uint8_t cmp_operator,
           const uint8_t cmp_signed)
{
    const uint32_t word2 = ((uint32_t)predicate_destination << 17)
                           | ((uint32_t)cmp_operator << 12) | ((uint32_t)cmp_signed << 9);
    const uint32_t word0 = make_word0(SM86_TEST_ISETP, 1U, SM86_PREDICATE_NONE, 0U, 0U, source0);
    return make_instruction(word0, (uint32_t)source1, word2);
}

static sm86_raw_instruction_t
make_ldc(const uint8_t destination, const uint8_t binding, const uint16_t byte_offset)
{
    const uint32_t word0 = make_word0(SM86_TEST_LDC, 0U, SM86_PREDICATE_NONE, 0U, destination, 0U);
    const uint32_t word1 = ((uint32_t)binding << 16) | (uint32_t)byte_offset;
    return make_instruction(word0, word1, 0U);
}

// -----------------------------------------------------------------------------
// IR and SPIR-V query helpers
// -----------------------------------------------------------------------------

static uint32_t
find_ir_op(const sm86_ir_program_t *program, const sm86_ir_opcode_t opcode)
{
    for (uint32_t i = 0; i < program->count; ++i)
    {
        if (program->opcode[i] == (uint8_t)opcode)
        {
            return i;
        }
    }

    return SM86_IR_NONE;
}

static uint32_t
count_ir_ops(const sm86_ir_program_t *program, const sm86_ir_opcode_t opcode)
{
    uint32_t count = 0;

    for (uint32_t i = 0; i < program->count; ++i)
    {
        if (program->opcode[i] == (uint8_t)opcode)
        {
            ++count;
        }
    }

    return count;
}

static uint32_t
count_uses(const sm86_ir_program_t *program, const uint32_t value)
{
    uint32_t count = 0;

    for (uint32_t use = program->use_head[value]; SM86_IR_NONE != use; use = program->use_next[use])
    {
        ++count;
    }

    return count;
}

/// A shared output buffer: cases run one at a time, so one buffer is enough and
/// it keeps 34 KiB off every test's stack.
static uint32_t g_spirv_words[SM86_SPIRV_MAX_WORDS];

static size_t
spirv_body_find(const uint32_t *words,
                const size_t    header_words,
                const size_t    total_words,
                const uint32_t  opcode)
{
    size_t i = header_words;

    while (i < total_words)
    {
        const uint32_t word = words[i];
        const uint32_t count = word >> 16;

        if ((word & 0xFFFFU) == opcode)
        {
            return i;
        }

        if (0U == count)
        {
            break; // Malformed; stop rather than walk off the end.
        }

        i += count;
    }

    return SIZE_MAX;
}

static bool
spirv_body_contains_prefix(const uint32_t *words,
                           const size_t    header_words,
                           const size_t    total_words,
                           const uint32_t  first_opcode,
                           const uint32_t  last_opcode)
{
    size_t i = header_words;

    while (i < total_words)
    {
        const uint32_t word  = words[i];
        const uint32_t count = word >> 16;
        const uint32_t op    = word & 0xFFFFU;

        if ((op >= first_opcode) && (op <= last_opcode))
        {
            return true;
        }

        if (0U == count)
        {
            break;
        }

        i += count;
    }

    return false;
}

// -----------------------------------------------------------------------------
// Decoder
// -----------------------------------------------------------------------------

POUND_TEST(sm86, decoder_opcode_name_table)
{
    POUND_CHECK_STR_EQ(sm86_opcode_name(SM86_OPCODE_MOV), "MOV");
    POUND_CHECK_STR_EQ(sm86_opcode_name(SM86_OPCODE_LDC), "LDC");
    POUND_CHECK_STR_EQ(sm86_opcode_name(SM86_OPCODE_IADD3), "IADD3");
    POUND_CHECK_STR_EQ(sm86_opcode_name(SM86_OPCODE_WARPSYNC), "WARPSYNC");
    POUND_CHECK_STR_EQ(sm86_opcode_name((sm86_opcode_t)SM86_OPCODE_MAX_INSTRUCTIONS), "UNKNOWN");
    POUND_CHECK_STR_EQ(sm86_opcode_name((sm86_opcode_t)-1), "UNKNOWN");
}

POUND_TEST(sm86, decoder_form_stripped_opcode_fallback)
{
    // 0x810 is the IADD3 base (0x010) with a nonzero source-form selector. The
    // table names the base encoding, so the fallback is what resolves it.
    const sm86_raw_instruction_t raw = make_instruction(0x810U, 0U, 0U);

    sm86_decoded_instruction_t decoded = {0};
    sm86_decode(&raw, &decoded);

    POUND_CHECK_EQ_U64(decoded.opcode, SM86_OPCODE_IADD3);
    POUND_CHECK_EQ_U64(decoded.unknown_opcode, 0U);
    POUND_CHECK_EQ_U64(decoded.raw_opcode, 0x810U);
}

POUND_TEST(sm86, decoder_unknown_encoding_is_flagged)
{
    // 0x003 names nothing, even after the form bits are masked off. It must not
    // decode to the real NOP at index 0.
    const sm86_raw_instruction_t raw = make_instruction(0x003U, 0U, 0U);

    sm86_decoded_instruction_t decoded = {0};
    sm86_decode(&raw, &decoded);

    POUND_CHECK_EQ_U64(decoded.unknown_opcode, 1U);
    POUND_CHECK_EQ_U64(decoded.raw_opcode, 0x003U);
}

POUND_TEST(sm86, decoder_lcd_keeps_constant_buffer_payload)
{
    const sm86_raw_instruction_t raw = make_ldc(4U, 3U, 24U);

    sm86_decoded_instruction_t decoded = {0};
    sm86_decode(&raw, &decoded);

    POUND_CHECK_EQ_U64(decoded.opcode, SM86_OPCODE_LDC);
    POUND_CHECK_EQ_U64(decoded.payload.constant_buffer.binding_index, 3U);
    POUND_CHECK_EQ_U64(decoded.payload.constant_buffer.byte_offset, 24U);
}

POUND_TEST(sm86, decoder_isetp_fields)
{
    const sm86_raw_instruction_t raw = make_isetp(2U, 1U, 2U, 5U, 1U); // P2 = (R1 != R2), signed

    sm86_decoded_instruction_t decoded = {0};
    sm86_decode(&raw, &decoded);

    POUND_CHECK_EQ_U64(decoded.opcode, SM86_OPCODE_ISETP);
    POUND_CHECK_EQ_U64(decoded.destination_register, 2U);
    POUND_CHECK_EQ_U64(decoded.source0_register, 1U);
    POUND_CHECK_EQ_U64(decoded.source1_register, 2U);
    POUND_CHECK_EQ_U64(decoded.cmp_operator, 5U);
    POUND_CHECK_EQ_U64(decoded.cmp_type, 1U);
}

POUND_TEST(sm86, decoder_general_fields)
{
    const sm86_raw_instruction_t raw = make_alu(SM86_TEST_IADD3, 5U, 6U, 7U, 9U);

    sm86_decoded_instruction_t decoded = {0};
    sm86_decode(&raw, &decoded);

    POUND_CHECK_EQ_U64(decoded.opcode, SM86_OPCODE_IADD3);
    POUND_CHECK_EQ_U64(decoded.form, 1U);
    POUND_CHECK_EQ_U64(decoded.predicate_register, SM86_PREDICATE_NONE);
    POUND_CHECK_EQ_U64(decoded.destination_register, 5U);
    POUND_CHECK_EQ_U64(decoded.source0_register, 6U);
    POUND_CHECK_EQ_U64(decoded.source1_register, 7U);
    POUND_CHECK_EQ_U64(decoded.source2_register, 9U);
}

// -----------------------------------------------------------------------------
// Rule 1 -- Structure of Arrays
// -----------------------------------------------------------------------------

POUND_TEST(sm86, ir_is_structure_of_arrays)
{
    // Two instructions, distinct fields: reading index 0 and index 1 from the
    // per-field arrays must give each instruction's own values.
    const sm86_raw_instruction_t code[2] = {
        make_alu(SM86_TEST_MOV, 3U, 0U, 1U, 0U),
        make_alu(SM86_TEST_MOV, 4U, 0U, 2U, 0U),
    };

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 2U));

    POUND_CHECK_EQ_U64(count_ir_ops(&program, SM86_IR_OP_COPY), 2U);

    const uint32_t first  = find_ir_op(&program, SM86_IR_OP_COPY);
    POUND_REQUIRE(first != SM86_IR_NONE);

    // The second copy is a later index, and the two results are distinct SSA
    // values -- the arrays are indexed by instruction, not interleaved.
    uint32_t second = SM86_IR_NONE;
    for (uint32_t i = first + 1U; i < program.count; ++i)
    {
        if (SM86_IR_OP_COPY == program.opcode[i])
        {
            second = i;
            break;
        }
    }

    POUND_REQUIRE(second != SM86_IR_NONE);
    POUND_CHECK(second > first);
    POUND_CHECK_EQ_U64(program.write_register[first], 3U);
    POUND_CHECK_EQ_U64(program.write_register[second], 4U);
    POUND_CHECK(program.result[first] != program.result[second]);
}

// -----------------------------------------------------------------------------
// Rule 2 -- def-use chains built during Pass 1
// -----------------------------------------------------------------------------

POUND_TEST(sm86, ir_def_use_chains)
{
    // MOV R1, R2 ; IADD3 R3, R1, R1, RZ
    const sm86_raw_instruction_t code[2] = {
        make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 0U),
        make_alu(SM86_TEST_IADD3, 3U, 1U, 1U, 255U), // src2 = RZ
    };

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 2U));

    const uint32_t copy_index  = find_ir_op(&program, SM86_IR_OP_COPY);
    const uint32_t add_index   = find_ir_op(&program, SM86_IR_OP_IADD3);
    POUND_REQUIRE(copy_index != SM86_IR_NONE);
    POUND_REQUIRE(add_index != SM86_IR_NONE);

    const uint32_t copy_value = program.result[copy_index];

    // The copy's result is defined by the copy and used twice, both by the add.
    POUND_CHECK_EQ_U64(program.definition[copy_value], copy_index);
    POUND_CHECK_EQ_U64(count_uses(&program, copy_value), 2U);

    uint32_t slots = 0;
    for (uint32_t use = program.use_head[copy_value]; SM86_IR_NONE != use; use = program.use_next[use])
    {
        POUND_CHECK_EQ_U64(program.use_instruction[use], add_index);
        POUND_CHECK((program.use_slot[use] == SM86_IR_SLOT_SOURCE0) || (program.use_slot[use] == SM86_IR_SLOT_SOURCE1));

        if (SM86_IR_SLOT_SOURCE0 == program.use_slot[use])
        {
            slots |= 1U;
        }

        if (SM86_IR_SLOT_SOURCE1 == program.use_slot[use])
        {
            slots |= 2U;
        }
    }
    POUND_CHECK_EQ_U64(slots, 3U);

    // The add's result is never used; an unused value has an empty chain.
    POUND_CHECK_EQ_U64(count_uses(&program, program.result[add_index]), 0U);
}

// -----------------------------------------------------------------------------
// Rule 3 -- predicates map to a boolean
// -----------------------------------------------------------------------------

POUND_TEST(sm86, ir_predicate_is_boolean)
{
    const sm86_raw_instruction_t code[1] = { make_isetp(0U, 1U, 2U, 5U, 0U) }; // P0 = R1 != R2

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 1U));

    const uint32_t compare = find_ir_op(&program, SM86_IR_OP_ISETP);
    POUND_REQUIRE(compare != SM86_IR_NONE);
    POUND_CHECK_EQ_U64(program.type[compare], SM86_IR_TYPE_PREDICATE);
    POUND_CHECK_EQ_U64(program.write_predicate[compare], 0U);
}

POUND_TEST(sm86, ir_undefined_predicate_is_predicate_typed)
{
    // @P5 with P5 never written: the guard must be a predicate-typed value.
    const sm86_raw_instruction_t code[1] = {
        make_predicated_alu(SM86_TEST_MOV, 1U, 0U, 2U, 5U, 0U),
    };

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 1U));

    bool saw_predicate_read = false;
    for (uint32_t i = 0; i < program.count; ++i)
    {
        if ((SM86_IR_OP_READ_REG == program.opcode[i])
            && (SM86_IR_TYPE_PREDICATE == program.type[i]))
        {
            saw_predicate_read = true;
        }
    }
    POUND_CHECK(saw_predicate_read);
}

// -----------------------------------------------------------------------------
// Rules 4 and 5 -- predication expands in Pass 1, unpredicated first
// -----------------------------------------------------------------------------

POUND_TEST(sm86, ir_predicated_expansion_orders_compute_before_select)
{
    // ISETP.NE.U32 P0, R1, R2 ; @!P0 IADD3 R3, R4, R5, RZ
    const sm86_raw_instruction_t code[2] = {
        make_isetp(0U, 1U, 2U, 5U, 0U),
        make_predicated_alu(SM86_TEST_IADD3, 3U, 4U, 5U, 0U, 1U), // @!P0
    };

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 2U));

    const uint32_t add_index    = find_ir_op(&program, SM86_IR_OP_IADD3);
    const uint32_t not_index    = find_ir_op(&program, SM86_IR_OP_NOT);
    const uint32_t select_index = find_ir_op(&program, SM86_IR_OP_SELECT);

    POUND_REQUIRE(add_index != SM86_IR_NONE);
    POUND_REQUIRE(not_index != SM86_IR_NONE);
    POUND_REQUIRE(select_index != SM86_IR_NONE);

    // The unpredicated compute and the inverted guard both precede the select.
    POUND_CHECK(add_index < select_index);
    POUND_CHECK(not_index < select_index);

    // Only the committing select carries the architectural write.
    POUND_CHECK_EQ_U64(program.write_register[add_index], SM86_IR_NO_REGISTER);
    POUND_CHECK_EQ_U64(program.write_register[select_index], 3U);

    // The select commits `guard ? computed : previous`.
    POUND_CHECK_EQ_U64(program.source0[select_index], program.result[not_index]);
    POUND_CHECK_EQ_U64(program.source1[select_index], program.result[add_index]);
    POUND_CHECK(program.source2[select_index] != SM86_IR_NONE);
}

POUND_TEST(sm86, ir_predicated_instruction_without_not)
{
    // @P0 MOV R1, R2 (non-inverted guard) still produces a select, but no NOT.
    const sm86_raw_instruction_t code[2] = {
        make_isetp(0U, 1U, 2U, 5U, 0U),
        make_predicated_alu(SM86_TEST_MOV, 1U, 0U, 2U, 0U, 0U),
    };

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 2U));

    POUND_CHECK_EQ_U64(count_ir_ops(&program, SM86_IR_OP_NOT), 0U);
    POUND_CHECK_EQ_U64(count_ir_ops(&program, SM86_IR_OP_SELECT), 1U);

    const uint32_t compare = find_ir_op(&program, SM86_IR_OP_ISETP);
    const uint32_t select  = find_ir_op(&program, SM86_IR_OP_SELECT);
    POUND_REQUIRE(compare != SM86_IR_NONE);
    POUND_REQUIRE(select != SM86_IR_NONE);
    POUND_CHECK_EQ_U64(program.source0[select], program.result[compare]);
}

// -----------------------------------------------------------------------------
// Undefined reads and unsupported input
// -----------------------------------------------------------------------------

POUND_TEST(sm86, ir_unwritten_register_reads_as_undef)
{
    const sm86_raw_instruction_t code[1] = { make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 0U) };

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 1U));

    const uint32_t read = find_ir_op(&program, SM86_IR_OP_READ_REG);
    POUND_REQUIRE(read != SM86_IR_NONE);

    const uint32_t copy = find_ir_op(&program, SM86_IR_OP_COPY);
    POUND_REQUIRE(copy != SM86_IR_NONE);
    POUND_CHECK_EQ_U64(program.source0[copy], program.result[read]);

    size_t words = 0;
    POUND_REQUIRE(POUND_SUCCESS
                  == sm86_spirv_emit(&program, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words));
    POUND_CHECK(spirv_body_find(g_spirv_words, sm86_spirv_header_word_count(), words, SM86_SPIRV_OP_UNDEF)
                != SIZE_MAX);
}

POUND_TEST(sm86, ir_rejects_unsupported_instruction)
{
    const sm86_raw_instruction_t code[1] = { make_alu(SM86_TEST_FADD, 1U, 2U, 3U, 0U) };

    sm86_ir_program_t program;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(sm86_ir_build(&program, code, 1U), POUND_ERROR_UNSUPPORTED_INSTRUCTION);
    POUND_CHECK_EQ_U64(program.count, 0U);
    POUND_CHECK_EQ_U64(program.value_count, 0U);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
    POUND_CHECK(pound_test_log_contains("FADD"));
}

POUND_TEST(sm86, ir_rejects_unknown_encoding)
{
    const sm86_raw_instruction_t code[1] = { make_instruction(0x003U, 0U, 0U) };

    sm86_ir_program_t program;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(sm86_ir_build(&program, code, 1U), POUND_ERROR_UNSUPPORTED_INSTRUCTION);
    POUND_CHECK_EQ_U64(program.count, 0U);
    POUND_CHECK(pound_test_log_contains("0x003"));
}

POUND_TEST(sm86, ir_rejects_null_arguments)
{
    sm86_ir_program_t   program;
    sm86_raw_instruction_t code[1] = { make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 0U) };

    POUND_CHECK_EQ_U64(sm86_ir_build(NULL, code, 1U), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(sm86_ir_build(&program, NULL, 1U), POUND_ERROR_INVALID_ARGUMENT);
}

POUND_TEST(sm86, ir_refuses_block_over_capacity)
{
    // Each `MOV Rd, RZ` expands to a constant plus a copy; a block long enough
    // to exceed the fixed IR arrays must be refused, not truncated.
    static sm86_raw_instruction_t code[SM86_IR_MAX_INSTRUCTIONS];

    for (uint32_t i = 0; i < SM86_IR_MAX_INSTRUCTIONS; ++i)
    {
        code[i] = make_alu(SM86_TEST_MOV, (uint8_t)(i % 256U), 0U, 255U, 0U); // RZ source
    }

    sm86_ir_program_t program;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(sm86_ir_build(&program, code, SM86_IR_MAX_INSTRUCTIONS),
                       POUND_ERROR_TRANSLATION_FAILED);
    POUND_CHECK_EQ_U64(program.count, 0U);
}

POUND_TEST(sm86, ir_control_flow_and_barriers_are_accepted)
{
    const sm86_raw_instruction_t code[5] = {
        make_instruction(SM86_TEST_BAR, 0U, 0U),
        make_instruction(SM86_TEST_WARPSYNC, 0U, 0U),
        make_instruction(SM86_TEST_MEMBAR, 0U, 0U),
        make_instruction(SM86_TEST_EXIT, 0U, 0U),
        make_instruction(SM86_TEST_KILL, 0U, 0U),
    };

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 5U));

    POUND_CHECK_EQ_U64(count_ir_ops(&program, SM86_IR_OP_BARRIER), 3U);
    POUND_CHECK_EQ_U64(count_ir_ops(&program, SM86_IR_OP_EXIT), 1U);
    POUND_CHECK_EQ_U64(count_ir_ops(&program, SM86_IR_OP_KILL), 1U);
}

// -----------------------------------------------------------------------------
// Rule 6 -- a pre-generated static header
// -----------------------------------------------------------------------------

POUND_TEST(sm86, spirv_header_is_static_and_well_formed)
{
    const uint32_t *header       = sm86_spirv_header_words();
    const size_t    header_words = sm86_spirv_header_word_count();

    POUND_REQUIRE_PTR_NON_NULL(header);
    POUND_CHECK(header_words > 0U);

    // The same array, every call, with the SPIR-V magic first.
    POUND_CHECK_PTR_EQ(header, sm86_spirv_header_words());
    POUND_CHECK_EQ_U64(header_words, sm86_spirv_header_word_count());
    POUND_CHECK_EQ_U64(header[0], 0x07230203U);

    // Walking the header by instruction word counts must land exactly on its
    // end, which is what proves every declared count is right. The first five
    // words are the module header (magic, version, generator, bound, schema),
    // not an instruction, so the walk starts after them.
    size_t i = 5;
    while (i < header_words)
    {
        const uint32_t count = header[i] >> 16;
        POUND_REQUIRE_MSG(count > 0U, "header instruction at word %zu has a zero word count", i);
        i += count;
    }
    POUND_CHECK_EQ_U64(i, header_words);
}

POUND_TEST(sm86, spirv_header_declares_sixteen_uniform_blocks)
{
    const uint32_t *header       = sm86_spirv_header_words();
    const size_t    header_words = sm86_spirv_header_word_count();

    uint32_t variables = 0;
    size_t   i         = 5; // Skip the five-word module header.
    while (i + 3U < header_words)
    {
        const uint32_t count = header[i] >> 16;

        if (((header[i] & 0xFFFFU) == SM86_SPIRV_OP_VARIABLE)
            && (header[i + 3U] == SM86_SPIRV_STORAGE_UNIFORM))
        {
            ++variables;
        }

        POUND_REQUIRE(count > 0U);
        i += count;
    }

    POUND_CHECK_EQ_U64(variables, SM86_SPIRV_UBO_BINDING_COUNT);
}

POUND_TEST(sm86, spirv_two_programs_share_the_same_header)
{
    const sm86_raw_instruction_t code_a[1] = { make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 0U) };
    const sm86_raw_instruction_t code_b[1] = { make_ldc(1U, 0U, 4U) };

    const size_t header_words = sm86_spirv_header_word_count();

    size_t words_a = 0;
    size_t words_b = 0;
    POUND_REQUIRE(POUND_SUCCESS == sm86_translate(code_a, 1U, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words_a, NULL));

    static uint32_t words_b_buffer[SM86_SPIRV_MAX_WORDS];
    POUND_REQUIRE(POUND_SUCCESS == sm86_translate(code_b, 1U, words_b_buffer, SM86_SPIRV_MAX_WORDS, &words_b, NULL));

    POUND_CHECK(words_a > header_words);
    POUND_CHECK(words_b > header_words);
    POUND_CHECK_EQ_U64(memcmp(g_spirv_words, words_b_buffer, header_words * sizeof(uint32_t)), 0U);
}

// -----------------------------------------------------------------------------
// Rule 7 -- pointer-increment emission
// -----------------------------------------------------------------------------

POUND_TEST(sm86, spirv_emission_is_contiguous_and_terminated)
{
    const sm86_raw_instruction_t code[3] = {
        make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 0U),
        make_isetp(0U, 1U, 2U, 5U, 0U),
        make_instruction(SM86_TEST_EXIT, 0U, 0U),
    };

    size_t words = 0;
    POUND_REQUIRE(POUND_SUCCESS == sm86_translate(code, 3U, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words, NULL));

    POUND_CHECK(words > sm86_spirv_header_word_count());
    POUND_CHECK(words <= SM86_SPIRV_MAX_WORDS);

    // The final word is OpFunctionEnd: the cursor stopped exactly at the end of
    // the module, with no trailing padding.
    POUND_CHECK_EQ_U64(g_spirv_words[words - 1U], SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_FUNCTION_END, 1));
}

POUND_TEST(sm86, spirv_rejects_short_output_buffer)
{
    const sm86_raw_instruction_t code[1] = { make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 0U) };

    uint32_t small[64];
    size_t   words = 0;
    POUND_CHECK_EQ_U64(sm86_translate(code, 1U, small, 64U, &words, NULL), POUND_ERROR_TRANSLATION_FAILED);
}

// -----------------------------------------------------------------------------
// Rule 8 -- no inline forward declarations; barriers emit nothing
// -----------------------------------------------------------------------------

POUND_TEST(sm86, spirv_body_has_no_type_or_global_declarations)
{
    const sm86_raw_instruction_t code[3] = {
        make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 0U),
        make_ldc(2U, 3U, 16U),
        make_instruction(SM86_TEST_EXIT, 0U, 0U),
    };

    size_t words = 0;
    POUND_REQUIRE(POUND_SUCCESS == sm86_translate(code, 3U, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words, NULL));

    const size_t header_words = sm86_spirv_header_word_count();

    // No OpType* (19..33), OpFunction (54), OpVariable (59) or OpDecorate (71)
    // may appear in the body: those all come from the header.
    POUND_CHECK(!spirv_body_contains_prefix(g_spirv_words, header_words, words, 19U, 33U));
    POUND_CHECK_EQ_U64(spirv_body_find(g_spirv_words, header_words, words, SM86_SPIRV_OP_FUNCTION), SIZE_MAX);
    POUND_CHECK_EQ_U64(spirv_body_find(g_spirv_words, header_words, words, SM86_SPIRV_OP_VARIABLE), SIZE_MAX);
    POUND_CHECK_EQ_U64(spirv_body_find(g_spirv_words, header_words, words, SM86_SPIRV_OP_DECORATE), SIZE_MAX);
}

POUND_TEST(sm86, spirv_barriers_emit_no_words)
{
    const sm86_raw_instruction_t code[3] = {
        make_instruction(SM86_TEST_BAR, 0U, 0U),
        make_instruction(SM86_TEST_WARPSYNC, 0U, 0U),
        make_instruction(SM86_TEST_MEMBAR, 0U, 0U),
    };

    sm86_translation_stats_t stats = {0};
    size_t                   words = 0;
    POUND_REQUIRE(POUND_SUCCESS == sm86_translate(code, 3U, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words, &stats));

    POUND_CHECK_EQ_U64(stats.ir_instruction_count, 3U);
    // Nothing but the header and the closing OpFunctionEnd.
    POUND_CHECK_EQ_U64(words, sm86_spirv_header_word_count() + 1U);
}

// -----------------------------------------------------------------------------
// Rule 9 -- constant buffers map to Uniform blocks
// -----------------------------------------------------------------------------

POUND_TEST(sm86, spirv_lcd_reads_a_uniform_block)
{
    // LDC R1, c[2][8] -> cb[2].u32[2]
    const sm86_raw_instruction_t code[1] = { make_ldc(1U, 2U, 8U) };

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 1U));

    const uint32_t load = find_ir_op(&program, SM86_IR_OP_LDC);
    POUND_REQUIRE(load != SM86_IR_NONE);
    POUND_CHECK_EQ_U64(program.constant_buffer[load], 2U);
    POUND_CHECK_EQ_U64(program.byte_offset[load], 8U);

    size_t words = 0;
    POUND_REQUIRE(POUND_SUCCESS == sm86_spirv_emit(&program, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words));

    const size_t header_words = sm86_spirv_header_word_count();

    const size_t access = spirv_body_find(g_spirv_words, header_words, words, SM86_SPIRV_OP_ACCESS_CHAIN);
    POUND_REQUIRE(access != SIZE_MAX);
    POUND_CHECK_EQ_U64(spirv_body_find(g_spirv_words, header_words, words, SM86_SPIRV_OP_LOAD) != SIZE_MAX, 1U);

    // OpAccessChain: [word, resultType, resultId, base, index0, index1]. The
    // base must be the uniform variable for binding 2 (id 13 + 2).
    POUND_CHECK_EQ_U64(g_spirv_words[access + 3U], 13U + 2U);
    POUND_CHECK_EQ_U64(g_spirv_words[access + 4U], 4U); // struct member 0
}

POUND_TEST(sm86, spirv_lcd_rejects_misaligned_offset)
{
    const sm86_raw_instruction_t code[1] = { make_ldc(1U, 0U, 6U) }; // not a multiple of 4

    size_t words = 0;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(sm86_translate(code, 1U, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words, NULL),
                       POUND_ERROR_TRANSLATION_FAILED);
    POUND_CHECK(pound_test_log_count_at(LOG_LEVEL_ERROR) >= 1U);
}

// -----------------------------------------------------------------------------
// Rules 10 and 11 -- UGPRs are thread-local; no subgroup broadcasts
// -----------------------------------------------------------------------------

POUND_TEST(sm86, spirv_uniform_and_non_uniform_translate_identically)
{
    // The uniform flag (word2 bit 27) must not change the emitted module.
    const sm86_raw_instruction_t plain[1]   = { make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 0U) };
    const sm86_raw_instruction_t uniform[1] = { make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 1U << 27) };

    size_t words_plain = 0;
    size_t words_uniform = 0;

    POUND_REQUIRE(POUND_SUCCESS == sm86_translate(plain, 1U, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words_plain, NULL));

    static uint32_t uniform_words[SM86_SPIRV_MAX_WORDS];
    POUND_REQUIRE(POUND_SUCCESS
                  == sm86_translate(uniform, 1U, uniform_words, SM86_SPIRV_MAX_WORDS, &words_uniform, NULL));

    POUND_CHECK_EQ_U64(words_plain, words_uniform);
    POUND_CHECK_EQ_U64(memcmp(g_spirv_words, uniform_words, words_plain * sizeof(uint32_t)), 0U);
}

POUND_TEST(sm86, spirv_never_emits_subgroup_operations)
{
    const sm86_raw_instruction_t code[2] = {
        make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 1U << 27), // uniform operand
        make_alu(SM86_TEST_IADD3, 3U, 1U, 2U, 255U),
    };

    size_t words = 0;
    POUND_REQUIRE(POUND_SUCCESS == sm86_translate(code, 2U, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words, NULL));

    // OpGroup* / OpGroupNonUniform* occupy 259..~450. None may appear.
    POUND_CHECK(!spirv_body_contains_prefix(g_spirv_words, sm86_spirv_header_word_count(), words, 259U, 500U));
}

// -----------------------------------------------------------------------------
// Combined integer ALU and immediate operands
// -----------------------------------------------------------------------------

POUND_TEST(sm86, ir_imad_uses_three_operands)
{
    const sm86_raw_instruction_t code[1] = { make_alu(SM86_TEST_IMAD, 3U, 1U, 2U, 255U) };

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 1U));

    const uint32_t imad = find_ir_op(&program, SM86_IR_OP_IMAD);
    POUND_REQUIRE(imad != SM86_IR_NONE);
    POUND_CHECK(program.source0[imad] != SM86_IR_NONE);
    POUND_CHECK(program.source1[imad] != SM86_IR_NONE);
    POUND_CHECK(program.source2[imad] != SM86_IR_NONE);

    size_t words = 0;
    POUND_REQUIRE(POUND_SUCCESS == sm86_spirv_emit(&program, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words));
    POUND_CHECK(spirv_body_find(g_spirv_words, sm86_spirv_header_word_count(), words, SM86_SPIRV_OP_IMUL) != SIZE_MAX);
}

POUND_TEST(sm86, ir_immediate_operand_becomes_a_constant)
{
    const sm86_raw_instruction_t code[1] = { make_alu_immediate(SM86_TEST_IADD3, 3U, 1U, 2U, 42) };

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 1U));

    const uint32_t constant = find_ir_op(&program, SM86_IR_OP_CONST_U32);
    POUND_REQUIRE(constant != SM86_IR_NONE);

    bool saw_42 = false;
    for (uint32_t i = 0; i < program.count; ++i)
    {
        if ((SM86_IR_OP_CONST_U32 == program.opcode[i]) && (42 == program.immediate[i]))
        {
            saw_42 = true;
        }
    }
    POUND_CHECK(saw_42);
}

POUND_TEST(sm86, ir_signed_compare_selects_signed_opcode)
{
    const sm86_raw_instruction_t code[1] = { make_isetp(0U, 1U, 2U, 1U, 1U) }; // .LT.S32

    sm86_ir_program_t program;
    POUND_REQUIRE(POUND_SUCCESS == sm86_ir_build(&program, code, 1U));

    const uint32_t compare = find_ir_op(&program, SM86_IR_OP_ISETP);
    POUND_REQUIRE(compare != SM86_IR_NONE);
    POUND_CHECK_EQ_U64(program.isetp_operator[compare], 1U);
    POUND_CHECK_EQ_U64(program.isetp_signed[compare], 1U);

    size_t words = 0;
    POUND_REQUIRE(POUND_SUCCESS == sm86_spirv_emit(&program, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words));
    POUND_CHECK(spirv_body_find(g_spirv_words, sm86_spirv_header_word_count(), words,
                                SM86_SPIRV_OP_S_LESS_THAN)
                != SIZE_MAX);
}

// -----------------------------------------------------------------------------
// The combined entry point
// -----------------------------------------------------------------------------

POUND_TEST(sm86, translate_end_to_end)
{
    const sm86_raw_instruction_t code[5] = {
        make_alu(SM86_TEST_MOV, 1U, 0U, 2U, 0U),
        make_alu(SM86_TEST_IADD3, 3U, 1U, 1U, 255U),
        make_isetp(0U, 3U, 1U, 2U, 0U),
        make_ldc(4U, 0U, 0U),
        make_instruction(SM86_TEST_EXIT, 0U, 0U),
    };

    sm86_translation_stats_t stats = {0};
    size_t                   words = 0;
    POUND_REQUIRE(POUND_SUCCESS == sm86_translate(code, 5U, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words, &stats));

    POUND_CHECK_EQ_U64(stats.instruction_count, 5U);
    POUND_CHECK(stats.ir_instruction_count > 0U);
    POUND_CHECK(stats.ir_value_count > 0U);
    POUND_CHECK(stats.ir_use_count > 0U);
    POUND_CHECK_EQ_U64(stats.spirv_word_count, words);
    POUND_CHECK_EQ_U64(g_spirv_words[words - 1U], SM86_SPIRV_INSTRUCTION(SM86_SPIRV_OP_FUNCTION_END, 1));
}

POUND_TEST(sm86, translate_rejects_bad_arguments)
{
    const sm86_raw_instruction_t code[1] = { make_instruction(SM86_TEST_EXIT, 0U, 0U) };
    size_t                       words   = 0;

    POUND_CHECK_EQ_U64(sm86_translate(NULL, 1U, g_spirv_words, SM86_SPIRV_MAX_WORDS, &words, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(sm86_translate(code, 1U, NULL, SM86_SPIRV_MAX_WORDS, &words, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(sm86_translate(code, 1U, g_spirv_words, SM86_SPIRV_MAX_WORDS, NULL, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);
}

// -----------------------------------------------------------------------------

POUND_TEST_SUITE(sm86,
                 POUND_TEST_CASE(sm86, decoder_opcode_name_table),
                 POUND_TEST_CASE(sm86, decoder_form_stripped_opcode_fallback),
                 POUND_TEST_CASE(sm86, decoder_unknown_encoding_is_flagged),
                 POUND_TEST_CASE(sm86, decoder_lcd_keeps_constant_buffer_payload),
                 POUND_TEST_CASE(sm86, decoder_isetp_fields),
                 POUND_TEST_CASE(sm86, decoder_general_fields),
                 POUND_TEST_CASE(sm86, ir_is_structure_of_arrays),
                 POUND_TEST_CASE(sm86, ir_def_use_chains),
                 POUND_TEST_CASE(sm86, ir_predicate_is_boolean),
                 POUND_TEST_CASE(sm86, ir_undefined_predicate_is_predicate_typed),
                 POUND_TEST_CASE(sm86, ir_predicated_expansion_orders_compute_before_select),
                 POUND_TEST_CASE(sm86, ir_predicated_instruction_without_not),
                 POUND_TEST_CASE(sm86, ir_unwritten_register_reads_as_undef),
                 POUND_TEST_CASE(sm86, ir_rejects_unsupported_instruction),
                 POUND_TEST_CASE(sm86, ir_rejects_unknown_encoding),
                 POUND_TEST_CASE(sm86, ir_rejects_null_arguments),
                 POUND_TEST_CASE(sm86, ir_refuses_block_over_capacity),
                 POUND_TEST_CASE(sm86, ir_control_flow_and_barriers_are_accepted),
                 POUND_TEST_CASE(sm86, spirv_header_is_static_and_well_formed),
                 POUND_TEST_CASE(sm86, spirv_header_declares_sixteen_uniform_blocks),
                 POUND_TEST_CASE(sm86, spirv_two_programs_share_the_same_header),
                 POUND_TEST_CASE(sm86, spirv_emission_is_contiguous_and_terminated),
                 POUND_TEST_CASE(sm86, spirv_rejects_short_output_buffer),
                 POUND_TEST_CASE(sm86, spirv_body_has_no_type_or_global_declarations),
                 POUND_TEST_CASE(sm86, spirv_barriers_emit_no_words),
                 POUND_TEST_CASE(sm86, spirv_lcd_reads_a_uniform_block),
                 POUND_TEST_CASE(sm86, spirv_lcd_rejects_misaligned_offset),
                 POUND_TEST_CASE(sm86, spirv_uniform_and_non_uniform_translate_identically),
                 POUND_TEST_CASE(sm86, spirv_never_emits_subgroup_operations),
                 POUND_TEST_CASE(sm86, ir_imad_uses_three_operands),
                 POUND_TEST_CASE(sm86, ir_immediate_operand_becomes_a_constant),
                 POUND_TEST_CASE(sm86, ir_signed_compare_selects_signed_opcode),
                 POUND_TEST_CASE(sm86, translate_end_to_end),
                 POUND_TEST_CASE(sm86, translate_rejects_bad_arguments))

/*** end of file ***/
