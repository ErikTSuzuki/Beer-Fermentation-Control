"""Loopback-only UI preview with in-memory fixtures, not a real fermenter.

Run: python3 tests/preview_local_ui.py
URL: http://127.0.0.1:8765/ ; test password: local-test-password
Mutations affect only disposable memory in this preview process.
"""
from copy import deepcopy
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import time

PAGE = Path(__file__).resolve().parents[1] / "main/web/index.html"
PASSWORD = "local-test-password"
profile = None
started_at = None
enabled = False
cancelled = False


class PreviewHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, body, status=200, content_type="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def authorized(self):
        if self.headers.get("X-Local-Key") == PASSWORD.encode().hex():
            return True
        self.reply({"error": "invalid_password"}, 401)
        return False

    def do_GET(self):
        if self.path == "/":
            self.reply(PAGE.read_bytes(), content_type="text/html; charset=utf-8")
            return
        if not self.authorized():
            return
        if self.path == "/api/recipe":
            self.reply(profile)
        elif self.path == "/api/status":
            step = profile["steps"][0] if profile else None
            self.reply({
                "sensors": {"temperature_c": 20.31, "ispindel_temperature_c": 20.2,
                            "gravity": 1.039, "gravity_unit": "sg"},
                "control": {"available": True, "target_c": step["target_c"] if step else 18,
                            "state": 3 if enabled else 0, "state_name": "RESFRIANDO" if enabled else "DESATIVADO",
                            "enabled": enabled, "heater_on": False, "cooler_on": enabled, "wait_seconds": 0},
                "recipe": {"available": True, "state": 1 if enabled else 0,
                           "name": profile["name"] if profile else "", "step_name": step["name"] if step else "",
                           "step": 1 if step else 0, "step_count": len(profile["steps"]) if profile else 0,
                           "elapsed_days": (time.monotonic() - started_at) / 86400 if enabled else 0,
                           "max_days": step["max_days"] if step else 0,
                           "gravity_limit": step.get("gravity_limit") if step else None,
                           "confirmation_count": 0, "gravity_available": True, "storage_ok": True,
                           "reason": 4 if cancelled else 1 if enabled else 0, "last_error": 0},
                "capabilities": {"target_min_c": -4.5, "target_max_c": 39.5, "max_steps": 16, "max_json_bytes": 4096},
                "cloud_configured": True, "uplink_connected": True,
            })
        else:
            self.reply({"error": "not_found"}, 404)

    def do_POST(self):
        global profile, started_at, enabled, cancelled
        if not self.authorized():
            return
        if self.headers.get("Origin") != "http://127.0.0.1:8765":
            self.reply({"error": "same_origin_required"}, 403)
            return
        if self.path == "/api/recipe":
            if enabled:
                self.reply({"error": "recipe_running_or_unavailable"}, 409)
                return
            try:
                candidate = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
                assert candidate["version"] == 1 and len(candidate["steps"]) <= 16
            except (ValueError, KeyError, AssertionError):
                self.reply({"error": "invalid_recipe"}, 400)
                return
            profile, started_at, enabled, cancelled = deepcopy(candidate), time.monotonic(), True, False
        elif self.path == "/api/recipe/cancel":
            enabled, cancelled = False, True
        else:
            self.reply({"error": "not_found"}, 404)
            return
        self.reply({"accepted": True}, 202)


if __name__ == "__main__":
    print("Local UI test preview: http://127.0.0.1:8765/ (in-memory fixture only)", flush=True)
    HTTPServer(("127.0.0.1", 8765), PreviewHandler).serve_forever()
