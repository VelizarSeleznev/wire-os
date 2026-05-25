#!/usr/bin/env python3
import sys
import os
import argparse
import http.client
import json
from urllib.parse import urlencode

def request(ip, port, method, path, body=b"", headers=None, timeout=10):
    conn = http.client.HTTPConnection(ip, port, timeout=timeout)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        response = conn.getresponse()
        payload = response.read()
        return response.status, payload
    except Exception as e:
        print(f"Error during network request to {ip}:{port}: {e}")
        sys.exit(1)
    finally:
        conn.close()

def main():
    parser = argparse.ArgumentParser(description="Switch Transfer Hub CLI - Deploy files directly to Nintendo Switch over Wi-Fi")
    parser.add_argument("local_file", help="Path to local file to upload")
    parser.add_argument("target_path", nargs="?", help="Target file path on Switch (e.g. /switch/vector-switch-control.nro). Defaults to /switch/<filename>")
    parser.add_argument("--ip", "-i", default="192.168.1.74", help="Switch IP address (default: 192.168.1.74)")
    parser.add_argument("--port", "-p", type=int, default=8080, help="Switch Transfer Hub port (default: 8080)")
    parser.add_argument("--pin", default="123456", help="Pairing PIN code (default: 123456)")
    parser.add_argument("--device-id", "-d", default="macbook-agent", help="Unique device identifier (default: macbook-agent)")
    parser.add_argument("--no-clean", action="store_true", help="Do not delete existing/temporary files on Switch before uploading")

    args = parser.parse_args()

    local_path = os.path.abspath(args.local_file)
    if not os.path.exists(local_path):
        print(f"Error: Local file '{local_path}' does not exist.")
        sys.exit(1)

    filename = os.path.basename(local_path)
    target_path = args.target_path
    if not target_path:
        target_path = f"/switch/{filename}"

    print(f"🚀 Switch Transfer Hub CLI")
    print(f"  Local file:  {local_path}")
    print(f"  Target path: {target_path}")
    print(f"  Switch Node: {args.ip}:{args.port}")

    # 1. Pair with the Switch
    pair_data = {"pin": args.pin, "device_id": args.device_id}
    pair_headers = {
        "Content-Type": "application/json",
        "X-Device-Id": args.device_id
    }
    print(f"🔑 Pairing using PIN '{args.pin}'...")
    status, payload = request(args.ip, args.port, "POST", "/api/pair", json.dumps(pair_data).encode("utf-8"), pair_headers)

    if status != 200:
        print(f"❌ Pairing failed (Status {status}): {payload.decode('utf-8')}")
        sys.exit(1)

    try:
        pair_res = json.loads(payload.decode("utf-8"))
    except Exception as e:
        print(f"❌ Failed to parse pairing response: {e}")
        sys.exit(1)

    if not pair_res.get("paired"):
        print("❌ Switch rejected pairing!")
        sys.exit(1)
    print("✅ Pairing successful!")

    # 2. Deleting old files to avoid write locks and chunked/part offsets
    if not args.no_clean:
        clean_headers = {
            "Content-Type": "application/json",
            "X-Device-Id": args.device_id
        }
        print(f"🗑️  Clearing target {target_path} (if present)...")
        request(args.ip, args.port, "POST", "/api/fs/delete", json.dumps({"path": target_path}).encode("utf-8"), clean_headers)
        print(f"🗑️  Clearing upload part cache {target_path}.part...")
        request(args.ip, args.port, "POST", "/api/fs/delete", json.dumps({"path": target_path + ".part"}).encode("utf-8"), clean_headers)

    # 3. Read and Upload File
    with open(local_path, "rb") as f:
        file_bytes = f.read()

    file_size = len(file_bytes)
    print(f"📤 Uploading {file_size} bytes...")

    upload_headers = {
        "Content-Type": "application/octet-stream",
        "X-Upload-Offset": "0",
        "X-Upload-Total": str(file_size),
        "X-Device-Id": args.device_id
    }

    upload_url = "/api/upload?" + urlencode({"path": target_path})
    status, payload = request(args.ip, args.port, "POST", upload_url, file_bytes, upload_headers, timeout=30)

    if status != 200:
        print(f"❌ Upload failed (Status {status}): {payload.decode('utf-8')}")
        sys.exit(1)

    try:
        res = json.loads(payload.decode("utf-8"))
    except Exception as e:
        print(f"❌ Failed to parse upload response: {e}")
        sys.exit(1)

    if res.get("completed"):
        print(f"\n🎉 SUCCESS! Directly deployed to Nintendo Switch!")
    else:
        print(f"⚠️ Upload response incomplete: {res}")
        sys.exit(1)

if __name__ == "__main__":
    main()
