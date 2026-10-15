//! Tests for the Horizon OS system call layer.
//!
//! ## The distinction most of these cases exist to hold down
//!
//! Every system call has two ways to go wrong, and they must not look alike: something
//! Horizon itself refuses, which is a `Result` in `X0` and a completed call, and something
//! Pound has not implemented, which is an error code and a trap left standing. A guest can
//! see the first and cannot see the second, so a case that asserts the *error code* and a
//! case that asserts the *Result* are testing opposite things even when they feed the
//! layer the same shape of bad input.
//!
//! ## Why the register file is the thing asserted on
//!
//! These cases write `X0..X3` and read them back, rather than calling handlers directly,
//! because the register file *is* the interface. A `Result` in the wrong register, or a
//! 32-bit answer that fails to zero the upper half of `X0`, is invisible from a handler's
//! return value and obvious from the registers.
//!
//! ## Why `pc` is checked as often as `X0`
//!
//! `pc` advancing is what makes a call complete. A refusal must leave it alone, because
//! a trap that silently steps past itself is a call that appears to have succeeded with no
//! answer -- so the `pc` assertions are load-bearing rather than incidental.

#include "pound_test.h"

#include "guest_state.h"
#include "hos/kernel.h"
#include "hos/syscall.h"
#include "log.h"
#include "memory/guest_memory.h"

#include <stddef.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Machine setup
// -----------------------------------------------------------------------------

#define TEST_GUEST_BASE 0x50000000ULL
#define TEST_GUEST_SIZE (1U << 16U)

/// Where a case puts things the guest is supposed to be able to point at.
#define TEST_NAME_ADDRESS (TEST_GUEST_BASE + 0x1000ULL)
#define TEST_HANDLES_ADDRESS (TEST_GUEST_BASE + 0x2000ULL)
#define TEST_STRING_ADDRESS (TEST_GUEST_BASE + 0x3000ULL)

/// One past the end of the window, so a pointer there is unmapped by construction.
#define TEST_UNMAPPED_ADDRESS (TEST_GUEST_BASE + TEST_GUEST_SIZE)

POUND_ALIGNED(64) static uint8_t g_memory[TEST_GUEST_SIZE];

static guest_memory_t        g_memory_region;
static guest_state_t         g_state;
static hos_kernel_t          g_kernel;
static hos_process_t         g_process;
static hos_syscall_context_t g_context;

static uint8_t *
host_at(const uint64_t guest_address)
{
    return &g_memory[(size_t)(guest_address - TEST_GUEST_BASE)];
}

/// Brings up a kernel, a process, a guest window and a context over all three.
static void
machine_reset(void)
{
    memset(g_memory, 0, sizeof(g_memory));
    memset(&g_kernel, 0, sizeof(g_kernel));
    memset(&g_process, 0, sizeof(g_process));
    memset(&g_context, 0, sizeof(g_context));

    POUND_REQUIRE(POUND_SUCCESS ==
                  guest_memory_init(&g_memory_region, g_memory, sizeof(g_memory), TEST_GUEST_BASE));
    POUND_REQUIRE(POUND_SUCCESS == guest_state_init(&g_state));
    g_state.pc = TEST_GUEST_BASE;

    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_init(&g_kernel));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create(&g_kernel, &g_process));

    hos_syscall_context_init(&g_context, &g_kernel, &g_process, &g_memory_region);

    // Non-zero, so a case that forgets to set them cannot pass by accident: a tick of
    // zero and a core of zero are both plausible-looking and both the zeroed default.
    g_context.processor_id = 2U;
    g_context.system_tick  = 0x1122334455667788ULL;
}

static void
machine_teardown(void)
{
    hos_process_close(&g_process);
    hos_kernel_destroy(&g_kernel);
}

/// Dispatches `number` with whatever is in the register file.
static error_t
call(const uint32_t number)
{
    return hos_syscall_dispatch(&g_context, number, &g_state);
}

/// Writes an eight-byte, space-padded service name, which is its form on the wire.
static void
put_name(const char *name, const uint64_t address)
{
    const size_t length = strlen(name);

    POUND_REQUIRE(length <= HOS_SYSCALL_SERVICE_NAME_MAX);

    uint8_t *bytes = host_at(address);
    memset(bytes, ' ', HOS_SYSCALL_SERVICE_NAME_MAX);
    memcpy(bytes, name, length);
}

