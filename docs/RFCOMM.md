# RFCOMM port

`port/c/rfcomm_frame` adapts the frame boundary from the pinned NetBSD
`sys/netbt/rfcomm_session.c`. It supports SABM, DISC, UA, DM and UIH frame types,
one- and two-octet length fields, and optional credit octets. Input is one
complete RFCOMM frame inside an L2CAP payload; this is not an HCI stream parser.

The decoder validates the address extension bit, recognized control type,
exact frame length and FCS before exposing borrowed payload data. The encoder
checks all arguments and capacity before writing. Information payloads are
limited to 32767 bytes, and control frames must have no information payload.
The channel owner must additionally enforce the negotiated MTU, valid DLCIs,
command/response direction and poll/final semantics.

The caller supplies the negotiated per-DLC credit mode. In credit mode, a UIH
frame with PF set on a nonzero DLCI includes a credit octet outside the declared
information length. The parser does not infer credit mode from packet length.
Encoding a credit requires that same frame shape. This component exposes credit
bytes but does not debit or grant flow-control credits.

For UIH frames, FCS covers address and control only. For the other supported
frames it also covers the length field. UIH payload, length and credit bytes
are not checksum-protected at the RFCOMM layer. Lower-layer reliability and
length validation remain necessary. FCS validation is not authentication.

The implementation uses a bitwise reflected CRC. Tests compare every supported
address/control combination with the lookup table extracted independently from
the unchanged, hash-checked NetBSD source. They cover 127/128-byte length
transitions, the maximum information length, all truncations of selected frames,
wrong credit mode, checksum corruption and unchanged output on encode failure.
`scripts/prepare-rfcomm-test.py` generates the test oracle under ignored build
output and refuses a modified upstream source.

The higher layers now implement outgoing DLC negotiation, general multiplexer
commands and an HFP AT-command byte stream. Passing these tests still does not
establish a physical RFCOMM or headset connection.

## DLCI-0 multiplexer lifecycle

`port/c/rfcomm_mux` adapts the NetBSD session's DLCI-0 SABM, UA, DISC and DM
handling. It operates on an already OPEN RFCOMM L2CAP channel. The parent fixes
the initiator/responder role from the L2CAP connection origin. The initiator
queues SABM and waits for UA; the responder waits for SABM and opens only after
its UA response is accepted into the ordered transmit queue.

The state machine validates role-dependent command/response bits and PF rules.
Responses received before the corresponding request was accepted cannot open
or close it. It acknowledges a repeated responder-side SABM without resetting
an open multiplexer. Incoming requests wait unchanged if the response slot is
busy. Either side can request close, including simultaneous DISC exchanges.
Pending startup and close each have a bounded 20-second deadline.

Output is a copied RFCOMM frame for the parent's L2CAP/ACL queue. As with the
other port components, `sent` immediately follows acceptance of copied bytes,
not physical USB completion. An OPEN multiplexer is not an open individual DLC.
Nonzero-DLCI traffic is left to the DLC owner. DLCI-0 UIH traffic is left to the
multiplexer-command owner, which may process it only while OPEN. That command
owner is not implemented yet. Initiator-side incoming SABM on DLCI 0 is treated
as an invalid role exchange requiring parent teardown.

FAILED requires parent L2CAP teardown. CLOSED does not prove transport drainage;
the parent must complete queued UA/ACL/USB lifetimes and close the L2CAP channel
before releasing state. Tests exchange encoded frames between initiator and
responder state machines and cover early/stale UA, retransmitted SABM, both
close directions, crossed close, response backpressure, role/PF errors, DM refusal
and timer wraparound. They do not exercise a physical remote device.

## Multiplexer command encoding

`port/c/rfcomm_mcc` encodes and decodes one MCC message inside a validated
DLCI-0 UIH payload. It implements the distinct MCC length format with seven
data bits and an extension bit in every length octet, including the three-octet
form. MCC output must fit the 32767-byte maximum RFCOMM information field.

Known message lengths are checked for PN, MSC, RPN, RLS, NSC, FCON and FCOFF.
TEST data and unknown one-octet types retain their payload for the command owner.
Extended type fields are unsupported. The inner command/response bit is exposed
separately from the role-dependent outer RFCOMM frame address bit. Decoder output
is borrowed; encoder input and output must not overlap. Errors leave outputs
unchanged.

