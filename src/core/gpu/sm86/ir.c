#include "gpu/sm86/ir.h"

#include "log.h"
#include <string.h>

/// The register file a straight-line block is translated against.
///
/// This is the SSA renaming table, not architectural state: entry `gpr[r]` is
/// the value id that register `r` currently holds, or `SM86_IR_NONE` before it
/// has been written. It lives only for the duration of `sm86_ir_build`, which
/// is what keeps the IR itself free of register variables (so every value has a
/// single definition in the emitted SPIR-V).
typedef struct
{
    uint32_t gpr[256];
    uint32_t predicate[8];
} sm86_register_map_t;

typedef struct
{
    sm86_ir_program_t     *program;
    sm86_register_map_t    registers;
    uint32_t               constant_zero; // Cached `OpConstant 0`, or SM86_IR_NONE
    uint32_t               current_instruction;
} sm86_ir_builder_t;

/// The SM86 zero register. Reads of it are the constant `0`, never a `READ_REG`
/// undef: `RZ` is architecturally defined to read as zero.
#define SM86_ZERO_REGISTER 255U

/// Upper bound on the IR ops one decoded instruction may append. Used to refuse
/// a block that could not fit the fixed arrays *before* a store runs off the
/// end, so a capacity failure is reported as such rather than as corruption.
#define SM86_IR_MAX_OPS_PER_INSTRUCTION 8U
#define SM86_IR_MAX_VALUES_PER_INSTRUCTION 8U
#define SM86_IR_MAX_USES_PER_INSTRUCTION 24U

void
sm86_ir_init(sm86_ir_program_t *POUND_RESTRICT program)
{
    if (NULL == program)
    {
        return;
    }

    memset(program, 0, sizeof(*program));
}

static uint32_t
ir_append(sm86_ir_builder_t *POUND_RESTRICT builder,
          const sm86_ir_opcode_t               opcode,
          const sm86_ir_value_type_t           type)
{
    sm86_ir_program_t *POUND_RESTRICT program = builder->program;
    const uint32_t                    index   = program->count++;

    program->opcode[index]              = (uint8_t)opcode;
    program->type[index]                = (uint8_t)type;
    program->result[index]              = SM86_IR_NONE;
    program->source0[index]             = SM86_IR_NONE;
    program->source1[index]             = SM86_IR_NONE;
    program->source2[index]             = SM86_IR_NONE;
    program->immediate[index]           = 0;
    program->has_immediate[index]       = 0;
    program->write_register[index]      = SM86_IR_NO_REGISTER;
    program->write_predicate[index]     = SM86_IR_NO_REGISTER;
    program->isetp_operator[index]      = 0;
    program->isetp_signed[index]        = 0;
    program->constant_buffer[index]     = 0;
    program->byte_offset[index]         = 0;
    program->source_instruction[index]  = builder->current_instruction;

    return index;
}

static uint32_t
ir_new_value(sm86_ir_builder_t *POUND_RESTRICT builder)
{
    sm86_ir_program_t *POUND_RESTRICT program = builder->program;
    const uint32_t                    value   = program->value_count++;

    program->definition[value] = SM86_IR_NONE;
    program->use_head[value]   = SM86_IR_NONE;

    return value;
}

static void
ir_define(sm86_ir_builder_t *POUND_RESTRICT builder,
          const uint32_t                       instruction_index,
          const uint32_t                       value)
{
    builder->program->result[instruction_index]     = value;
    builder->program->definition[value]             = instruction_index;
}

static void
ir_record_use(sm86_ir_builder_t *POUND_RESTRICT builder,
              const uint32_t                       value,
              const uint32_t                       instruction_index,
              const uint8_t                        slot)
{
    if (SM86_IR_NONE == value)
    {
        return;
    }

    sm86_ir_program_t *POUND_RESTRICT program = builder->program;
    const uint32_t                    use     = program->use_count++;

    program->use_next[use]        = program->use_head[value];
    program->use_instruction[use] = instruction_index;
    program->use_slot[use]        = slot;
    program->use_head[value]      = use;
}

