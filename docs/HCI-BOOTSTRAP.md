# NetBSD controller initialization port

`port/c/hci_bootstrap` adapts the controller-discovery portion of NetBSD's
`hci_unit.c` revision 1.16 and `hci_event.c` revision 1.26. The immutable originals
and their hashes are in `upstream/netbsd` and `upstream/manifest.json`. The port
retains their licence notice, including the notice covering the copied legacy
command mask. Binary distributions containing this code must include
`docs/NETBSD-NOTICE.md`.

The sequence issues Reset, Read BD_ADDR, Read Buffer Size, Read Local Supported
Features, applicable extended feature page reads, Read Local Version and, for
HCI 1.2 or later, Read Local Supported Commands. Older controllers use NetBSD's
legacy command mask with `commands_reported` false. That distinction prevents
the inferred mask from being mistaken for a controller response.

## Execution and ownership

NetBSD queues commands in mbufs and waits on its initialization condition
variable. This adaptation instead has one outstanding HCI command and uses
caller-owned state. The caller serializes every operation in a deferred worker
context. There are no dummy locks, sleeps, allocation, or IRQ-context callbacks
inside the shared state machine.

When used with the shared `hci_commands` scheduler, enable
`btv_hci_bootstrap_use_scheduler` before the first submission. This delegates
the credit gate to the controller-wide scheduler; see `HCI-COMMANDS.md`. The
standalone default retains the internal credit behavior described below.

Start is only valid for a fresh, exclusively owned controller. The owner must
first tear down old links, cancel and drain old USB transfers, and discard old
events. Resetting while old traffic can still enter the same state machine is
not supported. This component does not yet implement NetBSD's link teardown.

Call `tick` before requesting a command. `command` peeks without reserving a
credit, so a busy transport can retry it. Immediately after successful USB
submission, call `sent` before delivering any received events. Report a failed
USB command transfer through `abort`, even if the controller response has
already arrived; an ambiguously delivered command must not be retransmitted.

Only validated Command Complete/Command Status events update command credits.
The advertised value replaces the allowance rather than adding to it. An
unrelated opcode or successful Command Status does not finish the current
command. The power-up-noop option starts with zero credits until an event
advertises availability. The current Windows diagnostic uses the normal
one-credit startup; it does not detect or configure controller-specific quirks.

The owner supplies a wrapping 32-bit monotonic millisecond counter and must
service the state machine regularly. A stage has a five-second deadline and
the whole sequence a thirty-second deadline. Repeated unrelated events cannot
extend those deadlines. Successful stage completion or command submission
updates the stage clock; the overall clock remains fixed.

## Returned information

Snapshot publishes copied information only after every required response has
been validated. It includes the controller address in Bluetooth wire order,
ACL/SCO payload sizes and buffer counts, local version fields, features and
supported commands. Those are advertised controller properties. They do not
establish the host transport's capacity or usable audio formats.

Like the pinned NetBSD code, the port stores feature pages 0 through 2 and walks
all advertised extended pages. It keeps the original base-feature reply for
page zero. Higher pages are consumed without storing their unknown contents.
Replies must identify the requested page and maintain a consistent maximum;
the page counter cannot wrap after page 255.

`BTV_BOOT_DISCOVERED` means the controller information was obtained. It does not
mean a functional Bluetooth stack. Packet-type/policy masks, ACL/SCO credit
accounting, link management, pairing, L2CAP, SDP, RFCOMM, profiles and audio remain
outside this component. The RISC OS module still advertises no usable backend.

## Transport integration and evidence

The standalone Windows probe now has an explicit `--initialize-controller`
mode. It uses this state machine through the same DLL API and receive assembler
as the version diagnostic. Unlike `--read-version`, initialization sends HCI
Reset and discards existing controller link state. It requires an already
WinUSB-owned dongle. The build never invokes this mode against real hardware.

The x86 and x64 transport tests run this complete sequence with a simulated
controller. The simulation returns HCI events before USB command completion;
the host must wait for both and drain its requests before publishing success.
Printed controller values in those tests are fixtures.

Shared tests also cover the extended-page sequence, all truncated response
lengths, controller rejection at every stage, absolute credits, zero-credit
startup, pending Command Status, legacy fallback, invalid feature pages and
millisecond-counter wrap. Strict host C89, address/undefined-behaviour sanitizer
checks, EABI cross-build and execution of the same code in the isolated RISC OS
guest passed on 9 September 2026. `build/evidence/core-guest.txt` records the
guest run. No physical reset, discovery, pairing or audio success is claimed.