Tests compare a PN request with its expected bytes, exercise both MCC length
transitions (127/128 and 16383/16384), carry messages through the RFCOMM UIH
encoder/decoder, and reject truncation, oversized information, invalid known
message lengths and unsupported extended headers. This implements the wire
format, not PN negotiation, credit allocation or modem-status state transitions.

## Outgoing parameter negotiation

`port/c/rfcomm_pn` now supplies the outgoing PN transaction. The parent must
already have an OPEN multiplexer, reserve the DLCI and serialize first
negotiation of session-wide credit mode. It supplies an MTU that fits both
L2CAP and receive storage, and initial receive credits backed by packet slots.
The transaction proposes credit mode unless the session has already disabled it.

Only a matching PN response received after submission may complete negotiation.
Responses for another DLCI, peer commands and stale responses do not advance it.
The peer MTU must be at least 23 and no larger than the offered limit. Credit-mode
responses must agree with any established session mode. PN initial credits use
the low three bits, matching NetBSD; this differs from subsequent UIH credit
octets. A successful non-credit fallback clears both initial-credit values.

Output is MCC payload for a DLCI-0 UIH frame; parent acceptance follows the same
copied-output contract as other components. A 20-second deadline includes queue
wait. DONE provides negotiated parameters only. Parent security checks and
SABM/UA establishment must precede data. Incoming PN handling is not implemented.
Tests exercise the transaction through MCC/UIH codecs,
early and wrong-DLC responses, fallback, mode consistency, MTU bounds, credit
masking, backpressure and timer wraparound.

## Credit accounting

`port/c/rfcomm_credit` tracks one credit-mode DLC using caller-owned MTU-sized
packet slots. Initial PN receive credits reserve slots immediately. Nonempty
received frames consume one advertised credit and occupy one slot; a slot is
released only when its consumer has finished the whole packet. Available grants
exclude occupied slots and outstanding credits, and are capped at 255 per frame.

Outgoing data consumes one peer credit only after the ordered transmit queue
accepts its copied frame. A preview consumes nothing. Credit-only frames may be
sent with no transmit credits and do not debit them. Incoming data consumption
and credit addition are checked together before updating counters. Invalid
lengths, ungranted data, excess releases and counter overflow freeze accounting
without applying the invalid update. The parent must recover rather than refund
credits after a later physical transport failure.

The parent must supply real backing buffers, copy accepted incoming payloads
into those reserved slots, associate releases with consumed packets, enforce
frame/L2CAP overhead, and integrate this with an OPEN DLC. This component does
not allocate buffers or implement non-credit modem flow control. Tests cover
exhaustion, credit-only traffic, bounded grants, atomic rejection, overflow and
10000 producer/consumer cycles. Physical data transfer remains unverified.

## Outgoing DLC lifecycle

`port/c/rfcomm_dlc` consumes a successful PN result and establishes the selected
individual channel with SABM/UA. It reuses the tested control handshake while
mapping its address and command/response bits to the actual DLCI and multiplexer
role. An outgoing channel on an initiator multiplexer has an even DLCI; on a
responder multiplexer it has an odd DLCI. That direction is checked before start.

The caller must already satisfy its authentication/encryption policy and keep
the multiplexer OPEN. This component does not perform security checks itself.
It initializes credit accounting from PN only when the supplied receive-slot
count can back the advertised credits. OPEN requires a matching acknowledgement
after the SABM has been accepted into the ordered transmit queue. Stale UA and
other DLCI traffic cannot open it.

Local or remote close uses the same acknowledged lifecycle. Closing, failure
and timeout freeze credit accounting. The parent must drain queued frames and
physical IO before releasing buffers. Valid UIH for an OPEN DLC is left to its
data owner to enforce modem-status readiness, account credits and copy payloads;
this lifecycle component does not consume those bytes. Incoming DLC establishment
is not implemented. OPEN here does not yet mean MSC exchange or HFP is ready.

Tests derive parameters through the PN/MCC codecs, establish outgoing channels
for both multiplexer roles, verify early/wrong/stale acknowledgements, initialize
buffer-backed credits, preserve data-owner accounting, and exercise local/remote
close and timeout. These exchanges use constructed peer frames.

