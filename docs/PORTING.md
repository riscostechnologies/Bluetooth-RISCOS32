# Porting inventory and execution milestones

## Current evidence

The initial Windows enumeration found USB 0A12:0001. The pinned NetBSD
`sys/dev/usb/usbdevs` maps this to CSR BLUETOOTH. This is an advertised identity,
not proof of chipset revision, Bluetooth version, or SCO interoperability.

An isolated native emulator build now supplies a private direct transport SWI
over the existing x86 BluetoothUSB plugin. The RISC OS module probes this bridge
at initialization. A guest test traversed module, SWI, emulator and DLL and
reported ABI `&10000`, USB `0A12:0001` and result `WINDOWS_OWNED` (`-8`). The
bridge implements bounded open, voice-alt, submit, poll, cancel and close calls,
and `bridge_session` now drives the complete NetBSD-derived path through that
boundary against a controller/headset fixture. The public module connects this
owner to inquiry, PIN pairing, encryption, SDP, RFCOMM, HFP, CVSD SCO and copied
PCM playback/capture. See `HOST-BRIDGE.md`.

The inspected `C:/KBoxBuild/src/usb_host.c` documents an initial OHCI model
without transfer-descriptor processing. Live Windows inspection confirms the
desktop shortcut `Virtual Kinetic Box (Native).lnk` targets
`C:/VirtualKineticBox/VirtualKineticBox-Native.exe`, with working directory
`C:/VirtualKineticBox` and no arguments. A running process has that same executable
path and command line. The installed executable has not yet been matched to a
specific source commit or build tree.

`C:/VirtualKineticBox/Emulatorlog.txt`, timestamped 8 September 2026 at 20:18:02
local time, identifies version 2.2.12 (core 2.2.7), a 32-bit GCC 8.1.0 build,
native Win32/WGL, and a ROM patch identified as RISC OS 5.31. Its configuration
reports `system_439=0`, `model=Kinetic` and `sound_enabled=1`. This is evidence
for a RISC OS 5.31 ROM being selected in the current launch; a guest-side
OS/version query and USB module enumeration remain outstanding.

Live Windows PnP AudioEndpoint enumeration returned only DP and E2711F NVIDIA
display audio endpoints. No Bluetooth playback or microphone endpoint appeared
in that query. Earlier Bluetooth-device enumeration is not evidence of usable
audio endpoints. Headset connection and duplex audio must be verified separately.

The inspected native source contains `src/win32native/vkb_sound_win.cpp`,
described as a waveOut playback sink. Searching the native directory located
waveOut output and sound-thread integration but did not locate waveIn/WASAPI
capture support. This source inspection does not prove what is linked into the
installed executable and is not evidence of working microphone capture.

Windows has arm-none-eabi-gcc 14.2 and GCC available. Debian WSL has Python and
Perl. Neither the Windows command path nor the inspected Debian command path
provides riscos-amu or riscos-cport. The project-local PRM-in-XML tool works.
The approved implementation investigation located a working RISC OS EABI
cross-toolchain at `/opt/netsurf/arm-riscos-gnueabi/cross/bin` and an older
GCCSDK compiler at `/home/riscos/gccsdk/cross/bin`. The EABI toolchain builds
the guest exercisers successfully. `scripts/build-module.sh` builds the local
CMunge 0.76 tool from the existing GCCSDK source, generates no-SCL veneers and
links a runtime-free module using GCCSDK 4.7.4. `-zbase` makes the linker retain
the RO-base symbol required by module relocation; objcopy converts the linked
ELF into the raw relocatable module image. This module does not call C library
or compiler helper functions. Adding such calls requires a suitable module
runtime rather than the application-mode UnixLib.

## Source boundaries

| Source | Existing dependency | Required RISC OS adaptation |
| --- | --- | --- |
| sys/dev/usb/ubt.c | usbd pipes, transfers, descriptors, alternate settings, aborts | Verified asynchronous USB transport including isochronous receive/transmit |
| sys/netbt/hci_*.c | mbufs, queues, timers, kernel locks and device lifecycle | Packet ownership, bounded queues, deferred callbacks, reset/cancellation |
| sys/netbt/l2cap_*.c | HCI links, sockets, timers | Link/channel state and application-independent protocol API |
| sys/netbt/rfcomm_*.c | L2CAP, socket buffers and flow control | Serial control channels and headset service lifecycle |
| sys/netbt/sco_*.c | HCI synchronous links and sockets | Voice link setup, packet scheduling and bounded audio queues |
| sys/dev/bluetooth/btsco.c | NetBSD audio_if, proplib, kmem, mbufs and softints | RISC OS PCM interface, format negotiation and buffer completion |
| usr.bin/bthset/bthset.c | BSD sockets, libevent, mixer ioctls and SDP functions | Headset control and application configuration |

