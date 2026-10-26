# Inquiry, ACL connection and security

`port/c/hci_inquiry` is a bounded adaptation of the inquiry memo handling in
NetBSD `hci_event.c` revision 1.26. It emits the General Inquiry Access Code
command, accepts standard, RSSI and Extended Inquiry Result events, deduplicates
up to 16 addresses and records page-scan mode, class, clock offset, RSSI, a
bounded EIR name and advertised HSP/HFP UUIDs. A Bluetooth Audio/Video class or
voice UUID marks a candidate; neither proves that SDP or audio will succeed.

`bridge_controller` now runs inquiry through the actual RISC OS/emulator/plugin
ticket boundary. It can pass a selected result to `acl_link`, which uses the
observed page-scan mode and clock offset for Create Connection and retains the
controller-issued handle. The controller fixture proves connection-status and
connection-complete routing followed by a clean Disconnect transaction.

`port/c/hci_security` follows the authenticated/encrypted link policy from
NetBSD `hci_link.c`. It issues Authentication Requested and then Set Connection
Encryption. It reports secured only after each operation has a successful USB
receipt, matching command status and matching link event. Rejection remains
distinct from transport ambiguity, which requires controller recovery.

`port/c/hci_pairing` answers pairing requests only for the selected address.
It supports a copied stored link key, a copied legacy PIN, NoInputNoOutput SSP
capability, opt-in Just Works confirmation, explicit negative responses and
copying Link Key Notification data for later persistence. Pairing reply commands
have separate USB and HCI completion ownership in `bridge_controller`; they do
not satisfy the authentication command by accident.

The fixture injects a Link Key Request between Authentication Requested and
Authentication Complete. The guest must return the configured key before the
fixture continues, then it enables encryption and disconnects cleanly. This
validates the control path. It does not show that the physical controller or a
headset accepts the policy. A RISC OS configuration application and protected
link-key persistence are still required before real pairing is usable.
