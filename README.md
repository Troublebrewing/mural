# [getmural.me](https://getmural.me)

Please find the main documentation on https://getmural.me. 

Discord: https://discord.gg/J3ZautHMTw

# Additional Information

## Positioning of the Drawing on the Wall

Here's how the image is prepared and drawn:

- The user defines the pin distance as part of the setup in the UI. For example 1 meter (or 1000mm). (This is d_pins in the image below.)
- The top margin is 25% of that distance, so the top of the image will be 250mm below the line between the two pins.
- Each side also has a 20% margin, so you'll get total of 60% of the horizontal distance, or 600mm.
- Now that we have the max width (600mm). The SVG is resized so its width is 600 and the height gets resized proportionally.
- Then a processing step is performed on the SVG to figure out what to actually draw, with each SVG unit being treated as millimeter.
- Finally it's converted into a simple format for Mural to draw, containing mostly its coordinate movement commands and pen up/down. This file is then uploaded to the microcontroller and executed line by line.

![image_positioning](/images/doc/muralbot_image_positioning.svg)

## Mural's Kinematic Model

Please find the kinematic model [here](KinematicModel.md).

## REST API

The firmware exposes a small HTTP API for clients that need to read plotter
geometry, upload a compiled drawing, monitor execution, or stop a job. The API
uses the same Wi-Fi connection and HTTP server as the web UI. Replace
`mural.local` in the examples with the plotter's IP address if mDNS is not
available.

All API responses include `Access-Control-Allow-Origin: *`. JSON responses use
`Content-Type: application/json`. `OPTIONS` preflight requests are supported
for API routes with `GET, POST, OPTIONS` and the `Content-Type` request header.
The API currently has no authentication; use it only on a trusted network.

### `GET /api/canvas`

Returns the current geometry and readiness:

```json
{
  "d_pins": 1000,
  "homeX": 300,
  "homeY": 350,
  "canvasWidth": 600,
  "canvasHeight": -1,
  "canvasHeightKnown": false,
  "homed": true,
  "status": "idle",
  "safeXFraction": 0.2,
  "safeYFraction": 0.25,
  "leftRightMarginMm": 200,
  "topMarginMm": 250,
  "printableBounds": {
    "xMin": 0,
    "xMax": 600,
    "yMin": 0,
    "yMax": -1,
    "heightLimited": false
  }
}
```

`d_pins` is the calibrated distance between the pulleys in millimeters. The
printable width follows the web UI's scaling convention:
`canvasWidth = d_pins * (1 - 2 * safeXFraction)` (60% of pin spacing with the
current 20% side margins). The top margin is `d_pins * safeYFraction` (25%).
The firmware does not define a maximum printable height, so `canvasHeight` and
`printableBounds.yMax` are `-1`, and the corresponding `*Known`/`heightLimited`
fields indicate that no height limit is available. Coordinates in command files
are relative to this printable canvas, not the wall's absolute coordinates.

Pin spacing is set during setup in the current session and is not restored from
non-volatile storage after reboot. Before it has been set, `d_pins`,
`canvasWidth`, and margins are `-1`; home coordinates are returned as `0, 0`.
The pulley diameter is persisted, but it is not part of this canvas response.
`homed` is also false after reboot until the plotter has completed homing.

### `POST /api/print`

Uploads a command file as the raw request body. Supported content types are
`text/plain` and `application/octet-stream`. The body is streamed in chunks to
the SD card; it is not buffered as a complete file in RAM. The finished file
replaces the shared `/commands` file used by the web UI.

Query parameters:

| Parameter | Default | Description |
| --- | --- | --- |
| `autostart` | `true` | Start execution after the file has been completely received, validated, and installed. Set to `false` to upload without starting. |
| `require_home` | `true` | Reject with `412` unless the plotter is homed. Set to `false` only if you explicitly accept the mechanical risk of uploading without a homed plotter. |

Boolean values are `true` or `false` (case-insensitive). An upload still
requires a homed plotter by default when `autostart=false`. Autostarted jobs use
the firmware's default print speed.

The file is plain text, one command per line, using LF or CRLF line endings:

