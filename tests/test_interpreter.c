//! Tests for the ARM64 interpreter.
//!
//! ## Why the encodings are literals
//!
//! Every instruction word below is written as a literal with its assembly in a
//! comment, rather than assembled field by field the way `test_sm86.c` builds
//! SASS. The reason is what a bug looks like in each case: a SASS field is a
//! handful of adjacent bits, so building one by hand re-derives the layout and a
//! mistake in the layout shows up as a differing word. An A64 instruction spreads
//! its fields across the word with several fixed opcode groups in between, so a
//! field-by-field builder would be a second implementation of the decoder -- and a
//! shared misreading of the ARM ARM would make both agree on the wrong answer.
//! Taking the word from a disassembler instead makes the assembler and the
//! interpreter independent witnesses, so a disagreement is a real one.
//!
//! ## What is covered
//!
//! The cases group by the family the header lists: immediates and moves, the
//! add/subtract and logical pairs in both their immediate and register forms,
//! multiply/divide/shift, the conditional select family, the four branch shapes,
//! loads and stores, and the two ways the loop stops (a trap and a fault). The
//! last two cases check the loop's own contract -- the budget, the trap record,
//! and the agreement between `interpreter_probe` and `interpreter_run`.

#include "pound_test.h"

#include "cpu/interpreter.h"
#include "log.h"

#include <stddef.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Machine setup
// -----------------------------------------------------------------------------

// The region is a single page-aligned page. `guest_memory_init` requires the host
// buffer to be 16-byte aligned, and the interpreter fetches through it, so the
// instruction stream and the data it loads and stores into share it -- which is
// what lets a literal load read a word placed a few bytes after the instruction.
#define TEST_GUEST_BASE 0x40000000ULL
#define TEST_GUEST_SIZE (1U << 16U)

POUND_ALIGNED(64) static uint8_t g_memory[TEST_GUEST_SIZE];

static guest_memory_t    g_memory_region;
static guest_state_t     g_state;
static uint32_t          g_executed;
static interpreter_trap_t g_trap;

/// Resets the memory region, the register file, and PC to the base of the region.
static void
machine_reset(void)
{
    memset(g_memory, 0, sizeof(g_memory));
    POUND_REQUIRE(POUND_SUCCESS == guest_memory_init(&g_memory_region, g_memory, sizeof(g_memory), TEST_GUEST_BASE));
    POUND_REQUIRE(POUND_SUCCESS == guest_state_init(&g_state));
    g_state.pc = TEST_GUEST_BASE;
    g_executed = 0U;
    memset(&g_trap, 0, sizeof(g_trap));
}

static uint8_t *
host_at(const uint64_t guest_address)
{
    return &g_memory[(size_t)(guest_address - TEST_GUEST_BASE)];
}

static void
put_word(const uint64_t guest_address, const uint32_t word)
{
    uint8_t *const bytes = host_at(guest_address);
    bytes[0]             = (uint8_t)(word);
    bytes[1]             = (uint8_t)(word >> 8U);
    bytes[2]             = (uint8_t)(word >> 16U);
    bytes[3]             = (uint8_t)(word >> 24U);
}

static void
put_words(const uint32_t *const words, const size_t count)
{
    for (size_t i = 0U; i < count; ++i)
    {
        put_word(TEST_GUEST_BASE + (i * 4U), words[i]);
    }
}

/// Writes a 64-bit value the way a guest store would.
static void
poke_u64(const uint64_t guest_address, const uint64_t value)
{
    uint8_t *const bytes = host_at(guest_address);
    for (unsigned i = 0U; i < 8U; ++i)
    {
        bytes[i] = (uint8_t)(value >> (8U * i));
    }
}

/// Runs the loop from `pc` for at most `budget` instructions.
static error_t
run_at(const uint64_t pc, const uint32_t budget)
{
    g_state.pc  = pc;
    g_executed  = 0U;
    return interpreter_run(&g_state, &g_memory_region, budget, &g_executed, &g_trap);
}

/// Places one instruction at the base of the region and runs it.
static error_t
run_single(const uint32_t word, const uint32_t budget)
{
    machine_reset();
    put_word(TEST_GUEST_BASE, word);
    return run_at(TEST_GUEST_BASE, budget);
}

// -----------------------------------------------------------------------------
// No-ops and moves
// -----------------------------------------------------------------------------

