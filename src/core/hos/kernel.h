//! Horizon OS kernel objects, handles and services.
//!
//! Horizon OS does not expose its kernel as a set of calls that mutate global state.
//! It exposes a *namespace of handles*, and every subsystem a title talks to -- the
//! filesystem, the applet manager, the GPU driver -- is reached by first connecting to
//! a named service and then sending that service numbered requests. A handle is the
//! unit of authority: a title can do exactly what its handles let it do, and closing
//! one is how that authority is given back.
//!
//! This module is that layer and nothing above it. It owns three things:
//!
//! - **Objects** -- kernel-side things a handle can name. This slice implements events
//!   and sessions. Objects live in one kernel-wide table and are reference-counted by
//!   the number of handles that name them, so an object outlives any single process's
//!   handle and dies only when the last one closes.
//! - **Handles** -- one process's names for objects. A handle is an index into that
//!   process's own table, which is why the same object has a different handle in each
//!   process that holds one, and why two processes routinely hand out the same handle
//!   *value* for two different objects.
//! - **Services** -- the named request handlers a session can be opened against: the
//!   kernel-side half of "connect to `fsp-srv`".
//!
//! # Handle values are generation-checked
//!
//! A handle is 32 bits: a 20-bit slot index in the low bits and a 12-bit generation in
//! the high bits. Zero is not a valid handle, so a zeroed `hos_handle_t` is never
//! mistaken for a live one.
//!
//! The generation is the point. Closing a handle frees its slot, and the next opening
//! reuses it -- so without a generation, a handle that was closed and then copied
//! around would silently name whatever object landed in the slot afterwards. That is
//! the worst possible failure: not a crash, but a title reading another title's data
//! through a name it thought it had given back. Bumping the slot's generation on close
//! makes the stale handle fail to resolve, and it fails as a lookup miss rather than as
//! a wrong object.
//!
//! Twelve bits of generation wrap after 4096 close/open cycles of the *same slot*. The
//! wrap is not repairable in 32 bits, and 32 bits is what the guest passes around; what
//! matters is that the window in which a stale handle works again is bounded by slot
//! reuse rather than being the common case.
//!
//! # One lock, taken everywhere, and released before a handler runs
//!
//! The objects, the services and every process's handle table all sit behind a single
//! kernel mutex. Splitting it would give handle operations a lock order -- the target
//! process's table inside the kernel's -- that the object path would then have to take
//! in reverse, which is a deadlock waiting to happen for no gain this layer cares about:
//! nothing here is on the JIT dispatch path, so the lock is not the thing worth
//! optimising.
//!
//! The one place the lock is *not* held is a service handler. A handler is guest-code
//! policy and routinely wants to signal an event or send the next request, so calling it
//! under a non-recursive lock would deadlock the first handler that did. The service
//! entry is copied out under the lock, the lock is dropped, and the handler runs with the
//! kernel free for re-entry.
//!
//! # What this slice does not do
//!
//! - **No blocking waits and no scheduler.** `hos_process_event_wait` polls; a wait that
//!   blocks belongs to the scheduler slice, which is what would have to park and wake the
//!   guest thread.
//! - **No shared-memory or port objects.** Only the two object kinds above are
//!   implemented. An encoding outside them is refused rather than half-handled.
//! - **No request marshalling.** `hos_process_session_request` carries an opaque byte
//!   span and a command number. The Horizon IPC wire format -- headers, handle
//!   descriptors, the domain protocol -- is the syscall layer's job.
//! - **No service removal.** Services are append-only, which is what makes it safe to
//!   keep calling a handler after the lock is dropped: the entry cannot move or vanish.

#ifndef POUND_HOS_KERNEL_H
#define POUND_HOS_KERNEL_H

#include "attributes.h"
#include "errors.h"
#include "sync/mutex.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Longest service name, in characters, excluding the terminating NUL.
///
/// Horizon service names are exactly eight bytes on the wire, space-padded. A name
/// handed to this layer is a C string of one to eight characters and is compared
/// exactly; the padding is a marshalling concern for the IPC layer, not a property of
/// the name.
#define HOS_SERVICE_NAME_MAX 8U

