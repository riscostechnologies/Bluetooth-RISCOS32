# Classic L2CAP signaling boundary

`port/c/l2cap_signal` parses and encodes classic signaling PDUs on CID 1.
Its wire layout follows the project's pinned NetBSD `sys/netbt/l2cap.h`.
This is the bounded-buffer boundary used by `port/c/l2cap_channel`, the outgoing
Basic Mode channel state machine. Neither component implements SDP/RFCOMM.

The receive input is one complete, reassembled L2CAP PDU including its four-byte
header, as produced by `acl_receive`. It must remain unchanged during iteration.
Opening validates the outer length, every signaling command header, nonzero
identifiers, fixed parameter sizes for known commands, and configuration option
boundaries before exposing the first command. A malformed later command therefore
prevents dispatch of the whole batch. A different CID is reported separately.
Empty signaling PDUs are rejected. Unknown codes, options and future reject
reasons are retained for the owner to apply protocol policy.

Iteration returns borrowed parameter slices. The output command and iterator
remain unchanged on failure; reaching the end leaves the output unchanged.
The encoder produces a single-command PDU suitable for `acl_transmit`, validates
capacity and wire lengths before writing, and never partially writes on error.
Input parameters and output storage must not overlap.

This component validates framing, not channel semantics: the owner must allocate
identifiers per ACL link, correlate responses, validate PSMs and channel IDs,
negotiate options and continuation flags, manage both configuration directions,
send required rejects, and implement timeouts and disconnection. It must also
enforce the signaling MTU appropriate to its connection. Unknown commands must
not be treated as accepted merely because they parse successfully.

The core test suite checks multiple commands in one PDU, truncation, malformed
trailing commands, zero identifiers, option overruns, non-signaling traffic,
unknown commands/options, empty echo payloads, and output preservation when
encoding fails. These are software protocol tests, not physical dongle tests.

## Outgoing Basic Mode channels

The channel state transitions adapt the pinned NetBSD `l2cap_signal.c` connection
and configuration handlers, retaining their BSD license notice. Explicit copied
output slots replace kernel mbufs and request dispatch. The parent ACL owner
must reserve a dynamic local CID and two distinct signaling identifiers before
starting a channel, check peer CID uniqueness on Connection Response, and route
validated commands from the correct ACL link. Identifiers and CIDs must remain
reserved until old traffic is drained; a timeout does not prove remote cleanup.

The state machine sends Connection Request and waits for its matching response.
Pending responses do not extend the 60-second overall setup deadline. Success
records the remote CID and sends Configuration Request advertising the local
receive MTU. Peer configuration requests are handled independently; either
direction may finish first. OPEN requires both acceptance of our configuration
and acceptance of our successful peer-configuration response into the parent's
ordered ACL transmit queue. It is not evidence of physical USB completion.

Responses take priority over queued local requests. `output` copies a stable
PDU without consuming it. Call `sent` immediately after ACL TX accepts its copy,
before delivering any other channel event. On TX backpressure, retry without
calling `sent`. The single response slot reports BUSY without consuming a new
peer request; the dispatcher must retain that command and retry. Downstream
transport failure requires aborting the channel and recovering the parent link.

Peer MTU values of at least 48 are accepted; absent MTU means 672. Smaller values
receive a minimum-MTU counteroffer. Flush timeout is informational. Unsupported
mandatory options receive Unknown Option and optional hints are ignored.
Repeated effective configurations are acknowledged; changes after acceptance
are rejected. An MTU-only counteroffer to our request can be retried up to three
times. The proposed MTU must be at least 48, differ from the last proposal, and
fit within the original receive limit. The channel marks `retry_pending`; the
parent reserves a fresh link-wide identifier and calls `retry`. A per-channel
bitmap additionally prevents reuse of any identifier previously used by that
channel, including the first configuration identifier after a retry. Late
responses to old attempts cannot complete the current request. All attempts
share the original deadline. Unsupported or malformed counteroffers fail setup.
Configuration continuation is currently rejected; continued responses fail setup.
Enhanced retransmission modes and QoS are not implemented. These limitations
need interoperability work before claiming general headset compatibility.

An OPEN channel can encode data using the remote CID/transmit MTU and expose
received payloads only for its local CID and receive MTU. This enforces the two
directions separately. Local and peer-initiated disconnection stop data access;
peer disconnection gets its matching response. FAILED channels with an assigned
remote CID can queue a cleanup Disconnection Request. Failure before learning
the remote CID may require closing the ACL link to release remote state.
Incoming service acceptance and security policy are not present yet. The shared
dispatcher described below supplies the outgoing channel table and routing.

Channel tests exercise both configuration completion orders, an early peer
request, stable output while the queue is busy, pending/wrong/early responses,
MTU counteroffers, unknown options and hints, duplicate configuration, local and
remote close, failure cleanup, timer wraparound, and separate data MTU limits.
They also check counteroffers exceeding receive storage, below-minimum MTUs,
retry exhaustion, fresh identifiers, stale responses and successful retry.
Both directions of the test signaling exchange pass through the actual ACL
transmit/reassembly components with a five-byte fragment payload and one credit,
alternating USB/controller completion order. These are simulated transport
completions; no WinUSB or physical radio transfer is performed by this test.

## Shared dispatcher

`port/c/l2cap_dispatch` owns up to eight outgoing channel lifetimes on one
established classic ACL link. It assigns local CIDs and reserves distinct
request identifiers for connection, configuration, retry and disconnection.
Neither IDs nor slots are reused during that link's lifetime. Exhaustion requires
closing/reestablishing the ACL link; this conservative policy still needs a
production reuse/drain mechanism for long-lived sessions.

Complete signaling PDUs are validated before any command is dispatched, then
copied into one bounded input slot. A stalled command retains its cursor while
responses wait for ACL TX. The caller may release an accepted input PDU; BUSY
means the new PDU was not consumed and must be retried. Output peeks do not
consume replies. The owner calls `sent` immediately after its ordered ACL queue
accepts the copied PDU. Channels receive round-robin output service, with generic
link responses taking priority. All operations must remain serialized.

The dispatcher handles Echo Requests with empty replies, matching the imported
NetBSD implementation. Information Responses advertise Fixed Channels support
and only the classic signaling fixed channel. Unrecognized information types
receive Not Supported. Unknown commands and invalid channel requests receive
Command Reject; unmatched known responses are discarded to avoid reject loops.
Incoming Connection Requests currently receive PSM Not Supported because no
inbound service listener exists yet. The receive signaling MTU is 672; a complete
valid larger PDU receives MTU Exceeded without dispatching its commands.

Matching Connection Responses are checked for duplicate active remote CIDs
before assigning them. A collision aborts the dispatcher and all its channels,
requiring parent ACL recovery. Individual channel negotiation failures remain
visible through their state and can be closed by the parent. This layer does
not itself reset controllers or submit physical USB operations.

Dispatcher tests establish separate SDP/RFCOMM-PSM channels with reversed
response order, process batched peer configurations, close one channel while
the other stays open, and verify generic replies, copied-input backpressure,
malformed trailing commands, oversized signaling, CID collisions, capacity and
identifier exhaustion. Automatic MTU retry is also tested with a stale response
to the old identifier. These tests use simulated peer messages; opening a PSM in
the test does not implement that service or prove headset interoperability.
