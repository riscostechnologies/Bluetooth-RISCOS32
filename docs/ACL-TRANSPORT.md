# ACL data path

`port/c/acl_receive` and `port/c/acl_transmit` adapt the ACL portions of the
pinned NetBSD `hci_link.c` revision 1.26. Transmit credit accounting also draws
on `hci_event_num_compl_pkts` in `hci_event.c` revision 1.26. Source notices are
retained; include `NETBSD-NOTICE.md` with binaries incorporating these adapters.

The receive/transmit components move L2CAP packets over established classic
ACL links. `port/c/acl_link` now provides a separate outgoing connection and
disconnect state machine adapted from the same NetBSD link/event sources.
The components do not implement L2CAP signaling, channel configuration, SDP,
RFCOMM, security or voice profiles.

## Outgoing connection lifecycle

The link state machine copies a selected peer address and encodes Create
Connection using the initial DM1/DH1 packet mask, supplied inquiry paging/clock
information, and no role switch. Its caller must own the controller's global
command-credit scheduler. At most one Create Connection may await Command
Status per controller: that event identifies an opcode, not a peer address.
Route command status to the operation that issued it and also process its
credit field in the controller scheduler.

The command API peeks bytes; the owner records submission immediately after
USB accepts them, then separately delivers USB completion and HCI events.
OPEN requires successful USB transfer, successful Command Status and a matching
successful Connection Complete. The handle accessor remains unavailable until
all three have arrived. The initial encryption-mode field is recorded, but OPEN
does not establish authentication, pairing or a secure profile connection.

Disconnect uses HCI reason `0x13`. A successful matching Disconnection Complete
closes the link immediately; later USB/status callbacks cannot reopen it.
Failure to disconnect preserves the established handle after USB completion,
with an error for the owner. A remote disconnect is handled even while local
command cleanup is pending. CLOSED/REJECTED do not grant permission to free
state while a submitted USB command still has a callback outstanding.

Queue and accepted-operation deadlines are each thirty seconds. An unsent
connect timeout rejects the operation; an unsent disconnect timeout keeps the
link open. Timeout or transfer failure after submission enters RECOVER, which
requires the controller owner's drain/reset recovery. This component does not
silently retry uncertain operations or implement Create Connection Cancel.
Reuse requires fresh state after old transfers and events are drained.

This remains a per-peer component. `hci_commands` now supplies a controller-wide
scheduling component, including a tested handoff from initialization to ACL
setup. The event dispatcher, incoming-connection policy and physical USB worker
integration are still required. It does not accept unsolicited peers automatically.

## Receive

Each assigned connection handle has its own caller-owned receive storage. Feed
complete HCI ACL packets from `hci_stream`, without an H4 type octet. The adapter
implements NetBSD's classic point-to-point START(2) and FRAGMENT(1) receive
semantics. The initial fragment must contain the four-byte L2CAP header.

Only a complete packet of exactly the declared L2CAP length is exposed. The
caller must consume that packet before another input for the same link can be
accepted. BUSY means preserve and retry the input, not silently drop it. A new
START replaces incomplete data; a lost/invalid fragment discards the partial
packet and requires a new START. Unknown handles do not alter another link's
storage. No link is automatically disconnected by this adapter.

## Transmit

One controller owns eight link slots and 32 bounded fragment receipts. Each
link has caller-owned storage for one copied complete L2CAP packet. A second
packet waits until the first has completed and the owner consumes its result.
The controller ACL payload limit and the host transport packet limit both
constrain fragment size. Selection rotates among links after each submission.

Peek produces a complete USB HCI ACL packet with START or FRAGMENT flags. It
reserves a stable receipt ticket and leaves the controller allowance unchanged.
If USB submission is busy, retry the same peek. Once USB accepts its copied
buffer, immediately call `sent` before processing any completion. This consumes
one advertised controller buffer. Map that receipt ticket to the separate USB
transport ticket; the two ticket spaces are not interchangeable.

There are two independent completion paths:

- USB completion reports whether the whole fragment transfer succeeded. It
  never grants controller credit. Short/failed transfers freeze transmission;
  their ambiguous delivery prevents automatic retry or credit restoration.
- The controller's Number Of Completed Packets event returns buffer credits.
  The owning HCI event dispatcher must validate its wire format and partition
  entries by its typed link table before passing **only ACL entries** here.
  SCO entries need separate accounting. The adapter validates the complete
  ACL batch, combines duplicate handles, and rejects unknown positive counts
  or counts exceeding the recorded debt before applying the batch.

Receipts remain until both completion paths have finished, in either order.
This also bounds host memory when a controller acknowledges packets before
USB completes them. A packet becomes available for `take` only after all its
fragments have been submitted and all receipts have finished. That is a local
transport result, not an acknowledgement by a remote application.

`port/c/bridge_acl` now provides the concrete private-emulator binding for one
secured classic link. It keeps separate host and core ticket spaces, maintains
a persistent bulk input request, preserves unconsumed completion suffixes,
feeds `hci_stream` before reassembly, and submits copied output fragments through
the host bridge. Its event entry point validates Number Of Completed Packets
batches before returning credits to `acl_transmit`.

The owner must serialize calls in a deferred context, provide link/operation
deadlines, and perform disconnect/reset recovery. Unbind refuses a link with
an active packet. This adapter does not guess whether disconnect has freed
controller debt. On an error, freeze, drain physical USB operations, resolve
controller state, and only then initialize fresh accounting. Drain old traffic
before reusing a controller handle; no wire-level generation tag exists here.

## Evidence and integration boundary

Host C89, address/undefined-behaviour sanitizer and RISC OS EABI builds pass.
The shared test executable also runs in the isolated RISC OS guest. Tests
fragment copied packets across two simulated connections, feed them into the
actual receive implementation, and compare the reconstructed bytes. They test
round-robin selection, zero-credit stalls, both completion orders, duplicate
callback rejection, invalid completion batches, receipt-slot saturation,
transfer failure, copied input and malformed PDU lengths.

The bridge ACL test uses the real transport-owner API with a deterministic fake
host bridge. It proves bulk submission/completion, controller-credit events and
receive reassembly across the same API used by the guest. `bridge_sdp` and
`bridge_rfcomm` route this owner through service discovery and a negotiated byte
stream. These remain constructed-peer tests rather than physical USB evidence;
no real service discovery or headset audio is claimed.

Connection tests exercise all six USB/status/connection-complete orderings,
rejection, contradictory replies, wrong-peer events, malformed packet lengths,
remote disconnect during setup, failed local disconnect, timer wrap and recovery
after uncertain submission. A further test obtains the handle from the actual
connection state machine, binds it to the transmit/receive components, and
round-trips a fragmented L2CAP packet. These use simulated HCI replies. The same
tests pass in the isolated RISC OS guest; no physical peer was contacted.
