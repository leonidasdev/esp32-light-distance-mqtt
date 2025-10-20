ESP32 project — filesystem contents and setup

This repository expects a FAT-formatted data partition (mounted at `/filesystem`) that contains configuration and credential files the firmware reads at boot. The device uses this partition for Wi‑Fi credentials, MQTT token, Telegram token, CA certificate (for TLS) and the static files served by the onboard webserver.

This README documents what files are expected, their formats, examples and recommended ways to prepare and flash the data partition image.

## Files expected on the data partition
Place the files at the root of the FAT partition (the firmware mounts the partition at `/filesystem`):

- `wifi.txt`
  - Format: two lines
    - Line 1: SSID
    - Line 2: Password
  - Example:

    MyWiFiNetwork
    s3cr3tPassw0rd

  - Behavior: If `wifi.txt` is present and valid the device attempts to connect as a station using these credentials. If absent or connection fails the device starts a soft-AP and a local webserver to let you configure Wi‑Fi.

- `mqtt.txt`
  - Format: single line containing the ThingsBoard device access token (or other MQTT username/token).
  - Example:

    MY_DEVICE_ACCESS_TOKEN

  - Behavior: The `components/mqtt_manager` module reads this token and uses it as the MQTT username when connecting to the broker. If missing the MQTT client is not started.

- `tele.txt`
  - Format: up to three lines (the firmware reads the first three lines):
    - Line 1: Telegram bot token (required to enable the Telegram feature)
    - Line 2: Optional admin chat id (numeric)
    - Line 3: Optional last_update_id (an integer that the firmware may use to resume Telegram polling safely)
  - Example:

    123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11
    987654321
    42

  - Behavior: If `tele.txt` exists the firmware initializes the Telegram module and may use the additional lines as persisted state.

- `ca_root.pem` (or `ca-root.pem` or `cacert.pem`)
  - Format: PEM file containing one or more trusted CA certificate(s).
  - Behavior: The firmware will check for these filenames on the data partition and — if found — register the PEM content with the TLS certificate bundle helper. This allows HTTPS downloads (OTA, manifest fetching) and secure MQTT connections to work with private CAs.

- `index.htm`
  - The HTML page served when the device is running in AP + webserver configuration. The project already includes a default `index.htm` in the repo's `filesystem/` folder; you can customize it.

- Other files
  - `mqtt.txt`, `wifi.txt`, `tele.txt` and `ca_root.pem` are the most important. The `filesystem/` folder may also contain other static files that the webserver serves.

- Future/optional (OTA)
  - We'll add support for OTA in this repository. The OTA flow will read a manifest URL (or local `ota_config.json`) from the data partition. Example `ota_config.json` (future):

    {
      "manifest_url": "https://raw.githubusercontent.com/yourorg/firmware-repo/main/manifest.json",
      "on_boot": true,
      "scheduled_hour": 3,
      "scheduled_minute": 0
    }

  - This file is optional for now; it won't be used until OTA support is implemented.

## Example filesystem contents (from this repo)

The repository already contains a `filesystem/` folder with example files. To mirror that structure to your device data partition, place the same files at the root of the FAT partition.

Suggested minimal set for a production device:
- `wifi.txt` (two lines)
- `mqtt.txt` (one line with access token)
- `ca_root.pem` (if your broker or update server uses a CA not present in the default bundle)

## How to prepare the FAT image and flash it (overview)

The firmware mounts the partition with the ESP-IDF wear-levelling FAT helper, so the partition must already contain a valid FAT filesystem with the files in its root.

There are multiple ways to create and flash the data partition image. The exact partition offset depends on your `partitions.csv`. The project `partitions.csv` currently defines a `storage` partition that the firmware mounts. Use the partition table and `parttool.py` / `idf.py` to find the flash offset for the `storage` partition.

Linux / WSL example (recommended):

1. Create a FAT image from the `filesystem/` folder (1MiB example size):

```bash
# from the repo root
fallocate -l 1M build/fs_image.img
mkfs.vfat build/fs_image.img
# copy files from `filesystem/` into the image using mtools (mcopy)
# install mtools if needed: sudo apt install mtools
mcopy -i build/fs_image.img -s filesystem/* ::
```

2. Write the image to the storage partition on the ESP32 flash (replace OFFSET with the partition offset found using idf.py or partition tool):

```bash
# WARNING: the offset must match the `storage` partition offset in your partition table
esptool.py --chip esp32c3 write_flash OFFSET build/fs_image.img
```

Windows (PowerShell) options:
- Use WSL and follow the Linux instructions above (recommended).
- Or use a tool that can create FAT images and write them to flash. Many users find WSL simpler.

Important notes:
- The partition offset / address is driven by your `partitions.csv`; do not guess it. Use `idf.py partition-table` or the `parttool.py` script from ESP-IDF to query the partition addresses:

```bash
# print partition table addresses
python $IDF_PATH/components/partition_table/parttool.py -q partition-table.csv
```

- If you format the partition on-device the bootloader or firmware must support formatting; this project currently mounts the FAT partition with `format_if_mount_failed = false`, so the partition must be pre-populated.

## Troubleshooting
- If the device fails to connect to Wi‑Fi, check `wifi.txt` line endings and ensure the SSID/password are correct. The `persistence_read_config` helper strips CR/LF, but file corruption or binary data may break parsing.
- If TLS connections fail, ensure `ca_root.pem` is present (if your server uses a non-public CA) or rely on the built-in certificate bundle (ESP-IDF) if your endpoints use widely trusted CAs.
- If MQTT doesn't start, verify `mqtt.txt` contains the correct access token (no extra whitespace lines).

## Example sample files

wifi.txt

```
MySSID
MyPassword123
```

mqtt.txt

```
MY_DEVICE_ACCESS_TOKEN
```

tele.txt

```
123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11
987654321
0
```

`ca_root.pem`

- Add your CA certificate(s) in PEM format. This file may contain multiple `-----BEGIN CERTIFICATE-----` blocks.

## What's next
- I will add OTA support soon. When implemented, I'll document how to add `ota_config.json` and how to host manifests on ThingsBoard or GitHub releases.

If you'd like, I can also add a simple `make` or `idf.py` task to build the filesystem image automatically from the `filesystem/` folder and write it to the correct flash offset. Tell me if you want that and whether you are using WSL or native Windows tooling.