# RISC OS headset client

`BTVoiceRun` is a transient RISC OS client for the BluetoothVoice module. It
turns the module's asynchronous interface into a repeatable headset test:

1. verify ABI 1.0 and native HFP playback/capture capabilities;
2. enumerate the raw controller endpoint;
3. connect the first discoverable HFP headset with microphone capture enabled;
4. poll until the negotiated stream is ready;
5. send a 440 Hz test tone while draining microphone samples; and
6. cancel outstanding transfers and release the session.

After running `!Bluetooth`, put only the intended headset into pairing mode and
run this in a TaskWindow or at the command line:

    <BluetoothVoice$Dir>.BTVoiceRun -capture RAM:$.BTMic -seconds 10

To select a particular headset and change its legacy PIN, use its displayed
Bluetooth address in conventional most-significant-byte-first form:

    <BluetoothVoice$Dir>.BTVoiceRun -address 01:23:45:67:89:AB -pin 1234 -capture RAM:$.BTMic

The capture file contains headerless signed 16-bit little-endian PCM at 8000 Hz,
one channel. Omit `-capture` to test playback without enabling microphone input.
The duration can be 1 to 3600 seconds. `-report <file>` redirects diagnostics to
a file and is primarily used by the automated guest harness.

Use `-play <raw-file>` to send an existing file in the same PCM format instead
of the generated tone. Partial module writes are retained and retried. At end of
file, the client pumps the session until the queued playback frames have drained
before disconnecting. The duration remains a maximum, so increase `-seconds` for
longer files.

Without `-address`, the discovery owner selects the first voice-capable inquiry
result. The default pairing fallback PIN is `0000`; `-pin ""` disables a legacy
PIN response. Credentials are retained only for the live connection and are not
written to disc. A graphical headset chooser is still pending.

The client proves that a RISC OS application can exchange PCM with the module.
It does not install a SharedSound output driver, and RISC OS applications do not
automatically receive its microphone data. Those integrations require separate
adapters around the copied, nonblocking PCM SWIs.