POUND_TEST(interpreter, nop_and_hint_are_no_ops)
{
    // nop            ; yield          ; wfe            ; wfi
    POUND_CHECK_EQ_U64(run_single(0xD503201F, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_executed, 1U);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);

    POUND_CHECK_EQ_U64(run_single(0xD503203F, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(run_single(0xD503205F, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(run_single(0xD503207F, 1U), POUND_SUCCESS);
}

POUND_TEST(interpreter, move_wide_immediates)
{
    // movz x0, #0x1234
    POUND_CHECK_EQ_U64(run_single(0xD2824680, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x1234U);

    // movz x0, #0x1234, lsl #32
    POUND_CHECK_EQ_U64(run_single(0xD2C24680, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x0000123400000000ULL);

    // movn x1, #0
    POUND_CHECK_EQ_U64(run_single(0x92800001, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[1], 0xFFFFFFFFFFFFFFFFULL);

    // movn w2, #0 -> zeroes the upper half of x2
    POUND_CHECK_EQ_U64(run_single(0x12800002, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[2], 0x00000000FFFFFFFFULL);

    // mov r0, #0x1234 ; movk r0, #0x5678, lsl #16
    machine_reset();
    put_word(TEST_GUEST_BASE + 0U, 0xD2824680);
    put_word(TEST_GUEST_BASE + 4U, 0xF2AACF00);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 2U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x0000000056781234ULL);
}

// -----------------------------------------------------------------------------
// Add/subtract
// -----------------------------------------------------------------------------

POUND_TEST(interpreter, add_and_sub_immediate)
{
    // add x0, x1, #3
    machine_reset();
    g_state.x[1] = 0x10U;
    put_word(TEST_GUEST_BASE, 0x91000C20);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x13U);

    // sub x2, x3, #1
    machine_reset();
    g_state.x[3] = 0x40U;
    put_word(TEST_GUEST_BASE, 0xD1000462);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[2], 0x3FU);

    // add x4, x5, #1, lsl #12
    machine_reset();
    g_state.x[5] = 0U;
    put_word(TEST_GUEST_BASE, 0x914004A4);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[4], 0x1000U);
}

POUND_TEST(interpreter, add_and_sub_shifted_register)
{
    // add x0, x1, x2
    machine_reset();
    g_state.x[1] = 5U;
    g_state.x[2] = 7U;
    put_word(TEST_GUEST_BASE, 0x8B020020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 12U);

    // add x0, x1, x2, lsl #4
    machine_reset();
    g_state.x[1] = 1U;
    g_state.x[2] = 2U;
    put_word(TEST_GUEST_BASE, 0x8B021020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 33U);

    // sub w0, w1, w2 ; the result is a 32-bit result
    machine_reset();
    g_state.x[1] = 0x10U;
    g_state.x[2] = 0x1U;
    put_word(TEST_GUEST_BASE, 0x4B020020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xFU);

    // add w0, w1, w2 with a 64-bit addend: only the low halves are used
    machine_reset();
    g_state.x[1] = 0xFFFFFFFF00000001ULL;
    g_state.x[2] = 0xFFFFFFFF00000002ULL;
    put_word(TEST_GUEST_BASE, 0x0B020020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 3U); // upper half cleared
}

POUND_TEST(interpreter, subs_sets_the_flags)
{
    // cmp x0, #5 == subs xzr, x0, #5
    machine_reset();
    g_state.x[0] = 5U;
    put_word(TEST_GUEST_BASE, 0xF100141F);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.flag_z, 1U);
    POUND_CHECK_EQ_U64(g_state.flag_n, 0U);
    POUND_CHECK_EQ_U64(g_state.flag_c, 1U); // no borrow
    POUND_CHECK_EQ_U64(g_state.flag_v, 0U);

    // cmp x0, x1 with x0 < x1: negative result, borrow
    machine_reset();
    g_state.x[0] = 3U;
    g_state.x[1] = 5U;
    put_word(TEST_GUEST_BASE, 0xEB01001F);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.flag_n, 1U);
    POUND_CHECK_EQ_U64(g_state.flag_z, 0U);
    POUND_CHECK_EQ_U64(g_state.flag_c, 0U);
    POUND_CHECK_EQ_U64(g_state.flag_v, 0U);

    // subs x2, x0, #1 about the most negative 64-bit value: signed overflow
    machine_reset();
    g_state.x[0] = 0x8000000000000000ULL;
    put_word(TEST_GUEST_BASE, 0xF1000402);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[2], 0x7FFFFFFFFFFFFFFFULL);
    POUND_CHECK_EQ_U64(g_state.flag_n, 0U);
    POUND_CHECK_EQ_U64(g_state.flag_v, 1U);

    // adds x2, x0, #1 from all-ones: carry out and zero
    machine_reset();
    g_state.x[0] = 0xFFFFFFFFFFFFFFFFULL;
    put_word(TEST_GUEST_BASE, 0xB1000402);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[2], 0U);
    POUND_CHECK_EQ_U64(g_state.flag_z, 1U);
    POUND_CHECK_EQ_U64(g_state.flag_c, 1U);
}

