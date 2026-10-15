//! Horizon OS system call dispatch.
//!
//! An A64 `SVC` is a trap to a numbered kernel entry point: the immediate carries
//! the number, `X0..X7` carry the arguments, and `X0` (or a pointer it holds, or
//! `X0`/`X1`) carries the answer back. `interpreter_run` stops on one and reports
//! `INTERPRETER_TRAP_SVC` with the immediate; this module is what turns that
//! number into work, and it is the only thing in Pound that does.
//!
//! # The rule that decides what a failure looks like
//!
//! Every one of these calls can fail in two entirely different ways, and the
//! distinction is the whole design of the module:
//!
//! - **The guest asked for something Horizon itself refuses.** `svcSignalEvent`
//!   handed a session handle instead of a writable event is a real Horizon
//!   failure with a real, documented `Result` (`0xE401`, "the handle either does
//!   not exist, or is not a WritableEvent"). The call *completes*, `X0` gets that
//!   value, and the guest handles it exactly as it would on hardware. That is the
//!   emulation working.
//! - **Pound has not implemented it.** `svcSendSyncRequest` needs the CMIF wire
//!   format in the thread-local IPC buffer; `svcSetHeapSize` needs the memory
//!   layout. Neither exists yet. These do not complete: the call returns
//!   `POUND_ERROR_UNSUPPORTED_SYSCALL`, logs the number and its name, and leaves
//!   `X0` and `pc` alone so the trap is still standing when the log is read.
//!
//! Collapsing the second into the first would be the one thing this module must
//! not do. Handing back a plausible `Result` for a call nobody wrote would make a
//! title take a branch on an answer Pound invented, and the emulator would look
//! like firmware that replied. A refusal is a fact about Pound; a `Result` is a
//! fact about the guest, and only the guest's own failures produce one.
//!
//! # Where an answer goes
//!
//! Switch system calls do not all answer in `X0`. A call that carries a `Result`
//! puts it in `W0`; a call that returns an *output parameter* -- the ready index of
//! `svcWaitSynchronization`, the handle of `svcCreateThread` -- leaves a pointer in
//! `X0` and the callee writes through it. That makes `X0` overloaded: on entry it
//! is the pointer, and on return it is the `Result` that replaced it. This module
//! writes those pointers *before* it overwrites `X0`, since afterwards the address
//! is gone. A call that returns nothing at all (`svcExitThread`, `svcSleepThread`,
//! `svcFlushEntireDataCache`) must leave `X0` exactly as the guest left it.
//!
//! # Blocking
//!
//! `svcWaitSynchronization` and `svcSleepThread` can suspend the calling thread,
//! and this module is where that stops making it a poll. It polls first -- a wait
//! that can be satisfied *now* must never park a thread, because parking and
//! immediately waking is a reschedule the guest did not ask for and, on Horizon,
//! did not get. Only when the poll finds nothing does the call park the caller on
//! the scheduler and return `HOS_SYSCALL_BLOCKED`, leaving `pc` on the `SVC` and
//! the pointer in `X0` intact: the call is suspended mid-flight, not finished.
//!
//! The scheduler owns the wake. When it later makes the thread runnable it records
//! *why*, and the run loop -- the same piece of code that has the thread's register
//! file in hand -- calls `hos_syscall_complete_block`, which finishes the call:
//! writes the ready index through the pointer for a signal, puts `0xEA01` in `X0`
//! for a timeout, and does nothing to `X0` for a plain sleep. Then, and only then,
//! does `pc` step past the `SVC`.
//!
//! This only exists with a scheduler. A context that was never given one has no
//! clock and no run queue, so a call that would block is refused with
//! `POUND_ERROR_UNSUPPORTED_SYSCALL` and a log line: the same shape as any other
//! unimplemented feature, and for the same reason.
//!
//! # What is not here, and why
//!
//! Process objects (`svcGetProcessId`, `svcGetInfo`) need a process model beyond
//! the handle table. Memory-layout calls (`svcSetHeapSize`, `svcMapMemory`,
//! `svcQueryMemory`) belong to the memory-layout slice. IPC (`svcSendSyncRequest`,
//! `svcReplyAndReceive`) needs the CMIF wire format and the thread-local command
//! buffer at `TPIDR_EL0 + 0x80`. Each returns `POUND_ERROR_UNSUPPORTED_SYSCALL`
//! and says so in the log.
//!
//! # Result values
//!
//! The `HOS_RESULT_*` values below are the raw 32-bit `Result` words a title
//! compares against -- the values documented for these calls on the Switch
//! homebrew wiki, and the ones libnx and Atmosphère name. They are written as
//! literals rather than re-derived from the module/description bit encoding,
//! because the encoding has been revised across firmware versions while the words
//! a title compares against have not, and because a mistake in re-deriving them
//! would produce a value that is off by exactly the module field and therefore
//! looks plausible.

