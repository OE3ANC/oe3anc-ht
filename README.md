# OE3ANC HT

Experimental FM/M17 firmware for the C62 handheld, with a web companion and a
desktop emulator.

<img src="assets/c62.png" alt="C62 reverse-engineering illustration" width="600">

[Web companion](https://ht.oe3anc.at/) ·
[Releases](https://github.com/OE3ANC/oe3anc-ht/releases)

## Project status and disclaimer

This is experimental firmware that I develop and maintain for my personal use.
It provides a platform for testing ideas on the C62 and evaluating and improving
AI-assisted development workflows. AI tools are used extensively in development.

Ideas are welcome, but feature requests are not accepted, and fixes for reported
issues are not guaranteed. Continued maintenance depends on my interest in the
project; development may be discontinued.

Use it entirely at your own risk. Flashing or restoring may brick your radio or
lose data. I, and other authors and contributors, accept no responsibility for
broken devices, data loss or any other damage or harm.

For actual use, I recommend the better-designed and more stable
[OpenRTX](https://openrtx.org/). If you want to contribute to or support open-source
radio, support [M17](https://m17project.org/) and [OpenRTX](https://openrtx.org/).

## Quick-start

1. Download the firmware ZIP for your chosen [release](https://github.com/OE3ANC/oe3anc-ht/releases)
   and extract `firmware-bundle.json`.
2. Open the [web companion](https://companion.oe3anc.at/) in a desktop browser
   with Web Serial support. Use the companion version matching your firmware release.
3. Put the C62 in manual bootloader mode and connect its programming cable.
   Release physical PTT, then select **Connect bootloader** under **Firmware tools**.
   Use **Dump complete flash** and keep the backup before updating.
4. Select `firmware-bundle.json`, choose **Review firmware update**, and select
   the application/DSP pair for the first installation. Review and acknowledge
   the prompts, then flash. Keep the radio powered and connected until completion.
5. Disconnect the bootloader, unplug the cable and reboot the radio. Enable
   **Companion** in the radio's menu before reconnecting the cable, then select
   **Connect** in the matching web companion. Use the codeplug editor to program
   channels or the live display and virtual keypad to control the radio.

If the radio reports **Storage is read-only** with **Load error -134** after using
an earlier development firmware, retain a complete flash backup and use
**Firmware tools → Review settings reset**. This deletes saved settings, channels
and banks, preserves firmware and factory calibration, and verifies the reset.
Disconnect, unplug and reboot, then read the radio again before uploading a codeplug.
Normal firmware updates preserve settings and do not clear this error.

## Features

### Firmware

- FM with independent RX/TX CTCSS or DCS, and [M17](https://m17project.org/) voice with callsign
  addressing and CAN filtering.
- VFO and Memory operation, separate RX/TX frequencies, 256 channels and 16
  ordered banks. Edit them on the radio or share a JSON codeplug.
- Four themes, contrast settings, idle backlight dimming, keypad lock and a TX
  time limit. The desktop emulator runs the shared radio UI with fake hardware.
- Codec2-mod 3200 voice, with live processing statistics in **Status → Codec2**.
  Encoder/decoder average and maximum elapsed times are per 20 ms codec frame,
  including preemption. The page reports frame counts, processing above 20 ms,
  fixed state reservation and zero codec heap usage. **OK Reset** clears counters;
  it does not reset the live codec predictors. Emulator timings are simulated.
- M17 Home shows `BER~` (latest stream Viterbi distance divided by 272 received
  coded bits), `L` (inferred sequence-gap loss) and `B` (rejected stream frames).
  This is an error estimate, excluding sync/LICH bits, and does not measure true
  or residual BER. Loss includes rejected/undetected frames, is not a separate
  additional error total, and begins at the first observed frame. Counts above
  9999 display as `9999+`. The values
  clear on sync loss/new LSF, expire after 500 ms without a stream frame, and
  are unavailable during TX. Storage warnings retain priority over these stats.

<img src="assets/screenshots/radio-vfo.png" alt="Emulator FM VFO Home" width="320">
<img src="assets/screenshots/radio-m17.png" alt="Emulator M17 Memory Home" width="320">

### Browser-based companion

- Offline and connected channel programming

<img src="assets/screenshots/companion-cps.png" alt="Companion channel programming editor" width="680">

- Shared LVGL live display and virtual keypad

<img src="assets/screenshots/companion-remote.png" alt="Companion live radio display and virtual keypad" width="680">

- Separate bootloader tools for flash backups, application/DSP updates, optional
  post-write readback and read-only factory calibration inspection

<img src="assets/screenshots/companion-bootloader.png" alt="Companion CSK6 bootloader firmware tools" width="680">

## Acknowledgements

Thanks to everyone involved in reverse-engineering the C62, the OpenRTX
contributors, and the authors of the BK4819 driver and C62 integration we copied and
adapted as well as to ListenAI for their help with the DSP firmware. Their work
made this project possible. Imported code retains its original attribution and licenses.

Thanks also to David Rowe and the Codec2 contributors for Codec2, and Wojciech Kaczmarski SP5WWP and 
Silvano Seva IU2KWO for their work and ideas in [Codec2-mod](https://github.com/M17-Project/Codec2-mod).

We encourage amateur-radio manufacturers to publish tools, documentation and
schematics so the open-source community can develop innovative firmware and
extend the capabilities of existing hardware.

Project code uses GPL-3.0-or-later; see [license](LICENSES/GPL-3.0-or-later.txt) and the attribution above.
