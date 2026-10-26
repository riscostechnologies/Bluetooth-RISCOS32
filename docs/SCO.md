# Synchronous audio transport

`port/c/sco_frame` implements the USB HCI synchronous packet boundary described
in [Core 5.4, Vol 4, Part E, section 5.4.3](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/host-controller-interface-functional-specification.html).
The official HTML was retrieved and inspected locally; its ignored build copy
is reference material rather than a distribution artifact.

The receive decoder requires exactly one complete packet, validates the
connection handle range and exposes the payload with its controller status:
good, possibly erroneous, lost or partially lost. It ignores RFU bits on
receive. Invalid input leaves the result unchanged. Payload bytes are borrowed
and remain owned by the receive packet until copied or consumed.

Transmit encoding always clears packet status and RFU bits and accepts up to
255 payload bytes. It does not prepend an H4 type byte. Capacity errors leave
output untouched, and overlapping source/output is supported by moving the
payload before writing the header.

Tests cover all four status values and every split point of a maximum-sized
packet through the existing USB HCI stream assembler, plus exact lengths,
reserved handle rejection, empty payloads and transmit overlap/capacity checks.

This is framing, not a working audio channel. The parent still needs to create
and own a synchronous connection, validate the active handle and codec,
configure controller audio format and erroneous-data reporting, schedule USB
isochronous transfers, and preserve timing through loss. Controller-reported
loss is distinct from missing USB transfer data; the latter invalidates stream
framing until a new packet boundary is established. No PCM interpretation or
loss-concealment policy is implemented here. Physical voice IO remains untested.

## Setup wire boundary

`port/c/sco_setup` encodes the legacy Setup Synchronous Connection command
(0x0428) and decodes Synchronous Connection Complete events (0x2C), following
Core HCI sections 7.1.26 and 7.7.35 and the pinned NetBSD field layouts.
The caller supplies the established ACL handle, bandwidths, latency, voice
setting, retransmission effort and packet-type mask. The encoder checks field
bounds and accounts for the inverted EDR packet-exclusion bits; it does not
select a codec policy or send the command.

The completion decoder preserves status and peer address. On success it
exposes the new synchronous handle, link type, transmission interval, window,
directional packet lengths and air mode. Failed completions clear connection
parameters rather than exposing undefined bytes as a usable handle. Packet
lengths here describe the negotiated link and must not be mistaken for USB
transfer sizes or controller HCI buffer limits.

Tests compare exact command bytes, invalid parameter/capacity handling,
negotiated directional lengths and failed-event isolation. These are wire
codecs only. The owner still needs global command scheduling, peer correlation,
Command Status and USB completion receipts, setup timeout/recovery, disconnect
and active-handle validation before enabling audio traffic.

## Scheduled setup owner

`port/c/sco_link` now queues setup through the shared HCI command scheduler.
Its submission hook verifies the ticket has actually been accepted. OPEN
requires that submission, a successful scheduler reply (USB completion plus
Command Status), and a successful synchronous completion for the intended
peer. The new voice handle and negotiated parameters are exposed only while
OPEN and while the scheduler is not in recovery.

The parent routes command events and USB completions to the scheduler and
non-command events to this owner. Duplicate completion does not replace the
parameters; contradictory completion requires controller recovery. Remote
disconnection prevents later duplicate setup events from reopening the link.
The parent must also handle loss of the underlying ACL connection.

Setup has a 30-second overall queue/response deadline. An unsent command can
be cancelled locally; timeout after acceptance freezes the scheduler for
parent controller recovery. Physical drainage remains the parent's task.
Tests cover all six USB/Command Status/connection-event orders, wrong-peer
isolation, duplicate events, remote close and both timeout paths.

Codec/audio-format configuration,
SCO data scheduling and physical voice IO remain unimplemented or unverified.

Local synchronous disconnect now queues HCI Disconnect for the voice handle
through the same scheduler, using Remote User Terminated as its reason.
CLOSING immediately hides the audio parameters. CLOSED waits for the matching
disconnection event and command receipts, regardless of their arrival order.
This still does not mean outstanding isochronous audio IO has drained.

