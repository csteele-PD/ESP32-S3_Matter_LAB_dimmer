# MatterS3Lab

Minimal ESP32-S3 Matter-over-WiFi lab firmware for Hubitat commissioning tests.

The first target is an ESP32-S3 Supermini with 4MB flash and 2MB PSRAM. It exposes a single Matter Dimmable Light endpoint and logs all interesting commissioning, On/Off, and Level Control attribute activity to serial.

The Matter Basic Information identity is intentionally unique to this lab firmware:

```text
manufacturer: ACLYS_LAB
model: MatterS3Lab
vendor id: 0xFFF1
product id: 0x8001
```

That keeps Hubitat from matching the ESP32 Wall Keypad fingerprint while this board is only acting as a generic Matter test device. The product id deliberately avoids the default Matter example `0x8000` value while staying inside the ESP Matter example DAC set, and the Dimmable Light endpoint adds Level Control so the cluster fingerprint differs from the keypad.

## Wi-Fi

Create `main/wifi_secrets.h` from the example before building:

```c
#pragma once

#define LAB_WIFI_SSID "your-ssid"
#define LAB_WIFI_PASSWORD "your-password"
```

`main/wifi_secrets.h` is ignored by git.

## Build And Flash

Using Visual CodeStudio and ESP-IDF
```sh
source /Users/csteele/.espressif/v6.0.2/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem2344101 flash monitor
```

## Pairing

The firmware logs the manual pairing code on boot. With the current defaults it should be:

```text
34970112332
```

## Reset Matter State

Before pairing the board to a different hub, erase the NVS partition so the stored
Matter fabric state is clean:

```sh
python -m esptool --chip esp32s3 -p /dev/your-port erase-region 0x9000 0x6000
```

Then reset or power-cycle the board and pair it again. Do not hold BOOT/GPIO0
while resetting; on the ESP32-S3 Supermini that enters the ROM download mode.