The imported bthset command handler recognises AT+CKPD, AT+VGS and AT+VGM and
otherwise sends ERROR. It is not a complete HFP audio-gateway implementation.
HFP feature exchange, indicator exchange, event reporting and applicable codec
negotiation need an explicit implementation and headset interoperability tests.
Eight libbluetooth SDP files are now imported at the same pinned commit. The
nonblocking ServiceSearchAttribute client and bounded data-element validator
and service-record interpretation are implemented and tested. The discovery
session now coordinates these with the L2CAP channel manager. Controller-owner
integration and physical headset discovery remain outstanding; see `SDP.md`.

Do not replace mutexes, sleep/wakeup or soft interrupts with empty macros.
Each call site needs a defined execution context and ownership model. USB
completion callbacks must defer protocol and application work appropriately.
Cancellation must finish before resources are freed. Malformed packet lengths,
queue exhaustion and stale handles need explicit failure behaviour.

Keep upstream files immutable; put adaptations in a separate port tree or
recorded patches. Preserve all applicable licence terms when selecting further
dependencies. Do not import unrelated emulator source into this repository.

## Milestones and evidence required

1. Source baseline: pin sources and verify hashes. DONE for the 39 imported files.
2. Interface definition: XML lint and HTML rendering passed locally with
   PRM-in-XML 1.03.343. The user explicitly approved implementation on
   8 September 2026; the review gate is satisfied.
3. Target binding: live shortcut and running emulator identified; runtime log
   identifies a RISC OS 5.31 ROM. Confirm intended target, query the guest USB
   modules, establish sound APIs and match installed binary to build sources.
   The approved BluetoothVoice module boundary now builds and loads in the
   isolated RISC OS guest. Its seven SWIs are present. It advertises no endpoint
   for the Windows-owned physical radio, while the raw-controller fixture
   advertises native HFP PCM capabilities and completes the asynchronous session
   lifecycle.
4. Transport: implement and test real control, interrupt, bulk and isochronous
   transfers with cancellation. Prove HCI reset and version command responses.
   USB HCI length validation and command encoding are implemented, host-tested,
   cross-compiled and exercised in the isolated RISC OS guest. Real transfer
   submission remains outstanding.
   Live configuration capture and standard Bluetooth USB endpoint selection
   also pass; see USB-TRANSPORT.md for endpoints, alternate settings and the
   public RISC OS DeviceFS control-request limitation.
   A pinned USBDriver soft-load now runs in the isolated guest. OHCIDriver 0.56
   builds with the native Norcroft DDE and returns the specific "No OHCI devices
   found" error when loaded there. Host-controller exposure and actual USB
   transfer processing remain outstanding; these diagnostics do not complete
   the transport milestone.
   The private emulator bridge now carries the full plugin operation contract
   without host pointers. A RISC OS guest completes Reset, address, buffer,
   feature-page, version and supported-command discovery against the fake
   controller. Physical submissions remain blocked by the unchanged BTHUSB
   binding.
   The native RISC OS ticket and ownership contract is defined in
   `NATIVE-USB-BACKEND.md`. Its shared DeviceFS pathname builder is compiled into
   both the diagnostic and module and is covered by host, sanitizer and ARM tests.
5. Bluetooth core: connect discovery, pairing, security, L2CAP, SDP and RFCOMM.
   Demonstrate service discovery on the selected headset.
   The NetBSD controller initialization sequence has now been adapted and
   tested on host/guest and through simulated WinUSB transfers. This discovers
   controller properties only; that bootstrap does not complete these protocol milestones.
   See HCI-BOOTSTRAP.md for provenance and execution/credit contracts.
   Bounded inquiry, selected ACL connection, link-key/PIN/SSP response policy,
   authentication and encryption are implemented and pass the end-to-end RISC
   OS bridge fixture. The bounded ACL data pump, L2CAP, SDP and RFCOMM owners are
   integrated in that fixture. Physical pairing, key persistence and headset
   interoperability remain required.
6. Voice profile: implement the correct gateway role and negotiate an actual
   SCO/eSCO link and supported codec. Record control exchange and errors.
   The baseline HFP gateway and CVSD SCO path are implemented and pass the
   controller/headset fixture through the real module SWIs. Wideband codecs and
   a physical control/audio transcript remain outstanding.