/// Writes an array of handles the way the guest would have them laid out.
static void
put_handles(const hos_handle_t *handles, const uint32_t count, const uint64_t address)
{
    uint8_t *bytes = host_at(address);

    for (uint32_t index = 0U; index < count; index++)
    {
        const hos_handle_t handle = handles[index];
        const size_t       offset = (size_t)index * sizeof(uint32_t);

        bytes[offset + 0U] = (uint8_t)(handle);
        bytes[offset + 1U] = (uint8_t)(handle >> 8U);
        bytes[offset + 2U] = (uint8_t)(handle >> 16U);
        bytes[offset + 3U] = (uint8_t)(handle >> 24U);
    }
}

/// A service handler that copies its request into its reply.
static error_t
echo_handler(void *context,
             uint32_t command,
             const uint8_t *request,
             size_t request_size,
             uint8_t *response,
             size_t response_capacity,
             size_t *out_response_size)
{
    POUND_UNUSED(context);
    POUND_UNUSED(command);

    if (request_size > response_capacity)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (request_size > 0U)
    {
        memcpy(response, request, request_size);
    }

    *out_response_size = request_size;
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Dispatch
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, name_table)
{
    POUND_CHECK_STR_EQ(hos_syscall_name(HOS_SVC_SIGNAL_EVENT), "svcSignalEvent");
    POUND_CHECK_STR_EQ(hos_syscall_name(HOS_SVC_WAIT_SYNCHRONIZATION), "svcWaitSynchronization");
    POUND_CHECK_STR_EQ(hos_syscall_name(HOS_SVC_CONNECT_TO_NAMED_PORT), "svcConnectToNamedPort");

    // Known to Horizon and named, even though Pound does not implement it: the name is
    // what makes the refusal below readable.
    POUND_CHECK_STR_EQ(hos_syscall_name(0x21U), "svcSendSyncRequest");
    POUND_CHECK_STR_EQ(hos_syscall_name(0x01U), "svcSetHeapSize");

    // Not a Horizon number at all: 0x34 through 0x40 are not system calls.
    POUND_CHECK(hos_syscall_name(0x38U) == NULL);
    POUND_CHECK(hos_syscall_name(0xFFU) == NULL);
}

POUND_TEST(hos_syscall, unimplemented_call_is_refused_and_leaves_the_trap)
{
    machine_reset();

    // A number Horizon defines and Pound does not implement: the refusal must name it.
    g_state.x[0] = 0U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(0x21U), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("svcSendSyncRequest"));
    POUND_CHECK(pound_test_log_contains("has not implemented"));

    // A number Horizon does not define: there is no name to give, so the number is given.
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(0x38U), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("0x38"));

    // Neither refusal moved the guest: `pc` still points at the `SVC`, and `X0` was not
    // overwritten with a `Result` that would look like an answer.
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_state.x[0], 0U);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_COMPLETED);

    machine_teardown();
}

POUND_TEST(hos_syscall, a_completed_call_steps_over_itself)
{
    machine_reset();

    g_state.x[0] = (uint64_t)HOS_SVC_GET_SYSTEM_TICK;
    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_SYSTEM_TICK), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_COMPLETED);

    // A call that reports a *failure* to the guest still completed: the guest asked a
    // question, got an answer, and runs on. This is the case that separates a `Result`
    // from an error code.
    g_state.x[0] = HOS_HANDLE_INVALID;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_SIGNAL_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 8U);

    machine_teardown();
}

