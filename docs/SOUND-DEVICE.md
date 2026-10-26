# The audio device, and why it can lock the machine

BluetoothVoice presents itself to RISC OS as a sound controller, so that
anything which makes a noise - SharedSound, the system beep, a music
player - comes out of a Bluetooth speaker or headset.  `port/c/sound_device`
builds a HAL audio device (API 2, custom DMA) and adds it with
`OS_Hardware`.  SoundDMA then offers it in Configure as *Bluetooth speakers
(A2DP)* or *Bluetooth headset (HFP, 8 kHz)*.

## The hazard

The device is registered with the **kernel's** hardware device list, not
with SoundDMA, and everything that lists sound controllers walks that
list.  A device block that belongs to a module which has since been killed
leaves a node pointing into freed memory: the next walk takes a data abort
**inside the kernel**, which on the Titanium showed as

    Internal error: abort on data transfer at &FC052230

That address is not in SoundDMA - in the Titanium ROM images SoundDMA
starts around offset 0x35e604, while 0x052230 is kernel code.  Because
every subsequent enumeration aborts too (the Configure *Sound* plug-in
polls the mixer, so it re-aborts continually), the machine stops
responding and only a power cycle clears it.  This happened twice on
23 September 2026 before the cause was understood.

## What the module does about it

* **The device block lives in the RMA** (`OS_Module` claim) and is never
  freed, so the node the kernel holds cannot become freed memory.
* **Unregistering deselects first.**  `Sound_SelectDefaultController` hands
  sound back to the machine's own controller before the device leaves the
  list; removing the *selected* controller makes SoundDMA tear it down
  from inside the service call, while the stream is still live.
* **Removal is verified.**  The controller list is read back; if the device
  is still there, `registered` stays set, and `Mod_Final` then returns
  `Busy` so the module cannot be killed.
* **A finalisation that cannot be refused leaves an inert device.**  Its
  entries are rewritten to a "return 0" stub inside its own block (flushed
  with `OS_SynchroniseCodeAreas`), and its name is copied into the block
  too, so nothing it points at belongs to the departing module.
* **A stale device is detected.**  If a controller with our ID is already
  listed at registration time it belongs to a dead instance: the module
  refuses to register (state bit 2) and the application says *Restart the
  machine to use sound again*.
* **The application stops the core first.**  `client/c/btstack` asks the
  module to stop (which hands the device back) and waits for it before any
  module is killed.

Close the Configure *Sound* window before replacing the module by hand:
it holds the device's mixer pointer.  `SndRelease` (Core SWI reason 6) makes
the module hand the device back without stopping anything else.

## Feeding it

SoundDMA fills two buffers in the background and the module takes them as
the Bluetooth link drains them:

* Buffers are taken on a **sample-accurate clock** (frames earned per
  millisecond, with the remainder carried).  Taking them faster re-reads a
  buffer SoundDMA has not refilled yet, which repeats a fraction of a
  second of audio - the "choppy, repeating" fault heard on 23 September.
  Counting whole milliseconds instead of frames runs about 1 per cent
  fast, which drifts audibly over a few minutes.
* The callback into SoundDMA saves every register (its fill routine runs
  inline and corrupts R1-R9, R11 and R12; a clobbered R8 - the module's
  static base - once sent the buffer loop to a wild address), and is never
  re-entered.
* `AudioDiag` on the share reports packets a second, underruns, and why a
  transmit did not happen; A2DP needs roughly 43 packets a second at
  44.1 kHz, or 172 when each packet holds two SBC frames.

## What actually capped the stream at 50 packets a second

Measured on the Titanium with module 0.26 and a DM-40BT:

    acl: sent 50 (50/s) noslot 0 nocredit 50 busy 0 free 0/10 role master

Ten packets outstanding, every one of them waiting, and the trace showing
one `ACL>` followed by one `EVT 0x13` (Number Of Completed Packets, count 1)
over and over.  Neither the radio nor USB was the limit:

* The module's ticker ran at **`CORE_TICK_CS` = 2 centiseconds**, and
* `poll_transport` kept **one** event IN transfer in flight: it submitted a
  transfer, polled it once, got PENDING and broke out of the loop.

So the core could learn about at most one finished packet per pump, and the
pump ran 50 times a second.  50 packets a second of 255 bytes is 102 kbit/s,
under half of what 44.1 kHz SBC needs - the stutter, exactly.

The fix is the event queue: `BTC_EVENT_SLOTS` transfers are kept
outstanding so one pump drains the whole backlog.  The ticker **cannot**
help - `OS_CallEvery` takes the period minus one and the kernel rejects 0
(`Kernel/s/TickEvents`: "reject 0 and >= &FFFFFC00"), so 2 cs is the
fastest it allows, and asking for 1 cs returns Bad time, leaves the ticker
unstarted and the core pumped only by the desktop's polling.  50 pumps a
second carry A2DP only because each one now harvests several packets.

Guessing at the radio (role switch, link policy, packet types) cost a day
and changed nothing; the counters said "waiting on credits" from the start.

**The same fault was in the receive path.**  One ACL IN transfer in
flight means one incoming packet a pump - 50 a second shared between
every link on the machine, which is below what a Bluetooth mouse alone
sends.  `BTC_ACL_IN_SLOTS` transfers are now queued the same way, and the
LE HID test checks that 16 injected mouse reports are taken in several a
pump rather than one.

`test/test_bt_core.c` reproduces it: its fake transport refuses to complete
a transfer in the pump that submitted it, and `nocp_per_packet` makes the
fake controller report one finished packet per event, as the DM-40BT's
does.  With `BTC_EVENT_SLOTS` set to 1 the simulation delivers exactly the
**50 packets a second** measured on the hardware; with 8 it delivers 173,
against the 172 a 44.1 kHz stream needs.

## Never hand the device back from a callback

`pump()` runs from a transient callback.  Releasing the audio device means
`Sound_SelectDefaultController` and `OS_Hardware` - which cannot run while
SoundDMA may be filling a buffer underneath: quitting the task mid-stream
took the machine down twice that way.  The callback only sets a flag;
`btm_swi` hands the device back in the caller's own foreground, and
`Mod_Final` does it directly.

## Replacing the module by hand

`Mod_Final` refuses while the core is running or the device is registered,
so `*RMKill BluetoothVoice` answers *busy* while the icon bar task is up.
The supported order is:

1. Quit **!Bluetooth** from its icon bar menu.  It stops the core and hands
   the device back on the way out.
2. `*RMKill BluetoothVoice` - it should succeed silently.
3. Start !Bluetooth again; it loads the module beside it, and only replaces
   one already loaded when its version is older than the task needs.

Killing the module out from under a running task leaves the task showing
*Bluetooth stopped (error &1104)*: the native transport is still marked
open by a module that no longer exists, so every submit is refused.  A
restart clears it.
