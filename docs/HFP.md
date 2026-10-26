# Hands-free profile implementation

The intended role is Audio Gateway, with the remote headset acting as
Hands-Free. The Bluetooth SIG's published profile entry is
[Hands-Free Profile 1.10](https://www.bluetooth.com/specifications/specs/hands-free-profile-1-10/).
This project does not yet implement or claim conformance to that profile.
The full PDF was retrieved from the SIG download service and inspected locally
for sections 4.2.1 and 5.1-5.3. SHA-256:
`e8033114c0825a63c58e008ded5ea627c663b4048d63fcde12cb472683dff105`.
The downloaded specification and extracted text are ignored build inputs,
not distribution artifacts.
The NetBSD bthset source is a legacy headset gateway, not a complete HFP
service-level connection implementation.

## Command input boundary

`port/c/at_stream` is a bounded CR-terminated command collector for the incoming
RFCOMM byte stream. It preserves command bytes without case conversion or
grammar interpretation. Leading/inter-command CR and LF are skipped. Embedded
LF, other control bytes, non-ASCII bytes and lines exceeding 256 characters
fail the collector until the parent initializes a new session. The 256-byte
limit and strict character policy are current implementation choices, not a
claim about the maximum command length or every permitted syntax in HFP.

The collector stops after one complete nonempty command. It returns exactly
how many input bytes it consumed; the parent retains the remainder while the
command owner processes that line and queues its response. The line remains
available until `take`. An overlong or malformed command never has its suffix
treated as a new command. Incoming storage, state and returned line lifetime
belong to one serialized owner.

Tests cover every split of a feature-exchange command, multiple commands in
one input block, a retained line under backpressure, exact buffer limits and
failure isolation. The link pump below attaches it to RFCOMM.

## Baseline gateway exchange

`port/c/hfp_ag` consumes the collector's complete commands and queues verbose
CR/LF-delimited responses. It implements BRSF feature exchange, CIND mapping
and current values, and CMER event-reporting activation/deactivation. The
feature bitmap is zero: no optional codec negotiation, three-way calling or
HF-indicator support is advertised. The seven indicator values are supplied
by the caller and range checked, rather than invented from Bluetooth state.

Startup ordering is enforced. A response remains copied and stable until the
RFCOMM queue accepts it; only then does `sent` commit the next stage and
reporting state. Unknown commands, invalid values, overflow and out-of-order
requests receive ERROR. Decimal feature masks cover the full unsigned 32-bit
range. Command matching tolerates case and spaces for this numeric subset.
CMER ignores irrelevant numeric fields while validating mode and reporting.

READY marks completion of this baseline command exchange, not audio readiness
or full profile conformance. Other required gateway commands are still missing.
The link pump supplies protocol attachment;
physical controller integration remains unfinished.
Tests exercise bytewise input, exact reply framing, ordering, overflow, bounded
output, retry stability and delayed state commitment.

## RFCOMM attachment

`port/c/hfp_link` attaches the gateway to an already READY RFCOMM session and
exclusively owns its application byte stream. Each pump consumes at most 256
bytes and commits at most one response. It reads only until a complete command
is available, retains that command while copied TX storage is busy, then
advances gateway state only after RFCOMM has copied the entire response.
The RFCOMM sender subsequently segments it and accounts for peer credits.

Modem-status renegotiation pauses the pump without discarding command state.
Closing or failed RFCOMM stops the attachment. Invalid command framing aborts
RFCOMM so the parent connection owner can perform cleanup. The parent still
owns signaling, data transport, physical completion and teardown, and pumps
again when input, TX space or modem readiness changes.

Integration tests establish RFCOMM through real component state machines for
both roles, then send BRSF/CIND/CMER through encoded UIH frames and verify the
gateway's reassembled replies. The indicator mapping crosses multiple frames.
Two commands in one packet exercise reply backpressure and ordered delivery.
These tests use constructed peer traffic; no physical headset is connected.

## Indicator changes

`btv_hfp_ag_update` accepts an atomic snapshot of all seven actual indicator
values. When reporting is enabled, each changed value is queued as a CIEV
notification. Index ordering places call before callsetup. The 16-entry queue
preserves transitions rather than coalescing them. If the complete snapshot's
changes cannot fit, the operation applies nothing and the caller must retain
and retry it. Range errors also apply nothing.

Command replies take priority over notifications. Output previews do not
remove events; `sent` commits the most recently previewed output immediately
after RFCOMM copies it, before any other gateway operation. Disabling reporting
discards notifications still owned by the gateway. While disabled, updates
change CIND query values without queuing events. Already copied RFCOMM output
retains its ordering before the disabling command's reply.

The existing link pump transmits notifications through the same copied TX
path. Tests cover call/callsetup ordering, command reply priority, retained
events under insufficient output capacity, alternating transitions through the
ring, atomic queue exhaustion, and reporting disable/silent updates.

## Remaining profile work

Selective indicator reporting (`AT+BIA`) is implemented after the indicator
mapping has been exchanged. Omitted fields retain their prior settings;
excess fields beyond the seven advertised indicators are ignored. Call,
callsetup and callheld remain enabled regardless of requested deactivation.
Invalid values in recognized fields reject the whole command without changing
the mask. The accepted mask takes effect only after the OK response is copied.

Disabled indicators still update the values returned by CIND. They do not
consume notification queue space. Applying a new mask removes only unsent
notifications for disabled indicators, preserving the order of the others.
Tests cover omitted/excess fields, mandatory indicators, atomic rejection,
delayed commitment and filtering a queue containing mixed indicator changes.

The gateway now accepts `AT+VGM` and `AT+VGS` after baseline service-level
setup, following sections 4.29.2 and 5.3 of the inspected specification. Values
must be decimal integers in 0..15. Reports update stored headset microphone
and speaker gain only when their OK reply has been copied for transmission.
`gain_known` bit 0 identifies a received microphone report and bit 1 a speaker
report, so initial zero storage is not mistaken for a reported minimum gain.
Invalid reports leave both stored gains unchanged. These are headset settings;
receiving them does not change a RISC OS mixer or establish audio.

Tests cover reports before setup, exact minimum/maximum values, delayed
commitment, malformed and out-of-range values, and independent gain storage.
Gateway-originated remote gain changes are not yet implemented.

Continue applying the normative requirements to indicator updates, other
gateway commands, codec negotiation and audio connection control. Advertise
only capabilities actually implemented. The
gateway must not report successful audio establishment until the synchronous
transport and both PCM directions are operational. `bridge_session` enforces
that rule: public state READY is reached only after security, the HFP exchange,
SCO setup, both ISO workers and the copied PCM owner are active.

## Baseline CVSD audio owner

`host/c/hfp_audio` joins a completed baseline HFP service-level connection to
the scheduled synchronous-link and duplex USB components. Startup requires an
explicitly secured ACL and legacy voice setting `0x0060`; the coordinator does
not perform or infer pairing. Both bandwidth fields must be 8000 bytes/second.
It queues Setup Synchronous Connection only after
the HFP AG and RFCOMM session are READY.

USB voice starts after the command transfer, Command Status and peer-matching
Synchronous Connection Complete have all succeeded. The completion must report
CVSD air mode 2 and nonzero directional packet lengths. Any other air mode is
disconnected without enabling USB voice, because the current feature bitmap
does not advertise optional codec negotiation.

The parent continues to pump HFP control and the global HCI scheduler. During
shutdown the audio owner first stops and drains both USB directions, restores
voice alternate zero, and only then queues HCI Disconnect. An accepted setup is
still resolved and disconnected if control disappears while setup is in flight.

Tests cover the security gate, setup receipts, CVSD validation, duplex startup,
copied output, joint USB drain, ordered disconnect and non-CVSD rejection. They
use simulated USB transport and do not establish a physical headset connection.

## Baseline PCM boundary

Pinned NetBSD `sys/dev/bluetooth/btsco.c` identifies Voice Setting `0x0060` as
8000 Hz, mono, signed 16-bit little-endian PCM. `port/c/hfp_pcm` exposes exactly
that copied frame format above a RUNNING audio session. It provides separate
4096-byte playback and capture queues and never retains application buffers.

Playback is divided into caller-selected, whole-sample HCI payloads and retained
unchanged while the SCO output spool is full. Capture preserves good even-sized
PCM payloads. Controller-reported erroneous or lost data becomes equal-length
silence; a zero-length loss report and a missing sequence use one nominal packet
of silence. Sequence-gap insertion is bounded to one packet per pump call.

Pending good capture data has separate storage from the silence template. A
regression test specifically verifies that inserting silence before a later
good packet cannot overwrite that packet. This is a transport loss policy, not
acoustic packet-loss concealment. `BluetoothVoice_Write` and
`BluetoothVoice_Read` expose these queues directly to RISC OS applications;
the USB/controller cadence supplies the physical 8 kHz clock.

The isolated module fixture completes the full HFP and CVSD path, writes
playback samples, reads captured samples, then drains both ISO directions and
the ACL link. A physical headset has not yet supplied that clock or verified
interoperability.
