# Physical radio ownership and recovery

The selected live dongle is
`USB\VID_0A12&PID_0001\6&10D7DA3A&0&3`, with hardware revision `8891`.
The latest post-gate check on 9 September 2026 found it present and healthy on
service `BTHUSB` and `bth.inf`. The development certificate is absent from the
CurrentUser and LocalMachine Root and TrustedPublisher stores.

`scripts/capture-radio-binding.ps1` records a read-only identity and binding
snapshot. `host/driver/BluetoothUSB.inf.in` matches the exact hardware revision,
uses the Windows USBDevice class and registers interface GUID
`{c32ed701-587e-4a01-a85d-e0f146a91851}` for the host plug-in.

## Validated and signed package

`scripts/build-driver-package.ps1` validates a fresh package with InfVerif and
Inf2Cat from pinned Microsoft WDK package `10.0.28000.2526`. Its pinned NuGet
SHA256 is:

    63c939fb5a79295bf40e941db592681272219b04edff095fe2f3d123e5579a90

`scripts/sign-driver-package.ps1` creates a fresh copy and signs its catalog
with the local development certificate `CN=BluetoothVoice Development Driver`,
thumbprint `2A2DA289B9C19274C868386D650BC382396D28FC`. The certificate is for local
development only. The exact output directory, certificate and package hashes
are in `build/evidence/driver-signed.json`.

## Exact-instance switch

`host/c/exact_driver_install.c` uses SetupAPI to open the recorded device
instance, builds a compatible-driver list from only the signed INF, selects that
driver and invokes `DIF_INSTALLDEVICE` for that instance. Its noninstalling
selection check passes. This avoids `pnputil /install`, which applies a package
to every matching present and ghost device.

`scripts/switch-radio-driver.ps1` requires elevation and guards the instance,
hardware revision, starting binding, package hashes, signer and provider. It
adds the development certificate to the LocalMachine Root and TrustedPublisher
stores only for the test, records the published OEM INF, and restores by deleting
that recorded package. Restore waits for `bth.inf/BTHUSB` and removes the exact
certificate from both stores.

The first elevated recovery exercise predated the exact-instance helper. It
used `pnputil /install`, which temporarily selected `oem25.inf/WINUSB` for the
live instance and four matching ghost instances. The `finally` recovery deleted
only `oem25.inf`; all five instances returned to `bth.inf/BTHUSB`, the live radio
returned to status OK, and both machine trust entries were removed. No physical
HCI report was captured because the probe process did not start in that elevated
environment. This run validates recovery but does not validate exact-instance
installation or USB traffic.

## Physical hardware gate

`scripts/run-hardware-gate.ps1` now uses the exact-instance helper, supplies the
32-bit plug-in runtime path, clears stale gate reports, and always restores the
Windows driver in `finally`. Its intended successful sequence is:

1. Bind only the recorded live instance to WinUSB.
2. Run host HCI Read Local Version.
3. Boot the isolated RISC OS guest and run the complete HCI bootstrap through
   the emulator bridge.
4. Restore `bth.inf/BTHUSB` and remove development trust.

Two attempts to start this corrected gate ended at the Windows UAC prompt before
the elevated process began. They changed no driver or trust state. Therefore the
physical dongle's HCI/LMP version is still unknown, and the exact-instance live
install remains unverified. Do not infer the hardware version from the USB ID or
from the synthetic controller fixture.