## Modem-status exchange

`port/c/rfcomm_msc` implements MSC exchange on an already OPEN DLC. It queues
local status, matches its acknowledgement to the submitted status bytes and
echoes peer commands, preserving optional break information. Startup becomes
ready only when the local status has been acknowledged and the peer-status
response has been accepted into the ordered transmit queue. A stale or early
acknowledgement cannot establish readiness.

Local updates serialize behind completed exchanges. Peer updates immediately
record accepted status, but readiness waits until their reply is queued.
Responses take priority; a busy response slot leaves a new peer command
unconsumed for retry. Pending exchanges have a 20-second deadline. Modem/address
extension bits and optional break length are checked. Output is MCC bytes for
the parent's role-correct DLCI-0 UIH frame, using the same acceptance contract.

The parent must combine MSC readiness with OPEN DLC/multiplexer state, enforce
peer/session flow control for non-credit data, and stop this component on close.
This does not implement HFP service-level connection. Tests exchange MSC between
two components and cover matching/stale acknowledgements, simultaneous startup,
backpressure, updates, break echo, malformed extension bits and timer wraparound.

## Receive buffer binding

`port/c/rfcomm_rx` binds a credit-mode DLC to 1..32 actual caller-owned,
MTU-sized packet slots. It may attach after DLC initialization while SABM waits,
so storage backs the credits already advertised in PN. Data is accepted only
on the OPEN DLC. It validates the target frame and role, applies the credit
update, then copies its payload before returning to the serialized event owner.
Credit-only frames update the window without occupying a slot.

Reads expose a byte stream across packet boundaries. Partial consumption keeps
the slot occupied; only consuming the final byte releases it. The parent still
gates application reads on MSC/profile readiness and transmits replacement
credits through its ordered TX path. External code must not separately release
credits for slots owned by this queue. Closure/failure prevents further reads
until parent recovery and drainage permit disposing of the stored data.

Tests negotiate/open a DLC, copy fragmented command text, overwrite source
buffers to verify ownership, read across packet boundaries, exercise 1000 ring
reuse cycles, handle credit-only frames, preserve other-channel isolation and
reject an ungranted extra packet. Non-credit receive buffering and session-wide
integration remain unfinished; these are constructed-input tests.

## Transmit buffer binding

`port/c/rfcomm_tx` copies one pending byte block into caller-owned storage and
segments it into UIH frames within both the negotiated DLC MTU and the peer
L2CAP MTU, including RFCOMM header, optional credit byte and FCS overhead.
Output waits for an OPEN DLC and completed MSC exchange. Nonempty data waits
for peer credits; credit-only frames can advertise freed receive slots even
when there is no peer credit for pending data.

The first successful output preview fixes its payload and grant counts.
Repeated previews preserve the same frame even if receive processing frees
more slots in between. Neither preview nor a rejected lower-layer submission
debits credits or removes command bytes. The serialized parent calls `sent`
immediately after its ordered L2CAP/ACL queue accepts a copied frame. This
commits the credit accounting and advances the pending block. Acceptance does
not prove USB completion or remote delivery.

This sender must exclusively own data transmission and credit grants for its
DLC, sharing accounting with `rfcomm_rx`, whose actual slots must already back
the advertised credits. The parent must stop transmission when the enclosing
multiplexer closes and drain outstanding IO before disposing of buffers.
Closure or MSC failure prevents further output; non-credit flow control and
the full session coordinator remain unfinished.

Host tests, address/undefined-behaviour sanitizer tests and the ARM executable
in the isolated RISC OS test machine pass. They cover copied source ownership,
segmentation under a 48-byte L2CAP limit, unchanged retry previews, queue
backpressure, credit stalls, receive-slot grants and MSC/close gating. These
are constructed peer exchanges, not physical Bluetooth or audio tests.

## Generic multiplexer commands

`port/c/rfcomm_control` handles validated MCC framing on an already OPEN,
role-checked multiplexer. TEST commands return the exact supplied payload;
FCON/FCOFF commands update the peer's session flow state and queue an
acknowledgement. That state governs non-credit data only. Control responses
must remain sendable while data flow is stopped.