#ifndef POUND_HOS_SYSCALL_H
#define POUND_HOS_SYSCALL_H

#include "attributes.h"
#include "errors.h"
#include "guest_state.h"
#include "hos/kernel.h"
#include "hos/scheduler.h"
#include "memory/guest_memory.h"
#include <stdbool.h>
#include <stdint.h>

// -----------------------------------------------------------------------------
// System call numbers
// -----------------------------------------------------------------------------

/// The `SVC` immediates this module implements.
///
/// Only the numbers that are implemented are named. A number outside this list is
/// refused by name (`"svc 0x2D"`), so the list doubles as the documentation of
/// what Pound answers.
typedef enum
{
    /// Ends the calling process. Returns nothing.
    HOS_SVC_EXIT_PROCESS = 0x07,

    /// Creates a thread and returns a handle to it through the pointer in `X0`.
    HOS_SVC_CREATE_THREAD = 0x08,

    /// Starts a thread, making it schedulable.
    HOS_SVC_START_THREAD = 0x09,

    /// Ends the calling thread. Returns nothing.
    HOS_SVC_EXIT_THREAD = 0x0A,

    /// Yields, or (for a positive duration) sleeps. Returns nothing.
    HOS_SVC_SLEEP_THREAD = 0x0B,

    /// Answers a thread's priority through the pointer in `X0`.
    HOS_SVC_GET_THREAD_PRIORITY = 0x0C,

    /// Changes a thread's priority.
    HOS_SVC_SET_THREAD_PRIORITY = 0x0D,

    /// Answers which core the caller is running on.
    HOS_SVC_GET_CURRENT_PROCESSOR_NUMBER = 0x10,

    /// Signals an event.
    HOS_SVC_SIGNAL_EVENT = 0x11,

    /// Takes an event out of the signaled state.
    HOS_SVC_CLEAR_EVENT = 0x12,

    /// Gives up one handle.
    HOS_SVC_CLOSE_HANDLE = 0x16,

    /// Clears an event, reporting whether it was signaled.
    HOS_SVC_RESET_SIGNAL = 0x17,

    /// Waits for one of several handles, writing the ready index through the pointer in
    /// `X0`. Polls, and with a scheduler parks the caller when nothing is ready.
    HOS_SVC_WAIT_SYNCHRONIZATION = 0x18,

    /// Answers the system tick counter.
    HOS_SVC_GET_SYSTEM_TICK = 0x1E,

    /// Opens a session on a named service, by name.
    HOS_SVC_CONNECT_TO_NAMED_PORT = 0x1F,

    /// Answers a thread's kernel-wide identifier through the pointer in `X0`.
    HOS_SVC_GET_THREAD_ID = 0x25,

    /// Stops the guest deliberately, so a debugger can attach. Returns nothing.
    HOS_SVC_BREAK = 0x26,

    /// Writes a guest string to the emulator's log.
    HOS_SVC_OUTPUT_DEBUG_STRING = 0x27,

    /// Discards the whole data cache. Returns nothing; a host has no guest cache.
    HOS_SVC_FLUSH_ENTIRE_DATA_CACHE = 0x2A,

    /// Discards a range of the data cache.
    HOS_SVC_FLUSH_DATA_CACHE = 0x2B,

    /// Enters the system sleep state. Returns nothing; there is nothing to sleep.
    HOS_SVC_SLEEP_SYSTEM = 0x4D,
} hos_svc_t;

// -----------------------------------------------------------------------------
// Result values
// -----------------------------------------------------------------------------

/// Horizon's success `Result`. Every synchronous call returns one of these words.
#define HOS_RESULT_SUCCESS 0x00000000U

/// The handle either does not name a live object or names the wrong kind of one
/// for this call -- a writable-event handle where an event is needed, for example.
#define HOS_RESULT_INVALID_HANDLE 0x0000E401U

/// A guest pointer the call needs to read or write is not mapped.
#define HOS_RESULT_INVALID_ADDRESS 0x0000E601U