POUND_TEST(hos_syscall, a_context_that_names_nothing_is_refused)
{
    POUND_CHECK_EQ_U64(hos_syscall_dispatch(NULL, HOS_SVC_GET_SYSTEM_TICK, &g_state),
                       POUND_ERROR_INVALID_ARGUMENT);

    machine_reset();

    POUND_CHECK_EQ_U64(hos_syscall_dispatch(&g_context, HOS_SVC_GET_SYSTEM_TICK, NULL),
                       POUND_ERROR_INVALID_ARGUMENT);

    // A context that was never pointed anywhere: every field is zero.
    hos_syscall_context_t blank;
    memset(&blank, 0, sizeof(blank));
    POUND_CHECK(!hos_kernel_is_initialized(blank.kernel));
    POUND_CHECK_EQ_U64(hos_syscall_dispatch(&blank, HOS_SVC_GET_SYSTEM_TICK, &g_state),
                       POUND_ERROR_INVALID_ARGUMENT);

    // The kernel is real but the process is not.
    hos_process_t unopened;
    memset(&unopened, 0, sizeof(unopened));
    hos_syscall_context_t other;
    hos_syscall_context_init(&other, &g_kernel, &unopened, &g_memory_region);
    POUND_CHECK_EQ_U64(hos_syscall_dispatch(&other, HOS_SVC_GET_SYSTEM_TICK, &g_state),
                       POUND_ERROR_NOT_INITIALIZED);

    // The process is real but belongs to a different kernel, which would have the call
    // checked against one kernel and run against another.
    hos_kernel_t  stranger_kernel;
    hos_process_t stranger_process;
    memset(&stranger_kernel, 0, sizeof(stranger_kernel));
    memset(&stranger_process, 0, sizeof(stranger_process));
    POUND_REQUIRE(POUND_SUCCESS == hos_kernel_init(&stranger_kernel));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create(&stranger_kernel, &stranger_process));

    hos_syscall_context_init(&other, &g_kernel, &stranger_process, &g_memory_region);
    POUND_CHECK_EQ_U64(hos_syscall_dispatch(&other, HOS_SVC_GET_SYSTEM_TICK, &g_state),
                       POUND_ERROR_INVALID_ARGUMENT);

    // Initialising a context over a NULL context is a no-op rather than a crash, so a
    // caller cannot be made to fault by its own argument checking.
    hos_syscall_context_init(NULL, &g_kernel, &g_process, &g_memory_region);

    hos_process_close(&stranger_process);
    POUND_CHECK_EQ_U64(hos_kernel_destroy(&stranger_kernel), POUND_SUCCESS);
    machine_teardown();
}

// -----------------------------------------------------------------------------
// Calls that report on the machine
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, processor_number_and_system_tick)
{
    machine_reset();

    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_CURRENT_PROCESSOR_NUMBER), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 2U);

    // The upper half of the register must be clear: the guest reads `W0`, and a stale
    // upper half is the kind of bug that only shows up in a title that sign-extends.
    POUND_CHECK_EQ_U64(g_state.x[0] >> 32U, 0U);

    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_SYSTEM_TICK), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x1122334455667788ULL);

    // The tick is the caller's clock, so advancing it changes the answer without
    // anything else moving.
    g_context.system_tick = 0x1122334455667789ULL;
    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_SYSTEM_TICK), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x1122334455667789ULL);

    machine_teardown();
}

