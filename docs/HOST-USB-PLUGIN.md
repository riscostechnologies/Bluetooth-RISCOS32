# Windows USB transport plug-in

`host/c/usb_plugin` exports `btv_usb_plugin_get_api` from `BluetoothUSB.dll`.
The ABI is declared in `host/h/usb_plugin`; request and completion types are in
`host/h/winusb_transport`. This is a raw USB transport component. It has no
pairing, Bluetooth profiles, codec, PCM interface or guest connection yet.
It is not an operational RISC OS Bluetooth voice plug-in.

## Build and inspect

From the project directory on this Windows development machine:

    powershell -File scripts/build-usb-plugin.ps1 -Architecture x86
    powershell -File scripts/build-usb-plugin.ps1 -Architecture x64

The script uses the installed Qt MinGW compiler for x86 and Strawberry GCC for
x64. It temporarily prepends the selected compiler's bin directory to its own
process PATH, restoring PATH on exit. It builds and runs lifecycle tests, builds
the DLL and loads it into a matching probe executable. Outputs are under
`build/host/x86` and `build/host/x64`; reports are under `build/evidence`.
The build invokes the probe in its default mode, which calls only the read-only
inspection function, never `open`.

On 9 September 2026 both builds and both DLL probes passed. Both probes found
`0A12:0001` with service `BTHUSB`, returned `BTV_HOST_WINDOWS_OWNED`, and did not
open a raw USB device handle. No Windows driver was rebound or replaced.

An explicit controller diagnostic is also available:

    build\host\x86\plugin_probe.exe C:\VKBox\bluetooth-voice\build\host\x86\BluetoothUSB.dll --read-version

This mode requires a device already available through WinUSB. It queues an HCI
event read, submits the single Read Local Version command (opcode `0x1001`),
assembles fragmented event replies, validates the matching command completion,
then drains and closes the transport. It does not reset the controller, pair a
headset or install/rebind a driver. The command has a ten-second deadline and
close has a further five-second drain deadline. If operations still own buffers
after that deadline, the standalone probe retains the DLL and exits the process
without manually freeing their storage. An embedded host must instead preserve
the instance and continue draining as described below.

The diagnostic prints numeric HCI/LMP version codes and revision/manufacturer
fields from the actual response. It does not infer them from USB `bcdDevice`.
The present dongle's response remains unmeasured: the live explicit diagnostic
returned the expected Windows-owned refusal before opening it. Evidence is in
`build/evidence/controller-ownership-gate.txt`.

`--initialize-controller` is a separate explicit mode which **sends HCI Reset**
and then runs the NetBSD-derived controller discovery sequence. It requires
fresh exclusive ownership and tears down controller links through that reset.
Do not use it as an inspection-only command. The build invokes it only through
the simulated-controller test, never on real hardware. Its shared state
machine and remaining stack work are documented in `HCI-BOOTSTRAP.md`.

Initialization now uses `hci_commands` for its command queue, credits and
response ownership. The worker maps scheduler tickets to USB tickets and
retains a queued command across a transport-busy retry. Simulated tests return
zero-credit replies followed by separate no-op credit updates, with USB
completion arriving later. Both architectures pass this integrated path; see
`HCI-COMMANDS.md` for its contract.

## Ownership and caller contract

Load the DLL of the same architecture as the host process. Resolve the exported
entry point, request `BTV_USB_PLUGIN_ABI`, and check the returned structure size
and version before using its functions. Unknown ABI versions return NULL.
The ABI uses the compiler's default C calling convention; clients must use the
supplied headers with compatible Windows C structure layout and calling
convention. Never pass guest addresses as host pointers.

One host worker must serialize calls for each transport instance. Discovery,
opening and alternate-setting changes use synchronous Windows APIs and must
not execute in a guest IRQ handler or the emulator's real-time audio callback.
The plug-in itself creates no threads. Keep the DLL loaded until every instance
has completed close.

Inspection selects the sole matching CSR USB device or an exact supplied PnP
instance. Multiple matches require explicit selection. The public `open` first
requires the selected device's service to be WinUSB and locates its registered
device interface GUID and path. It then validates the opened device's identity
and descriptors. It does not install drivers. The internal `open_path` function
is for the implementation and fault tests, not the public ownership gate.