// -----------------------------------------------------------------------------
// Logical
// -----------------------------------------------------------------------------

POUND_TEST(interpreter, logical_immediate)
{
    // and x0, x1, #0xff
    machine_reset();
    g_state.x[1] = 0x1234U;
    put_word(TEST_GUEST_BASE, 0x92401C20);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x34U);

    // orr x0, x1, #0xff
    machine_reset();
    g_state.x[1] = 0x1200U;
    put_word(TEST_GUEST_BASE, 0xB2401C20);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x12FFU);

    // eor x0, x1, #0xff
    machine_reset();
    g_state.x[1] = 0x1234U;
    put_word(TEST_GUEST_BASE, 0xD2401C20);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x12CBU);

    // and w0, w1, #0xff : the 32-bit form, written zero-extended
    machine_reset();
    g_state.x[1] = 0xFFFFFFFFFFFF12A5ULL;
    put_word(TEST_GUEST_BASE, 0x12001C20);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xA5U);

    // mov x0, #0xff == orr x0, xzr, #0xff
    machine_reset();
    put_word(TEST_GUEST_BASE, 0xB2401FE0);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xFFU);
}

POUND_TEST(interpreter, logical_shifted_register)
{
    // and x0, x1, x2
    machine_reset();
    g_state.x[1] = 0xFFU;
    g_state.x[2] = 0x0FU;
    put_word(TEST_GUEST_BASE, 0x8A020020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x0FU);

    // orr x0, x1, x2
    machine_reset();
    g_state.x[1] = 0xF0U;
    g_state.x[2] = 0x0FU;
    put_word(TEST_GUEST_BASE, 0xAA020020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xFFU);

    // eor x0, x1, x2
    machine_reset();
    g_state.x[1] = 0xFFU;
    g_state.x[2] = 0x0FU;
    put_word(TEST_GUEST_BASE, 0xCA020020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xF0U);

    // bic x0, x1, x2 == and x0, x1, ~x2
    machine_reset();
    g_state.x[1] = 0xFFU;
    g_state.x[2] = 0x0FU;
    put_word(TEST_GUEST_BASE, 0x8A220020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xF0U);

    // mvn x0, x1 == orn x0, xzr, x1
    machine_reset();
    g_state.x[1] = 0x00000000FFFFFFFFULL;
    put_word(TEST_GUEST_BASE, 0xAA2103E0);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xFFFFFFFF00000000ULL);
}

POUND_TEST(interpreter, mov_alias_and_tst_flags)
{
    // mov x0, x1 == orr x0, xzr, x1
    machine_reset();
    g_state.x[1] = 0x123456789ABCDEF0ULL;
    put_word(TEST_GUEST_BASE, 0xAA0103E0);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x123456789ABCDEF0ULL);

    // tst x0, x1 == ands xzr, x0, x1
    machine_reset();
    g_state.x[0] = 0x0U;
    g_state.x[1] = 0x8U;
    put_word(TEST_GUEST_BASE, 0xEA01001F);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.flag_z, 1U); // AND is zero
    POUND_CHECK_EQ_U64(g_state.flag_c, 0U); // ANDS always clears C and V
    POUND_CHECK_EQ_U64(g_state.flag_v, 0U);

    POUND_CHECK_EQ_U64(g_state.x[31], 0U); // xzr was the destination
}

// -----------------------------------------------------------------------------
// Multiply, divide, shift
// -----------------------------------------------------------------------------