/// Sets a source operand and records the matching def-use edge.
static void
ir_set_source(sm86_ir_builder_t *POUND_RESTRICT builder,
              const uint32_t                       instruction_index,
              const sm86_ir_slot_t                 slot,
              const uint32_t                       value)
{
    sm86_ir_program_t *POUND_RESTRICT program = builder->program;

    switch (slot)
    {
        case SM86_IR_SLOT_SOURCE0:
            program->source0[instruction_index] = value;
            break;

        case SM86_IR_SLOT_SOURCE1:
            program->source1[instruction_index] = value;
            break;

        case SM86_IR_SLOT_SOURCE2:
            program->source2[instruction_index] = value;
            break;

        case SM86_IR_SLOT_PREDICATE:
        default:
            // The predicate of a SELECT is carried in `source0`; the slot is
            // still recorded so a consumer can tell an operand use from a
            // condition use without re-deriving it from the opcode.
            program->source0[instruction_index] = value;
            break;
    }

    ir_record_use(builder, value, instruction_index, (uint8_t)slot);
}

/// Appends `result = immediate` and returns the new value.
static uint32_t
ir_emit_constant(sm86_ir_builder_t *POUND_RESTRICT builder, const uint32_t bits)
{
    const uint32_t index = ir_append(builder, SM86_IR_OP_CONST_U32, SM86_IR_TYPE_U32);
    const uint32_t value = ir_new_value(builder);

    builder->program->immediate[index]     = (int32_t)bits;
    builder->program->has_immediate[index] = 1;
    ir_define(builder, index, value);

    return value;
}

/// The value of the constant `0`, materialised at most once per block.
static uint32_t
ir_constant_zero(sm86_ir_builder_t *POUND_RESTRICT builder)
{
    if (SM86_IR_NONE == builder->constant_zero)
    {
        builder->constant_zero = ir_emit_constant(builder, 0U);
    }

    return builder->constant_zero;
}

/// Reads the current value of a GPR.
///
/// A register never written in this block (and not `RZ`) becomes
/// `SM86_IR_OP_READ_REG`, which lowers to `OpUndef`. The read is cached in the
/// register map, so a second read yields a second `OpUndef` only if it happens
/// before the first is used -- which cannot happen, since the map is updated
/// here.
static uint32_t
ir_read_gpr(sm86_ir_builder_t *POUND_RESTRICT builder, const uint8_t register_index)
{
    if (SM86_ZERO_REGISTER == register_index)
    {
        return ir_constant_zero(builder);
    }

    if (SM86_IR_NONE != builder->registers.gpr[register_index])
    {
        return builder->registers.gpr[register_index];
    }

    const uint32_t index = ir_append(builder, SM86_IR_OP_READ_REG, SM86_IR_TYPE_U32);
    const uint32_t value = ir_new_value(builder);

    ir_define(builder, index, value);
    builder->registers.gpr[register_index] = value;

    return value;
}

/// Reads the current value of a predicate register, which is a boolean.
static uint32_t
ir_read_predicate(sm86_ir_builder_t *POUND_RESTRICT builder, const uint8_t register_index)
{
    if (SM86_IR_NONE != builder->registers.predicate[register_index])
    {
        return builder->registers.predicate[register_index];
    }

    const uint32_t index = ir_append(builder, SM86_IR_OP_READ_REG, SM86_IR_TYPE_PREDICATE);
    const uint32_t value = ir_new_value(builder);

    ir_define(builder, index, value);
    builder->registers.predicate[register_index] = value;

    return value;
}

/// Materialises the instruction's src1 operand, which is a register or an
/// immediate depending on the source form.
///
/// A constant-buffer operand (`form == 5`) is refused here: the only instruction
/// that reads a constant buffer in the supported set is `LDC`, which reads its
/// buffer directly rather than through `src1`, and accepting an `LDC`-style
/// operand on an ALU op would silently mistranslate it.
static error_t
ir_operand1(sm86_ir_builder_t *POUND_RESTRICT               builder,
            const sm86_decoded_instruction_t *POUND_RESTRICT instruction,
            uint32_t *POUND_RESTRICT                         out_value)
{
    switch (instruction->form)
    {
        case 4: // Imm32
            *out_value = ir_emit_constant(builder, (uint32_t)instruction->payload.immediate_value);
            return POUND_SUCCESS;

        case 5: // CBuf
            POUND_LOG_ERROR(&thread_logger,
                            "A constant-buffer operand on %s is not supported; only LDC reads "
                            "a constant buffer.",
                            sm86_opcode_name((sm86_opcode_t)instruction->opcode));
            return POUND_ERROR_UNSUPPORTED_INSTRUCTION;

        default:
            // Forms 1 and 6 are GPR and UGPR; rule 10 reads a UGPR exactly like
            // a GPR, so both land on the same register map.
            *out_value = ir_read_gpr(builder, instruction->source1_register);
            return POUND_SUCCESS;
    }
}