POUND_TEST(hos_syscall, cache_flushes_validate_the_range_they_are_given)
{
    machine_reset();

    // Returns nothing, and there is nothing to do, so the register file is left alone.
    g_state.x[0] = 0xDEADBEEFULL;
    POUND_CHECK_EQ_U64(call(HOS_SVC_FLUSH_ENTIRE_DATA_CACHE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xDEADBEEFULL);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);

    // A zero-length range is a success with nothing to check.
    g_state.x[0] = TEST_GUEST_BASE;
    g_state.x[1] = 0U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_FLUSH_DATA_CACHE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    // A range inside the window flushes as a no-op on the host, but is accepted.
    g_state.x[0] = TEST_GUEST_BASE + 0x100ULL;
    g_state.x[1] = 0x100ULL;
    POUND_CHECK_EQ_U64(call(HOS_SVC_FLUSH_DATA_CACHE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    // A range that runs off the end is the guest's mistake, and it is reported the way
    // the kernel reports it rather than crashing the emulator.
    g_state.x[0] = TEST_GUEST_BASE + 0x100ULL;
    g_state.x[1] = TEST_GUEST_SIZE;
    POUND_CHECK_EQ_U64(call(HOS_SVC_FLUSH_DATA_CACHE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_ADDRESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 16U);

    machine_teardown();
}

POUND_TEST(hos_syscall, output_debug_string_writes_to_the_log)
{
    machine_reset();

    static const char message[] = "hello from the guest";
    memcpy(host_at(TEST_STRING_ADDRESS), message, sizeof(message) - 1U);

    g_state.x[0] = TEST_STRING_ADDRESS;
    g_state.x[1] = sizeof(message) - 1U;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_OUTPUT_DEBUG_STRING), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK(pound_test_log_contains(message));

    // An empty string is a success: there is nothing to write, which is not an error.
    g_state.x[0] = TEST_STRING_ADDRESS;
    g_state.x[1] = 0U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_OUTPUT_DEBUG_STRING), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    // A string that is not fully mapped is reported, not read.
    g_state.x[0] = TEST_UNMAPPED_ADDRESS;
    g_state.x[1] = 8U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_OUTPUT_DEBUG_STRING), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_ADDRESS);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Calls that stop the guest
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, exit_process_ends_the_process)
{
    machine_reset();

    POUND_CHECK_EQ_U64(call(HOS_SVC_EXIT_PROCESS), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_EXITED_PROCESS);

    // `pc` is left on the `SVC` rather than stepped over: this is the last instruction
    // the process ran, and a debugger looking at the stop wants to see it.
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);

    // And the outcome does not stick to the context: the next call resets it, so a
    // caller cannot read a previous call's exit.
    POUND_CHECK_EQ_U64(call(HOS_SVC_GET_SYSTEM_TICK), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_COMPLETED);

    machine_teardown();
}

POUND_TEST(hos_syscall, break_stops_the_guest_without_pretending_to_answer)
{
    machine_reset();

    g_state.x[0] = 0x1234U;
    g_state.x[1] = 0x5678U;
    g_state.x[2] = 0x9ABCU;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_BREAK), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_BREAK);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(g_state.x[0], 0x1234U);

    // The reason is in the log, because the emulator's job with a break is to get out of
    // the way with the reason still visible.
    POUND_CHECK(pound_test_log_contains("svcBreak"));
    POUND_CHECK(pound_test_log_contains("0x1234"));

    machine_teardown();
}

POUND_TEST(hos_syscall, yields_return_and_sleeps_are_refused)
{
    machine_reset();

    // The three specified yields. A yield returns immediately, so with no scheduler
    // there is nothing to approximate and these are exactly right.
    const int64_t yields[] = { 0, -1, -2 };

    for (uint32_t index = 0U; index < 3U; index++)
    {
        g_state.x[0] = (uint64_t)yields[index];
        POUND_CHECK_EQ_U64(call(HOS_SVC_SLEEP_THREAD), POUND_SUCCESS);
        POUND_CHECK_EQ_U64(g_context.outcome, HOS_SYSCALL_COMPLETED);
    }

    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 12U);

    // A real sleep must block, and returning early would run the guest against a clock
    // that did not advance -- so it is refused rather than shortened.
    g_state.x[0] = 1000000U;
    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_SLEEP_THREAD), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK(pound_test_log_contains("svcSleepThread"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 12U);

    machine_teardown();
}

POUND_TEST(hos_syscall, sleep_system_returns)
{
    machine_reset();

    g_state.x[0] = 0xFFFFULL;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SLEEP_SYSTEM), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xFFFFULL);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// Handles and events
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, signal_and_clear_an_event)
{
    machine_reset();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_process_create_event(&g_process, HOS_EVENT_MANUAL, false, &event));

    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SIGNAL_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE + 4U);

    bool signaled = false;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(signaled);

    // `X0` is the call's answer as well as its argument, so every call below has to be
    // handed the handle again: leaving the previous `Result` in place would send
    // `0x00000000` to the next call, and the case would be testing a zero handle.
    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CLEAR_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(!signaled);

    // Clearing an event that is already clear is a success: `svcClearEvent` has to be
    // usable as "make sure this is clear".
    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CLEAR_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    machine_teardown();
}

