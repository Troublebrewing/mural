#!/usr/bin/env python3
"""Upload a small square test print using Mural's REST API."""

import argparse
import json
import math
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


def request_json(url, method="GET", body=None, content_type=None, timeout=10):
    headers = {"Accept": "application/json"}
    if content_type:
        headers["Content-Type"] = content_type
    request = Request(url, data=body, headers=headers, method=method)
    try:
        with urlopen(request, timeout=timeout) as response:
            data = response.read()
            return response.status, json.loads(data) if data else {}
    except HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {error.code} from {url}: {detail}") from error
    except (URLError, TimeoutError) as error:
        raise RuntimeError(f"Request to {url} failed: {error}") from error


def make_test_commands(canvas, size_mm):
    width = float(canvas["canvasWidth"])
    home_x = float(canvas["homeX"])
    home_y = float(canvas["homeY"])
    if not math.isfinite(width) or width <= 0:
        raise RuntimeError("Canvas width is unavailable; set the pin spacing first.")
    if not all(math.isfinite(value) for value in (home_x, home_y)):
        raise RuntimeError("Firmware returned invalid home coordinates.")

    half_size = size_mm / 2
    left = home_x - half_size
    right = home_x + half_size
    top = home_y - half_size
    bottom = home_y + half_size
    if left < 0 or right > width - 1:
        raise RuntimeError(
            f"A {size_mm:g} mm test square centered at home does not fit "
            f"the {width:g} mm printable width."
        )
    if top < 0:
        raise RuntimeError("Test square would extend below Y=0.")

    points = [
        (left, top),
        (right, top),
        (right, bottom),
        (left, bottom),
        (left, top),
        (home_x, home_y),
    ]
    distance = math.dist((home_x, home_y), points[0])
    distance += sum(math.dist(a, b) for a, b in zip(points, points[1:]))

    lines = [f"d{distance:.1f}", f"h{size_mm:.1f}", "p0", f"{left:.2f} {top:.2f}", "p1"]
    lines.extend(f"{x:.2f} {y:.2f}" for x, y in points[1:5])
    lines.extend(("p0", f"{home_x:.2f} {home_y:.2f}"))
    return ("\n".join(lines) + "\n").encode("ascii")


def wait_for_completion(base_url, poll_interval, timeout):
    deadline = time.monotonic() + timeout
    saw_printing = False
    while time.monotonic() < deadline:
        _, status = request_json(f"{base_url}/api/status")
        state = status.get("status")
        print(
            f"status={state} homed={status.get('homed')} "
            f"bytes={status.get('bytesRead')}/{status.get('totalBytes')}"
        )
        if state == "error":
            raise RuntimeError("Firmware reported a print error.")
        if state in ("printing", "homing"):
            saw_printing = True
        elif state == "idle":
            if not status.get("homed", False):
                raise RuntimeError("Plotter became unhomed before completion (possibly stopped).")
            bytes_read = status.get("bytesRead", 0)
            total_bytes = status.get("totalBytes", 0)
            if saw_printing or (total_bytes > 0 and bytes_read >= total_bytes):
                print("Test print completed.")
                return
        time.sleep(poll_interval)
    raise RuntimeError(f"Timed out after {timeout:g}s waiting for the test print.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--base-url",
        default="http://mural.local",
        help="Plotter base URL (default: http://mural.local)",
    )
    parser.add_argument(
        "--size-mm",
        type=float,
        default=10.0,
        help="Side length of the square in millimeters (default: 10)",
    )
    parser.add_argument(
        "--upload-only",
        action="store_true",
        help="Upload the test commands without starting the print",
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="Skip the confirmation prompt before a live print",
    )
    parser.add_argument("--poll-interval", type=float, default=2.0)
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()

    if not math.isfinite(args.size_mm) or not 1 <= args.size_mm <= 50:
        parser.error("--size-mm must be between 1 and 50")
    if args.poll_interval <= 0 or args.timeout <= 0:
        parser.error("--poll-interval and --timeout must be positive")

    base_url = args.base_url.rstrip("/")
    try:
        _, canvas = request_json(f"{base_url}/api/canvas")
        _, state = request_json(f"{base_url}/api/status")
        if not canvas.get("homed") or not state.get("homed"):
            raise RuntimeError("Plotter is not homed. Home it using the web UI first.")
        if state.get("status") != "idle" or state.get("uploadInProgress"):
            raise RuntimeError(f"Plotter is busy (status={state.get('status')}).")

        payload = make_test_commands(canvas, args.size_mm)
        print(f"Test job is {args.size_mm:g} mm square near home:")
        print(payload.decode("ascii"), end="")
        if not args.upload_only and not args.yes:
            confirmation = input("This will move the plotter. Type PRINT to continue: ")
            if confirmation != "PRINT":
                print("Cancelled; nothing uploaded.")
                return 1

        query = urlencode({"autostart": str(not args.upload_only).lower(), "require_home": "true"})
        response_code, response = request_json(
            f"{base_url}/api/print?{query}",
            method="POST",
            body=payload,
            content_type="text/plain",
        )
        print(f"Upload returned HTTP {response_code}: {json.dumps(response)}")
        if args.upload_only:
            print("Uploaded without starting; no motion was requested.")
            return 0

        wait_for_completion(base_url, args.poll_interval, args.timeout)
        return 0
    except (RuntimeError, KeyError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
