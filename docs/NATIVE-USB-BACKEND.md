# Native RISC OS USB backend contract

## Purpose and selection

`BluetoothVoice` already drives the NetBSD-derived controller, ACL, L2CAP,
SDP, RFCOMM, HFP and SCO state machines through the ticketed `host_bridge`
contract. The native backend must implement that same contract with USBDriver
and DeviceFS. Protocol owners must not contain separate native and emulator
state machines.

At module initialisation, transport selection is ordered as follows:

1. Inspect the private emulator bridge. Select it only when its ABI and raw USB
   result are usable.
2. Enumerate `Service_USB` records and select a supported native Bluetooth
   controller only when the complete native operation table is present.
3. Otherwise publish zero capabilities and return `BackendUnavailable` from
   `BluetoothVoice_Connect`.

A detected controller alone is not enough to advertise playback, capture, HFP
or native transport. The module publishes those capabilities only after event,
command, ACL and SCO operations can all be opened, pumped, cancelled and closed.

## Controller discovery and ownership

The backend enumerates `Service_USB` reason `Connected`, copies the selected
record through `btv_usb_service_decode`, and releases every provider-owned RMA
record with `OS_Module` reason 7. It accepts the standard Bluetooth class tuple
E0/01/01 and the descriptor roles validated by `btv_usb_select`; it does not
bind only by the current `USBn` enumeration name.

One module workspace owns one controller. `open` claims interface 0 alternate 0
in this order:

- event IN endpoint 1, interrupt;
- ACL IN endpoint 2, bulk;
- ACL OUT endpoint 2, bulk.

All DeviceFS special fields use the shared `btv_usb_stream_path` builder. Its
semicolon-separated output is the only supported pathname construction path.
Each stream handle is retained until the final ticket using it has drained.

The implementation in `port/c/native_usb` provides module-safe `Service_USB`
discovery, transactional event/ACL stream open and rollback, ticketed control,
bulk and isochronous I/O, plus SCO alternate stream ownership. `host_bridge`
selects this backend when the emulator's private transport SWI is absent, so
the public BluetoothVoice SWIs can operate directly through DeviceFS. Physical
qualification remains required before this path is described as supported.

The native backend requires DeviceFS capability bits 0 and 1 from extension
call `0x80000009`. These identify the patched USBDriver asynchronous control
and completed-request release paths.
The stock USBDriver 1.31 implementation ignores R6 poll mode and enters a
synchronous default-pipe request; that request froze the physical i.MX6 during
HCI Reset. A first patched implementation still entered EHCI from inside the
DeviceFS call and also froze the iMX6. The corrected implementation copies the
request and defers controller submission to an OS callback after
`DeviceFS_CallDevice` returns. The probe refuses control commands when either
required capability is absent. Its `--control-safety` gate exercises the same
deferred path first with a read-only USB device-descriptor request.

Device removal marks every outstanding ticket as an I/O failure and prevents
new submissions. The module listens for `Service_DeviceDead` and matches its
R2 DeviceFS handle to the handle captured from `GetHandles2`; USBDriver dying
or dead notifications invalidate the selected native device as well. It does
not dereference or release a control-status address after either notification.
The next foreground pump drains failed completions and transitions the session
to failed/disconnected state.

## Non-blocking ticket operations

The public module SWIs must not block RISC OS. The backend therefore maps the
existing bridge operations to fixed workspace tickets:

| Bridge operation | Native operation |
| --- | --- |
| `inspect` | Copy and validate `Service_USB` records without opening a stream. |
| `open` | Open the three interface-0 streams and initialise ticket queues. |
| command `submit` | Copy the HCI command, issue the USB class request with R6 bit 0, and retain the returned pollable status byte. |
| event/ACL input `submit` | Reserve the next read ticket for its persistent DeviceFS stream. |
| ACL output `submit` | Copy data into ticket storage and wait for sufficient DeviceFS buffer space before writing. |
| `poll` | Inspect control status or DeviceFS buffer space, perform only a bounded available read/write, and return `Pending` or one completion. |
| `cancel` | Mark the ticket cancelled, stop new I/O, and retain it until the underlying request is no longer active. |
| `close` | Refuse while tickets remain, then close SCO and HCI streams and clear copied device state. |

The implementation uses a monotonically increasing nonzero ticket identifier
and a fixed slot count. A slot is not reused until its completion has been
returned exactly once. Caller buffers are never retained: output and command
payloads are copied at submission, and input is copied into
`btv_host_bridge_completion` at poll time.

USBDriver control status values 0, 2 and 4 remain pending. Value 1 completes
successfully; stall, timeout, inactive and unknown values complete with an I/O
error. USBDriver 1.31 can execute the request synchronously despite R6 bit 0
and return no R7 status pointer; a successful SWI in that case is an immediate
completion. A foreground warning threshold never frees a request whose
USBDriver status is still active; its copied payload and ticket remain owned
until a terminal status is observed.

## Voice streams

`voice(alternate)` validates interface 1 through the copied descriptors before
calling `USBSetInterface`. Alternate zero closes SCO streams and releases voice
bandwidth. Alternates 1 through 5 open isochronous IN and OUT endpoint 3 using
the shared path builder and the packet size advertised by that exact alternate.

SCO submission preserves the bridge packet contract for the fixed-size CVSD
schedule: up to 32 packet records with calculated byte offsets and lengths.
DeviceFS exposes a byte stream rather than per-packet USB completion status, so
the native backend can report only successful fixed-size records; a stream I/O
failure fails the whole ticket. Four input and four output tickets may be
outstanding because `bridge_sco` pipelines four slots per direction.
Cancellation drains every slot before changing the alternate or closing the
controller. Physical loss and short-packet behaviour are still unverified.

## Physical qualification gates

Qualification on each physical board runs in this order:

1. `--transport-open`: open and close event plus ACL streams without commands.
2. `--hci-control`: pollable HCI Reset and Read Local Version using event input.
3. `--hci-version`: combined event, ACL and controller bootstrap gate.
4. Inquiry with the headset kept out of pairing mode until the inquiry command
   is ready.
5. Timed pairing, authentication, encryption, SDP, RFCOMM and HFP negotiation.
6. Bidirectional SCO playback/capture, disconnect, reconnect and unplug recovery.

An earlier gate cannot qualify a later one. The i.MX6 result does not qualify a
Pi 4, Titanium or emulator.