POUND_TEST(interpreter, multiply_and_divide)
{
    // mul x0, x1, x2
    machine_reset();
    g_state.x[1] = 6U;
    g_state.x[2] = 7U;
    put_word(TEST_GUEST_BASE, 0x9B027C20);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 42U);

    // msub x0, x1, x2, x3 == x3 - x1 * x2
    machine_reset();
    g_state.x[1] = 6U;
    g_state.x[2] = 7U;
    g_state.x[3] = 100U;
    put_word(TEST_GUEST_BASE, 0x9B028C20);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 58U);

    // udiv x0, x1, x2
    machine_reset();
    g_state.x[1] = 100U;
    g_state.x[2] = 7U;
    put_word(TEST_GUEST_BASE, 0x9AC20820);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 14U);

    // sdiv x0, x1, x2 truncates toward zero
    machine_reset();
    g_state.x[1] = (uint64_t)(int64_t)-100;
    g_state.x[2] = 7U;
    put_word(TEST_GUEST_BASE, 0x9AC20C20);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], (uint64_t)(int64_t)-14);

    // division by zero is defined to yield zero, not a trap
    machine_reset();
    g_state.x[1] = 100U;
    g_state.x[2] = 0U;
    put_word(TEST_GUEST_BASE, 0x9AC20820);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0U);
}

POUND_TEST(interpreter, variable_shift)
{
    // lsl x0, x1, x2
    machine_reset();
    g_state.x[1] = 1U;
    g_state.x[2] = 4U;
    put_word(TEST_GUEST_BASE, 0x9AC22020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 16U);

    // lsr x0, x1, x2
    machine_reset();
    g_state.x[1] = 0x10U;
    g_state.x[2] = 4U;
    put_word(TEST_GUEST_BASE, 0x9AC22420);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 1U);

    // asr x0, x1, x2 sign-fills
    machine_reset();
    g_state.x[1] = (uint64_t)(int64_t)-16;
    g_state.x[2] = 2U;
    put_word(TEST_GUEST_BASE, 0x9AC22820);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], (uint64_t)(int64_t)-4);

    // A shift amount is taken modulo the register width: 64 means no shift.
    machine_reset();
    g_state.x[1] = 0x1234U;
    g_state.x[2] = 64U;
    put_word(TEST_GUEST_BASE, 0x9AC22020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x1234U);
}

// -----------------------------------------------------------------------------
// Conditional select
// -----------------------------------------------------------------------------

POUND_TEST(interpreter, conditional_select_family)
{
    // csel x0, x1, x2, eq
    machine_reset();
    g_state.x[1] = 0xAAU;
    g_state.x[2] = 0xBBU;
    g_state.flag_z = 1U;
    put_word(TEST_GUEST_BASE, 0x9A820020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xAAU);

    machine_reset();
    g_state.x[1] = 0xAAU;
    g_state.x[2] = 0xBBU;
    g_state.flag_z = 0U;
    put_word(TEST_GUEST_BASE, 0x9A820020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xBBU);

    // csinc x0, x1, x2, eq : the false path is x2 + 1
    POUND_CHECK_EQ_U64(run_single(0x9A820420, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 1U); // x2 == 0, so 0 + 1

    // csinv x0, x1, x2, eq : the false path is ~x2
    machine_reset();
    g_state.x[2] = 0U;
    g_state.flag_z = 0U;
    put_word(TEST_GUEST_BASE, 0xDA820020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xFFFFFFFFFFFFFFFFULL);

    // csneg x0, x1, x2, eq : the false path is -x2
    machine_reset();
    g_state.x[2] = 5U;
    g_state.flag_z = 0U;
    put_word(TEST_GUEST_BASE, 0xDA820420);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], (uint64_t)(int64_t)-5);
}

POUND_TEST(interpreter, cset_alias)
{
    // cset x0, eq == csinc x0, xzr, xzr, ne
    machine_reset();
    g_state.flag_z = 1U; // eq holds
    put_word(TEST_GUEST_BASE, 0x9A9F17E0);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 1U);

    machine_reset();
    g_state.flag_z = 0U; // eq does not hold
    put_word(TEST_GUEST_BASE, 0x9A9F17E0);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0U);
}

// -----------------------------------------------------------------------------
// Address generation
// -----------------------------------------------------------------------------

