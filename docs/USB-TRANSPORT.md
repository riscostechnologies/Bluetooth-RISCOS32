# USB transport findings and implementation boundary

## Verified dongle descriptors

The live Windows device uses service BTHUSB, driver version 10.0.26100.8972,
and compatible class/subclass/protocol E0/01/01. The read-only descriptor probe
in `host/c/usb_probe` uses SetupAPI to enumerate USB hubs and Microsoft's
IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION to read configuration zero.
It does not submit class commands, reset the controller, select an alternate
setting or change the Windows driver binding.

The probe found 0A12:0001 on hub enumeration index 4, port 3, with bcdDevice
8891 and bcdUSB 0200 at full speed. Hub enumeration indices can change; the
probe matches the vendor/product identity instead of hard-coding hub 4.
bcdDevice is a device revision field, not the Bluetooth core version.

The actual 177-byte configuration is stored under `test/fixtures` with its
SHA256 and metadata. Interface 0 supplies event IN 81, ACL OUT 02 and ACL IN 82.
Interface 1 supplies SCO OUT 03 and IN 83, with alternate packet sizes
0/9/17/25/33/49 bytes. Zero-bandwidth alternate 0 must not be used for voice.

`port/c/usb_transport` identifies those roles using the standard interface
layout used by NetBSD ubt_attach. It validates descriptor bounds, endpoint
counts, duplicate addresses, required roles and requested voice alternate.
It copies its result only after the whole configuration passes validation.
This first selector rejects high-bandwidth transaction multipliers; a later
backend for high-speed controllers needs explicit speed/scheduling support.

The code passes host, ASan/UBSan and isolated RISC OS guest execution using
all six actual alternate settings and malformed/truncated variants. Endpoint
selection is not evidence of submitted USB transfers or a voice connection.

## RISC OS USB interface

The authoritative USBDriver repository is:
https://gitlab.riscosopen.org/RiscOS/Sources/HWSupport/USB/USBDriver

Inspected revision: df856fcdcf373bd70a3a26686d3b8402933415bf.
The local inspection copy is under ignored `tools/rool-usb-reference`.
Relevant files:

- `build/Doc/USB`: public USB DeviceFS API, revision 0.95.
- `build/h/USBDevFS`: service blocks and DeviceFS reason codes.
- `build/Hdr/USBDriver`: host-controller registration SWIs.
- `build/c/usbmodule`: DeviceFS transfer implementation, including isochronous
  setup through usbd_setup_isoc_xfer.

Device-class drivers use FileSwitch/DeviceFS streams. Endpoint selection uses
special fields such as interface, alternate, endpoint and transfer type. These
fields are separated by semicolons in a DeviceFS pathname, for example
`devices#interface0;alternate0;endpoint1;interrupt:USB3`.
USBDriver_RegisterBus and InsertTransfer are host-controller interfaces and
must not be mistaken for the public class-driver API.

DeviceFS_CallDevice reason 0x80000000 sends a USB control request. R3 packs
bmRequestType, bRequest and wValue; R4 packs wIndex and wLength; R5 points at
the payload. R6 bit 0 requests immediate return with R7 pointing at a pollable
completion-status byte. The native probe uses this form and yields while polling.
After two seconds it reports a warning but retains the command buffer until
USBDriver reports completion or a terminal error, including device removal.
The callback form in R6 bit 1 is not available on RISC OS 5, so the module command
scheduler still needs deferred polling outside BluetoothVoice_Connect and other
non-blocking SWIs.

Service_USB reason Connected enumerates devices. DeviceFS provides buffer-space
and transfer-status operations. Inspect callback lifetimes, device removal,
isochronous output, alternate-setting ownership and cancellation before adding
the real backend; no empty compatibility macros may replace these mechanisms.

`port/c/usb_service` now decodes these service records. It validates the record
size and descriptor offset, copies the DeviceFS name, checks the speed and
Bluetooth class, and uses the tested endpoint selector. Full-speed is encoded
as 2 in current RISC OS USB service records, unlike the Windows speed enum 1.
Tests build an explicitly synthetic service-record envelope around the actual
captured CSR configuration and cover every truncation, invalid offsets and
unsafe DeviceFS name characters. Both host sanitizer execution and the isolated
RISC OS guest pass these tests.