/// The predicate guarding `instruction`, already inverted for `@!Pn`, or
/// `SM86_IR_NONE` when the instruction is unpredicated.
static uint32_t
ir_guard_value(sm86_ir_builder_t *POUND_RESTRICT               builder,
               const sm86_decoded_instruction_t *POUND_RESTRICT instruction)
{
    if (SM86_PREDICATE_NONE == instruction->predicate_register)
    {
        return SM86_IR_NONE;
    }

    uint32_t predicate = ir_read_predicate(builder, instruction->predicate_register);

    if (0 != instruction->predicate_not)
    {
        // Rule 4: the inverted guard is its own boolean value, so the SELECT
        // below always reads "execute when true".
        const uint32_t index = ir_append(builder, SM86_IR_OP_NOT, SM86_IR_TYPE_PREDICATE);

        ir_set_source(builder, index, SM86_IR_SLOT_SOURCE0, predicate);

        const uint32_t inverted = ir_new_value(builder);
        ir_define(builder, index, inverted);
        predicate = inverted;
    }

    return predicate;
}

/// Commits `value` to the instruction's destination, expanding a predicated
/// instruction exactly as rules 4 and 5 require.
///
/// `compute_index` is the IR op that produced `value` (or `SM86_IR_NONE` for a
/// void op); `writes_predicate` selects the predicate destination of `ISETP`
/// from the GPR destination of everything else. When `value` is `SM86_IR_NONE`
/// the instruction produces no result: a predicate on it is out of scope and is
/// deliberately ignored (the supported control-flow and barrier ops are not
/// predicated by the shaders the translator targets).
static void
ir_finish_instruction(sm86_ir_builder_t *POUND_RESTRICT               builder,
                      const sm86_decoded_instruction_t *POUND_RESTRICT instruction,
                      const uint32_t                                   compute_index,
                      const uint32_t                                   value,
                      const bool                                       writes_predicate)
{
    sm86_ir_program_t *POUND_RESTRICT program = builder->program;

    if (SM86_IR_NONE == value)
    {
        return;
    }

    const uint32_t guard = ir_guard_value(builder, instruction);

    if (SM86_IR_NONE == guard)
    {
        if (writes_predicate)
        {
            program->write_predicate[compute_index] = instruction->destination_register;
            builder->registers.predicate[instruction->destination_register] = value;
        }
        else
        {
            program->write_register[compute_index] = instruction->destination_register;
            builder->registers.gpr[instruction->destination_register] = value;
        }

        return;
    }

    // Predicated: the compute above is unpredicated (rule 5); commit it with a
    // select against the destination's previous value. Reading the previous
    // value may itself materialise an undef for a register never written.
    const uint32_t previous = writes_predicate ? ir_read_predicate(builder, instruction->destination_register)
                                               : ir_read_gpr(builder, instruction->destination_register);

    const uint32_t select_index
        = ir_append(builder, SM86_IR_OP_SELECT, (sm86_ir_value_type_t)program->type[compute_index]);

    ir_set_source(builder, select_index, SM86_IR_SLOT_SOURCE0, guard);
    ir_set_source(builder, select_index, SM86_IR_SLOT_SOURCE1, value);
    ir_set_source(builder, select_index, SM86_IR_SLOT_SOURCE2, previous);

    const uint32_t committed = ir_new_value(builder);
    ir_define(builder, select_index, committed);

    if (writes_predicate)
    {
        program->write_predicate[select_index] = instruction->destination_register;
        builder->registers.predicate[instruction->destination_register] = committed;
    }
    else
    {
        program->write_register[select_index] = instruction->destination_register;
        builder->registers.gpr[instruction->destination_register] = committed;
    }
}

