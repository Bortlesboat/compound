#include "gpu/sm86/decoder.h"

#include "attributes.h"
#include <string.h>

/// Names in `sm86_opcode_t` order. The static assertion below pins the count so
/// a new opcode cannot be added to the enum without a name here.
static const char *const k_sm86_opcode_names[SM86_OPCODE_MAX_INSTRUCTIONS] = {
    "NOP",   "AL2P", "ALD",   "AST",   "ATOM",   "BAR",    "BCLEAR", "BMOV",  "BMSK",  "BRA",
    "BREAK", "BREV", "BSSY",  "BSYNC", "CCTL",   "CS2R",   "DADD",   "DFMA",  "DMUL",  "DSETP",
    "EXIT",  "F2FP", "FADD",  "FFMA",  "FLO",    "FMNMX",  "FMUL",   "FSET",  "FSETP", "FSWZADD",
    "HADD2", "HFMA2", "HMMA", "HMNMX2", "HMUL2", "HSET2",  "HSETP2", "I2F",   "IABS",  "IADD3",
    "IDP4",  "IMAD", "IMAD64", "IMMA", "IMNMX",  "IPA",    "ISBERD", "ISBEWR", "ISETP", "KILL",
    "LD",    "LDC",  "LDSM",  "LDTRAM", "LEA",   "LEAX",   "LOP3",   "MATCH", "MEMBAR", "MOV",
    "MOVM",  "MUFU", "OUT",   "OUTFINAL", "PIXLD", "PLOP3", "POPC",  "PRMT",  "R2UR",  "REDUX",
    "S2R",   "SEL",  "SGXT",  "SHF",   "SHFL",   "ST",     "SUATOM", "SULD",  "SUST",  "TEX",
    "TLD",   "TLD4", "TMML",  "TXD",   "TXQ",    "VOTE",   "WARPSYNC",
};

_Static_assert(((sizeof(k_sm86_opcode_names) / sizeof(k_sm86_opcode_names[0])) ==
                (size_t)SM86_OPCODE_MAX_INSTRUCTIONS),
               "k_sm86_opcode_names must name every opcode, in enum order");

const char *
sm86_opcode_name(const sm86_opcode_t opcode)
{
    if ((opcode < 0) || (opcode >= SM86_OPCODE_MAX_INSTRUCTIONS))
    {
        return "UNKNOWN";
    }

    return k_sm86_opcode_names[opcode];
}