A remote close before submission cancels the now-unnecessary queued command.
A rejected disconnect without a close event returns to OPEN with the controller
status retained for explicit caller recovery/retry. An unsent timeout also
returns OPEN with `timed_out`; an accepted timeout requires controller recovery.
Tests cover all six local-disconnect completion orders, rejection followed by
retry, and remote closure before submission. Global scheduler recovery also
propagates to this owner when ticked outside setup.

## Copied receive packets

`port/c/sco_receive` binds an eight-packet copied queue to an OPEN voice handle.
Each entry retains the opaque payload, controller packet status, local receive
time and a sequence number. The local timestamp is not a radio timestamp and
must not replace the negotiated audio cadence. Payload bytes are not assumed
to be PCM; the audio-format/codec owner still needs to interpret them.

Incoming USB buffers can be reused after feed returns. When the queue is full,
the newest packet is dropped and the sequence advances; a later queued packet
exposes the gap. Existing entries are preserved. Reads copy one whole packet,
including zero-length loss reports, and insufficient output capacity consumes
nothing. Other valid handles are left unconsumed. Malformed packets or loss of
the bound OPEN link stop this queue until parent recovery/reinitialization;
already queued storage is retained for safe disposal after physical drainage.

Tests attach to a link established by the scheduler/receipt tests, overwrite
incoming storage, verify all status values, fill and drain the queue, observe
an overflow sequence gap, and check handle isolation and malformed framing.
This is receive-buffer integration, not physical microphone capture or loss
concealment. Active transport scheduling and audio conversion remain required.
# USB frame ingestion

`btv_sco_usb_receive` joins the USB HCI fragment assembler to the copied SCO
receive queue. The transport owner supplies each USB frame's valid byte range
in submission order, with its USB status, excluding unused gaps in completion
storage. A frame may complete several HCI packets or only part of one. HCI
packet loss flags remain attached to completed packets independently of USB
status. Queue overflow uses the existing counted drop-newest policy.

A failed USB frame or transfer freezes the receiver, including reads of its
queue. Subsequent plausible headers do not restart it. The parent must drain
old requests and establish a known HCI boundary before initialization; simply
receiving the next successful USB frame is insufficient. The receiver contains
an internal storage pointer and must not move after initialization.

Tests cover every split of two concatenated SCO packets and a failed USB frame
in a partial header. `host/c/sco_completion` adapts the plugin's copied completion
structure to this interface. It checks the caller's expected request ticket,
transfer kind, transfer error, array bounds and every frame descriptor before
feeding any bytes. A failed frame discards the entire completion and freezes
reception. Valid gaps are skipped, rather than parsed as HCI bytes.

The owner must deliver completions once, in request submission order, retaining
later completions while earlier requests are pending. `host/c/sco_worker` now
provides that owner for input: it keeps up to four requests queued through the
plugin ABI, polls only the oldest receipt and refills consumed slots. BUSY leaves
it running for retry. Each pump is bounded and does not wait for completion.
The caller supplies the configured endpoint's transfer size and frame count.

Stop, link loss, parsing failure or a transport error freezes audio delivery.
Subsequent pumps cancel outstanding reads and continue consuming terminal
receipts. Successful cancellation alone does not remove a ticket. A poll error
retains the ticket; an unrecoverable observation requires the parent to close
and drain the shared device. The worker does not own the device or DLL, change
voice alternates, negotiate codecs or run its own thread.

Tests exercise copied data across gaps, wrong tickets, overlap, late out-of-bounds
descriptors and frame loss. Worker tests cover later requests completing first,
ordered delivery and refill, queue BUSY, failed observation and delayed
cancellation completion. Physical microphone testing remains outstanding; this
code is not connected to the installed emulator's audio path.
# Copied output spool

`sco_transmit` encodes each supplied audio payload with its active synchronous
handle and concatenates the HCI packets in a bounded 4096-byte ring. It exposes
only complete caller-sized USB blocks. Packet headers and payloads may span
blocks; insufficient data waits without adding padding or invented silence.
Successful transport acceptance releases the copied spool bytes, while the
parent must retain and drain the transport receipt separately.

