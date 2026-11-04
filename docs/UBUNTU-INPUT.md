# Ubuntu for Qualcomm and audio input

## Current project baseline

The deployment target is RUBIK Pi 3 with **Ubuntu for Qualcomm, arm64**.
The default capture profile uses the microphone connection on the board's 3.5 mm
headphone/headset jack. The connector needs a compatible analog microphone or
headset with a microphone; there is no assumption of a built-in microphone.
Software WAV input is the integration path while physical input is unavailable.
For the USB microphone array, see [XVF3800 setup](XVF3800.md).

The setup script checks Ubuntu and aarch64 before installing build dependencies.
It does not install an OS image, QNN, PyTorch, the AI team's model or Node.js.
Use the board's supported Ubuntu image and match the engine runtime to that
image. A Fedora host is useful for development but does not validate the board.

## Choose what the test needs to cover

| Input path | What it covers | Requirements and limits |
|---|---|---|
| Python WAV feeder | WAV conversion, ASTK frames, engine, session/transcript HTTP and web events | No sound card. Bypasses C++ capture, the daemon high-pass filter, mixer routing and XRUN recovery. HTTP forwarding is synchronous test code. |
| WAV through snd-aloop | ALSA capture, real C++ daemon, preprocessing, engine and backend | Linux with snd-aloop available. Does not validate the 3.5 mm electrical input or microphone quality. |
| 3.5 mm headset microphone | Board capture, routing and the downstream pipeline | Compatible microphone/headset, verified ALSA PCM and mixer route. Requires real board testing. |
| XVF3800 USB array | USB multichannel capture, processed beam selection and the same mono engine path | Verified USB firmware mode and capture channel mapping. See [XVF3800 setup](XVF3800.md). |

The dummy engine returns fake text. Use the actual AI engine to measure CER.
The WAV feeder's backpressure and file pacing are not capture latency measurements.

## Set up the 3.5 mm microphone connection

The [RUBIK Pi official audio guide](https://www.thundercomm.com/rubik-pi-3/en/docs/rubik-pi-3-user-manual/1.0.0-u/peripherals-and-interfaces/audio/)
provides the following example, assuming the onboard sound card is card 0:

```bash
cat /proc/asound/cards
cat /proc/asound/pcm
arecord -l
amixer -c 0 cset "name=MultiMedia3 Mixer PRIMARY_MI2S_TX" 1
arecord -D plughw:0,2 -f S16_LE -r 48000 -c 1 -d 5 -t wav /tmp/headset.wav
```

Confirm the route and capture PCM on the installed Ubuntu image. If the card
number is different, adjust `-c` and `plughw`; a stable card ID such as
`plughw:CARD=<actual-id>,DEV=2` avoids relying on enumeration order.
Check the control with `amixer -c <card> controls` if the example name is absent.

`config/audiostack.conf` and the daemon's built-in fallback select `plughw:0,2`
and prefer 48 kHz. The capture tool uses the same device and rate preference.
Audio is converted to mono S16_LE at 16 kHz before reaching the engine.
Because `plughw` may perform conversions, a successful capture at a requested
rate does not by itself identify the hardware's native sample rate.

Use `bash scripts/audio_probe.sh` to list devices and inspect sound-server state.
Pass `--device <PCM>` to record a short test and, if built, verify the C++ WAV
capture output. The probe does not change mixer routing or stop sound servers.

## Permissions and sound-server ownership

Check `ls -l /dev/snd` and `fuser /dev/snd/*`. The optional
`scripts/udev/99-audio.rules` grants capture-node access to the `audio` group;
it does not create a sound card, fix a card number or enable an input route.
For a dedicated board account that needs direct access, add that account to
`audio`, then log in again. Check the resulting device permissions.

PulseAudio or PipeWire can hold the capture device on Ubuntu. Select the
headset source and use a configured ALSA bridge PCM, or stop the owning user
service for a direct ALSA test and restore it afterwards. Inspect the actual
service and socket units first; socket activation can restart the server.
Do not infer that a running process necessarily blocks the selected PCM.

The official guide also documents PulseAudio source selection. Those sources
are not interchangeable with an ALSA hardware device name in the daemon.

## Software input

Follow the three-terminal WAV example in [README](../README.md). The feeder
binds its own engine socket and sends HELLO, SESSION_BEGIN and 20 ms AUDIO
frames. It sends SESSION_END, waits for final engine results, and posts the
backend session end afterwards. Start the feeder before connecting the engine.

The feeder and daemon are alternative servers. Use a writable socket path and
do not run both on the same path. The WAV feeder supports real-time pacing,
`--no-realtime` and `--loop`; only real speech samples can validate transcription.

To include ALSA capture, start an engine on `/tmp/astk-loopback/engine.sock`,
then run:

```bash
bash scripts/feed_wav_loopback.sh samples/jfk.wav -- \
  --engine-socket /tmp/astk-loopback/engine.sock
```

The script uses `config/loopback.conf`, detects the Loopback card, waits for
confirmation before playback and keeps the daemon alive until the final result
has been inspected. It requires `sudo modprobe snd-aloop` if the module is not
loaded. If the kernel does not provide it, use the Python feeder instead.

## Service deployment

Verify the mixer route and microphone recording before installing the service.
The installer preserves an existing `/etc/audiostack/audiostack.conf`, so update
its capture device/rate if migrating an earlier installation. The service starts
the daemon only, restarts it on failure and does not set mixer controls. Verify
the input route after reboot as well as engine/backend startup ordering.

The experimental I2S overlay is retained for later research; it is not required
for either current input path and has not been validated on the selected image.