/// The handle that names no object.
///
/// Zero, so that a zeroed `hos_handle_t` is a well-defined miss rather than an
/// accidental reference to the first slot of a table.
#define HOS_HANDLE_INVALID 0U

/// Number of low bits of a handle that carry the slot index.
///
/// The remaining high bits carry the slot's generation. See the module comment.
#define HOS_HANDLE_INDEX_BITS 20U

/// Number of objects a fresh kernel's table holds before it first grows.
#define HOS_KERNEL_INITIAL_OBJECTS 8U

/// Number of services a fresh kernel's table holds before it first grows.
#define HOS_KERNEL_INITIAL_SERVICES 4U

/// Number of handles a fresh process's table holds before it first grows.
#define HOS_PROCESS_INITIAL_HANDLES 8U

/// What a handle names.
typedef enum
{
    /// A signal a process can set, clear and wait on.
    HOS_OBJECT_EVENT = 0,

    /// A live connection to a registered service.
    HOS_OBJECT_SESSION = 1,
} hos_object_type_t;

/// What a wait on an event does to it.
typedef enum
{
    /// Stays signalled until something clears it explicitly.
    HOS_EVENT_MANUAL = 0,

    /// Cleared by the first wait that observes it signalled, so one signal wakes one
    /// waiter. This is Horizon's default clear mode.
    HOS_EVENT_AUTO = 1,
} hos_event_mode_t;

/// A process's name for an object.
///
/// Opaque by type and comparable by value, but not meaningful across processes: a
/// handle returned by one process names nothing in another, and passing one to a
/// different process is not detectable here. That is not a gap to be closed -- it is how
/// a per-process handle namespace works, and Horizon has the same property.
typedef uint32_t hos_handle_t;

/// Handles one command sent over a session.
///
/// `context` is whatever was registered with the service. `request` and `response` are
/// the caller's spans and are never retained. A handler must not write more than
/// `response_capacity` bytes; if the reply does not fit it returns an error instead of
/// truncating, because a truncated reply the caller cannot detect is worse than a
/// refusal. `command` is the guest's request number, passed through unread.
///
/// Called with the kernel lock released, so a handler may call back into this module.
/// See the module comment.
typedef error_t (*hos_service_handler_fn)(void *context,
                                          uint32_t command,
                                          const uint8_t *request,
                                          size_t request_size,
                                          uint8_t *response,
                                          size_t response_capacity,
                                          size_t *out_response_size);

/// The layout of an object, a service entry and a handle slot.
///
/// All three are incomplete here on purpose: their fields are this module's business,
/// and a caller that could see them could make one out of step with the others. The
/// tables below are pointers to them so that a kernel can be declared without them.
struct hos_object;
struct hos_service;
struct hos_handle_slot;

/// The kernel: the objects, the services and the lock over both.
///
/// Caller-owned. `hos_kernel_init` prepares it and `hos_kernel_destroy` releases it;
/// both are safe on a zeroed struct, so declaring one and calling destroy unconditionally
/// is correct.
///
/// A process holds a pointer to its kernel, so the kernel must outlive every process
/// created from it. `hos_kernel_destroy` enforces that rather than trusting it: it
/// refuses while a process is still open.
typedef struct
{
    /// Set by `hos_kernel_init` and cleared by `hos_kernel_destroy`. Anything else means
    /// the struct was never initialised, or was initialised and then torn down.
    uint32_t magic;

    /// Guards every field below and every handle table of every process on this kernel.
    mutex_t lock;

    /// Object slots, `object_capacity` of them. A slot is live when its reference count
    /// is non-zero; `object_count` is how many are.
    struct hos_object *POUND_RESTRICT objects;
    uint32_t                         object_capacity;
    uint32_t                         object_count;

    /// Service entries, `service_count` of them. Append-only, so an entry never moves.
    struct hos_service *POUND_RESTRICT services;
    uint32_t                          service_capacity;
    uint32_t                          service_count;

    /// Number of processes created from this kernel and not yet closed.
    uint32_t live_processes;
} hos_kernel_t;