POUND_TEST(hos_syscall, event_calls_report_an_invalid_handle)
{
    machine_reset();

    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_kernel_register_service(&g_kernel, "test-srv", echo_handler, NULL));

    hos_handle_t session = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_connect(&g_process, "test-srv", &session));

    // Zero names no object.
    g_state.x[0] = HOS_HANDLE_INVALID;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SIGNAL_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    // A session is a live handle of the wrong kind, and Horizon reports the two the same
    // way: the words for 0xE401 cover both. Each call is given the handle again, because
    // the previous call's `Result` is sitting in `X0` where the handle has to be.
    g_state.x[0] = session;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SIGNAL_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    g_state.x[0] = session;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CLEAR_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    g_state.x[0] = session;
    POUND_CHECK_EQ_U64(call(HOS_SVC_RESET_SIGNAL), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    machine_teardown();
}

POUND_TEST(hos_syscall, reset_signal_reports_whether_it_was_signaled)
{
    machine_reset();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_process_create_event(&g_process, HOS_EVENT_MANUAL, false, &event));

    // Nothing to reset: a failure rather than a no-op, which is how a guest tells
    // "I consumed the signal" from "something else did".
    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_RESET_SIGNAL), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_STATE);

    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_SIGNAL_EVENT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_RESET_SIGNAL), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    bool signaled = true;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, event, &signaled), POUND_SUCCESS);
    POUND_CHECK(!signaled);

    // Once consumed it stays consumed, so the second reset reports the same thing the
    // first one would have.
    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_RESET_SIGNAL), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_STATE);

    machine_teardown();
}

POUND_TEST(hos_syscall, close_handle_gives_the_handle_back)
{
    machine_reset();

    hos_handle_t event = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &event));

    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 1U);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 1U);

    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CLOSE_HANDLE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);
    POUND_CHECK_EQ_U64(hos_kernel_object_count(&g_kernel), 0U);

    // Closing it again is a failure, not a second success: a double close that reported
    // success would hide a handle the guest still believes it holds. The handle is the
    // same value as before, which no longer names anything once closed.
    g_state.x[0] = event;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CLOSE_HANDLE), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// svcWaitSynchronization
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, wait_synchronization_polls_the_handles)
{
    machine_reset();

    hos_handle_t quiet = HOS_HANDLE_INVALID;
    hos_handle_t ready = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &quiet));
    POUND_REQUIRE(POUND_SUCCESS == hos_process_create_event(&g_process, HOS_EVENT_AUTO, false, &ready));
    POUND_CHECK_EQ_U64(hos_process_event_signal(&g_process, ready), POUND_SUCCESS);

    const hos_handle_t handles[2] = { quiet, ready };
    put_handles(handles, 2U, TEST_HANDLES_ADDRESS);

    g_state.x[0] = 0U;
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = 2U;
    g_state.x[3] = 0U;

    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    // The index of the handle that was ready, counted from zero in the array.
    POUND_CHECK_EQ_U64(g_state.x[1], 1U);

    // An auto-reset event is handed to exactly one waiter, so the wait consumed it and
    // the next one over the same array has nothing left to find.
    bool signaled = true;
    POUND_CHECK_EQ_U64(hos_process_event_is_signaled(&g_process, ready, &signaled), POUND_SUCCESS);
    POUND_CHECK(!signaled);

    // The second wait is over the same array, which now has nothing ready in it: the
    // signal was consumed above.
    g_state.x[0] = 0U;
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = 2U;
    g_state.x[3] = 0U;

    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_TIMEOUT);

    // `W1` is the handle-array pointer going in and the ready index coming out, so a wait
    // that found nothing must leave the pointer untouched. An index written on a timeout
    // would be indistinguishable from the pointer's low half, and would point the guest at
    // a handle it never asked about.
    POUND_CHECK_EQ_U64(g_state.x[1], TEST_HANDLES_ADDRESS);

    machine_teardown();
}

