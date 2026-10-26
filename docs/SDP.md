# SDP service discovery client

The baseline now includes eight unchanged files from NetBSD libbluetooth at the
same pinned commit as the USB/HCI stack. `scripts/import-sdp.py` records their
immutable URLs and SHA256 hashes; `verify_upstream.py` checks all 39 files.

`port/c/sdp_query` adapts `sdp_service_search_attribute` from `sdp_service.c` into
a nonblocking client. It requests Service Class ID List (0001), Protocol
Descriptor List (0004), Bluetooth Profile Descriptor List (0009), and Supported
Features (0311) for a caller-selected 16-bit service UUID. For example, Hands-Free
is 111E and Headset is 1108. This retrieves advertised service data; the record
interpreter below selects an RFCOMM channel but does not implement those profiles.

The client requires an already OPEN SDP L2CAP channel, PSM 1. Request bytes are
SDP payloads with big-endian fields, not HCI or L2CAP packets. The owner must use
the channel's data encoder and ordered ACL queue, calling `sent` immediately
after accepting a copied request. Its negotiated receive MTU determines the
maximum response attribute count, allowing room for the five-byte SDP header,
two-byte count and maximum 17-byte continuation field. Requests fit within the
classic minimum L2CAP MTU even with the maximum continuation token.

One query stream is allowed per SDP channel. The owner reserves the starting
transaction identifier and its successors. Each continuation uses a fresh ID;
wrap is rejected. Responses for a different transaction are ignored. A response
for the current transaction must have the correct PDU type and exact parameter
length, a nonzero attribute byte count within the advertised and storage limits,
and a valid continuation field of at most 16 token bytes. Validation occurs
before copying each fragment. Total collection is bounded by caller storage
(up to 65535 bytes), 64 response pages and a 60-second overall deadline.

Only a final, complete and valid outer data-element sequence becomes a result.
Partial data remains inaccessible through the result API after errors. Remote
SDP error codes are preserved in the failed state; FFFE denotes local timeout
and FFFF denotes other local failures. The owner must drain old traffic before
initializing another stream. Buffers and query state must not overlap request
output or received input; received data is copied into caller-provided storage.

`port/c/sdp_data` adapts NetBSD element sizing/validation with explicit bounded
length arithmetic. It supports the standard scalar, UUID, string, sequence,
alternative and URL encodings, including 32-bit length fields. Recursive
validation is limited to 16 container levels and 4096 elements. This validates
encoding structure, not attribute semantics or service identity.

Tests split a representative Hands-Free attribute stream at every byte,
including within data-element headers, and verify copied continuation state,
stale transaction handling, buffer/response limits, remote errors, transaction
exhaustion, nested malformed elements and timer wraparound. Host and guest tests
use constructed responses; they are not evidence of discovery on a physical
headset. Physical validation is still required; protocol session integration is
described below.

## Service record interpretation

`port/c/sdp_record` interprets the completed query result. It requires a matching
Service Class ID List and the protocol sequence L2CAP followed by RFCOMM, with
a server channel from 1 to 30. An explicit L2CAP PSM, if present, must be 3.
Service and protocol UUIDs accept 16-bit, 32-bit and full Bluetooth-base 128-bit
encodings; a different UUID namespace does not match merely because its short
numeric part is equal. Optional profile versions and supported features are
copied with presence flags, and remain unverified peer-advertised metadata.

Attribute IDs must be unique and increasing, with complete key/value pairs.
Relevant attributes require their defined element types. The whole result is
checked before returning a candidate. No match returns zero; more than one
matching record returns ambiguity rather than selecting an arbitrary channel.
Malformed input and ambiguous results leave the caller's output unchanged.
Protocol alternatives and stacks with additional layers are not selected.

Tests cover service identity, both UUID aliases, different UUID namespaces,
metadata, server-channel bounds, duplicate attributes, multiple records,
ambiguity and malformed later records. Every continuation split in the SDP
query tests now runs through the record interpreter and verifies the resulting
RFCOMM channel. This proves constructed record handling, not live discovery.

## Discovery session

`port/c/sdp_session` coordinates the L2CAP dispatcher, query client and record
interpreter on an already established ACL connection. It allocates a dedicated
PSM-1 channel, waits for both configuration directions, then starts the SDP
transaction stream using that channel's negotiated receive MTU. Continuation
requests are encoded as complete L2CAP data PDUs using the assigned remote CID.
Only incoming data for the session's local CID is consumed.

The serialized parent must route CID-1 signaling to the dispatcher, service
dispatcher output first, then service session data output. Both output paths
feed the same ordered ACL transmit queue. Each corresponding `sent` call must
immediately follow acceptance of copied bytes, before any other state operation.
Backpressure consumes nothing. Neither acceptance nor these session tests prove
physical USB completion; the parent retains all real transport lifetimes.

On a unique record, no match, or query/record error, the session closes its SDP
channel. A discovered service becomes publicly available only after channel
closure. A clean no-match is distinguished from failure. Temporary response-slot
backpressure delays the close request; it does not reset the result or deadline.
Other channels on the dispatcher are preserved unless parent link failure or
the session's 180-second overall timeout requires link recovery. Channel and
query stages also retain their own shorter deadlines.

`needs_acl_recovery` explicitly reports unresolved transport/channel state,
including close timeout, ambiguous parent state, and identifier exhaustion.
The parent must tear down/recover that ACL link and drain physical IO before
reinitializing state. A clean refusal or completed error cleanup can finish
without this flag. The session does not itself submit USB IO or perform reset.

Tests run setup, query continuation, CID isolation, record extraction and close
through the real port components using constructed peer PDUs. They verify no
early result, unchanged output under backpressure, coexistence with another
channel, clean no-match, invalid record cleanup, query/close timeout and parent
abort. Remote-device discovery/pairing and physical headset validation remain
required.

`port/c/bridge_sdp` supplies the concrete host-bridge parent for this session.
It pumps ticketed ACL bulk input/output, routes signaling before SDP data,
commits each protocol output only after the ACL owner copies it, and waits
separately for USB completion. Its peer-driven test opens and configures PSM 1,
returns a Hands-Free record with RFCOMM channel 7, and closes the channel before
publishing the result. That result can be handed to `bridge_rfcomm` on the same
dispatcher without reusing channel state or request identifiers.