POUND_TEST(interpreter, adr_and_adrp)
{
    // adr x0, .
    POUND_CHECK_EQ_U64(run_single(0x10000000, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], TEST_GUEST_BASE);

    // adr x1, #16
    POUND_CHECK_EQ_U64(run_single(0x10000081, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[1], TEST_GUEST_BASE + 16U);

    // adrp x2, . : the base here is already page-aligned
    POUND_CHECK_EQ_U64(run_single(0x90000002, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[2], TEST_GUEST_BASE);

    // adrp x3, #1 : page(base) + 1 page
    POUND_CHECK_EQ_U64(run_single(0xB0000003, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[3], TEST_GUEST_BASE + 0x1000U);
}

// -----------------------------------------------------------------------------
// Branches
// -----------------------------------------------------------------------------

POUND_TEST(interpreter, unconditional_branch_and_link)
{
    // b .+8
    POUND_CHECK_EQ_U64(run_single(0x14000002, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 8U);

    // bl .+8 : records the return address then branches
    machine_reset();
    put_word(TEST_GUEST_BASE, 0x94000002);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[30], TEST_GUEST_BASE + 4U);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 8U);

    // A backward branch is a negative displacement.
    machine_reset();
    put_word(TEST_GUEST_BASE + 8U, 0x17FFFFFF); // b .-4
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE + 8U, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);
}

POUND_TEST(interpreter, conditional_branch_uses_the_flags)
{
    // cmp x0, x1 ; b.eq .+8
    machine_reset();
    g_state.x[0] = 5U;
    g_state.x[1] = 5U;
    put_word(TEST_GUEST_BASE + 0U, 0xEB01001F);
    put_word(TEST_GUEST_BASE + 4U, 0x54000040);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 2U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 12U);
    POUND_CHECK_EQ_U64(g_executed, 2U);

    machine_reset();
    g_state.x[0] = 5U;
    g_state.x[1] = 6U;
    put_word(TEST_GUEST_BASE + 0U, 0xEB01001F);
    put_word(TEST_GUEST_BASE + 4U, 0x54000040);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 2U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 8U); // fell through
}

POUND_TEST(interpreter, compare_and_branch)
{
    // cbz x0, .+8
    machine_reset();
    g_state.x[0] = 0U;
    put_word(TEST_GUEST_BASE, 0xB4000040);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 8U);

    // cbz w0 with only the upper half set: the 32-bit view is zero
    machine_reset();
    g_state.x[0] = 0xFFFFFFFF00000000ULL;
    put_word(TEST_GUEST_BASE, 0x34000040);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 8U);

    // cbnz x0, .+8
    machine_reset();
    g_state.x[0] = 1U;
    put_word(TEST_GUEST_BASE, 0xB5000040);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 8U);
}

POUND_TEST(interpreter, test_and_branch)
{
    // tbz x0, #0, .+8 : taken when bit 0 is clear
    machine_reset();
    g_state.x[0] = 0U;
    put_word(TEST_GUEST_BASE, 0x36000040);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 8U);

    // tbnz x0, #0, .+8 : not taken when bit 0 is clear
    machine_reset();
    g_state.x[0] = 0U;
    put_word(TEST_GUEST_BASE, 0x37000040);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);

    // The test bit index spans bit 5 in the instruction's bit 31: tbz x0, #40
    machine_reset();
    g_state.x[0] = 1ULL << 40U;
    put_word(TEST_GUEST_BASE, 0xB6400040); // tbz x0, #40, .+8
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U); // bit 40 set, so no branch
}

POUND_TEST(interpreter, branch_register_and_return)
{
    // br x0
    machine_reset();
    g_state.x[0] = TEST_GUEST_BASE + 0x20U;
    put_word(TEST_GUEST_BASE, 0xD61F0000);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 0x20U);

    // blr x0
    machine_reset();
    g_state.x[0] = TEST_GUEST_BASE + 0x30U;
    put_word(TEST_GUEST_BASE, 0xD63F0000);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[30], TEST_GUEST_BASE + 4U);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 0x30U);

    // ret x2
    machine_reset();
    g_state.x[2] = TEST_GUEST_BASE + 0x40U;
    put_word(TEST_GUEST_BASE, 0xD65F0040);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 0x40U);

    // ret : the default x30
    machine_reset();
    g_state.x[30] = TEST_GUEST_BASE + 0x50U;
    put_word(TEST_GUEST_BASE, 0xD65F03C0);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 0x50U);
}

// -----------------------------------------------------------------------------
// Loads and stores
// -----------------------------------------------------------------------------