The transport currently supports the captured full-speed, single-configuration
Bluetooth USB layout: HCI interface 0 and voice interface 1. Opening requires
both interfaces at alternate zero. Voice alternate selection validates the
descriptor and WinUSB pipe information and refuses changes while any voice
request remains pending or its completion is unconsumed.

## Request lifecycle

There are 16 request slots and a 4096-byte buffer per slot. Submission copies
outgoing bytes before returning. Incoming requests supply no caller buffer;
the completion carries copied bytes. Tickets are nonzero and are not reused
within an instance. A full queue returns BUSY.

Polling does not wait. PENDING leaves the caller's output unchanged. A terminal
poll consumes the ticket once, even if the completion contains an I/O error.
The poll function's success means that a completion was delivered; callers must
also inspect its Windows error and individual isochronous packet statuses.

Cancellation requests cancellation only. It does not release buffers or mean
that the device has stopped accessing them. Close rejects further submissions,
requests cancellation, and returns PENDING until every outstanding operation
has reached a terminal state. The caller must continue polling close. Only its
terminal return frees storage and sets the instance pointer to NULL. Cancellation
errors do not permit early freeing. Disconnect and cancellation behavior has
been tested with a simulated driver, not with physical USB transfers.

## Isochronous data

Voice transfers describe up to 32 full-speed USB frames. Their byte length is
the selected endpoint's bytes per frame multiplied by the frame count. The
transport does not convert PCM to HCI SCO packets, insert HCI headers, or decide
codec negotiation. Those remain responsibilities of the Bluetooth stack.

For successful input, `bytes` is the extent of the returned storage, including
gaps. Use each packet's offset, length and status; never treat the entire extent
as valid contiguous audio. Out-of-bounds descriptors and overlapping or
backwards nonempty packet ranges fail the completion. Empty missed frames may
have an unchanged offset and retain their individual failure status.
For successful output, `bytes` records the submitted length: WinUSB's
overlapped-result byte count is zero for isochronous operations. Neither value
proves that a headset played the audio.

The implementation follows the documented [WinUSB isochronous transfer
API](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/getting-set-up-to-use-windows-devices-usb),
[overlapped result
contract](https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_getoverlappedresult)
and [CancelIoEx lifetime
rules](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex).

## Validation boundary and remaining integration

The test executable replaces WinUSB calls with a fault-injected implementation
and uses real Windows manual-reset event objects. It checks copied ownership,
queue saturation, immediate and delayed completion, oversized completion
rejection, voice alternate exclusion, packet errors and bounds, continuation
recovery, and cancellation/disconnect draining before buffer unregistration.
These tests do not substitute for controller testing.

The Windows test now also connects `sco_worker` through the real transport
implementation to the simulated WinUSB device, using real completion events.
The synchronous link is opened by the command scheduler test before starting
the worker. Four ISO reads complete out of order; delivery remains ordered,
and queued audio survives reuse of the underlying USB buffers. Stopping retains
all pending requests, blocks voice alternate changes, and unregisters buffers
only after terminal completion and device close. This integration test passes
in both x86 and x64 builds. It does not transfer audio through the physical dongle.

`sco_duplex` adds the shared lifetime above the two directional workers. It
keeps the voice alternate selected until both sides have consumed their USB
receipts. This prevents teardown of interface 1 while either microphone or
playback buffers remain controller-owned.

The same controller diagnostic has also run through the transport implementation
against a simulated device. That test splits the version event after its first
byte and delivers the reply before command-transfer completion. The diagnostic
must assemble the response, wait for both sides of the transaction, and close
cleanly. Its printed version numbers are explicit test fixtures, not observed
dongle properties. The shared parser separately tests truncation, mismatched
opcodes, rejected commands and incomplete command status; those tests pass in
the isolated RISC OS guest as well as host/sanitizer builds.

Before a functional release, the project still needs a guest transport binding,
the NetBSD-derived Bluetooth protocol stack, pairing and service discovery,
voice profile negotiation, SCO packet scheduling and codec integration, and
actual microphone/playback tests. The existing isolated RISC OS controller
probe reports no OHCI device. The legacy emulator's proposed SWI offset +7 is
also labelled retired/reserved elsewhere in its source; this DLL does not claim
or install that interface. A verified emulator or native USB connection remains
necessary. Windows driver ownership must be resolved with a concrete recovery
procedure before physical raw-USB testing.