One caller-owned response buffer preserves the reply after the incoming frame
is released. A second command returns BUSY without applying its flow change,
so the parent retains and retries that input. Output previews consume nothing;
`sent` follows immediate acceptance by the ordered copied transmit queue.
The parent must provide storage and outgoing L2CAP capacity large enough for
the permitted TEST response. Capacity failure requires session teardown.

Unknown commands receive NSC with the original command type octet. Unknown
responses are consumed without a reply, preventing response loops. Receiving
NSC fails this mandatory-command-only session, following NetBSD's policy; the
rejected type is preserved for diagnostics. Pending output becomes inaccessible
on failure. PN and MSC are returned to their respective owners rather
than misclassified as unsupported. The enclosing coordinator still needs to
route them and enforce lifecycle, role and transport-drain requirements.

Tests cover empty, short and extended-length TEST payloads, copied ownership,
unchanged output retries, flow changes under backpressure, exact NSC encoding,
unknown-response suppression, fatal NSC, malformed input and bounded storage.

RPN current-settings queries return NetBSD's virtual-port defaults: nominal
9600 baud, eight data bits, no parity, one stop bit, no serial flow control,
XON 0x11 and XOFF 0x13. Negotiation responses retain proposed values while the
acceptance mask marks only supported requested settings. Unlike NetBSD's
all-flow-bits acknowledgement, a partial flow mask acknowledges only requested
bits. Baud rate is nominal metadata; no physical UART is configured. Parity
type is not acknowledged for this no-parity port. RLS replies normalize the
address EA/C/R bits and retain only the four defined status bits, matching
NetBSD's behaviour. The parent must route these requests only for eligible
DLCs; this generic handler does not own their lifecycle.

RPN responses require at least ten bytes of response storage, including when
the incoming query is only three bytes. Tests check the default wire response,
unsupported parameter rejection, partial masks, response suppression and RLS
normalization. These supplement the copied-response backpressure tests.

## Composed session owner

`port/c/rfcomm_session` coordinates one outgoing DLC on a dedicated, already
OPEN RFCOMM L2CAP channel. Its caller supplies the actual multiplexer role,
validated SDP server channel, directional L2CAP limits, and must already have
satisfied authentication/encryption policy. Startup advances through mux,
PN, DLC and MSC before exposing READY byte-stream operations. Four embedded
receive slots back the initial credit offer; transmit storage copies up to
1024 pending bytes. The local L2CAP receive limit is currently 48..518 bytes.
The negotiated payload MTU accounts conservatively for framing overhead in
both directions. Non-credit fallback fails explicitly pending implementation.

One output API prioritizes mux control, generic MCC replies, MSC, PN, DLC
control and then application data. Its `sent` call must immediately follow
ordered acceptance of the most recently previewed copied frame, before another
session operation. A retry obtains output anew, permitting newly queued
control replies to take priority. Received payloads are copied into the shared
credit-backed queue; application reads wait for MSC readiness. RPN/RLS are
routed only to the selected OPEN DLC. Incoming DLCs and PN commands remain
unowned for the enclosing connection layer to handle.

Local close shuts the dedicated multiplexer and freezes DLC data. Remote mux
close follows the same path. Remote close of the individual DLC immediately
enters CLOSING and suppresses queued application, MSC and generic control
output. Its UA acknowledgement is accepted into the ordered queue before the
owner starts multiplexer DISC. This avoids reporting a normal DLC close as a
protocol failure or sending stale modem replies ahead of the close response.
Protocol failure or timeout aborts this owner;
the caller must close L2CAP and drain queued ACL/USB IO before reinitializing
or releasing it. CLOSED is a protocol state, not proof of that drainage.
READY is RFCOMM readiness, not an HFP service-level or audio connection.

The integration exerciser uses actual frame/MCC codecs and all component state
machines, for both multiplexer roles, to negotiate a DLC, exchange MSC, send
AT bytes, receive copied OK bytes and close. It also checks control priority,
stable previews, early application gating, startup timeout, wrong-role frames
and explicit rejection of non-credit fallback. Local, remote-DLC and remote-mux
shutdown are exercised for both roles with application bytes, MSC replies and
TEST replies pending; no such output escapes after closure starts.
Physical transport and real-device validation remain outstanding; discovery
orchestration is described below.

## L2CAP data binding