`BTUSBProbe` is a native RISC OS diagnostic application. It resolves USBDriver
and DeviceFS SWI names, issues OS_ServiceCall with R0=1, R1=&D2 and R2=0,
processes the returned list and calls OS_Module reason 7 for each returned RMA
allocation after saving its next link. Device data is copied before freeing
the record. It neither opens endpoints nor submits control requests.

The actual isolated-guest probe on 8 September 2026 returned:

    USBDriver SWI present: no
    DeviceFS SWI present: yes
    USB records: 0; supported Bluetooth controllers: 0
    NO_TRANSPORT: no supported Bluetooth controller is exposed to this guest

The `--transport-open` mode is the non-command direct-target transport gate. It
opens and closes interface 0 alternate 0 interrupt IN endpoint 1 and both ACL
bulk endpoints without sending an HCI command. The `--hci-version` mode performs
the same stream setup and then runs the controller exchange. After
finding a supported controller it opens interface 0, alternate 0, interrupt IN
endpoint 1 and both ACL bulk endpoints as non-padding DeviceFS streams. It sends HCI Reset and Read Local
Version as pollable Bluetooth class requests through DeviceFS_CallDevice,
polls DeviceFS buffer space, reads no more than the advertised available byte
count, assembles fragmented HCI events and accepts only matching successful
Command Complete responses. The event stream is closed on every outcome.

Run it on the native target with the dongle attached directly:

    BTUSBProbe RAM:$.BTUSBOpen --transport-open
    BTUSBProbe RAM:$.BTUSBControl --hci-control
    BTUSBProbe RAM:$.BTUSBReport --hci-version

If an existing DeviceFS client owns an ACL endpoint, `--hci-control` runs the
same HCI Reset and Read Local Version exchange using only the control and
interrupt pipes. This separates controller communication from ACL ownership;
it does not satisfy the stricter ACL transport gate.

The first physical iMX6 run on 9 September 2026 found the CSR8510 A10 as
`USB3` and later enumeration numbers, `0A12:0001`, through USBDriver 1.31. The
initial probe incorrectly separated DeviceFS special fields with slashes. Only
the first field was parsed, so both apparent input opens selected the default
bulk endpoint and the second reported `Endpoint in use`. EtherUSB and
SCSISoftUSB were isolated and did not own the endpoint. With the required
semicolon separators, USBDriver accepted distinct ACL input and output streams;
the command-free gate passed on the physical iMX6 on 9 September 2026 for event
IN, ACL IN and ACL OUT. The subsequent HCI Reset exposed a USBDriver 1.31
defect: its RISC OS DeviceFS handler ignores R6 poll mode, calls the synchronous
`usbd_do_request`, and froze the target. The probe now checks extension
capability bits 0 and 1 through DeviceFS call `0x80000009` and refuses the
unsafe control request unless a patched asynchronous USBDriver reports both.
The first asynchronous patch also froze because it submitted EHCI work inside
`DeviceFS_CallDevice`. The corrected build defers submission to an OS callback.
Before another HCI command, `--control-safety` performs a read-only USB device
descriptor request through that deferred path and verifies the returned vendor
and product IDs. Physical qualification of this corrected build is pending.

This diagnostic claims and closes the ACL streams but deliberately stops before
submitting ACL data or opening SCO isochronous streams. A pass proves native
control and interrupt transfers plus endpoint ownership; it does not prove ACL
traffic, pairing, HFP or audio.

The completed report is `build/evidence/usb-guest.txt`. This verifies service
query execution with no USB provider. Discovery and record cleanup with an
actual USBDriver/provider are still untested; the probe does not imply that
the user's separate running guest has the same installed module set.

## Soft-load stack and native controller build

