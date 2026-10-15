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
static error_t svc_sleep_thread(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_current_processor_number(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_signal_event(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_clear_event(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_close_handle(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_reset_signal(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_wait_synchronization(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_get_system_tick(hos_syscall_context_t *context, guest_state_t *state);
static error_t svc_connect_to_named_port(hos_syscall_context_t *context, guest_state_t *state);
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
    { HOS_SVC_SLEEP_THREAD, "svcSleepThread", svc_sleep_thread },
    { HOS_SVC_GET_CURRENT_PROCESSOR_NUMBER, "svcGetCurrentProcessorNumber", svc_get_current_processor_number },
    { HOS_SVC_SIGNAL_EVENT, "svcSignalEvent", svc_signal_event },
    { HOS_SVC_CLEAR_EVENT, "svcClearEvent", svc_clear_event },
    { HOS_SVC_CLOSE_HANDLE, "svcCloseHandle", svc_close_handle },
    { HOS_SVC_RESET_SIGNAL, "svcResetSignal", svc_reset_signal },
    { HOS_SVC_WAIT_SYNCHRONIZATION, "svcWaitSynchronization", svc_wait_synchronization },
    { HOS_SVC_GET_SYSTEM_TICK, "svcGetSystemTick", svc_get_system_tick },
    { HOS_SVC_CONNECT_TO_NAMED_PORT, "svcConnectToNamedPort", svc_connect_to_named_port },
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
    { 0x08U, "svcCreateThread", NULL },
    { 0x09U, "svcStartThread", NULL },
    { 0x0AU, "svcExitThread", NULL },
    { 0x0CU, "svcGetThreadPriority", NULL },
    { 0x0DU, "svcSetThreadPriority", NULL },
    { 0x13U, "svcMapSharedMemory", NULL },
    { 0x14U, "svcUnmapSharedMemory", NULL },
    { 0x15U, "svcCreateTransferMemory", NULL },
    { 0x19U, "svcCancelSynchronization", NULL },
    { 0x20U, "svcSendSyncRequestLight", NULL },
    { 0x21U, "svcSendSyncRequest", NULL },
    { 0x22U, "svcSendSyncRequestWithUserBuffer", NULL },
    { 0x24U, "svcGetProcessId", NULL },
    { 0x25U, "svcGetThreadId", NULL },
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
    const uint64_t handles_address = state->x[1];
    const int32_t  count           = (int32_t)(uint32_t)state->x[2];
    const int64_t  timeout         = (int64_t)state->x[3];

    if (count == 0)
    {
        // Zero handles is the "wait forever" form, which is the one behaviour a poll
        // cannot imitate: returning a timeout would tell the guest something that has
        // not happened.
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcWaitSynchronization with no handles: waiting forever needs a "
                        "scheduler to wake the caller.");
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    if ((count < 0) || ((uint32_t)count > HOS_SYSCALL_MAX_WAIT_HANDLES))
    {
        state->x[0] = HOS_RESULT_TOO_MANY_HANDLES;
        return POUND_SUCCESS;
    }

    if (timeout != 0)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "Refusing svcWaitSynchronization for %lld ns: only the zero-timeout poll is "
                        "implemented, and reporting a timeout before it expires would be a lie the "
                        "guest cannot detect.",
                        (long long)timeout);
        return POUND_ERROR_UNSUPPORTED_SYSCALL;
    }

    const uint8_t *bytes = NULL;

    if (!read_guest(context, handles_address, (size_t)(uint32_t)count * sizeof(uint32_t), &bytes))
    {
        state->x[0] = HOS_RESULT_INVALID_ADDRESS;
        return POUND_SUCCESS;
    }

    for (uint32_t index = 0U; index < (uint32_t)count; index++)
    {
        const hos_handle_t handle = (hos_handle_t)read_u32(bytes + ((size_t)index * sizeof(uint32_t)));

        hos_object_type_t type  = HOS_OBJECT_EVENT;
        error_t           error = hos_process_handle_type(context->process, handle, &type);

        if (error == POUND_ERROR_INVALID_HANDLE)
        {
            // Horizon reports an invalid handle in the array as the call's failure,
            // rather than skipping it: a wait over a bad handle can never be satisfied.
            state->x[0] = HOS_RESULT_INVALID_HANDLE;
            return POUND_SUCCESS;
        }

        if (error != POUND_SUCCESS)
        {
            return error;
        }

        if (type != HOS_OBJECT_EVENT)
        {
            // A session is a legitimate thing to wait on -- it becomes ready when a
            // message is waiting -- so this is not an error and not a refusal. Nothing
            // queues a message in this slice, so a session is simply never the ready
            // one, and the call goes on to the next handle. That is the right answer for
            // a guest waiting on a session and an event at once, which is a normal shape
            // for a title's main loop.
            continue;
        }

        bool signaled = false;
        error         = hos_process_event_wait(context->process, handle, &signaled);

        if (error != POUND_SUCCESS)
        {
            return error;
        }

        if (signaled)
        {
            state->x[1] = index;
            state->x[0] = HOS_RESULT_SUCCESS;
            return POUND_SUCCESS;
        }
    }

    state->x[0] = HOS_RESULT_TIMEOUT;
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
        state->x[0] = HOS_RESULT_SUCCESS;
        state->x[1] = handle;
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
    POUND_UNUSED(context);

    const int64_t nanoseconds = (int64_t)state->x[0];

    if ((nanoseconds == 0) || (nanoseconds == -1) || (nanoseconds == -2))
    {
        // The three specified yields, all of which return without sleeping: zero gives
        // the rest of the slice to threads of the same priority, -1 and -2 give it to any
        // thread at all. With no scheduler there is nothing to hand anything to, which
        // makes a yield exactly the no-op it looks like -- and unlike a sleep, that is
        // the correct behaviour rather than an approximation of it.
        return POUND_SUCCESS;
    }

    POUND_LOG_ERROR(&thread_logger,
                    "Refusing svcSleepThread(%lld): only the yields are implemented, because a sleep "
                    "needs a scheduler to wake the caller and returning early would run the guest "
                    "against a clock that did not advance.",
                    (long long)nanoseconds);
    return POUND_ERROR_UNSUPPORTED_SYSCALL;
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