/// A process's handle table.
///
/// Caller-owned, like the kernel. One process's handles are invisible to another, which
/// is the whole point of the table living here rather than in the kernel.
typedef struct
{
    /// Set by `hos_process_create` and cleared by `hos_process_close`.
    uint32_t magic;

    /// The kernel this process was created from, which owns the objects its handles
    /// name. Must outlive the process.
    hos_kernel_t *POUND_RESTRICT kernel;

    /// Handle slots, `handle_capacity` of them. `handle_count` are in use.
    struct hos_handle_slot *POUND_RESTRICT handles;
    uint32_t                              handle_capacity;
    uint32_t                              handle_count;
} hos_process_t;

// -----------------------------------------------------------------------------
// Kernel lifecycle
// -----------------------------------------------------------------------------

/// Prepares `kernel` to hold objects, services and processes.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if `kernel` is NULL,
/// `POUND_ERROR_ALREADY_INITIALIZED` if it is already initialised, and
/// `POUND_ERROR_ALLOCATION_FAILED` if the initial tables could not be allocated -- in
/// which case the kernel is left uninitialised, so `hos_kernel_destroy` on it is a
/// no-op rather than a double free.
error_t hos_kernel_init(hos_kernel_t *POUND_RESTRICT kernel);

/// Releases the kernel's tables.
///
/// Safe on a zeroed struct and safe to call twice. Refuses to tear down a kernel that
/// still has open processes, returning `POUND_ERROR_BUSY` and logging which count is
/// outstanding, because those processes hold pointers into the tables this would free.
/// The refusal is the point: an ordering mistake surfaces as one loud error at teardown
/// instead of as a use-after-free much later.
error_t hos_kernel_destroy(hos_kernel_t *POUND_RESTRICT kernel);

/// Whether `kernel` is initialised and has not been destroyed.
bool hos_kernel_is_initialized(const hos_kernel_t *POUND_RESTRICT kernel);

/// Number of live objects, summed over every process.
///
/// For assertions and diagnostics. An object is live while at least one handle names it.
uint32_t hos_kernel_object_count(const hos_kernel_t *POUND_RESTRICT kernel);

/// Number of processes created from this kernel and not yet closed.
uint32_t hos_kernel_live_process_count(const hos_kernel_t *POUND_RESTRICT kernel);

// -----------------------------------------------------------------------------
// Services
// -----------------------------------------------------------------------------

/// Registers `handler` under `name`.
///
/// `name` is a C string of one to `HOS_SERVICE_NAME_MAX` characters.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if any argument is NULL or the name is empty or
/// too long, `POUND_ERROR_ALREADY_INITIALIZED` if a service of that name is already
/// registered -- names are the namespace a title connects by, so a second registration
/// would silently redirect every existing session -- `POUND_ERROR_NOT_INITIALIZED` if the
/// kernel is not initialised, and `POUND_ERROR_ALLOCATION_FAILED` if the service table
/// could not grow.
///
/// The context is borrowed, not owned: it must outlive the kernel, and this layer never
/// frees it.
error_t hos_kernel_register_service(hos_kernel_t *POUND_RESTRICT kernel,
                                    const char *POUND_RESTRICT name,
                                    hos_service_handler_fn handler,
                                    void *context);

/// Whether a service called `name` is registered.
///
/// A miss is an ordinary answer -- a title probing for a service a firmware version
/// does not have -- so this does not log.
bool hos_kernel_has_service(const hos_kernel_t *POUND_RESTRICT kernel, const char *POUND_RESTRICT name);

/// Number of registered services.
uint32_t hos_kernel_service_count(const hos_kernel_t *POUND_RESTRICT kernel);