/// A wait finished without anything it was waiting for becoming ready.
#define HOS_RESULT_TIMEOUT 0x0000EA01U

/// More handles were passed to a wait than the kernel accepts.
#define HOS_RESULT_TOO_MANY_HANDLES 0x0000EE01U

/// The object is in a state this call cannot act on: `svcResetSignal` on an event
/// that is not signaled, or on a readable event rather than a writable one.
#define HOS_RESULT_INVALID_STATE 0x0000FA01U

// -----------------------------------------------------------------------------
// Limits
// -----------------------------------------------------------------------------

/// The most handles `svcWaitSynchronization` accepts, which is Horizon's own cap.
#define HOS_SYSCALL_MAX_WAIT_HANDLES 0x40U

/// The longest service name `svcConnectToNamedPort` accepts, matching the eight
/// bytes a Horizon service name occupies on the wire.
#define HOS_SYSCALL_SERVICE_NAME_MAX 8U

/// How much of an `svcOutputDebugString` payload is written to the log. A guest
/// that logs a megabyte in one call should not cost the log a megabyte of file.
#define HOS_SYSCALL_DEBUG_STRING_LOG_MAX 512U

// -----------------------------------------------------------------------------
// Dispatch
// -----------------------------------------------------------------------------

/// What happened to the instruction after the call was made.
typedef enum
{
    /// The call finished. `X0` holds its answer, and `pc` has advanced past the
    /// `SVC`. Any call that returns a `Result` also returns this outcome, whether
    /// that `Result` is success or a Horizon failure: to the guest, being told
    /// `0xE401` and being told success are the same event.
    HOS_SYSCALL_COMPLETED = 0,

    /// `svcExitProcess` ran. There is no further instruction to run in this
    /// process, so `pc` and `X0` are untouched.
    HOS_SYSCALL_EXITED_PROCESS,

    /// `svcBreak` ran: the guest asked to stop. The emulator's job is to hand the
    /// stop to whatever is debugging it, so `pc` is left on the `SVC` and `X0` is
    /// untouched, leaving the call visible.
    HOS_SYSCALL_BREAK,

    /// The call suspended the calling thread instead of finishing it. `pc` is left
    /// on the `SVC` and the argument registers are exactly as the call left them,
    /// because the call has not finished: the scheduler has parked the thread, and
    /// whoever runs it next must first call `hos_syscall_complete_block` with the
    /// wake it was given.
    HOS_SYSCALL_BLOCKED,
} hos_syscall_outcome_t;

/// Everything a system call needs that is not in the guest's registers.
///
/// This is plain data with no ownership: the caller owns the kernel, the process
/// and the memory it points at, and all three must outlive the call.
POUND_ALIGNED(64) typedef struct
{
    /// The kernel whose objects the calling process's handles name.
    hos_kernel_t *kernel;

    /// The process making the call. Its handle table is what handles resolve in.
    hos_process_t *process;

    /// The guest address space, for every pointer passed as an argument.
    guest_memory_t *memory;

    /// The scheduler that parks a thread this layer blocks, or NULL.
    ///
    /// Optional, and set with `hos_syscall_context_set_scheduler`. Without it the
    /// context has no clock and no run queue, so a call that would block is refused
    /// rather than shortened into a poll: a wait that answered `0xEA01` early and a
    /// sleep that returned before its duration would each tell the guest something
    /// that has not happened.
    hos_scheduler_t *scheduler;

    /// What `svcGetSystemTick` reports. The caller advances this, because the tick
    /// is the emulator's clock and this module has no clock of its own.
    uint64_t system_tick;

    /// What `svcGetCurrentProcessorNumber` reports.
    uint32_t processor_id;

    /// Where the last dispatched call ended up. Set by `hos_syscall_dispatch`, and
    /// reset to `HOS_SYSCALL_COMPLETED` at the start of every call, so a caller
    /// checking it cannot see a previous call's exit.
    hos_syscall_outcome_t outcome;

    char pad[16];
} hos_syscall_context_t;

/// The symbolic name of a system call, for logs and for tests.
///
/// Answers for every number the table carries, implemented or not, because the
/// name is most useful precisely where the call is *not* implemented: it is what
/// turns `svc 0x21` into `svcSendSyncRequest (0x21)` in a refusal. Returns `NULL`
/// only for a number Horizon does not define. Never allocates.
const char *hos_syscall_name(uint32_t number);

