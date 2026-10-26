# Controller-wide HCI command scheduling

`port/c/hci_commands` adapts the command queue/credit behavior of the pinned
NetBSD `hci_unit.c` and response dispatch from `hci_event.c`. It retains their
licence notice. Include `NETBSD-NOTICE.md` with binaries using this component.

All command producers for one controller must share a single scheduler. Its
16 slots hold copied command packets and copied replies. Slots include queued,
submitted and completed-but-unconsumed work, so slow clients cannot cause
unbounded allocation. Tickets are not reused before controller reinitialization;
exhausting the 32-bit ticket space refuses further queueing.

## Submission and response routing

The producer queues a complete HCI command without an H4 type byte and declares
whether HCI specifies Command Complete or Command Status as its response. It
retains the returned ticket and associates that ticket with the owning protocol
operation. The oldest eligible command is selected for USB submission. Other
opcodes can proceed while an older command is blocked by another unanswered
command of the same opcode. This avoids assigning a response to the wrong peer
when Command Status carries only an opcode.

Peek copies the outgoing packet without debiting credit. After the physical
transport accepts that copy, the owner immediately records `sent`, which debits
one credit. On transport backpressure, leave it queued and retry. The scheduler
does not own USB handles and does not submit or cancel physical transfers.
Keep its ticket separate from the transport's ticket and maintain that mapping
until the USB completion arrives.

Validated Command Complete and Command Status events replace the command
allowance with the controller's advertised value. They do not increment it.
No-op/unowned command events still update credits, but do not become a queued
client's reply. Other HCI event types are returned to the main event dispatcher.
A successful intermediate Status does not finish a command expecting Complete;
an error Status does produce a reply for that command's owner to inspect.

The scheduler retains each response until the corresponding USB command has
also completed successfully. Reply then copies and consumes that ticket once.
Its OK result means a response was delivered, not that the command succeeded:
the owning protocol must validate status and command-specific return parameters.
This is especially important for connection rejection and bootstrap failures.

## Initialization handoff

Standalone controller initialization still supports its own credit gate for
existing diagnostic callers. When embedding it with this scheduler, call
`btv_hci_bootstrap_use_scheduler` before the first submission. Initialization
then produces commands without maintaining a second gating allowance; every
command must pass through `hci_commands`. Call bootstrap/link `sent` only when
the queued command has actually been accepted by USB, not when merely queued.

This delegation matters when a no-op credit update arrives while an earlier
reply is retained pending USB completion. Delivering that older reply later
must not overwrite the scheduler's newer credit information or leave bootstrap
stalled on a stale local zero allowance. The shared integration test exercises
zero-credit startup and repeated no-op credit updates delivered only to the
scheduler, then hands the same scheduler from initialization to ACL setup.

## Cancellation, deadlines and recovery

Only an unsent command can be cancelled locally without changing controller
credit. The protocol owner supplies deadlines; the initialization and link
components already have timers. An accepted command timeout requires abort,
physical cancellation/drain and verified controller recovery. It must not be
retried solely because its USB result or HCI reply is missing.

A short/failed USB transfer, malformed HCI framing, Hardware Error, or an
unexpected matching response type freezes further transmission. USB completions
can still be recorded during recovery. `drained` checks only the scheduler's
tracked USB completions; it does not prove the OS event queue is empty or the
controller has recovered. The owner must establish both before reinitializing
the scheduler or reusing protocol state.

## Validation and remaining integration

Strict C89 host builds, address/undefined-behaviour checks and the RISC OS EABI
build pass. The same tests execute in the isolated RISC OS guest. Tests cover
same-opcode exclusion, unrelated-opcode progress, credit exhaustion, absolute
credit updates, response routing, both completion orders, queue saturation,
unsent cancellation, error Status, malformed framing and ticket exhaustion.

A combined test drives the actual NetBSD-derived initialization component,
then uses the same scheduler for the actual ACL connection state machine. It
delivers Connection Complete before the command's USB completion and verifies
that the handle becomes usable only after all required evidence arrives.
The controller replies in this test are simulated.

The Windows initialization worker now uses this scheduler through the WinUSB
transport API. It keeps separate scheduler/USB tickets, retries a busy command
submission without duplicating it, passes complete HCI events to the scheduler,
and delivers matched replies to initialization only after USB completion.
The simulation returns zero command credits in each reply and then advertises
credit in a separate no-op event. Both x86 and x64 complete all six initialization
commands through this path, including an injected transport-busy retry.

The live radio still uses BTHUSB. The build's real-device operation remains
read-only inspection; it does not invoke Reset or rebind the driver. Connection
and profile worker integration, native USB integration and the general HCI event
dispatcher remain outstanding. These results do not establish a physical
connection, pairing or audio path.
