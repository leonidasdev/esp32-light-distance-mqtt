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

## OTA support (ThingsBoard & URL/GitHub)

This project includes a full device-side OTA flow using ThingsBoard-hosted firmware as well as URL-based OTA (for GitHub releases or other HTTPS hosts).

High-level behavior

- The device listens for ThingsBoard attribute updates. When a shared attribute set includes firmware metadata (for example `fw_title` + `fw_version` or a `fw_url`), the device will perform a secure preflight check and then download and apply the firmware.
- If the attribute contains `fw_url`, the device uses `esp_https_ota()` to download that URL and apply the image.
- If ThingsBoard provides `fw_title` + `fw_version` (no `fw_url`), the device uses the ThingsBoard device firmware API (v1) to request the firmware package and streams it to the OTA partition while computing SHA256 to validate the package before switching boot partition.

Persistence & confirmation

- After a successful OTA write/verification the device persists the firmware identity to NVS (namespace `ota`, keys: `version`, `title`) and sets an internal `confirmed` flag to `0`.
- On the next boot the device will publish the persisted firmware identity as client attributes (`current_fw_title` and `current_fw_version`) and then publish a one-time confirmation telemetry `{ "fw_state": "UPDATED", "current_fw_version": "<version>" }`. That confirmation sets `confirmed=1` in NVS so the device doesn't repeatedly confirm the same update.

ThingsBoard attribute shape (example)

If you manage OTA from ThingsBoard, set shared attributes similar to the following. Two example flows are shown below:

- URL-based OTA (use an HTTPS URL for `fw_url`):

```json
{
  "fw_title": "esp_sbc_ota",
  "fw_version": "1.4",
  "fw_size": 1099120,
  "fw_checksum": "9af6211b58966ae9728fcfe8cb13fb68b5ceebf24472adb77c0b0d8a2eadbc4b",
  "fw_checksum_algorithm": "SHA256",
  "fw_url": "https://my.cdn.example.com/firmware/esp_sbc_ota-1.4.bin"
}
```

- ThingsBoard-hosted package (no `fw_url`): the device will use the v1 firmware API and the device access token to download by title+version:

```json
{
  "fw_title": "esp_sbc_ota",
  "fw_version": "1.4",
  "fw_size": 1099120,
  "fw_checksum": "9af6211b58966ae9728fcfe8cb13fb68b5ceebf24472adb77c0b0d8a2eadbc4b",
  "fw_checksum_algorithm": "SHA256"
}
```

Telemetry and attribute keys produced by the device

- The device publishes these client attributes on MQTT connect: `current_fw_title`, `current_fw_version`.
- The device publishes telemetry states during OTA: `fw_state` values include `DOWNLOADING`, `DOWNLOADED`, `VERIFIED`, `UPDATED`, and `FAILED`. When failed the device publishes `fw_error` (for example `checksum_mismatch` or `empty_download`).

Diagnostics

- The OTA code includes extra diagnostics for ThingsBoard downloads: it logs HTTP status, content-length, total bytes downloaded, and a small hex preview of the first bytes of the payload. These logs help determine if ThingsBoard returned an empty response, JSON wrapper, redirect, or the expected raw firmware.

Security notes and behavior

- Before attempting TLS connections the device ensures the system time is sane (SNTP) to avoid certificate validation failures.
- The device prefers a runtime CA PEM loaded from the data partition (see CA PEM section below), but will fall back to the global CA store when no filesystem PEM is present.

GitHub-hosted OTA

- If you host a firmware binary on GitHub (release asset or raw file), provide a stable HTTPS `fw_url` pointing to the asset or a CDN-backed URL. The device will call `esp_https_ota()` to download and apply the image.
- Ensure the device trusts the host certificate chain (GitHub's CA chain is typically trusted by default, but if you use a custom host or CDN you must include the root certificate in the `ca_root.pem`).

Implementation notes

- On success the device persists `version` and `title` in NVS (namespace `ota`) and sets `confirmed=0`. On boot it publishes attributes and sends one confirmation telemetry which sets `confirmed=1`.
- The device will compare incoming OTA metadata against the persisted NVS `version` and will skip OTA if the device already reports the same version (this prevents reapplying the same firmware repeatedly when ThingsBoard and the device attribute sync are out of sync).

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

## Troubleshooting

- If the device fails to connect to Wi‑Fi, check `wifi.txt` line endings and ensure the SSID/password are correct. The `persistence_read_config` helper strips CR/LF, but file corruption or binary data may break parsing.
- If TLS connections fail, ensure `ca_root.pem` is present (if your server uses a non-public CA) or rely on the built-in certificate bundle (ESP-IDF) if your endpoints use widely trusted CAs.
- If MQTT doesn't start, verify `mqtt.txt` contains the correct access token (no extra whitespace lines).

- If you format the partition on-device the bootloader or firmware must support formatting; this project currently mounts the FAT partition with `format_if_mount_failed = false`, so the partition must be pre-populated.

OTA-specific troubleshooting

- If ThingsBoard OTA fails with a checksum mismatch, check the `fw_checksum` value you provided in ThingsBoard (it must be the SHA256 hex digest of the binary that the device will download).
- If ThingsBoard downloads return empty or a small JSON error/redirect, the OTA diagnostics log will show the HTTP status and the first bytes (hex) of the response. That usually indicates an authentication or URL issue (check the token, or if using a package-id endpoint ensure Bearer auth is used).
- If TLS verification fails for ThingsBoard but other HTTPS (telegram) succeeds, verify the runtime `ca_root.pem` includes the correct root/intermediate certificates for the ThingsBoard host. The included CA PEM used during testing contains both ThingsBoard (demo.thingsboard.io) and Telegram (api.telegram.org) roots so the device can reach both services.

## Example sample files

wifi.txt

```text
MySSID
MyPassword123
```

mqtt.txt

```text
MY_DEVICE_ACCESS_TOKEN
```

tele.txt

```text
123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11
987654321
0
```

`ca_root.pem`

- Add your CA certificate(s) in PEM format. This file may contain multiple `-----BEGIN CERTIFICATE-----` blocks.