POUND_TEST(interpreter, load_and_store_unsigned_offset)
{
    const uint64_t data_address = TEST_GUEST_BASE + 0x200U;

    // ldr x0, [x1] ; str x2, [x1]
    machine_reset();
    poke_u64(data_address, 0xDEADBEEFCAFEBABEULL);
    g_state.x[1] = data_address;
    put_word(TEST_GUEST_BASE + 0U, 0xF9400020); // ldr x0, [x1]
    put_word(TEST_GUEST_BASE + 4U, 0xF9000022); // str x2, [x1]
    g_state.x[2] = 0x0123456789ABCDEFULL;
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 2U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xDEADBEEFCAFEBABEULL);
    POUND_CHECK_EQ_U64(*(const uint64_t *)(const void *)host_at(data_address), 0x0123456789ABCDEFULL);

    // ldr x0, [x1, #8]
    machine_reset();
    poke_u64(data_address + 8U, 0x1122334455667788ULL);
    g_state.x[1] = data_address;
    put_word(TEST_GUEST_BASE, 0xF9400420);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x1122334455667788ULL);

    // stp x0, x1, [x2] then ldp x3, x4, [x2]
    machine_reset();
    g_state.x[0] = 0xAAAAULL;
    g_state.x[1] = 0xBBBBULL;
    g_state.x[2] = data_address;
    put_word(TEST_GUEST_BASE + 0U, 0xA9000440); // stp x0, x1, [x2]
    put_word(TEST_GUEST_BASE + 4U, 0xA9401043); // ldp x3, x4, [x2]
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 2U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[3], 0xAAAAULL);
    POUND_CHECK_EQ_U64(g_state.x[4], 0xBBBBULL);
}

POUND_TEST(interpreter, load_sizes_and_sign_extension)
{
    const uint64_t data_address = TEST_GUEST_BASE + 0x300U;

    // A byte of 0x80, read zero-extended and then sign-extended two ways.
    machine_reset();
    host_at(data_address)[0] = 0x80U;
    g_state.x[1]            = data_address;

    put_word(TEST_GUEST_BASE, 0x39400020); // ldrb w0, [x1]
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x0000000000000080ULL);

    put_word(TEST_GUEST_BASE, 0x39800020); // ldrsb x0, [x1]
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xFFFFFFFFFFFFFF80ULL);

    put_word(TEST_GUEST_BASE, 0x39C00020); // ldrsb w0, [x1]
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x00000000FFFFFF80ULL);

    // A halfword of 0x8000.
    machine_reset();
    host_at(data_address)[0] = 0x00U;
    host_at(data_address)[1] = 0x80U;
    g_state.x[1]             = data_address;

    put_word(TEST_GUEST_BASE, 0x79400020); // ldrh w0, [x1]
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x0000000000008000ULL);

    put_word(TEST_GUEST_BASE, 0x79800020); // ldrsh x0, [x1]
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xFFFFFFFFFFFF8000ULL);

    // A word of 0xFFFFFFFF read with ldrsw.
    machine_reset();
    host_at(data_address)[0] = 0xFFU;
    host_at(data_address)[1] = 0xFFU;
    host_at(data_address)[2] = 0xFFU;
    host_at(data_address)[3] = 0xFFU;
    g_state.x[1]             = data_address;

    put_word(TEST_GUEST_BASE, 0xB9400020); // ldr w0, [x1]
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x00000000FFFFFFFFULL);

    put_word(TEST_GUEST_BASE, 0xB9800020); // ldrsw x0, [x1]
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xFFFFFFFFFFFFFFFFULL);
}

POUND_TEST(interpreter, load_literal)
{
    // ldr x0, .+8 : the literal follows the instruction
    machine_reset();
    poke_u64(TEST_GUEST_BASE + 8U, 0x0123456789ABCDEFULL);
    put_word(TEST_GUEST_BASE, 0x58000040);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x0123456789ABCDEFULL);
}

POUND_TEST(interpreter, register_31_is_stack_pointer_in_address_roles)
{
    // add x0, sp, #16 : Rn == 31 reads the stack pointer
    machine_reset();
    g_state.x[31] = 0x1000U;
    put_word(TEST_GUEST_BASE, 0x910043E0);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x1010U);

    // add x0, xzr, x1 : Rn == 31 in the shifted-register form is zero
    machine_reset();
    g_state.x[31] = 0x1000U;
    g_state.x[1]  = 5U;
    put_word(TEST_GUEST_BASE, 0x8B0103E0);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 5U);

    // str x0, [sp] : the base of a store is the stack pointer
    machine_reset();
    g_state.x[31] = TEST_GUEST_BASE + 0x400U;
    g_state.x[0]  = 0xFEEDFACEULL;
    put_word(TEST_GUEST_BASE, 0xF90003E0);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(*(const uint64_t *)(const void *)host_at(TEST_GUEST_BASE + 0x400U), 0xFEEDFACEULL);

    // add sp, sp, #16 : Rd == 31 for the flag-free form writes the stack pointer
    machine_reset();
    g_state.x[31] = 0x1000U;
    put_word(TEST_GUEST_BASE, 0x910043FF);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[31], 0x1010U);
}