This follows the byte-stream packing approach in pinned NetBSD
`sys/dev/usb/ubt.c`, `ubt_xmit_sco_start1`. That implementation returns copied
packets through `hci_complete_sco`; it explicitly cannot rely on controller
completion notifications for them. This spool does not itself choose SCO
flow-control mode, pacing, codec, USB alternate or payload MTU. Those must be
established by the session owner before use. A transfer error requires abort
and parent recovery, not replay of uncertain bytes.

[Microsoft's WinUsb_WriteIsochPipeAsap documentation](https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_writeisochpipeasap)
describes how WinUSB packetizes output and gives transfer-size guidance for
streaming. The caller must derive the block from the actual endpoint; a fixed
block passed to this spool is not evidence of a compatible audio rate.

Tests round-trip 352 payloads through 264-byte USB blocks into the receive
assembler, covering packet splits, ring wraparound, stable retries and copied
ownership. A full spool rejects additional data atomically. This is not yet
connected to a codec and has not played physical audio.

`btv_sco_worker_init_output` binds this spool to the same bounded polling and
draining machinery as input. Separate input/output instances may share the
device under one serialized owner. Output submits only complete blocks, retires
spool bytes after copied acceptance, and retains USB receipts until terminal
polls. BUSY preserves the queued stream. Bad identity, errors or short output
completions freeze playback and require draining. A final partial block waits;
stopping discards its eventual use rather than manufacturing padding.

The Windows integration test compares submitted buffers with concatenated HCI
packets and verifies WinUSB's zero overlapped ISO byte count is translated into
the accepted block length by the transport. This remains simulated USB testing.

## Duplex USB lifetime

`host/c/sco_duplex` selects the requested voice alternate and binds one input
and one output worker to the same open plugin device. Both are pumped from the
same serialized owner. Input and output completion order is independent, while
ordering within each direction remains strict. Application reads and writes are
available only in RUNNING state.

Stopping either explicitly or because one direction fails freezes both paths.
The alternate remains selected while either worker owns a request. Only after
both workers consume every terminal receipt does the coordinator select
alternate zero. BUSY while disabling is retried; another disable error leaves
the session failed for parent device close and drain. The coordinator does not
own the device, DLL, link, audio clock or codec selection.

The pure coordinator test starts both directions, completes output before an
older input request, verifies copied input delivery, and checks that alternate
zero is selected only after joint cancellation drain. The same code compiles in
the x86 and x64 plugin validation builds and executes in the RISC OS core test.

## HFP audio orchestration

`host/c/hfp_audio` is the current top-level owner for baseline CVSD audio. It
requires READY HFP/RFCOMM control and an explicit secured-ACL assertion, queues
synchronous setup through the shared HCI command scheduler, validates CVSD air
mode, and then starts the duplex USB lifetime. Endpoint transfer dimensions and
the controller HCI SCO payload limit remain caller-supplied observed values.

Application access is exposed only while the full session is RUNNING. On stop,
control loss, format mismatch or data failure, the owner prevents further audio
access. USB is drained and alternate zero is selected before HCI Disconnect is
queued. Controller-recovery outcomes remain distinct from a clean close.

Above this owner, `port/c/hfp_pcm` provides the baseline 8 kHz mono signed
16-bit little-endian queue boundary derived from pinned NetBSD `btsco.c`.
It packetizes playback without losing data on backpressure and substitutes
silence for controller loss status or receive sequence gaps.

`port/c/bridge_sco` binds the synchronous link to the guest host bridge with
four outstanding tickets per direction, validates every ISO packet descriptor,
copies output before submission and retains cancellations until terminal poll
results. `bridge_pcm`, `bridge_voice` and `bridge_session` expose this as the
module's 8 kHz mono PCM SWIs. The isolated RISC OS fixture exercises real module
calls through these workers; physical voice traffic remains unverified.