POUND_TEST(hos_syscall, wait_synchronization_treats_a_session_as_never_ready)
{
    machine_reset();

    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_kernel_register_service(&g_kernel, "test-srv", echo_handler, NULL));

    hos_handle_t session = HOS_HANDLE_INVALID;
    POUND_REQUIRE(POUND_SUCCESS == hos_process_connect(&g_process, "test-srv", &session));

    const hos_handle_t handles[1] = { session };
    put_handles(handles, 1U, TEST_HANDLES_ADDRESS);

    g_state.x[0] = 0U;
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = 1U;
    g_state.x[3] = 0U;

    // A session is a legitimate thing to wait on, so this is a timeout rather than an
    // error: the guest asked a fair question and the answer is "not yet".
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_TIMEOUT);

    machine_teardown();
}

POUND_TEST(hos_syscall, wait_synchronization_rejects_what_it_cannot_do)
{
    machine_reset();

    const hos_handle_t handles[1] = { HOS_HANDLE_INVALID };
    put_handles(handles, 1U, TEST_HANDLES_ADDRESS);

    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = 1U;
    g_state.x[3] = 0U;

    // A handle in the array that names nothing can never be satisfied, so it is the
    // call's failure rather than something to skip past.
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_HANDLE);

    // An array the guest does not own.
    g_state.x[1] = TEST_UNMAPPED_ADDRESS;
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_ADDRESS);

    // More handles than the kernel takes.
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = HOS_SYSCALL_MAX_WAIT_HANDLES + 1U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_TOO_MANY_HANDLES);

    // A negative count is not a count.
    g_state.x[2] = 0xFFFFFFFFU;
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_TOO_MANY_HANDLES);

    machine_teardown();
}

POUND_TEST(hos_syscall, wait_synchronization_refuses_the_forms_that_must_block)
{
    machine_reset();

    const hos_handle_t handles[1] = { HOS_HANDLE_INVALID };
    put_handles(handles, 1U, TEST_HANDLES_ADDRESS);

    // Zero handles is the "wait forever" form, which a poll cannot imitate: a timeout
    // would tell the guest something that has not happened.
    g_state.x[1] = TEST_HANDLES_ADDRESS;
    g_state.x[2] = 0U;
    g_state.x[3] = 0U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);

    // A non-zero timeout must block, and answering it early with a timeout would be a
    // lie the guest cannot detect -- so it is refused rather than shortened.
    g_state.x[2] = 1U;
    g_state.x[3] = 1000000U;
    POUND_CHECK_EQ_U64(call(HOS_SVC_WAIT_SYNCHRONIZATION), POUND_ERROR_UNSUPPORTED_SYSCALL);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);

    machine_teardown();
}

// -----------------------------------------------------------------------------
// svcConnectToNamedPort
// -----------------------------------------------------------------------------

POUND_TEST(hos_syscall, connect_to_named_port_returns_a_session)
{
    machine_reset();

    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_kernel_register_service(&g_kernel, "fsp-srv", echo_handler, NULL));

    put_name("fsp-srv", TEST_NAME_ADDRESS);
    g_state.x[1] = TEST_NAME_ADDRESS;

    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);

    const hos_handle_t session = (hos_handle_t)(uint32_t)g_state.x[1];
    POUND_CHECK(session != HOS_HANDLE_INVALID);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 1U);

    hos_object_type_t type = HOS_OBJECT_EVENT;
    POUND_CHECK_EQ_U64(hos_process_handle_type(&g_process, session, &type), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(type, HOS_OBJECT_SESSION);

    machine_teardown();
}

POUND_TEST(hos_syscall, connect_to_named_port_accepts_either_padding)
{
    machine_reset();

    POUND_REQUIRE(POUND_SUCCESS ==
                  hos_kernel_register_service(&g_kernel, "fsp-srv", echo_handler, NULL));

    // Space-padded is the wire form; a NUL-padded name is what a guest that copied a C
    // string in produces. Both mean the same port, so both must connect.
    put_name("fsp-srv", TEST_NAME_ADDRESS);
    g_state.x[1] = TEST_NAME_ADDRESS;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 1U);

    host_at(TEST_NAME_ADDRESS)[7] = 0U; // "fsp-srv" plus a NUL where the space was
    g_state.x[1] = TEST_NAME_ADDRESS;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_SUCCESS);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 2U);

    machine_teardown();
}