/// Points `context` at the kernel, process and memory a call will run against.
///
/// Zeroes everything else, including `system_tick` and `processor_id`: a caller
/// that has not thought about the clock should get tick zero rather than whatever
/// was on the stack.
void hos_syscall_context_init(hos_syscall_context_t *POUND_RESTRICT context,
                              hos_kernel_t *POUND_RESTRICT          kernel,
                              hos_process_t *POUND_RESTRICT         process,
                              guest_memory_t *POUND_RESTRICT        memory);

/// Points `context` at the scheduler that will park a thread it blocks.
///
/// Deliberately separate from `hos_syscall_context_init`: a scheduler is optional, and a
/// caller that only polls -- a test harness, or a core with no run queue of its own --
/// leaves it unset and gets a refusal, rather than a shortened call, for anything that
/// must block. Passing NULL puts the context back to that state.
void hos_syscall_context_set_scheduler(hos_syscall_context_t *POUND_RESTRICT context,
                                       hos_scheduler_t *POUND_RESTRICT      scheduler);

/// Finishes a blocked system call after the scheduler woke the thread that made it.
///
/// Called with the thread's register file exactly as `hos_syscall_dispatch` left it when
/// it reported `HOS_SYSCALL_BLOCKED`, so the pointer the suspended call needs is still in
/// `X0`. `wake` says what ended the wait and that is what chooses the answer:
///
/// - `HOS_WAKE_SIGNALED` -- one of the waited handles became ready. Its index is written
///   through the pointer in `X0`, and then `X0` is replaced with `HOS_RESULT_SUCCESS`.
/// - `HOS_WAKE_TIMED_OUT` -- nothing was ready before the deadline. `X0` becomes
///   `HOS_RESULT_TIMEOUT`, and the pointer is not written: a timeout has no index to
///   report.
/// - `HOS_WAKE_SLEPT` -- a plain sleep ended. `svcSleepThread` returns nothing, so `X0`
///   is left exactly as the guest left it.
///
/// Every wake ends the call, so `pc` steps past the `SVC`.
///
/// Why the wake reason is enough to know what to write, without the scheduler having to
/// remember which call parked the thread: only `svcWaitSynchronization` ever parks with a
/// deadline or on handles, and only `svcSleepThread` ever parks with nothing to wait on,
/// so `HOS_WAKE_SLEPT` is a sleep and the other two are a wait. Keeping that mapping here
/// rather than in the scheduler is what lets the scheduler stay ignorant of system call
/// registers.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument.
error_t hos_syscall_complete_block(hos_syscall_context_t *POUND_RESTRICT context,
                                   guest_state_t *POUND_RESTRICT        state,
                                   const hos_wake_t *POUND_RESTRICT     wake);

/// Runs system call `number` on behalf of `context`'s process.
///
/// `state` supplies the argument registers and receives the answer. On
/// `POUND_SUCCESS`, `context->outcome` says what happened: `pc` has advanced past
/// the `SVC` for a completed call, and is left on it for the outcomes that are not
/// completions -- a process exit, a break, and a block, which is a suspension that
/// `hos_syscall_complete_block` will finish later.
///
/// Returns:
/// - `POUND_SUCCESS` -- the call ran: successfully, with a Horizon `Result` the
///   guest will handle, or by parking the caller (`HOS_SYSCALL_BLOCKED`).
/// - `POUND_ERROR_UNSUPPORTED_SYSCALL` -- the number, or a feature inside it, is
///   not implemented. Logged. `X0` and `pc` are as the guest left them. A call
///   that must block is this when the context has no scheduler.
/// - `POUND_ERROR_NOT_FOUND` -- the service named to `svcConnectToNamedPort` is
///   not registered with this kernel. Never logged: a title probing for a service
///   a firmware version does not have is asking a question, and the answer to it
///   is this code. The system call number stays in the caller's hands, so a caller
///   that wants the refusal seen is the one that has to report it.
/// - `POUND_ERROR_INVALID_ARGUMENT` -- a NULL `context`, `state`, or a `context`
///   that was never pointed at a kernel, process and memory.
/// - `POUND_ERROR_NOT_INITIALIZED` -- the context's process is not open, which
///   means the emulator dispatched a call for a process that has already gone.
error_t hos_syscall_dispatch(hos_syscall_context_t *POUND_RESTRICT context,
                             uint32_t number,
                             guest_state_t *POUND_RESTRICT state);

#endif // POUND_HOS_SYSCALL_H

/*** end of file ***/
