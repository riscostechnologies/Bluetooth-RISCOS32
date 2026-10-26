# BluetoothVoice changes, 0.26 to 0.35

## Desktop app 0.44 (15 Oct 2026)

* The legacy PreDesk `LoadUSB` script is inert. The icon bar task loads USB
  drivers only after checking for an adapter. Saving settings removes the old
  PreDesk entry. This avoids replacing USBDriver during boot without restoring
  the Pi 4's DWC controller.
* The app refuses to replace a loaded BluetoothVoice older than 0.34, whose
  finalisation can abort on a machine without an adapter. It also treats an
  adapter-check error as absence instead of proceeding with a USB swap.
* These changes are built and staged but not yet verified on hardware.

Written from the code and the test suite, not from memory.  "Verified on
hardware" means a fix confirmed on a real machine; "observed" is something seen there. Everything else is covered by
the host simulation (`test/run_bt_core.sh`, `scripts/test-core.sh`) only.

## Observed on hardware (Titanium, 23 Sep 2026)

* **0.26 - role change.**  `AudioDiag` reports `role master`: the core no
  longer allows a role switch, so it stays master of the link.  This did
  not by itself raise the packet rate.
* **0.28 - quitting mid-stream took the machine down**, once: it still
  answered ping but never reached the desktop again.  The cause given under
  0.29 is a diagnosis from the code, not something reproduced; the earlier
  "NULL POINTER" quit on 0.15 had a different cause (an unsafe unregister
  order) and is not the same fault.

## Fixed in code, not yet verified on hardware

* **0.28 - A2DP capped at 50 packets/s.**  Measured: `sent 50 (50/s)
  nocredit 50 free 0/10`, one `ACL>` then one `EVT 0x13` (count 1) forever.
  Cause: one Event IN transfer in flight, polled once per 2 cs pump, so the
  core learned of at most one finished packet per pump.  Now
  `BTC_EVENT_SLOTS` (8) transfers are queued.  `OS_CallEvery` cannot tick
  faster than 2 cs (the kernel rejects a period of 0), so the answer is to
  drain more per pump, not to pump faster; `BTC_PUMP_MS` is checked at
  build time.
* **0.31 - receive path capped the same way.**  One ACL IN transfer meant
  50 incoming packets/s shared across every link, below what one mouse
  sends.  `BTC_ACL_IN_SLOTS` (4) are now queued.
* **0.29 - audio device released from a callback.**  Handing the controller
  back means `Sound_SelectDefaultController`/`OS_Hardware`, which cannot run
  while SoundDMA fills a buffer.  The callback now sets a flag and the next
  SWI does it in the caller's foreground.
* **0.27 - USB stack only swapped when an adapter is present** (new Core
  SWI reason 15), so a machine without one never has its keyboard and mouse
  restarted at start-up.
* **0.32 - automatic reconnection of bonded devices** (`bt_reconnect`,
  ported from the 64-bit tree).  Low Energy input devices, then classic
  input devices, then speakers.  The call is made again when the desktop
  hands over the bonds, because here they arrive after the core starts.
* **0.33 - report-mode mouse "feel"** (`hid_input`, ported from the 64-bit
  tree): gentle acceleration, sub-pixel remainders, wheel notches, natural
  scrolling, extra buttons that start applications.
* **0.34 - `Mod_Final` aborted on a machine with no adapter.**  Found on a
  Pi 4 (offset &243F0 of 0.30, in `btp_le_link_closed`) and mapped from a
  rebuilt 0.30.  `core->profiles` is set only by `btc_start()`, so a core
  that never started has none, and finalisation dereferenced it, leaving a
  `DEADDEAD` module.  `btp_shutdown` now returns early.
* **0.35 - the Audio SWI had the same flaw** in `btp_a2dp_status`, so
  `AudioDiag` or any audio status poll aborted on such a machine.

## Application (!Bluetooth 0.43)

* `DWCDriver` is in the controller-restart list.  The Pi 4's USB is DWC
  OTG; without it a USB-stack swap would kill USBDriver and restart nothing.
* The status line names a saved speaker by its saved name, not its address.
* Core failures read as sentences, with the hex code kept on the end.

## Test changes worth knowing

* The fake transport no longer completes a transfer in the pump that
  submitted it, and can report one finished packet per event (as the
  DM-40BT's controller does).  With `BTC_EVENT_SLOTS` set to 1 the
  simulation reproduces the 50 packets/s measured on the hardware; with 8 it
  carries 173 against the 172 a 44.1 kHz stream needs.
* `shutdown_never_started` reproduces both no-adapter crashes under UBSan.

## Not done

* LE Secure Connections (`p256`, `hid_report`, and the larger `bt_le`) from
  the 64-bit tree: about 1900 lines across pairing and input, not worth
  landing without an adapter to test against.
* Nothing in 0.27-0.35 has run against a real Bluetooth adapter.