// -----------------------------------------------------------------------------
// The loop's stop conditions
// -----------------------------------------------------------------------------

POUND_TEST(interpreter, exceptions_are_reported_as_traps)
{
    // svc #0x80
    POUND_CHECK_EQ_U64(run_single(0xD4001001, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_executed, 0U);
    POUND_CHECK_EQ_U64(g_trap.kind, INTERPRETER_TRAP_SVC);
    POUND_CHECK_EQ_U64(g_trap.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_trap.instr, 0xD4001001U);
    POUND_CHECK_EQ_U64(g_trap.imm, 0x80U);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE); // left at the trap

    // brk #0
    POUND_CHECK_EQ_U64(run_single(0xD4200000, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_trap.kind, INTERPRETER_TRAP_BREAKPOINT);

    // hlt #0 is a deliberate halt, so it shares the breakpoint kind
    POUND_CHECK_EQ_U64(run_single(0xD4400000, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_trap.kind, INTERPRETER_TRAP_BREAKPOINT);
}

POUND_TEST(interpreter, undefined_and_unsupported_encodings_trap)
{
    // udf #0
    POUND_CHECK_EQ_U64(run_single(0x00000000U, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_executed, 0U);
    POUND_CHECK_EQ_U64(g_trap.kind, INTERPRETER_TRAP_UNDEFINED);
    POUND_CHECK_EQ_U64(g_trap.instr, 0x00000000U);

    // fadd s0, s1, s2 : a valid instruction this interpreter does not model
    machine_reset();
    put_word(TEST_GUEST_BASE, 0x1E222820);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_trap.kind, INTERPRETER_TRAP_UNDEFINED);
}

POUND_TEST(interpreter, probe_agrees_with_run)
{
    POUND_CHECK_EQ_U64(interpreter_probe(0xD503201F), 1); // nop
    POUND_CHECK_EQ_U64(interpreter_probe(0x8B020020), 1); // add
    POUND_CHECK_EQ_U64(interpreter_probe(0xD4000001), 1); // svc, a handled trap
    POUND_CHECK_EQ_U64(interpreter_probe(0x00000000), 0); // udf
    POUND_CHECK_EQ_U64(interpreter_probe(0x1E222820), 0); // fadd
}

POUND_TEST(interpreter, run_stops_at_the_budget)
{
    machine_reset();
    for (size_t i = 0U; i < 5U; ++i)
    {
        put_word(TEST_GUEST_BASE + (i * 4U), 0xD503201F); // nop
    }

    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 3U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_executed, 3U);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 12U);
    POUND_CHECK_EQ_U64(g_trap.kind, INTERPRETER_TRAP_NONE);

    // A zero budget retires nothing and lands nowhere.
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 0U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_executed, 0U);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
}

POUND_TEST(interpreter, run_rejects_null_arguments)
{
    machine_reset();

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(interpreter_run(NULL, &g_memory_region, 1U, &g_executed, &g_trap), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("NULL"));

    POUND_CHECK_EQ_U64(interpreter_run(&g_state, NULL, 1U, &g_executed, &g_trap), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK_EQ_U64(interpreter_run(&g_state, &g_memory_region, 1U, NULL, &g_trap), POUND_ERROR_INVALID_ARGUMENT);

    // A NULL trap record is not an error: the loop still runs, it just discards
    // what stopped it.
    POUND_CHECK_EQ_U64(run_single(0xD503201F, 1U), POUND_SUCCESS);
    machine_reset();
    put_word(TEST_GUEST_BASE, 0xD503201F);
    g_state.pc = TEST_GUEST_BASE;
    POUND_CHECK_EQ_U64(interpreter_run(&g_state, &g_memory_region, 1U, &g_executed, NULL), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_executed, 1U);
}

POUND_TEST(interpreter, unmapped_fetch_is_a_memory_fault)
{
    machine_reset();
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE + TEST_GUEST_SIZE, 1U), POUND_ERROR_MEMORY_FAULT);
    POUND_CHECK_EQ_U64(g_executed, 0U);
}