void
sm86_decode(const sm86_raw_instruction_t *POUND_RESTRICT raw_instruction,
            sm86_decoded_instruction_t *POUND_RESTRICT   out_instruction)
{
    sm86_decoded_instruction_t instruction = {0};

    const uint64_t raw_low  = raw_instruction->low;
    const uint64_t raw_high = raw_instruction->high;
    const uint32_t word0    = (uint32_t)(raw_low & 0xFFFFFFFFULL);
    const uint32_t word1    = (uint32_t)(raw_low >> 32);
    const uint32_t word2    = (uint32_t)(raw_high & 0xFFFFFFFFULL);
    const uint32_t word3    = (uint32_t)(raw_high >> 32);
    const uint16_t raw_opcode = (uint16_t)(word0 & 0x0FFFU);

    // Local scalars, so the compiler keeps the running identity in a register
    // rather than reloading through the output pointer (Programming Rules 1-2).
    sm86_opcode_t opcode        = g_sm86_opcodes_bits_to_enum[raw_opcode];
    uint8_t       unknown_opcode = 0;

    if ((SM86_OPCODE_NOP == opcode) && (0U != raw_opcode))
    {
        // The 12-bit index fuses the opcode with the 3-bit source-form selector
        // in bits 9..11. A miss on the exact index is retried with those bits
        // masked off, which names the same instruction.
        const uint16_t stripped_bits = (uint16_t)(raw_opcode & (uint16_t)~0x0E00U);
        const sm86_opcode_t stripped = g_sm86_opcodes_bits_to_enum[stripped_bits];

        if (SM86_OPCODE_NOP != stripped)
        {
            opcode = stripped;
        }
        else
        {
            // Neither the index nor the form-stripped index names an
            // instruction. The IR builder refuses this rather than treating it
            // as the real `NOP` at index 0.
            unknown_opcode = 1;
        }
    }

    instruction.opcode         = opcode;
    instruction.raw_opcode     = raw_opcode;
    instruction.unknown_opcode = unknown_opcode;
    instruction.form                 = (uint8_t)((word0 >> 9) & 0x07U);
    instruction.predicate_register   = (uint8_t)((word0 >> 12) & 0x07U);
    instruction.predicate_not        = (uint8_t)((word0 >> 15) & 0x01U);
    instruction.destination_register = (uint8_t)((word0 >> 16) & 0xFFU);
    instruction.source0_register     = (uint8_t)((word0 >> 24) & 0xFFU);
    instruction.delay_cycles         = (uint8_t)((word3 >> 9) & 0x0FU);
    instruction.yield_flag           = (uint8_t)((word3 >> 13) & 0x01U);
    instruction.read_barrier         = (uint8_t)((word3 >> 14) & 0x07U);
    instruction.write_barrier        = (uint8_t)((word3 >> 17) & 0x07U);

    if (SM86_OPCODE_LDC == opcode)
    {
        // A constant-buffer read always carries its `{binding, byte_offset}`
        // pair in word1. Decoding it here, before the class switch (which for
        // the memory class would otherwise fold word1 into a `memory_offset`),
        // is what rule 9 needs; see the header.
        instruction.payload.constant_buffer.byte_offset   = (uint16_t)(word1 & 0xFFFFU);
        instruction.payload.constant_buffer.binding_index = (uint8_t)((word1 >> 16) & 0xFFU);
    }
    else if (4 == instruction.form)
    {
        // Safely reinterpret word1 as a signed immediate without violating
        // strict aliasing.
        const union
        {
            uint32_t u;
            int32_t  i;
        } pun = {.u = word1};

        instruction.payload.immediate_value = pun.i;
    }
    else if (5 == instruction.form)
    {
        instruction.payload.constant_buffer.byte_offset   = (uint16_t)(word1 & 0xFFFFU);
        instruction.payload.constant_buffer.binding_index = (uint8_t)((word1 >> 16) & 0xFFU);
    }
    else
    {
        instruction.source1_register = (uint8_t)(word1 & 0xFFU);
    }

    instruction.source2_register = (uint8_t)(word2 & 0xFFU);

    const sm86_instruction_class_t instruction_class
        = (sm86_instruction_class_t)g_sm86_opcode_metadata[opcode].instruction_class;

    switch (instruction_class)
    {
        case SM86_CLASS_FLOAT_ALU:
        case SM86_CLASS_HALF_FLOAT_ALU:
            instruction.source0_neg = (uint8_t)((word2 >> 8) & 0x01U);
            instruction.source0_abs = (uint8_t)((word2 >> 9) & 0x01U);
            instruction.source1_abs = (uint8_t)((word1 >> 30) & 0x01U);
            instruction.source1_neg = (uint8_t)((word1 >> 31) & 0x01U);
            instruction.saturate    = (uint8_t)((word2 >> 13) & 0x01U);
            instruction.ftz         = (uint8_t)((word2 >> 16) & 0x01U);
            instruction.is_uniform  = (uint8_t)((word2 >> 27) & 0x01U);
            break;

        case SM86_CLASS_INT_ALU:
            instruction.source0_neg   = (uint8_t)((word2 >> 8) & 0x01U);
            instruction.source1_neg   = (uint8_t)((word1 >> 31) & 0x01U);
            instruction.is_uniform    = (uint8_t)((word2 >> 27) & 0x01U);
            instruction.extended_math = (uint8_t)((word2 >> 10) & 0x01U);

            if (SM86_OPCODE_ISETP == opcode)
            {
                instruction.cmp_type                  = (uint8_t)((word2 >> 9) & 0x01U);
                instruction.bool_operator             = (uint8_t)((word2 >> 10) & 0x03U);
                instruction.cmp_operator              = (uint8_t)((word2 >> 12) & 0x07U);
                instruction.destination_register      = (uint8_t)((word2 >> 17) & 0x07U);
                instruction.accumulator_predicate     = (uint8_t)((word2 >> 23) & 0x07U);
                instruction.accumulator_predicate_not = (uint8_t)((word2 >> 26) & 0x01U);
            }
            break;

        case SM86_CLASS_MEMORY_LOAD_STORE:
            instruction.is_uniform = (uint8_t)((word2 >> 27) & 0x01U);

            if (SM86_OPCODE_LDC != opcode)
            {
                instruction.payload.memory_offset = (int32_t)(word1 >> 8);
            }
            break;

        case SM86_CLASS_TEXTURE_FETCH:
        case SM86_CLASS_SURFACE_ATOMIC:
        case SM86_CLASS_CONTROL_FLOW:
            instruction.is_uniform            = (uint8_t)((word2 >> 27) & 0x01U);
            instruction.payload.memory_offset = (int32_t)(word1 >> 8);
            break;

        case SM86_CLASS_SYNC_AND_YIELD:
            // Handled by the universal barrier bits in word 3.
            break;

        case SM86_CLASS_UNKNOWN:
        default:
            break;
    }

    *out_instruction = instruction;
}

void
sm86_decode_block(const sm86_raw_instruction_t *POUND_RESTRICT raw_instructions,
                  sm86_decoded_instruction_t *POUND_RESTRICT   out_decoded_instructions,
                  const uint32_t                               instruction_count)
{
    const sm86_raw_instruction_t *POUND_RESTRICT raw_cursor     = raw_instructions;
    sm86_decoded_instruction_t *POUND_RESTRICT   decoded_cursor = out_decoded_instructions;

    for (uint32_t i = 0; i < instruction_count; ++i)
    {
        sm86_decode(raw_cursor++, decoded_cursor++);
    }
}

/*** end of file ***/