/// Appends the binary/ternary integer op `opcode` over src0, src1 (the flexible
/// operand) and src2, returning its index and value.
static error_t
ir_emit_alu3(sm86_ir_builder_t *POUND_RESTRICT               builder,
             const sm86_ir_opcode_t                           opcode,
             const sm86_decoded_instruction_t *POUND_RESTRICT instruction,
             uint32_t *POUND_RESTRICT                         out_index,
             uint32_t *POUND_RESTRICT                         out_value)
{
    // src1 is materialised first: it is the only operand that can be refused (a
    // constant-buffer form), and refusing after src0/src2 have already been
    // appended would leave an orphaned read in the IR.
    uint32_t source1 = SM86_IR_NONE;
    const error_t operand_status = ir_operand1(builder, instruction, &source1);

    if (POUND_SUCCESS != operand_status)
    {
        return operand_status;
    }

    const uint32_t source0 = ir_read_gpr(builder, instruction->source0_register);
    const uint32_t source2 = ir_read_gpr(builder, instruction->source2_register);

    const uint32_t index = ir_append(builder, opcode, SM86_IR_TYPE_U32);
    ir_set_source(builder, index, SM86_IR_SLOT_SOURCE0, source0);
    ir_set_source(builder, index, SM86_IR_SLOT_SOURCE1, source1);
    ir_set_source(builder, index, SM86_IR_SLOT_SOURCE2, source2);

    const uint32_t value = ir_new_value(builder);
    ir_define(builder, index, value);

    *out_index = index;
    *out_value = value;

    return POUND_SUCCESS;
}

static error_t
sm86_translate_instruction(sm86_ir_builder_t *POUND_RESTRICT               builder,
                           const sm86_decoded_instruction_t *POUND_RESTRICT instruction)
{
    sm86_ir_program_t *POUND_RESTRICT program = builder->program;

    if (0 != instruction->unknown_opcode)
    {
        // The decoder could not name this encoding; a bare `NOP` is only a real
        // no-op when the opcode field was actually zero.
        POUND_LOG_ERROR(&thread_logger,
                        "SM86 encoding 0x%03X names no known instruction.",
                        (unsigned)instruction->raw_opcode);
        return POUND_ERROR_UNSUPPORTED_INSTRUCTION;
    }

    switch ((sm86_opcode_t)instruction->opcode)
    {
        case SM86_OPCODE_NOP:
            return POUND_SUCCESS;

        case SM86_OPCODE_MOV:
        {
            uint32_t operand = SM86_IR_NONE;
            const error_t operand_status = ir_operand1(builder, instruction, &operand);

            if (POUND_SUCCESS != operand_status)
            {
                return operand_status;
            }

            const uint32_t index = ir_append(builder, SM86_IR_OP_COPY, SM86_IR_TYPE_U32);
            ir_set_source(builder, index, SM86_IR_SLOT_SOURCE0, operand);

            const uint32_t value = ir_new_value(builder);
            ir_define(builder, index, value);
            ir_finish_instruction(builder, instruction, index, value, false);
            return POUND_SUCCESS;
        }

        case SM86_OPCODE_IADD3:
        case SM86_OPCODE_IMAD:
        {
            const sm86_ir_opcode_t opcode
                = (SM86_OPCODE_IADD3 == instruction->opcode) ? SM86_IR_OP_IADD3 : SM86_IR_OP_IMAD;

            uint32_t      index = SM86_IR_NONE;
            uint32_t      value = SM86_IR_NONE;
            const error_t alu_status = ir_emit_alu3(builder, opcode, instruction, &index, &value);

            if (POUND_SUCCESS != alu_status)
            {
                return alu_status;
            }

            ir_finish_instruction(builder, instruction, index, value, false);
            return POUND_SUCCESS;
        }

        case SM86_OPCODE_ISETP:
        {
            const uint32_t source0 = ir_read_gpr(builder, instruction->source0_register);
            uint32_t       source1 = SM86_IR_NONE;
            const error_t  operand_status = ir_operand1(builder, instruction, &source1);

            if (POUND_SUCCESS != operand_status)
            {
                return operand_status;
            }

            const uint32_t index = ir_append(builder, SM86_IR_OP_ISETP, SM86_IR_TYPE_PREDICATE);
            ir_set_source(builder, index, SM86_IR_SLOT_SOURCE0, source0);
            ir_set_source(builder, index, SM86_IR_SLOT_SOURCE1, source1);

            program->isetp_operator[index] = instruction->cmp_operator;
            program->isetp_signed[index]   = instruction->cmp_type;

            const uint32_t value = ir_new_value(builder);
            ir_define(builder, index, value);
            ir_finish_instruction(builder, instruction, index, value, true);
            return POUND_SUCCESS;
        }

        case SM86_OPCODE_LDC:
        {
            const uint32_t index = ir_append(builder, SM86_IR_OP_LDC, SM86_IR_TYPE_U32);

            program->constant_buffer[index] = instruction->payload.constant_buffer.binding_index;
            program->byte_offset[index]     = (int32_t)instruction->payload.constant_buffer.byte_offset;

            const uint32_t value = ir_new_value(builder);
            ir_define(builder, index, value);
            ir_finish_instruction(builder, instruction, index, value, false);
            return POUND_SUCCESS;
        }

        case SM86_OPCODE_EXIT:
        {
            (void)ir_append(builder, SM86_IR_OP_EXIT, SM86_IR_TYPE_VOID);
            return POUND_SUCCESS;
        }

        case SM86_OPCODE_KILL:
        {
            (void)ir_append(builder, SM86_IR_OP_KILL, SM86_IR_TYPE_VOID);
            return POUND_SUCCESS;
        }

        case SM86_OPCODE_BAR:
        case SM86_OPCODE_WARPSYNC:
        case SM86_OPCODE_MEMBAR:
        {
            // Rule 8: in a single basic block these carry no cross-lane ordering
            // that SPIR-V needs to represent, so they become an explicit
            // no-op IR op -- recorded so a future block-structured translator
            // can see them, emitted as nothing.
            (void)ir_append(builder, SM86_IR_OP_BARRIER, SM86_IR_TYPE_VOID);
            return POUND_SUCCESS;
        }

        default:
            POUND_LOG_ERROR(&thread_logger,
                            "SM86 instruction %s is not supported by the SPIR-V translator.",
                            sm86_opcode_name((sm86_opcode_t)instruction->opcode));
            return POUND_ERROR_UNSUPPORTED_INSTRUCTION;
    }
}