The baseline above remains useful as a no-provider test. A separate USBStack
test now loads the publisher's USBDriver 1.31 (04 Jul 2020), marked
"isochronous", and the locally installed !System RTSupport module. The IsocUSB
archive from https://ftpc.iconbar.com/usb/IsocUSB.zip is byte-identical to the
existing `C:/ShareFS/WindowsServer/IsocUSB (1).zip`. Its SHA256 is
`f94faeb3dfe7cbce1c8655bcfe14b88bbdbe793cd70612651fb640a2eea8d2fb`.
`scripts/prepare-usb-stack.py` checks that hash and the extracted module hash,
preserves the publisher's readme, and writes `build/usb-softload/manifest.json`.
This binary is a local test dependency, not included in the BluetoothVoice
distribution. Its advertised isochronous support has not been tested with a
controller or voice device here.

The real USBStack guest report now contains:

    Loaded HostFS:$.USBDriver
    USBDriver SWI present: yes
    USBDriver version: 131
    USB records: 0; supported Bluetooth controllers: 0

The official Iyonix ROM package was also inspected. Its compressed OS payload
can be decoded using the `Kernel/hdr/OSEntries` header layout and zlib, and its
module length chain follows `Kernel/s/ModHand`. However, the extracted ROM
RTSupport and USBDriver builds did not complete soft-loading in the isolated
guest. `scripts/inspect-usb-rom.py` preserves that inspection as `.bin` files
under `build/rom-usb-reference`; they are not the modules used by these tests.

For a proper OHCIDriver soft-load build, `scripts/prepare-ohci-build.py` stages
the official ROOL source archive:

- URL: https://www.riscosopen.org/zipfiles/sources/src-iyo-dev.5.31.tar.bz2?1788827786
- SHA256: `ee2416a1917e90aa56b56c1a3edf12310f74e46fdf8f1ea7a556312dd2653ef4`.
- Selected components: OHCIDriver 0.56, USBDriver interface headers, callx 0.09,
  and the HostFS assembly header needed by callx. Original source notices remain.

The build uses copies of the local `AcornC.C++` DDE and Perl distribution from
`C:/RISCOSBOX-V1/Machines/RISC OS BOX 4.39/hostfs`, inside the isolated HostFS.
Norcroft 5.97 (13 Dec 2025), ObjAsm 4.13, CMHG and the standard CLibrary/CModule
makefiles execute in RISC OS. `callx` is built and exported into the copied DDE.
The preparation changes OHCIDriver's include search roots and supplies the
missing exported USB/PCI constants from their official headers. Driver C and
assembly remain unchanged. DDEUtils enables long tool command lines; a real
temporary directory is provided for preprocessing. `TOUCH=Create` is used for
the empty directory stamps in these build targets. The build does not exercise
the separate resource timestamp-copy operation that requires Touch's `-r`.

The guest runner uses the C runtime to launch native tools, checks their return
status and writes `build/evidence/buildohci-guest.txt`. The host also requires a
module output with the OHCIDriver title before copying it to
`build/ohci/OHCIDriver,ffa`. The initial successful output was 16,808 bytes,
SHA256 `b29a33d6e9bbf96536ce03727af6ac6f4e1318b13dced8ab36a0f56f75358a3b`.
Upstream compiler warnings are retained in the native build evidence.
After backing up and removing the generated callx/OHCI objects, module header,
library and module output, a complete rebuild produced the identical module
SHA256. The controller diagnostic was then repeated with that rebuilt binary.

The USBController probe attempts to load that source-built module after
USBDriver and receives the specific upstream error:

    NO_OHCI_CONTROLLER: &820A01: No OHCI devices found
    USBDriver version: 131
    USB records: 0; supported Bluetooth controllers: 0

This proves execution through the module's discovery/error path. It does not
prove attachment, interrupts, DMA, transfers, cancellation or audio. An
unexpected module-load error fails the diagnostic instead of being accepted
as this known no-controller outcome. See `build/evidence/usbcontroller-guest.txt`.

The local emulator's inspected OHCI model still lacks transfer processing.
Windows currently owns the real radio through BTHUSB. A guest-native USB path
therefore needs working emulator USB passthrough and explicit radio ownership,
in addition to the RISC OS class driver. A host-managed audio backend can be
additional functionality but cannot satisfy the native-driver milestone.

## Microsoft references

- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ni-usbioctl-ioctl_usb_get_descriptor_from_node_connection
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ns-usbioctl-_usb_descriptor_request