/// Copies the name of the service at `index` into `out`, which must have room for
/// `HOS_SERVICE_NAME_MAX + 1` characters.
///
/// Returns false if the kernel is not initialised or `index` is out of range, leaving
/// `out` untouched.
bool hos_kernel_service_name_at(const hos_kernel_t *POUND_RESTRICT kernel,
                                uint32_t                           index,
                                char *POUND_RESTRICT                out);

// -----------------------------------------------------------------------------
// Process lifecycle
// -----------------------------------------------------------------------------

/// Creates a process with an empty handle table, belonging to `kernel`.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if either argument is NULL,
/// `POUND_ERROR_NOT_INITIALIZED` if the kernel is not initialised,
/// `POUND_ERROR_ALREADY_INITIALIZED` if `process` already holds an open process, and
/// `POUND_ERROR_ALLOCATION_FAILED` if the handle table could not be allocated.
error_t hos_process_create(hos_kernel_t *POUND_RESTRICT kernel, hos_process_t *POUND_RESTRICT process);

/// Closes every handle the process holds and releases its table.
///
/// Safe on a zeroed struct and safe to call twice. Each handle drops one reference to
/// its object, so an object another process still names survives; only the last
/// reference destroys it.
///
/// A process must not be closed while another thread is inside any `hos_process_*` entry
/// point on it, and must not be used after this returns. Both are the caller's contract,
/// as with any other object whose memory is released.
void hos_process_close(hos_process_t *POUND_RESTRICT process);

/// Whether `process` holds an open handle table.
bool hos_process_is_open(const hos_process_t *POUND_RESTRICT process);

/// Number of handles the process currently holds.
uint32_t hos_process_handle_count(const hos_process_t *POUND_RESTRICT process);

// -----------------------------------------------------------------------------
// Handles
// -----------------------------------------------------------------------------

/// Creates an event and returns a handle to it.
///
/// An auto-reset event that is created signalled has already consumed its signal the
/// moment anything waits on it, which is why the initial state is an argument rather
/// than an afterthought.
///
/// Returns `POUND_ERROR_INVALID_ARGUMENT` if `process` or `out_handle` is NULL,
/// `POUND_ERROR_NOT_INITIALIZED` if the process is not open,
/// `POUND_ERROR_ALLOCATION_FAILED` if a table could not grow.
error_t hos_process_create_event(hos_process_t *POUND_RESTRICT process,
                                 hos_event_mode_t             mode,
                                 bool                         initially_signaled,
                                 hos_handle_t *POUND_RESTRICT out_handle);

/// Opens a session with the service called `name` and returns a handle to it.
///
/// Returns `POUND_ERROR_NOT_FOUND` if no service has that name -- logged by nothing,
/// because a title probing for an absent service is ordinary -- plus the errors
/// `hos_process_create_event` lists for the arguments and the allocation.
error_t hos_process_connect(hos_process_t *POUND_RESTRICT process,
                            const char *POUND_RESTRICT name,
                            hos_handle_t *POUND_RESTRICT out_handle);

/// Copies `handle` into `target`, giving the target its own name for the same object.
///
/// The object's reference count rises by one, so closing `handle` in the source no longer
/// destroys it. Both processes must have been created from the same kernel, or the target
/// would hold a handle into another kernel's object table; that is
/// `POUND_ERROR_INVALID_ARGUMENT`.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` if `handle` names nothing live in `source`,
/// `POUND_ERROR_INVALID_ARGUMENT` for a NULL argument or a kernel mismatch,
/// `POUND_ERROR_NOT_INITIALIZED` if either process is not open, and
/// `POUND_ERROR_ALLOCATION_FAILED` if the target's table could not grow.
error_t hos_process_duplicate_handle_into(hos_process_t *POUND_RESTRICT source,
                                          hos_handle_t                 handle,
                                          hos_process_t *POUND_RESTRICT target,
                                          hos_handle_t *POUND_RESTRICT out_handle);