`port/c/rfcomm_l2cap` binds the session to a dedicated OPEN channel owned by
the parent L2CAP dispatcher. It derives the directional receive/transmit MTUs
from that channel, emits complete L2CAP PDUs using its remote CID and accepts
reassembled input only on its local CID. Other CIDs remain unconsumed for
their owners. Malformed input on this channel aborts the RFCOMM session.

The parent still creates the PSM 3 channel, supplies the SDP server channel
and actual multiplexer role, and satisfies security policy before binding.
It ticks signaling first, prioritizes signaling output over RFCOMM data, and
closes L2CAP when the RFCOMM session terminates. Losing OPEN or changing the
negotiated MTUs stops RFCOMM output; pending acceptance is invalidated. Channel
storage and identifiers must remain stable until all old IO is drained.

Output contains the complete L2CAP header and RFCOMM frame. A buffer too small
for that whole PDU writes nothing and cannot be committed by `sent`. Accepted
output follows the existing ordered-copy contract; it is not USB completion.
Tests cover asymmetric CIDs and MTUs, mux/PN traffic across this boundary,
retry previews, wrong-CID isolation, malformed lengths and channel loss.
Automatic SDP-to-channel setup is provided by the connection owner below. The
host-bridge owner described after it connects this boundary to ticketed ACL USB.

## Discovery-to-connection owner

`port/c/rfcomm_connect` now implements the automatic handoff above. Start
requires a successful SDP result and verifies that its dedicated SDP channel
has reached CLOSED on the same dispatcher. It copies the selected service
metadata, allocates a fresh PSM 3 channel and waits for both configuration
directions before starting the RFCOMM binding as multiplexer initiator.
The caller must satisfy security policy before starting; this layer does not
authenticate a device or choose a pairing policy.

RUNNING includes RFCOMM negotiation, so applications must still check the
embedded session for READY. The parent routes signaling to the shared
dispatcher and gives its output priority over this owner's data output.
The owner preserves the existing immediate copied-acceptance contract.

After RFCOMM closes or fails, it disconnects its L2CAP channel and waits for
terminal state. Clean cleanup yields DONE; an error yields FAILED. A missing
remote CID during cancellation, exhausted identifiers or failed disconnect
requires parent ACL recovery, explicitly reported by `needs_acl_recovery`.
Failed disconnects are not restarted indefinitely. Initial setup has a
180-second overall limit; once READY has been reached, later modem updates
do not reuse that startup deadline. Lower-layer stage deadlines still apply.

Tests execute the real SDP query/result/close path, then new PSM channel
creation and bidirectional configuration before checking outgoing RFCOMM
SABM against the assigned peer CID. They also check premature-handoff rejection,
negotiated limits, clean cancellation, unknown-CID cancellation and disconnect
timeout recovery. The successful path now continues through PN, DLC and MSC,
an AT/OK byte exchange over complete L2CAP PDUs, a peer MSC update after the
180-second startup period, RFCOMM DISC/UA, and finally L2CAP disconnect. This
checks the combined owners and ordered teardown rather than only their
individual entry points. These are constructed peer protocol exchanges. Physical
transport, pairing and voice IO remain unfinished.

## Host-bridge RFCOMM owner

`port/c/bridge_rfcomm` composes one secured bridge ACL link, its existing L2CAP
dispatcher and the SDP-selected RFCOMM server channel. It opens PSM 3 with the
RFCOMM maximum receive MTU of 518 bytes, services signaling before data, and
keeps protocol acceptance separate from physical USB completion. It exposes
the byte stream only after mux, PN, DLC and MSC negotiation reach READY.

Local close drains RFCOMM DISC/UA before requesting L2CAP disconnect. DONE is
reported only after the channel closes and the last accepted bridge output has
completed. Malformed CID traffic, USB failure, session failure and unresolved
channel state become terminal errors rather than apparent connections.

The peer-driven test runs the real bridge ACL, L2CAP and RFCOMM code: it opens
PSM 3, negotiates both configuration directions, completes SABM, PN, DLC and
MSC, exchanges `AT` and `OK` bytes, then closes RFCOMM and L2CAP in order. The
same case passes native, address/undefined-behaviour sanitizer and ARM builds.
It is deterministic constructed-peer evidence; the Windows UAC hardware gate
and a headset in pairing mode are still required for physical validation.