7. Guest audio: play and capture from a RISC OS application through the selected
   device. Measure sustained duplex behaviour and queue error counters.
   The copied PCM queue implementation passes host/model and sanitizer tests
   and the same 10,000-operation model in the isolated RISC OS guest. The module
   fixture also writes and captures PCM through live SCO workers. No physical
   headset audio is claimed by those tests.
8. Recovery/package: unplug/replug, reset, reconnect, clean shutdown and a
   reproducible guest/module/emulator package with installation documentation.

An optional Windows render/capture backend can share the application interface.
Its presence must not mark native transport or Bluetooth stack milestones done.
Do not change the Windows radio binding merely to test a host-side abstraction.

## Receive path and controller diagnostic, 9 September 2026

`port/c/acl_receive` now adapts `hci_acl_recv` from the pinned NetBSD
`hci_link.c` revision 1.26. Each established classic ACL link owns bounded
storage; only complete L2CAP packets become available. The caller must retain
input on BUSY until the preceding completed packet is consumed. A new START
replaces an incomplete packet, while loss or an invalid fragment discards the
partial packet and requires another START. Different handles do not alter a
receiver's state. The supported receive flags match this NetBSD snapshot:
point-to-point START(2) and FRAGMENT(1). Broadcast and other boundary flags are
rejected. This component is not an LE stack.

The adapter requires fully assembled HCI packets from the preceding USB stream
layer. Its tests drive that actual combined path with single-byte USB fragments,
as well as interleaved link handles, missing fragments, retained completions,
truncation and size violations. It does not create links, disconnect unknown
handles, manage L2CAP channels or submit traffic to real hardware. Those remain
owner/stack work. The shared source builds with host/sanitizer and RISC OS EABI
toolchains and runs in the isolated guest.

The shared C receive path now assembles HCI headers and payloads across arbitrary
USB fragments. Tests cover every split point for event, command, maximum-length
SCO and 1024-byte ACL payload fixtures, batched packets, bytewise input, bounded
storage and a latched loss state. Read Local Version parsing validates the
matching opcode, exact successful response length and controller error status.
The host/sanitizer builds and the same code running in the isolated RISC OS
guest pass these checks.

The Windows transport DLL is exercised by an explicit controller diagnostic,
including a simulated fragmented response delivered before the outgoing command
completes. Both x86 and x64 pass. A live invocation refuses the current BTHUSB
binding before raw open. Thus the code path for querying HCI/LMP version is
implemented, but the dongle's HCI response and physical transfers remain
unverified. This does not complete the transport or Bluetooth-core milestone.

## Shared-core verification, 8 September 2026

`scripts/test-core.sh` passes strict C89 host compilation, the normal host test,
address/undefined-behaviour sanitizer execution and RISC OS EABI cross-build.
`scripts/test-guest.ps1` passes the same queue/model and framing tests in an
isolated native emulator with its own working directory. The report is saved
to `build/evidence/core-guest.txt`.

The initial guest attempt was stopped by the emulator's global single-instance
guard. The harness now changes the fixed-width mutex name in its disposable
copy only, so the installed running machine remains separate. A subsequent
minimal command-line boot did not complete; using the known !System/monitor
startup and a Desktop file with a 4096K application slot produced the passing
guest report. The test writes its own report file and checks a completion marker.

## Module verification, 9 September 2026

`scripts/build-module.sh` produces a raw module image, not an ELF renamed with
a RISC OS module suffix. `scripts/test-guest.ps1 -Test Module` loads that image
in the isolated guest and verifies ABI/capability reporting, endpoint
enumeration, untouched output buffers, reserved flags, invalid handles,
unavailable backend, unknown SWI rejection, R1-R9 preservation on errors, and
three unload/reload cycles. With the fixture DLL it additionally completes
Connect, HFP/CVSD playback, microphone capture and drained Disconnect. The
passing report is `build/evidence/module-guest.txt`.

An early load test wrongly treated every nonzero `_kernel_oscli` return as a
loader error. The corrected test uses OS_CLI through `_kernel_swi` and checks
the actual error pointer. Module load and unload pass with that explicit check.

CMHG declares temporary SWI base &D7BC0 and error base &A79B00 as UNALLOCATED.
Do not distribute them as registered allocations. The module owns every backend
ticket until terminal completion; the generated no-SCL veneers manage their own
workspace. This test is not proof of physical USB transactions, pairing or
audio delivery.