POUND_TEST(hos_syscall, connect_to_a_service_pound_does_not_have_is_a_question)
{
    machine_reset();

    put_name("nosuchsv", TEST_NAME_ADDRESS);
    g_state.x[1] = TEST_NAME_ADDRESS;
    g_state.x[0] = 0xABCDULL;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_ERROR_NOT_FOUND);

    // Not a guest-visible failure and not logged: this is a gap in Pound, and handing
    // back a `Result` for it would make the emulator look like firmware that answered.
    POUND_CHECK_EQ_U64(pound_test_log_count(), 0U);
    POUND_CHECK_EQ_U64(g_state.x[0], 0xABCDULL);
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);
    POUND_CHECK_EQ_U64(hos_process_handle_count(&g_process), 0U);

    machine_teardown();
}

POUND_TEST(hos_syscall, connect_to_named_port_rejects_a_nameless_port)
{
    machine_reset();

    // All eight bytes blank, which is no name at all. Horizon documents no `Result` for
    // this, so it is refused rather than answered.
    memset(host_at(TEST_NAME_ADDRESS), ' ', HOS_SYSCALL_SERVICE_NAME_MAX);
    g_state.x[1] = TEST_NAME_ADDRESS;

    pound_test_log_reset();
    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_ERROR_INVALID_ARGUMENT);
    POUND_CHECK(pound_test_log_contains("empty service name"));
    POUND_CHECK_EQ_U64(g_state.pc, TEST_GUEST_BASE);

    // And a name the guest does not own is reported the way the kernel reports it.
    g_state.x[1] = TEST_UNMAPPED_ADDRESS;
    POUND_CHECK_EQ_U64(call(HOS_SVC_CONNECT_TO_NAMED_PORT), POUND_SUCCESS);
    POUND_CHECK_EQ_U64(g_state.x[0], HOS_RESULT_INVALID_ADDRESS);

    machine_teardown();
}

// -----------------------------------------------------------------------------

POUND_TEST_SUITE(hos_syscall,
                 POUND_TEST_CASE(hos_syscall, name_table),
                 POUND_TEST_CASE(hos_syscall, unimplemented_call_is_refused_and_leaves_the_trap),
                 POUND_TEST_CASE(hos_syscall, a_completed_call_steps_over_itself),
                 POUND_TEST_CASE(hos_syscall, a_context_that_names_nothing_is_refused),
                 POUND_TEST_CASE(hos_syscall, processor_number_and_system_tick),
                 POUND_TEST_CASE(hos_syscall, cache_flushes_validate_the_range_they_are_given),
                 POUND_TEST_CASE(hos_syscall, output_debug_string_writes_to_the_log),
                 POUND_TEST_CASE(hos_syscall, exit_process_ends_the_process),
                 POUND_TEST_CASE(hos_syscall, break_stops_the_guest_without_pretending_to_answer),
                 POUND_TEST_CASE(hos_syscall, yields_return_and_sleeps_are_refused),
                 POUND_TEST_CASE(hos_syscall, sleep_system_returns),
                 POUND_TEST_CASE(hos_syscall, signal_and_clear_an_event),
                 POUND_TEST_CASE(hos_syscall, event_calls_report_an_invalid_handle),
                 POUND_TEST_CASE(hos_syscall, reset_signal_reports_whether_it_was_signaled),
                 POUND_TEST_CASE(hos_syscall, close_handle_gives_the_handle_back),
                 POUND_TEST_CASE(hos_syscall, wait_synchronization_polls_the_handles),
                 POUND_TEST_CASE(hos_syscall, wait_synchronization_treats_a_session_as_never_ready),
                 POUND_TEST_CASE(hos_syscall, wait_synchronization_rejects_what_it_cannot_do),
                 POUND_TEST_CASE(hos_syscall, wait_synchronization_refuses_the_forms_that_must_block),
                 POUND_TEST_CASE(hos_syscall, connect_to_named_port_returns_a_session),
                 POUND_TEST_CASE(hos_syscall, connect_to_named_port_accepts_either_padding),
                 POUND_TEST_CASE(hos_syscall, connect_to_a_service_pound_does_not_have_is_a_question),
                 POUND_TEST_CASE(hos_syscall, connect_to_named_port_rejects_a_nameless_port))

/*** end of file ***/
