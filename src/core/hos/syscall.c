#include "syscall.h"
#include "log.h"
#include <string.h>

/// Handles one system call. `X0..X7` are read from and written to `state`, and the
/// context says what happened by setting `context->outcome`.
typedef error_t (*hos_syscall_fn_t)(hos_syscall_context_t *POUND_RESTRICT context,
                                    guest_state_t *POUND_RESTRICT          state);

/// One row of the system call table.
typedef struct
{
    /// The `SVC` immediate.
    uint32_t number;

    /// The name Horizon gives the call, used in every log line about it.
    const char *name;

    /// What runs it, or NULL for a number this module knows but does not implement.
    ///
    /// The NULL rows are not padding: their only job is to let a refusal say
    /// `svcSendSyncRequest (0x21)` instead of `svc 0x21`, which is the difference
    /// between a log line that names the missing feature and one that names a number.
    /// The numbers and spellings are the ones the homebrew wiki lists.
    hos_syscall_fn_t handler;
} hos_syscall_entry_t;

// -----------------------------------------------------------------------------
// Forward declarations, so the table below can sit above the handlers
// -----------------------------------------------------------------------------

static error_t svc_exit_process(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_create_thread(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_start_thread(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_exit_thread(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_sleep_thread(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_thread_priority(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_set_thread_priority(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_current_processor_number(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_signal_event(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_clear_event(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_close_handle(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_reset_signal(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_wait_synchronization(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_system_tick(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_connect_to_named_port(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_thread_id(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_break(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_output_debug_string(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_flush_entire_data_cache(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_flush_data_cache(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_sleep_system(hos_syscall_context_t *context, guest_state_t *state);

// -----------------------------------------------------------------------------
// The table
// -----------------------------------------------------------------------------

static const hos_syscall_entry_t HOS_SYSCALLS[] = {
    // Implemented.
    { HOS_SVC_EXIT_PROCESS, "svcExitProcess", svc_exit_process },
    { HOS_SVC_CREATE_THREAD, "svcCreateThread", svc_create_thread },
    { HOS_SVC_START_THREAD, "svcStartThread", svc_start_thread },
    { HOS_SVC_EXIT_THREAD, "svcExitThread", svc_exit_thread },
    { HOS_SVC_SLEEP_THREAD, "svcSleepThread", svc_sleep_thread },
    { HOS_SVC_GET_THREAD_PRIORITY, "svcGetThreadPriority", svc_get_thread_priority },
    { HOS_SVC_SET_THREAD_PRIORITY, "svcSetThreadPriority", svc_set_thread_priority },
    { HOS_SVC_GET_CURRENT_PROCESSOR_NUMBER, "svcGetCurrentProcessorNumber", svc_get_current_processor_number },
    { HOS_SVC_SIGNAL_EVENT, "svcSignalEvent", svc_signal_event },
    { HOS_SVC_CLEAR_EVENT, "svcClearEvent", svc_clear_event },
    { HOS_SVC_CLOSE_HANDLE, "svcCloseHandle", svc_close_handle },
    { HOS_SVC_RESET_SIGNAL, "svcResetSignal", svc_reset_signal },
    { HOS_SVC_WAIT_SYNCHRONIZATION, "svcWaitSynchronization", svc_wait_synchronization },
    { HOS_SVC_GET_SYSTEM_TICK, "svcGetSystemTick", svc_get_system_tick },
    { HOS_SVC_CONNECT_TO_NAMED_PORT, "svcConnectToNamedPort", svc_connect_to_named_port },
    { HOS_SVC_GET_THREAD_ID, "svcGetThreadId", svc_get_thread_id },
    { HOS_SVC_BREAK, "svcBreak", svc_break },
    { HOS_SVC_OUTPUT_DEBUG_STRING, "svcOutputDebugString", svc_output_debug_string },
    { HOS_SVC_FLUSH_ENTIRE_DATA_CACHE, "svcFlushEntireDataCache", svc_flush_entire_data_cache },
    { HOS_SVC_FLUSH_DATA_CACHE, "svcFlushDataCache", svc_flush_data_cache },
    { HOS_SVC_SLEEP_SYSTEM, "svcSleepSystem", svc_sleep_system },

    // Known, named, and deliberately not implemented. Each of these is waiting on a
    // slice that does not exist yet, and each says which one in the header.
    { 0x01U, "svcSetHeapSize", NULL },
    { 0x02U, "svcSetMemoryPermission", NULL },
    { 0x04U, "svcMapMemory", NULL },
    { 0x05U, "svcUnmapMemory", NULL },
    { 0x06U, "svcQueryMemory", NULL },
    { 0x13U, "svcMapSharedMemory", NULL },
    { 0x14U, "svcUnmapSharedMemory", NULL },
    { 0x15U, "svcCreateTransferMemory", NULL },
    { 0x19U, "svcCancelSynchronization", NULL },
    { 0x20U, "svcSendSyncRequestLight", NULL },
    { 0x21U, "svcSendSyncRequest", NULL },
    { 0x22U, "svcSendSyncRequestWithUserBuffer", NULL },
    { 0x24U, "svcGetProcessId", NULL },
    { 0x29U, "svcGetInfo", NULL },
    { 0x41U, "svcAcceptSession", NULL },
    { 0x42U, "svcReplyAndReceiveLight", NULL },
    { 0x43U, "svcReplyAndReceive", NULL },
    { 0x44U, "svcReplyAndReceiveWithUserBuffer", NULL },
    { 0x45U, "svcCreateEvent", NULL },
    { 0x50U, "svcCreateSharedMemory", NULL },
    { 0x70U, "svcCreatePort", NULL },
    { 0x71U, "svcManageNamedPort", NULL },
    { 0x72U, "svcConnectToPort", NULL },
    { 0x7FU, "svcCallSecureMonitor", NULL },
};

#define HOS_SYSCALL_COUNT (sizeof(HOS_SYSCALLS) / sizeof(HOS_SYSCALLS[0]))

/// Returns the table row for `number`, or NULL for a number the table does not carry.
static const hos_syscall_entry_t *
find_syscall(uint32_t number)
{
    for (size_t index = 0U; index < HOS_SYSCALL_COUNT; index++)
    {
        if (HOS_SYSCALLS[index].number == number)
        {
            return &HOS_SYSCALLS[index];
        }
    }

    return NULL;
}

const char *
hos_syscall_name(uint32_t number)
{
    const hos_syscall_entry_t *entry = find_syscall(number);
    return (entry == NULL) ? NULL : entry->name;
}

// -----------------------------------------------------------------------------
// Guest memory
// -----------------------------------------------------------------------------

/// Translates `size` contiguous readable guest bytes at `address`.
///
/// Returns false when the span is not fully mapped, which every caller turns into the
/// kernel's own `HOS_RESULT_INVALID_ADDRESS` rather than into an emulator error: a guest
/// pointer that is not mapped is something Horizon itself reports, and a guest is
/// allowed to make that mistake without stopping the machine.
static bool
read_guest(const hos_syscall_context_t *POUND_RESTRICT context,
           uint64_t                                    address,
           size_t                                      size,
           const uint8_t **POUND_RESTRICT              out)
{
    size_t available = 0U;

    if (size == 0U)
    {
        *out = NULL;
        return true;
    }

    const uint8_t *pointer = guest_memory_translate_read(context->memory, address, &available);

    if ((pointer == NULL) || (available < size))
    {
        return false;
    }

    *out = pointer;
    return true;
}

/// Reads the little-endian 32-bit word at `bytes`.
///
/// Spelled out rather than `memcpy`-ed through an integer so the byte order is part of
/// the code rather than a property of the host it happens to be compiled for.
static uint32_t
read_u32(const uint8_t *POUND_RESTRICT bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/// Returns the handle an argument register carries.
///
/// Handles are 32 bits and arrive in the low half of a 64-bit register, so the upper
/// half is discarded rather than rejected: the guest is entitled to leave anything
/// there, because AArch64 writes of `W0` zero it anyway.
static hos_handle_t
handle_argument(uint64_t value)
{
    return (hos_handle_t)(uint32_t)value;
}

/// Writes a little-endian 32-bit word to guest memory at `address`.
///
/// The counterpart to `read_guest`, and the reason the out-parameter calls write through
/// the pointer in `X0` before they replace `X0` with their `Result`: once the `Result` is
/// in the register, the address is gone. Returns false when the four bytes are not
/// writable, which every caller turns into `HOS_RESULT_INVALID_ADDRESS`.
static bool
write_guest_u32(const hos_syscall_context_t *POUND_RESTRICT context, uint64_t address, uint32_t value)
{
    size_t available = 0U;

    uint8_t *pointer = guest_memory_translate_write(context->memory, address, &available);

    if ((pointer == NULL) || (available < sizeof(uint32_t)))
    {
        return false;
    }

    pointer[0] = (uint8_t)value;
    pointer[1] = (uint8_t)(value >> 8);
    pointer[2] = (uint8_t)(value >> 16);
    pointer[3] = (uint8_t)(value >> 24);
    return true;
}

/// Writes a little-endian 64-bit word to guest memory at `address`.
///
/// A separate function rather than a call to `write_guest_u32` twice, because a guest
/// address is not guaranteed to have eight contiguous writable bytes just because it has
/// four: the span is checked for the width actually being written.
static bool
write_guest_u64(const hos_syscall_context_t *POUND_RESTRICT context, uint64_t address, uint64_t value)
{
    size_t available = 0U;

    uint8_t *pointer = guest_memory_translate_write(context->memory, address, &available);

    if ((pointer == NULL) || (available < sizeof(uint64_t)))
    {
        return false;
    }

    for (size_t index = 0U; index < sizeof(uint64_t); index++)
    {
        pointer[index] = (uint8_t)(value >> (8U * index));
    }

    return true;
}

// -----------------------------------------------------------------------------
// Handle calls
// -----------------------------------------------------------------------------

/// Translates a failure from the handle layer into the `Result` Horizon reports.
///
/// The two failures a handle can produce -- naming nothing, and naming the wrong kind of
/// object -- are one `Result` on hardware, because Horizon's `0xE401` covers both with
/// the words "either does not exist, or is not a WritableEvent". Keeping them apart here
/// would be more precise than the machine being emulated, which is a different bug.
static bool
handle_failure_result(error_t error, uint32_t *POUND_RESTRICT out_result)
{
    if (error == POUND_SUCCESS)
    {
        return false;
    }

    if ((error == POUND_ERROR_INVALID_HANDLE) || (error == POUND_ERROR_INVALID_ARGUMENT))
    {
        *out_result = HOS_RESULT_INVALID_HANDLE;
        return true;
    }

    return false;
}

static error_t
svc_signal_event(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    uint32_t      result = HOS_RESULT_SUCCESS;
    const error_t error  = hos_process_event_signal(context->process, handle_argument(state->x[0]));

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_clear_event(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    uint32_t      result = HOS_RESULT_SUCCESS;
    const error_t error  = hos_process_event_clear(context->process, handle_argument(state->x[0]));

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // Clearing an event that is not signaled is a success: `svcClearEvent` is allowed to
    // be redundant, which is what makes it usable as "make sure this is clear".
    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_close_handle(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    uint32_t      result = HOS_RESULT_SUCCESS;
    const error_t error  = hos_process_close_handle(context->process, handle_argument(state->x[0]));

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_reset_signal(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    const hos_handle_t handle = handle_argument(state->x[0]);
    bool               signaled = false;

    uint32_t      result = HOS_RESULT_SUCCESS;
    error_t       error  = hos_process_event_is_signaled(context->process, handle, &signaled);

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (!signaled)
    {
        // Not a no-op, and deliberately so: this is how a guest distinguishes having
        // consumed a signal from something else having got there first. Reporting
        // success for an event that was already clear would erase that distinction.
        state->x[0] = HOS_RESULT_INVALID_STATE;
        return POUND_SUCCESS;
    }

    error = hos_process_event_clear(context->process, handle);

    if (error != POUND_SUCCESS)
    {
        // Unreachable: the handle was resolved and typed one call ago, and nothing has
        // run in between because the kernel lock is released and retaken per call.
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_wait_synchronization(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // Horizon's signature: X0 is a pointer to the ready index, X1 the handle array, W2 the
    // count and X3 the timeout. X0 is a pointer on entry and the `Result` on return, which
    // is why the index is written *through* it before it is replaced.
    const uint64_t out_index_address = state->x[0];
    const uint64_t handles_address   = state->x[1];
    const int32_t  count             = (int32_t)(uint32_t)state->x[2];
    const int64_t  timeout           = (int64_t)state->x[3];

    if (count == 0)
    {
        // Zero handles is Horizon's "wait forever", and this is a gap rather than a poll:
        // a wait on nothing can only be ended by the clock, and there is no honest way to
        // shorten it into a timeout the guest never asked for.
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcWaitSynchronization with no handles: a wait on nothing can only be "
                        "ended by a deadline, and Horizon documents this form as waiting forever.");
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    if ((count < 0) || ((uint32_t)count > HOS_SYSCALL_MAX_WAIT_HANDLES))
    {
        state->x[0] = HOS_RESULT_TOO_MANY_HANDLES;
        return POUND_SUCCESS;
    }

    const uint8_t *bytes = NULL;

    if (!read_guest(context, handles_address, (size_t)(uint32_t)count * sizeof(uint32_t), &bytes))
    {
        state->x[0] = HOS_RESULT_INVALID_ADDRESS;
        return POUND_SUCCESS;
    }

    hos_handle_t handles[HOS_SYSCALL_MAX_WAIT_HANDLES];

    for (uint32_t index = 0U; index < (uint32_t)count; index++)
    {
        handles[index] = (hos_handle_t)read_u32(bytes + ((size_t)index * sizeof(uint32_t)));
    }

    // The whole set is polled at once, in the kernel, because the answer is more than "is
    // anything ready": an auto-reset signal is consumed by the wait that wins it, and the
    // order that decides which one wins lives there. Polling one handle at a time, as an
    // earlier version did, could express neither.
    uint32_t ready_index = 0U;
    bool     ready       = false;

    error_t error = hos_process_wait_any(context->process, handles, (uint32_t)count, &ready_index, &ready);

    if (error == POUND_ERROR_INVALID_HANDLE)
    {
        // Horizon reports an invalid handle in the array as the call's failure rather than
        // skipping it: a wait over a handle that names nothing can never be satisfied, so
        // it is not something to look past.
        state->x[0] = HOS_RESULT_INVALID_HANDLE;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (ready)
    {
        // The common case, and the one that must never park a thread: a wait that can be
        // satisfied now is answered now, which is what a poll is.
        state->x[0] =
            write_guest_u32(context, out_index_address, ready_index) ? HOS_RESULT_SUCCESS : HOS_RESULT_INVALID_ADDRESS;
        return POUND_SUCCESS;
    }

    if (timeout == 0)
    {
        state->x[0] = HOS_RESULT_TIMEOUT;
        return POUND_SUCCESS;
    }

    if (context->scheduler == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcWaitSynchronization for %lld ns: nothing in the set is ready, and "
                        "parking the caller until something is needs a scheduler. Answering a timeout "
                        "before it expires would be a lie the guest cannot detect.",
                        (long long)timeout);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // A negative timeout is Horizon's "wait forever": parked with no deadline, so only one
    // of the handles becoming ready can wake it. A positive one becomes a deadline on the
    // scheduler's clock. The deadline is taken now, not when the thread is parked, so the
    // wait is measured from the call rather than from whenever the scheduler next looked.
    const bool     has_deadline = timeout > 0;
    const uint64_t deadline =
        has_deadline
            ? hos_scheduler_tick(context->scheduler) + hos_scheduler_ticks_from_nanoseconds((uint64_t)timeout)
            : 0U;

    error = hos_scheduler_block_current(context->scheduler, handles, (uint32_t)count, has_deadline, deadline);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // The call is suspended, not finished: `pc` stays on the `SVC` and the pointer is
    // still in `X0`, because `hos_syscall_complete_block` needs it when the scheduler
    // wakes the thread.
    context->outcome = HOS_SYSCALL_BLOCKED;
    return POUND_SUCCESS;
}

static error_t
svc_connect_to_named_port(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    const uint8_t *bytes = NULL;

    if (!read_guest(context, state->x[1], HOS_SYSCALL_SERVICE_NAME_MAX, &bytes))
    {
        state->x[0] = HOS_RESULT_INVALID_ADDRESS;
        return POUND_SUCCESS;
    }

    // Eight raw bytes on the wire, padded with spaces or with NULs depending on how the
    // guest built the string. Both paddings are stripped here: the padding is a
    // marshalling concern, and this layer compares names as C strings.
    char   name[HOS_SYSCALL_SERVICE_NAME_MAX + 1U];
    size_t length = 0U;

    while ((length < HOS_SYSCALL_SERVICE_NAME_MAX) && (bytes[length] != 0U))
    {
        name[length] = (char)bytes[length];
        length++;
    }

    while ((length > 0U) && (name[length - 1U] == ' '))
    {
        length--;
    }

    name[length] = '\0';

    if (length == 0U)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcConnectToNamedPort with an empty service name: Horizon documents "
                        "no Result for a nameless port, and inventing one would look like an answer.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    hos_handle_t  handle = HOS_HANDLE_INVALID;
    const error_t error  = hos_process_connect(context->process, name, &handle);

    if (error == POUND_SUCCESS)
    {
        // The handle goes through the pointer Horizon leaves in X0; writing it into X1
        // instead would put it where the guest is not looking and leave the `Result` in
        // X0 to be read as the handle.
        if (!write_guest_u32(context, state->x[0], handle))
        {
            // The session was opened, but the guest cannot be told its handle. The handle
            // is taken straight back so the process does not keep a session it can never
            // name, and the bad pointer is reported exactly as Horizon would.
            hos_process_close_handle(context->process, handle);
            state->x[0] = HOS_RESULT_INVALID_ADDRESS;
            return POUND_SUCCESS;
        }

        state->x[0] = HOS_RESULT_SUCCESS;
        return POUND_SUCCESS;
    }

    if (error == POUND_ERROR_NOT_FOUND)
    {
        // Left as a code rather than turned into a Result: a title connecting to a
        // service this build has not registered is a gap in Pound, not a failure the
        // guest caused, and there is no honest Result for it. Not logged, per the
        // contract the code carries; the caller has the system call number and is
        // expected to report it.
        return POUND_ERROR_NOT_FOUND;
    }

    return error;
}

// -----------------------------------------------------------------------------
// Calls that report on the machine
// -----------------------------------------------------------------------------

static error_t
svc_get_current_processor_number(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // A 32-bit result: the guest reads W0, so the upper half of the register is left
    // zero by the store.
    state->x[0] = context->processor_id;
    return POUND_SUCCESS;
}

static error_t
svc_get_system_tick(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    state->x[0] = context->system_tick;
    return POUND_SUCCESS;
}

static error_t
svc_flush_data_cache(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    const uint64_t size = state->x[1];
    const uint8_t *bytes = NULL;

    if (size == 0U)
    {
        state->x[0] = HOS_RESULT_SUCCESS;
        return POUND_SUCCESS;
    }

    // A host has no guest data cache, so the flush itself is a no-op. The range is still
    // checked, because a flush of an address the guest does not own is a guest bug and
    // this is the last place it is cheap to notice.
    if (!read_guest(context, state->x[0], (size_t)size, &bytes))
    {
        state->x[0] = HOS_RESULT_INVALID_ADDRESS;
        return POUND_SUCCESS;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_flush_entire_data_cache(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // Returns nothing, and there is nothing to do: the guest's cache is the host's.
    POUND_UNUSED(context);
    POUND_UNUSED(state);
    return POUND_SUCCESS;
}

static error_t
svc_output_debug_string(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    const uint64_t size  = state->x[1];
    const uint8_t *bytes = NULL;

    if (size == 0U)
    {
        state->x[0] = HOS_RESULT_SUCCESS;
        return POUND_SUCCESS;
    }

    if (!read_guest(context, state->x[0], (size_t)size, &bytes))
    {
        state->x[0] = HOS_RESULT_INVALID_ADDRESS;
        return POUND_SUCCESS;
    }

    const size_t logged =
        ((size_t)size > HOS_SYSCALL_DEBUG_STRING_LOG_MAX) ? HOS_SYSCALL_DEBUG_STRING_LOG_MAX : (size_t)size;

    if (logged < (size_t)size)
    {
        POUND_LOG_INFO(&thread_logger,
                       "Guest debug output (%llu bytes, first %llu): %.*s",
                       (unsigned long long)size,
                       (unsigned long long)logged,
                       (int)logged,
                       (const char *)bytes);
    }
    else
    {
        POUND_LOG_INFO(&thread_logger, "Guest debug output: %.*s", (int)logged, (const char *)bytes);
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Thread calls
// -----------------------------------------------------------------------------
//
// A thread has two names and these calls use both. The guest only ever holds a *handle*,
// which is the per-process authority that says it may touch this thread at all, so every
// call here starts from a handle and resolves it to the kernel-wide *identifier* the
// kernel and scheduler work in. Nothing here trusts a handle without resolving it: a
// thread another process owns has a different handle there and none here.
//
// A context with no scheduler has no run queue, so creating or starting a thread through
// it would make a thread that can never run. That is a gap rather than a shorter answer,
// and it is refused as one.

/// Reports whether a thread call must be refused because the context has no scheduler.
///
/// True means the caller should return `POUND_ERROR_UNSUPPORTED_SYSCALL`, and the log line
/// naming the call has already been written.
static bool
thread_call_needs_scheduler(const hos_syscall_context_t *POUND_RESTRICT context, const char *POUND_RESTRICT call)
{
    if (context->scheduler != NULL)
    {
        return false;
    }

    POUND_LOG_ERROR(&thread_logger,
                    "Refusing %s: the context has no scheduler, so there is no run queue for a thread to "
                    "be created in or started from.",
                    call);
    return true;
}

static error_t
svc_create_thread(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (thread_call_needs_scheduler(context, "svcCreateThread"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 is a pointer to the new handle, X1 the entry point, X2 the
    // argument, X3 the stack bottom, W4 the priority and W5 the core. The core is ignored
    // rather than checked: one scheduler is one core, and a title naming a second is asking
    // for a second of something Pound has one of.
    const uint64_t out_handle_address = state->x[0];
    const uint64_t entry              = state->x[1];
    const uint64_t argument           = state->x[2];
    const uint64_t stack_bottom       = state->x[3];
    const uint32_t priority           = (uint32_t)state->x[4];

    if (priority > HOS_THREAD_PRIORITY_LOWEST)
    {
        // A gap rather than a `Result`: Horizon refuses an out-of-range priority, but Pound
        // has not established which `Result` word it uses, and inventing one would be an
        // answer the guest could branch on that nobody verified.
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcCreateThread with priority %u: Horizon's own Result for a priority "
                        "outside %u..%u has not been established, and inventing one would look like an "
                        "answer.",
                        (unsigned int)priority,
                        (unsigned int)HOS_THREAD_PRIORITY_HIGHEST,
                        (unsigned int)HOS_THREAD_PRIORITY_LOWEST);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // The register file the thread starts with: entry in `pc`, the argument in `x[0]` and
    // the stack pointer in `x[31]`, with everything else zero. Built here because this is
    // the layer that knows Horizon's calling convention; the kernel only copies it.
    guest_state_t initial;
    const error_t initialized = guest_state_init(&initial);

    if (initialized != POUND_SUCCESS)
    {
        return initialized;
    }

    initial.pc    = entry;
    initial.x[0]  = argument;
    initial.x[31] = stack_bottom;

    hos_handle_t  handle = HOS_HANDLE_INVALID;
    const error_t error  = hos_process_create_thread(context->process, &initial, priority, &handle);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (!write_guest_u32(context, out_handle_address, handle))
    {
        // Created but undeliverable: give the handle straight back so the process does not
        // hold a thread it can never name.
        hos_process_close_handle(context->process, handle);
        state->x[0] = HOS_RESULT_INVALID_ADDRESS;
        return POUND_SUCCESS;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_start_thread(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (thread_call_needs_scheduler(context, "svcStartThread"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    const hos_handle_t handle = handle_argument(state->x[0]);
    uint64_t           id     = 0U;

    uint32_t      result = HOS_RESULT_SUCCESS;
    error_t       error  = hos_process_thread_id(context->process, handle, &id);

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // The status is read here rather than left to the kernel, because the kernel answers a
    // second start with an error code and this layer is the only place that can turn it
    // into the `Result` a guest compares against. Starting a thread that is already started
    // or has already finished is `0xFA01`, invalid state.
    hos_thread_status_t status = HOS_THREAD_CREATED;
    error                      = hos_kernel_thread_status(context->kernel, id, &status);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    if (status != HOS_THREAD_CREATED)
    {
        state->x[0] = HOS_RESULT_INVALID_STATE;
        return POUND_SUCCESS;
    }

    error = hos_kernel_thread_start(context->kernel, id);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    error = hos_scheduler_admit(context->scheduler, context->process, id);

    if (error != POUND_SUCCESS)
    {
        // Started but unschedulable, so it could never run: finish it rather than leave a
        // thread the guest believes is running and the scheduler has never heard of.
        hos_kernel_thread_terminate(context->kernel, id);
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_exit_thread(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    POUND_UNUSED(state);

    if (thread_call_needs_scheduler(context, "svcExitThread"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // The calling thread is the one the scheduler most recently ran. There is no handle in
    // the arguments to name it with, so the identifier is the only name available.
    const uint64_t id = hos_scheduler_current_thread_id(context->scheduler);

    if (id == 0U)
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting svcExitThread: no thread is currently running.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const error_t error = hos_kernel_thread_terminate(context->kernel, id);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // Returns nothing, and leaves `pc` on the `SVC`: the thread is over, and the last thing
    // it did is still where a debugger looks. The scheduler reaps the terminated thread on
    // its next decision.
    POUND_LOG_INFO(&thread_logger, "A guest thread called svcExitThread; it is finished.");
    return POUND_SUCCESS;
}

static error_t
svc_get_thread_priority(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (thread_call_needs_scheduler(context, "svcGetThreadPriority"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 is a pointer to the priority, X1 the handle.
    const uint64_t     out_priority_address = state->x[0];
    const hos_handle_t handle               = handle_argument(state->x[1]);
    uint64_t           id                   = 0U;

    uint32_t      result = HOS_RESULT_SUCCESS;
    error_t       error  = hos_process_thread_id(context->process, handle, &id);

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    uint32_t priority = 0U;
    error             = hos_kernel_thread_priority(context->kernel, id, &priority);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] = write_guest_u32(context, out_priority_address, priority) ? HOS_RESULT_SUCCESS
                                                                         : HOS_RESULT_INVALID_ADDRESS;
    return POUND_SUCCESS;
}

static error_t
svc_set_thread_priority(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (thread_call_needs_scheduler(context, "svcSetThreadPriority"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 is the handle, W1 the new priority. The order is the other
    // way round from `svcGetThreadPriority`, which is a real asymmetry and not a typo.
    const hos_handle_t handle   = handle_argument(state->x[0]);
    const uint32_t     priority = (uint32_t)state->x[1];

    if (priority > HOS_THREAD_PRIORITY_LOWEST)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcSetThreadPriority with priority %u: Horizon's own Result for a "
                        "priority outside %u..%u has not been established, and inventing one would look "
                        "like an answer.",
                        (unsigned int)priority,
                        (unsigned int)HOS_THREAD_PRIORITY_HIGHEST,
                        (unsigned int)HOS_THREAD_PRIORITY_LOWEST);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    uint64_t id = 0U;

    uint32_t      result = HOS_RESULT_SUCCESS;
    error_t       error  = hos_process_thread_id(context->process, handle, &id);

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    error = hos_kernel_thread_set_priority(context->kernel, id, priority);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] = HOS_RESULT_SUCCESS;
    return POUND_SUCCESS;
}

static error_t
svc_get_thread_id(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    if (thread_call_needs_scheduler(context, "svcGetThreadId"))
    {
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // Horizon's signature: X0 is a pointer to the identifier, X1 the handle. The answer is
    // 64 bits, so it goes through a 64-bit write even though the identifier is small.
    const uint64_t     out_thread_id_address = state->x[0];
    const hos_handle_t handle                = handle_argument(state->x[1]);
    uint64_t           id                    = 0U;

    uint32_t      result = HOS_RESULT_SUCCESS;
    error_t       error  = hos_process_thread_id(context->process, handle, &id);

    if (handle_failure_result(error, &result))
    {
        state->x[0] = result;
        return POUND_SUCCESS;
    }

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    state->x[0] =
        write_guest_u64(context, out_thread_id_address, id) ? HOS_RESULT_SUCCESS : HOS_RESULT_INVALID_ADDRESS;
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Calls that stop the guest
// -----------------------------------------------------------------------------

static error_t
svc_exit_process(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    POUND_UNUSED(state);

    // Reports nothing back: the process is over, and there is no instruction after this
    // one in it to read a result. `pc` is left on the `SVC` rather than advanced past it,
    // so the last thing the process did is still where a debugger looks.
    POUND_LOG_INFO(&thread_logger, "The guest called svcExitProcess; stopping the process.");
    context->outcome = HOS_SYSCALL_EXITED_PROCESS;
    return POUND_SUCCESS;
}

static error_t
svc_break(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // Logged at warning rather than error: unlike an unimplemented call, a break is the
    // guest doing exactly what it meant to, and the emulator's job is to get out of the
    // way with the reason visible.
    POUND_LOG_WARN(&thread_logger,
                   "The guest called svcBreak (reason 0x%llX, argument 0x%llX, argument 0x%llX).",
                   (unsigned long long)state->x[0],
                   (unsigned long long)state->x[1],
                   (unsigned long long)state->x[2]);

    context->outcome = HOS_SYSCALL_BREAK;
    return POUND_SUCCESS;
}

static error_t
svc_sleep_thread(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    const int64_t nanoseconds = (int64_t)state->x[0];

    if (nanoseconds <= 0)
    {
        // A yield: zero gives the rest of the slice to threads of the same priority, and a
        // negative duration gives it to every thread. Both are the same request to hand the
        // core over early, which is what a round-robin priority queue does with a move to
        // the back. Returns without sleeping, and returns nothing: `X0` keeps the duration
        // the guest passed, because `svcSleepThread` is a `void` call.
        if (context->scheduler != NULL)
        {
            hos_scheduler_yield(context->scheduler);
        }

        return POUND_SUCCESS;
    }

    if (context->scheduler == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcSleepThread(%lld): a sleep needs a scheduler to wake the caller, and "
                        "returning early would run the guest against a clock that did not advance.",
                        (long long)nanoseconds);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    // The deadline is on the scheduler's clock, not the wall: a sleep of half a second is
    // a number of ticks added to whatever the clock reads now, so it cannot outlive the
    // guest's own sense of time or drift against the instructions it retires.
    const uint64_t deadline =
        hos_scheduler_tick(context->scheduler) + hos_scheduler_ticks_from_nanoseconds((uint64_t)nanoseconds);

    const error_t error = hos_scheduler_sleep_current(context->scheduler, deadline);

    if (error != POUND_SUCCESS)
    {
        return error;
    }

    // Suspended, not finished: the run loop finishes it with `hos_syscall_complete_block`
    // when the clock reaches the deadline. `X0` is left holding the duration, untouched,
    // since a sleep reports nothing.
    context->outcome = HOS_SYSCALL_BLOCKED;
    return POUND_SUCCESS;
}

static error_t
svc_sleep_system(hos_syscall_context_t *POUND_RESTRICT context, guest_state_t *POUND_RESTRICT state)
{
    // Returns nothing. There is no system to put to sleep, so there is nothing to do and
    // nothing to approximate: the call has no effect a guest can observe.
    POUND_UNUSED(context);
    POUND_UNUSED(state);
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Dispatch
// -----------------------------------------------------------------------------

void
hos_syscall_context_init(hos_syscall_context_t *POUND_RESTRICT context,
                         hos_kernel_t *POUND_RESTRICT          kernel,
                         hos_process_t *POUND_RESTRICT         process,
                         guest_memory_t *POUND_RESTRICT        memory)
{
    if (context == NULL)
    {
        return;
    }

    memset(context, 0, sizeof(hos_syscall_context_t));
    context->kernel  = kernel;
    context->process = process;
    context->memory  = memory;

    // Zeroed tick and processor number, rather than whatever was on the stack: a caller
    // that has not said what the clock reads gets tick zero, which is at least a value
    // somebody can look at and ask about.
    context->outcome = HOS_SYSCALL_COMPLETED;
}

void
hos_syscall_context_set_scheduler(hos_syscall_context_t *POUND_RESTRICT context, hos_scheduler_t *POUND_RESTRICT scheduler)
{
    if (context == NULL)
    {
        return;
    }

    context->scheduler = scheduler;
}

error_t
hos_syscall_complete_block(hos_syscall_context_t *POUND_RESTRICT context,
                           guest_state_t *POUND_RESTRICT        state,
                           const hos_wake_t *POUND_RESTRICT     wake)
{
    if ((context == NULL) || (state == NULL) || (wake == NULL))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_syscall_complete_block: context, state or wake is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    switch (wake->reason)
    {
        case HOS_WAKE_SIGNALED:
            // A handle the thread was waiting on became ready. The index goes through the
            // pointer the blocking call left in X0, and only then is X0 replaced by the
            // `Result`: afterwards the address is gone.
            state->x[0] = write_guest_u32(context, state->x[0], wake->ready_index) ? HOS_RESULT_SUCCESS
                                                                                  : HOS_RESULT_INVALID_ADDRESS;
            break;

        case HOS_WAKE_TIMED_OUT:
            // Nothing became ready before the deadline. A timeout carries no index, so the
            // pointer is not written at all -- which is also what keeps a stale index from
            // pointing the guest at a handle it never asked about.
            state->x[0] = HOS_RESULT_TIMEOUT;
            break;

        case HOS_WAKE_SLEPT:
            // A plain sleep ended. `svcSleepThread` returns nothing, so X0 keeps the
            // duration the guest passed. Writing a `Result` here would be the one thing a
            // void call must not do.
            break;

        default:
            POUND_LOG_ERROR(&thread_logger,
                            "Aborting hos_syscall_complete_block: unknown wake reason %d.",
                            (int)wake->reason);
            return POUND_ERROR_INVALID_ARGUMENT;
    }

    context->outcome = HOS_SYSCALL_COMPLETED;

    // The suspension is over, so the trap is: the call finishes and `pc` steps past the
    // `SVC` it was left on when the thread parked.
    state->pc += 4U;
    return POUND_SUCCESS;
}

error_t
hos_syscall_dispatch(hos_syscall_context_t *POUND_RESTRICT context,
                     uint32_t                             number,
                     guest_state_t *POUND_RESTRICT        state)
{
    if ((context == NULL) || (state == NULL))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting hos_syscall_dispatch: context or state is NULL.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    // Reset first, so a caller that reads the outcome after a refusal sees this call's
    // outcome and not the last one's.
    context->outcome = HOS_SYSCALL_COMPLETED;

    if ((context->kernel == NULL) || (context->process == NULL) || (context->memory == NULL))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting svc 0x%02X: the context names no kernel, process or guest memory.",
                        (unsigned int)number);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    if (!hos_process_is_open(context->process))
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting svc 0x%02X: the calling process is not open, so there is no handle "
                        "table for the call to act on.",
                        (unsigned int)number);
        return POUND_ERROR_NOT_INITIALIZED;
    }

    if (context->process->kernel != context->kernel)
    {
        // The process's own kernel is the one its handles resolve in, so a context
        // pointing at a different one would have the call checked against one kernel and
        // run against another.
        POUND_LOG_ERROR(&thread_logger,
                        "Aborting svc 0x%02X: the context's kernel is not the calling process's kernel.",
                        (unsigned int)number);
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    const hos_syscall_entry_t *entry = find_syscall(number);

    if (entry == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svc 0x%02X: no Horizon OS system call has that number, so there is "
                        "nothing to dispatch it to.",
                        (unsigned int)number);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    if (entry->handler == NULL)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing %s (0x%02X): Pound has not implemented it.",
                        entry->name,
                        (unsigned int)number);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    const error_t error = entry->handler(context, state);

    if ((error == POUND_SUCCESS) && (context->outcome == HOS_SYSCALL_COMPLETED))
    {
        // The call is over, so the trap is: `pc` steps past the `SVC`. The two outcomes
        // that are not `COMPLETED` leave it alone on purpose -- see the header.
        state->pc += 4U;
    }

    return error;
}

/*** end of file ***/
