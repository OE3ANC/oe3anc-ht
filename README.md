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
  FM explicitly enables the BK4819 microphone AGC, 300 Hz voice high-pass,
  low-pass, pre-emphasis and audio level controller. The chip generates
  CTCSS/DCS separately from microphone audio. Previously the voice filters
  and level controller were enabled, but microphone AGC was left at its
  disabled reset setting (see the [BK4819 register list](https://alfaexploit.com/files/BK4819V3Registers_List_20201218.pdf)).
  CTCSS/DCS transmit gain (`51[6:0]`) is set when enabling a TX tone; previously
  it remained at the minimum reset value. Provisional values are 74 for CTCSS
  and 51 for DCS, following the [egzumer BK4819 driver](https://github.com/egzumer/uv-k5-firmware-custom/blob/main/driver/bk4819.c).
  **FM CTCSS level** in the radio menu adjusts the global CTCSS value from 0–127
  with P1/P2 (OK increases); default 74. The companion's global settings editor
  provides the same setting. It applies to VFO and all memories, saves after
  10 seconds or power-off, and is locked during TX. It does not change DCS level
  or M17. Zero selects minimum gain; use TX tone Off to disable CTCSS.
  Existing saved settings/codeplug JSON load with default 74. New saves use a
  v2 manifest, which older firmware cannot read; export a backup before a downgrade.
  These are raw gain codes, not calibrated deviation in Hz. M17 retains its flat
  audio path. Verify tone frequency, tone deviation and peak voice deviation on
  both bands with matching channel bandwidths, including after an M17/FM switch.
  For the reported 162.2 Hz CTCSS case, check that the CS7000 opens with no speech
  and stays open during loud speech. If dropout persists, also check the upstream
  microphone ADC for clipping; BK4819 AGC cannot repair it.
- Requested transmit power is shown in watts on the radio and in the companion
  editor. The C62 uses provisional 1, 2.5 and 5 W PWM settings; these are not
  measured power readings. Builds with `config/resources.conf` log the APC duty,
  timer configuration and PA pin mux at TX setup for hardware diagnosis.
- VFO and Memory operation, separate RX/TX frequencies, 256 channels and 16
  ordered banks. Edit them on the radio or share a JSON codeplug.
- Seven themes (including Terminal Green, Amber and Ice), contrast settings,
  idle backlight dimming, keypad lock and a TX time limit. The desktop emulator runs the shared radio UI with fake hardware.
- Codec2-mod 3200 voice, with live processing statistics in **Status → Codec2**.
  Encoder/decoder average and maximum elapsed times are per 20 ms codec frame,
  including preemption. The page reports frame counts, processing above 20 ms,
  fixed state reservation and zero codec heap usage. **OK Reset** clears counters;
  it does not reset the live codec predictors. Emulator timings are simulated.
- **Status → RX Path / RX Gain / RX Squelch** shows read-only BK4819 register
  values in hexadecimal and the sample age. The radio samples every 250 ms in
  RX without stopping reception; values are unavailable in the emulator and
  hidden outside RX. RX Path covers power (`30`, `37`), LNA GPIOs (`33`), tuning
  (`38`, `39`), filters (`43`) and audio (`47`, `48`). RX Gain covers the gain
  table (`10`–`14`) and AGC (`49`, `7B`, `7E`). RX Squelch covers status (`0C`),
  glitches (`63`), noise (`65`), RSSI (`67`) and hardware thresholds (`4D`–`4F`,
  `78`). Read errors follow the existing latched radio-fault shutdown path.
  For FM sensitivity checks, hold Monitor to bypass software RSSI/tone gating:
  default SQL 4 opens above −109 dBm on the uncalibrated RSSI scale. Compare
  readings with no signal and with a known weak signal on each band. Register
  observations do not establish measured RF sensitivity.
- **FM weak BW** and **FM AF DAC** in the menu are temporary FM RX test controls.
  Use P1/P2 to decrease/increase, or OK to increase. Weak BW adjusts only
  `43[11:9]` through the eight filter settings and displays the effective kHz
  value (doubled for 25 kHz channels). AF DAC adjusts only `48[3:0]`, 0–15,
  approximately 2 dB per step; this is audio output gain, not RF gain. The
  defaults are filter index 0 and DAC gain 1. Changes briefly restart reception,
  preserve the selected channel and survive retuning/PTT. They apply only to
  FM, retain the original M17 levels, and reset on reboot. They are not saved
  in the codeplug. Adjustments are rejected during TX.
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
