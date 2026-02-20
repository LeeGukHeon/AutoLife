#!/usr/bin/env python3
import argparse
import base64
import hashlib
import hmac
import json
import os
import pathlib
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timezone

from _script_common import resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check Upbit authenticated API HTTP status with current API key."
    )
    parser.add_argument("--base-url", default="https://api.upbit.com")
    parser.add_argument("--endpoint", default="/v1/accounts")
    parser.add_argument("--timeout-sec", type=float, default=10.0)
    parser.add_argument("--env-file", default=r".\.env")
    return parser.parse_args(argv)


def load_env_file(path_value: pathlib.Path) -> dict:
    out = {}
    if not path_value.exists():
        return out
    text = path_value.read_text(encoding="utf-8-sig", errors="ignore")
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        out[key.strip()] = value.strip().strip('"').strip("'")
    return out


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def make_jwt(access_key: str, secret_key: str) -> str:
    header = {"alg": "HS512", "typ": "JWT"}
    payload = {"access_key": access_key, "nonce": str(uuid.uuid4())}
    header_b64 = b64url(json.dumps(header, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))
    payload_b64 = b64url(json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))
    signing_input = f"{header_b64}.{payload_b64}".encode("utf-8")
    signature = hmac.new(secret_key.encode("utf-8"), signing_input, hashlib.sha512).digest()
    return f"{header_b64}.{payload_b64}.{b64url(signature)}"


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def main(argv=None) -> int:
    args = parse_args(argv)
    env_path = resolve_repo_path(args.env_file)
    env_map = load_env_file(env_path)

    access_key = os.environ.get("UPBIT_ACCESS_KEY") or env_map.get("UPBIT_ACCESS_KEY", "")
    secret_key = os.environ.get("UPBIT_SECRET_KEY") or env_map.get("UPBIT_SECRET_KEY", "")
    if not access_key or not secret_key:
        print(json.dumps({
            "ts_utc": utc_now_iso(),
            "ok": False,
            "error": "UPBIT_ACCESS_KEY/UPBIT_SECRET_KEY missing",
        }, ensure_ascii=False))
        return 2

    token = make_jwt(access_key=access_key, secret_key=secret_key)
    url = f"{str(args.base_url).rstrip('/')}{str(args.endpoint)}"
    request = urllib.request.Request(url=url, method="GET")
    request.add_header("Authorization", f"Bearer {token}")

    status_code = 0
    response_body = ""
    remaining_req = ""
    err_message = ""
    try:
        with urllib.request.urlopen(request, timeout=float(args.timeout_sec)) as response:
            status_code = int(getattr(response, "status", 200))
            response_body = response.read().decode("utf-8", errors="ignore")
            remaining_req = str(response.headers.get("Remaining-Req", ""))
    except urllib.error.HTTPError as exc:
        status_code = int(getattr(exc, "code", 0) or 0)
        remaining_req = str(exc.headers.get("Remaining-Req", "")) if exc.headers else ""
        try:
            response_body = exc.read().decode("utf-8", errors="ignore")
        except Exception:
            response_body = ""
        err_message = str(exc)
    except Exception as exc:
        err_message = str(exc)

    payload = {
        "ts_utc": utc_now_iso(),
        "url": url,
        "http_status": int(status_code),
        "remaining_req": remaining_req,
        "ok": 200 <= int(status_code) < 300,
    }
    if payload["ok"]:
        summary = {}
        try:
            parsed = json.loads(response_body)
            if isinstance(parsed, list):
                summary = {"response_type": "list", "item_count": len(parsed)}
            elif isinstance(parsed, dict):
                summary = {"response_type": "object", "keys": sorted(list(parsed.keys()))[:10]}
            else:
                summary = {"response_type": type(parsed).__name__}
        except Exception:
            summary = {"response_type": "unknown"}
        payload["body_summary"] = summary
    else:
        body_preview = response_body.strip().replace("\r", " ").replace("\n", " ")
        if len(body_preview) > 300:
            body_preview = body_preview[:300]
        payload["body_preview"] = body_preview
    if err_message:
        payload["error"] = err_message
    print(json.dumps(payload, ensure_ascii=False))
    return 0 if payload["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