/// Copies `handle` into a second handle in the same process.
///
/// The two handles are independent: closing either leaves the object alive through the
/// other. Returns what `hos_process_duplicate_handle_into` returns.
error_t hos_process_duplicate_handle(hos_process_t *POUND_RESTRICT process,
                                     hos_handle_t                 handle,
                                     hos_handle_t *POUND_RESTRICT out_handle);

/// Closes `handle`, dropping one reference to the object it names.
///
/// The slot's generation advances, so this handle value -- and any copy of it -- stops
/// naming anything, even if the slot is reopened for a different object. When the last
/// reference goes the object is destroyed.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` if `handle` names nothing live in `process`,
/// `POUND_ERROR_INVALID_ARGUMENT` if `process` is NULL, and
/// `POUND_ERROR_NOT_INITIALIZED` if the process is not open.
error_t hos_process_close_handle(hos_process_t *POUND_RESTRICT process, hos_handle_t handle);

/// Reports what `handle` names.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` if it names nothing live, plus the argument and
/// open-state errors above.
error_t hos_process_handle_type(const hos_process_t *POUND_RESTRICT process,
                                hos_handle_t                       handle,
                                hos_object_type_t *POUND_RESTRICT   out_type);

// -----------------------------------------------------------------------------
// Events
// -----------------------------------------------------------------------------

/// Marks the event signalled.
///
/// Signalling an already-signalled event is not an error and does not queue a second
/// wake: an event is a state, not a counter.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` if the handle names nothing live,
/// `POUND_ERROR_INVALID_ARGUMENT` if it names something other than an event, plus the
/// argument and open-state errors.
error_t hos_process_event_signal(hos_process_t *POUND_RESTRICT process, hos_handle_t handle);

/// Marks the event unsignalled, whatever its clear mode.
error_t hos_process_event_clear(hos_process_t *POUND_RESTRICT process, hos_handle_t handle);

/// Reads the event's state without changing it.
error_t hos_process_event_is_signaled(const hos_process_t *POUND_RESTRICT  process,
                                      hos_handle_t                        handle,
                                      bool *POUND_RESTRICT                 out_signaled);

/// Waits for the event and does not block.
///
/// Reports whether the event was signalled, and clears an auto-reset event that was, so
/// that the signal is consumed exactly once. A manual-reset event is left signalled.
/// A wait on an unsignalled event returns `POUND_SUCCESS` with `*out_signaled` false:
/// "not yet" is a result, not a failure, and a blocking wait is the scheduler's job.
///
/// Returns the same errors as `hos_process_event_is_signaled`.
error_t hos_process_event_wait(hos_process_t *POUND_RESTRICT process,
                               hos_handle_t                 handle,
                               bool *POUND_RESTRICT         out_signaled);

// -----------------------------------------------------------------------------
// Sessions
// -----------------------------------------------------------------------------

/// Sends `command` and `request` over a session and collects the reply.
///
/// `response` and `response_capacity` describe the caller's buffer and may be NULL and
/// zero for a request that expects no reply; `out_response_size` reports what the handler
/// wrote and must not be NULL. The handler runs with the kernel lock released, so it may
/// call back into this module -- see the module comment.
///
/// Returns `POUND_ERROR_INVALID_HANDLE` if `session` names nothing live,
/// `POUND_ERROR_INVALID_ARGUMENT` if it names something other than a session or an
/// argument is NULL, and otherwise whatever the handler returned.
error_t hos_process_session_request(const hos_process_t *POUND_RESTRICT process,
                                    hos_handle_t                        session,
                                    uint32_t                            command,
                                    const void *POUND_RESTRICT          request,
                                    size_t                              request_size,
                                    void *POUND_RESTRICT                response,
                                    size_t                              response_capacity,
                                    size_t *POUND_RESTRICT              out_response_size);

#endif // POUND_HOS_KERNEL_H

/*** end of file ***/
