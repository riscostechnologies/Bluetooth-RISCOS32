# Virtual Kinetic Box host transport bridge

The Windows native emulator now has a private Bluetooth transport bridge at
`ArcEm` SWI `&56AC8`. It loads the separately built 32-bit `BluetoothUSB.dll`
from the emulator directory, or from the absolute path named by
`VKB_BLUETOOTH_PLUGIN`. The DLL is loaded only when the guest first calls the
bridge. Machine startup does not inspect, open, rebind or pair a radio.

This private SWI is an emulator implementation detail. RISC OS applications use
the approved `BluetoothVoice` SWIs. `port/c/host_bridge` is the module-side
adapter and is the only component that should call `&56AC8`.

## Contract

R0 is the reason code. The bridge returns the plugin result in R0 and a Windows
error in R1 for operations that can fail. Version is the exception: it returns
bridge ABI `&00010000` in R0, maximum transfer bytes (4096) in R1 and maximum
isochronous packet descriptors (32) in R2.

| Reason | Operation | Inputs and outputs |
| --- | --- | --- |
| 0 | Version | Returns ABI and fixed bounds in R0-R2. |
| 1 | Inspect | Returns result/error and packed USB VID/PID in R2. It is read-only. |
| 2 | Open | Opens the sole matching raw WinUSB controller. |
| 3 | Voice | R1 is the USB interface alternate; zero disables voice. |
| 4 | Submit | R1 kind, R2 buffer, R3 bytes, R4 ISO packet count; returns ticket in R2. |
| 5 | Poll | R1 ticket, R2 4096-byte buffer, R3 capacity, R4 packet array, R5 capacity; returns completion metadata in R1-R4. |
| 6 | Cancel | R1 is the ticket. Cancellation is not completion. |
| 7 | Close | Repeated calls drain cancellation and unload device state. |

Transfer kinds match the plugin ABI: command, event input, ACL input/output and
SCO input/output. OUT data is copied from guest memory before plugin submission.
Poll refuses undersized guest storage before asking the plugin for a completion,
so it cannot consume a result that the guest cannot retain. It then copies only
the validated byte count and packet descriptor count. Host pointers and function
pointers never cross the boundary.

All bridge and plugin calls run serially on the emulator thread. Machine reset
initiates plugin close. If cancellation receipts are still pending, later bridge
calls continue close before accepting a new operation. The installed emulator is
unchanged; this implementation is built and tested in the isolated source tree.

## Current live evidence

The isolated RISC OS module test used
`C:/KBoxBuild/VirtualKineticBox-Native.exe` and the x86 plugin. The guest report
records:

    Bluetooth host bridge: ABI &10000, inspect -8, USB 0A12:0001

Result `-8` is `WINDOWS_OWNED`. The matching emulator log records successful DLL
ABI loading and service inspection. Windows still owns the radio through
`BTHUSB`, so the bridge correctly advertises no usable native endpoint and opens
no raw handle.

`port/c/bridge_controller` now provides the exclusive asynchronous owner above
this bridge. It keeps an event read queued, submits each command once, consumes
every ticket, feeds complete events to `hci_bootstrap`, and drains close before
releasing storage. After authentication and encryption, it retains ownership of
the interrupt endpoint and exposes one stable complete HCI event at a time to
the ACL/SCO link owner. It does not post another read until that event is
consumed. An outstanding event read can remain active while the owner submits a
Disconnect command, avoiding a receive gap during shutdown. The x86
controller/headset fixture completed Reset, Read BD_ADDR, buffer sizes, local
and extended features, local version, supported commands, inquiry, PIN pairing,
encryption, SDP, RFCOMM, HFP and CVSD SCO through the isolated RISC OS guest.
The public SWIs wrote playback PCM, captured microphone PCM and drained
Disconnect before three module unload/reload cycles. Its controller and headset
values are deliberately synthetic and do not identify the physical dongle.

Run the same isolated check with:

    wsl -d Debian -- sh /mnt/c/VKBox/bluetooth-voice/scripts/build-module.sh
    powershell -File scripts/test-guest.ps1 -Test Module -TimeoutSeconds 90 `
      -EmulatorPath C:\KBoxBuild\VirtualKineticBox-Native.exe `
      -PluginPath C:\VKBox\bluetooth-voice\build\host\x86\BluetoothUSB.dll

For the complete fixture session, pass
`build/host/x86/BluetoothUSBFake.dll` as `-PluginPath`. The physical gate uses
the signed development package and exact-instance helper documented in
`RADIO-OWNERSHIP.md`. It must still produce a real controller transcript,
physical pairing and sustained audio.