```text
d1234.5
h300.0
p0
300 350
p1
310.5 360
p0
```

The format is:

1. `d<number>` — total travel distance in millimeters. Must be a finite positive
   number.
2. `h<number>` — drawing height in millimeters. Must be a finite positive
   number; the firmware currently checks this header but does not use it to
   enforce a maximum Y coordinate.
3. Zero or more drawing commands:
   - `p0` — pen up.
   - `p1` — pen down.
   - `<x> <y>` — move to the given finite non-negative coordinates in
     millimeters. If pin spacing is set, X may not exceed the printable width
     by more than 1 mm. The API does not enforce a maximum Y coordinate.

Each line is limited to 128 characters. Blank command lines are invalid. A
final newline is optional. This is the serialized format generated by the
existing drawing worker and consumed by the firmware runner.

Successful uploads return `202 Accepted`:

```json
{"bytesWritten":1234,"status":"printing"}
```

With `autostart=false`, the returned status is normally `"idle"`. Common error
responses are JSON objects with an `error` field:

| HTTP status | Meaning |
| --- | --- |
| `400 Bad Request` | Empty/incomplete upload, malformed command file, or invalid Boolean query value. |
| `409 Conflict` | Plotter is busy or a stop was requested during an upload. The existing command file is not replaced when a busy upload is rejected. |
| `412 Precondition Failed` | `require_home=true` and the plotter has not been homed. |
| `415 Unsupported Media Type` | Request content type is not `text/plain` or `application/octet-stream`. |
| `507 Insufficient Storage` | Not enough SD-card space or a write failed. |
| `500 Internal Server Error` | The SD command file could not be prepared or installed. |

### `GET /api/status`

Returns the current job and plotter state:

```json
{
  "status": "printing",
  "homed": true,
  "bytesRead": 128,
  "totalBytes": 1234,
  "uploadInProgress": false
}
```

`bytesRead` and `totalBytes` report the runner's command-file read progress;
`uploadInProgress` separately indicates an active API upload. A completed
non-autostarted upload reports zero current runner progress until execution
starts. The implemented statuses are `"idle"`, `"printing"`, `"homing"`, and
`"error"`. There is currently no pause endpoint or paused state.

### `POST /api/stop`

Requests a safe stop. If a print is running, the endpoint responds with
`202 Accepted`; stopping is performed by the firmware loop, which stops motor
motion and attempts to lift the pen. Poll `GET /api/status` to observe when it
has finished stopping. Stopping invalidates the homed position, so home the
plotter again before starting another job.

```json
{"status":"printing","stopRequested":true}
```

If no runner job is active, the endpoint returns `200 OK` with the current
status. An upload in progress cannot be stopped through this route and returns
`409 Conflict`; finish or cancel the upload first.

### curl examples

Read geometry and status, upload a file, and stop a job:

```sh
curl -i http://mural.local/api/canvas

curl -i -X POST \
  'http://mural.local/api/print?autostart=true&require_home=true' \
  -H 'Content-Type: text/plain' \
  --data-binary '@commands.txt'

curl -i http://mural.local/api/status

curl -i -X POST http://mural.local/api/stop
```

To upload without starting the job, use
`/api/print?autostart=false&require_home=true`. To intentionally bypass the
homing precondition, specify `require_home=false`. This does not home the
plotter or establish its position; an autostarted job without valid position
data may fail and report `"error"` through `GET /api/status`. Keep the default
precondition enabled for normal use.

### Python test-print client

The standard-library-only script [scripts/test_rest_api.py](scripts/test_rest_api.py)
fetches the canvas and status, creates a small square around the reported home
position, uploads it, and polls for completion. It requires the plotter to be
idle and homed. By default, it asks you to type `PRINT` before starting motion:

```sh
python scripts/test_rest_api.py
python scripts/test_rest_api.py --base-url http://192.168.1.42 --size-mm 5
```

Use `--upload-only` to test the API upload without starting the print. This
still replaces the stored `/commands` file. Use `--yes` to skip the live-print
confirmation when running unattended. `--size-mm` accepts values from 1 to 50.