error_t
sm86_ir_build(sm86_ir_program_t *POUND_RESTRICT              program,
              const sm86_raw_instruction_t *POUND_RESTRICT  code,
              const uint32_t                                instruction_count)
{
    if ((NULL == program) || (NULL == code))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "sm86_ir_build requires a non-NULL program and instruction stream.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    sm86_ir_init(program);

    sm86_ir_builder_t builder = {0};
    builder.program           = program;
    builder.constant_zero     = SM86_IR_NONE;

    for (uint32_t i = 0; i < 256U; ++i)
    {
        builder.registers.gpr[i] = SM86_IR_NONE;
    }

    for (uint32_t i = 0; i < 8U; ++i)
    {
        builder.registers.predicate[i] = SM86_IR_NONE;
    }

    for (uint32_t pc = 0; pc < instruction_count; ++pc)
    {
        if ((program->count + SM86_IR_MAX_OPS_PER_INSTRUCTION > SM86_IR_MAX_INSTRUCTIONS)
            || (program->value_count + SM86_IR_MAX_VALUES_PER_INSTRUCTION > SM86_IR_MAX_VALUES)
            || (program->use_count + SM86_IR_MAX_USES_PER_INSTRUCTION > SM86_IR_MAX_USES))
        {
            POUND_LOG_ERROR(&thread_logger,
                            "The decoded block does not fit the IR capacity (%u instructions, "
                            "%u values, %u uses) at instruction %u.",
                            SM86_IR_MAX_INSTRUCTIONS,
                            SM86_IR_MAX_VALUES,
                            SM86_IR_MAX_USES,
                            pc);
            sm86_ir_init(program);
            return POUND_ERROR_TRANSLATION_FAILED;
        }

        sm86_decoded_instruction_t decoded = {0};
        sm86_decode(&code[pc], &decoded);

        builder.current_instruction = pc;

        const error_t status = sm86_translate_instruction(&builder, &decoded);

        if (POUND_SUCCESS != status)
        {
            sm86_ir_init(program);
            return status;
        }
    }

    return POUND_SUCCESS;
}

/*** end of file ***/
