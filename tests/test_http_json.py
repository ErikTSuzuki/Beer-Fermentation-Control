"""Test the actual ESP32 JSON decoder using the managed cJSON dependency.

Run after building ESP32 and test_hybrid_sender.py:
    python tests/test_http_json.py [path-to-gcc]
Generated files stay in build/hybrid-tests.
"""
from pathlib import Path
import json
import math
import re
import shutil
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build/hybrid-tests"
CJSON = PROJECT / "managed_components/espressif__cjson/cJSON"

HARNESS = r'''
#include "BrewJson.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    char body[2048];
    const size_t size = fread(body, 1, sizeof(body), stdin);
    BrewTelemetry packet, original;
    memset(&packet, 0x55, sizeof(packet));
    original = packet;
    if (!brew_json_parse(body, size, &packet)) {
        assert(memcmp(&packet, &original, sizeof(packet)) == 0);
        puts("null");
        return 0;
    }
    assert(brew_header_valid(&packet.header, BREW_TELEMETRY));
    printf("{\"sensor_id\":%" PRIu32 ",\"boot_id\":%" PRIu32
           ",\"sequence\":%" PRIu32 ",\"gravity_unit\":%u,"
           "\"temperature_c\":%.9g,\"tilt_degrees\":%.9g,"
           "\"gravity\":%.9g,\"battery_v\":%.9g}\n",
           packet.header.sensorId, packet.header.bootId, packet.header.sequence,
           packet.header.gravityUnit, (double)packet.temperatureC,
           (double)packet.tiltDegrees, (double)packet.gravity, (double)packet.batteryV);
}
'''


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("gcc")
    if not compiler or not (CJSON / "cJSON.c").exists():
        raise SystemExit("Host gcc and the managed cJSON dependency are required; build ESP32 first.")
    BUILD.mkdir(parents=True, exist_ok=True)
    harness = BUILD / "json_test.c"
    harness.write_text(HARNESS, encoding="utf-8")
    executable = BUILD / ("json_test.exe" if sys.platform == "win32" else "json_test")
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DCJSON_NESTING_LIMIT=8", "-I" + str(PROJECT / "main"), "-I" + str(CJSON),
        str(harness), str(PROJECT / "main/BrewJson.c"), str(CJSON / "cJSON.c"),
        "-lm", "-o", str(executable),
    ], check=True)
    sample_file = BUILD / "sender-sample.json"
    if not sample_file.exists():
        raise SystemExit("Run test_hybrid_sender.py first to produce a real sender sample.")
    sample = sample_file.read_bytes()
    reading = json.loads(sample)

    def decode(body):
        return json.loads(subprocess.run(
            [str(executable)], input=body, capture_output=True, check=True, timeout=5,
        ).stdout)

    result = decode(sample)
    for key, value in result.items():
        assert math.isclose(value, reading[key], rel_tol=1e-6), (key, value)
    scenarios = 1
    assert decode(b" " * (511 - len(sample)) + sample)
    scenarios += 1
    for key in reading:
        missing = dict(reading)
        del missing[key]
        assert decode(json.dumps(missing).encode()) is None, key
        scenarios += 1
    replacements = {
        "version": [0, 2, "1", True],
        "sensor_id": [0, -1, 1.5, 4294967296, "123"],
        "boot_id": [0, -1, 1.5],
        "sequence": [0, -1, 1.5],
        "gravity_unit": [-1, 3, "sg"],
        "temperature_c": [-56, 126, 85, None, "20"],
        "tilt_degrees": [-1, 91, None],
        "gravity": [None, "1.05", 1e100],
        "battery_v": [0, -1, 6.1],
    }
    for key, values in replacements.items():
        for value in values:
            invalid = dict(reading, **{key: value})
            assert decode(json.dumps(invalid).encode()) is None, (key, value)
            scenarios += 1
    for body in [
        b"", b"[]", b"{}", b"null", b"broken", sample[:-1], sample + b"garbage",
        sample + b"\0hidden", b" " * 512, b"[" * 200 + b"]" * 200,
        b'{"nested":{"x":1}}', sample[:-1] + b',"sequence":999}',
        re.sub(rb'("gravity":)[^,}]+', rb'\g<1>1e400', sample),
    ]:
        assert decode(body) is None, body
        scenarios += 1
    for key in ["sensor_id", "boot_id", "sequence"]:
        boundary = dict(reading, **{key: 4294967295})
        assert decode(json.dumps(boundary).encode())[key] == 4294967295
        scenarios += 1
    print(f"{scenarios} JSON receiver scenarios passed (including real sender interoperability)")


if __name__ == "__main__":
    main()