POUND_TEST(interpreter, unmapped_load_is_a_memory_fault)
{
    // ldr x0, [x1] with a base outside the region
    machine_reset();
    g_state.x[1] = 0x1000U;
    put_word(TEST_GUEST_BASE, 0xF9400020);
    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 1U), POUND_ERROR_MEMORY_FAULT);
    POUND_CHECK_EQ_U64(g_executed, 0U);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE); // left at the faulting instruction
}

// -----------------------------------------------------------------------------
// A whole program
// -----------------------------------------------------------------------------

POUND_TEST(interpreter, accumulating_loop_runs_to_completion)
{
    // movz x0, #5           ; x0 = 5
    // movz x1, #0           ; x1 = 0
    // loop: add x1, x1, x0  ; x1 += x0
    //       sub x0, x0, #1  ; x0 -= 1
    //       cbnz x0, loop
    //       ret             ; x30 == TEST_GUEST_BASE + 0x40 from the harness
    const uint32_t code[] = {
        0xD28000A0U, // movz x0, #5
        0xD2800001U, // movz x1, #0
        0x8B000021U, // add x1, x1, x0
        0xD1000400U, // sub x0, x0, #1
        0xB5FFFFC0U, // cbnz x0, .-8
        0xD65F03C0U, // ret
    };

    machine_reset();
    put_words(code, sizeof(code) / sizeof(code[0]));
    g_state.x[30] = TEST_GUEST_BASE + 0x40U;

    POUND_CHECK_EQ_U64(run_at(TEST_GUEST_BASE, 32U), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[1], 15U); // 5 + 4 + 3 + 2 + 1
    POUND_CHECK_EQ_U64(g_state.x[0], 0U);
    POUND_CHECK_EQ_U64(g_executed, 18U); // 2 setup + 5 * 3 loop + 1 return
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 0x40U);
}

// -----------------------------------------------------------------------------

POUND_TEST_SUITE(interpreter,
                 POUND_TEST_CASE(interpreter, nop_and_hint_are_no_ops),
                 POUND_TEST_CASE(interpreter, move_wide_immediates),
                 POUND_TEST_CASE(interpreter, add_and_sub_immediate),
                 POUND_TEST_CASE(interpreter, add_and_sub_shifted_register),
                 POUND_TEST_CASE(interpreter, subs_sets_the_flags),
                 POUND_TEST_CASE(interpreter, logical_immediate),
                 POUND_TEST_CASE(interpreter, logical_shifted_register),
                 POUND_TEST_CASE(interpreter, mov_alias_and_tst_flags),
                 POUND_TEST_CASE(interpreter, multiply_and_divide),
                 POUND_TEST_CASE(interpreter, variable_shift),
                 POUND_TEST_CASE(interpreter, conditional_select_family),
                 POUND_TEST_CASE(interpreter, cset_alias),
                 POUND_TEST_CASE(interpreter, adr_and_adrp),
                 POUND_TEST_CASE(interpreter, unconditional_branch_and_link),
                 POUND_TEST_CASE(interpreter, conditional_branch_uses_the_flags),
                 POUND_TEST_CASE(interpreter, compare_and_branch),
                 POUND_TEST_CASE(interpreter, test_and_branch),
                 POUND_TEST_CASE(interpreter, branch_register_and_return),
                 POUND_TEST_CASE(interpreter, load_and_store_unsigned_offset),
                 POUND_TEST_CASE(interpreter, load_sizes_and_sign_extension),
                 POUND_TEST_CASE(interpreter, load_literal),
                 POUND_TEST_CASE(interpreter, register_31_is_stack_pointer_in_address_roles),
                 POUND_TEST_CASE(interpreter, exceptions_are_reported_as_traps),
                 POUND_TEST_CASE(interpreter, undefined_and_unsupported_encodings_trap),
                 POUND_TEST_CASE(interpreter, probe_agrees_with_run),
                 POUND_TEST_CASE(interpreter, run_stops_at_the_budget),
                 POUND_TEST_CASE(interpreter, run_rejects_null_arguments),
                 POUND_TEST_CASE(interpreter, unmapped_fetch_is_a_memory_fault),
                 POUND_TEST_CASE(interpreter, unmapped_load_is_a_memory_fault),
                 POUND_TEST_CASE(interpreter, accumulating_loop_runs_to_completion))

/*** end of file ***/
