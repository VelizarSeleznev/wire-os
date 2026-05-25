#!/usr/bin/env python3
import sys
import os
import http.client
import json
from urllib.parse import urlencode

SWITCH_IP = "192.168.1.74"
SWITCH_PORT = 8080
PIN = "123456"
DEVICE_ID = "macbook-agent"
NRO_PATH = "/Users/velizard/Projects/wire-os/artifacts/dist/vector-switch-control.nro"
TARGET_PATH = "/switch/vector-switch-control.nro"

def request(method, path, body=b"", headers=None):
    conn = http.client.HTTPConnection(SWITCH_IP, SWITCH_PORT, timeout=10)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        response = conn.getresponse()
        payload = response.read()
        return response.status, payload
    except Exception as e:
        print(f"Error during network request: {e}")
        sys.exit(1)
    finally:
        conn.close()

def main():
    if not os.path.exists(NRO_PATH):
        print(f"Error: Compiled NRO not found at {NRO_PATH}")
        sys.exit(1)

    print(f"Connecting to Switch at {SWITCH_IP}:{SWITCH_PORT}...")
    
    # 1. Pair with the Switch
    pair_data = {"pin": PIN, "device_id": DEVICE_ID}
    pair_headers = {
        "Content-Type": "application/json",
        "X-Device-Id": DEVICE_ID
    }
    print(f"Pairing device '{DEVICE_ID}' using PIN '{PIN}'...")
    status, payload = request("POST", "/api/pair", json.dumps(pair_data).encode("utf-8"), pair_headers)
    
    if status != 200:
        print(f"Pairing failed (Status {status}): {payload.decode('utf-8')}")
        sys.exit(1)
        
    pair_res = json.loads(payload.decode("utf-8"))
    if not pair_res.get("paired"):
        print("Switch rejected pairing!")
        sys.exit(1)
    print("Pairing successful!")

    # 2. Delete the old file and its temporary .part file if they exist to allow overwriting
    delete_headers = {
        "Content-Type": "application/json",
        "X-Device-Id": DEVICE_ID
    }
    print(f"Clearing old binary at {TARGET_PATH} (if present)...")
    request("POST", "/api/fs/delete", json.dumps({"path": TARGET_PATH}).encode("utf-8"), delete_headers)
    print("Clearing temporary upload state...")
    request("POST", "/api/fs/delete", json.dumps({"path": TARGET_PATH + ".part"}).encode("utf-8"), delete_headers)

    # 3. Upload the compiled NRO
    with open(NRO_PATH, "rb") as f:
        file_data = f.read()

    file_size = len(file_data)
    print(f"Uploading vector-switch-control.nro ({file_size} bytes) to {TARGET_PATH}...")

    upload_headers = {
        "Content-Type": "application/octet-stream",
        "X-Upload-Offset": "0",
        "X-Upload-Total": str(file_size),
        "X-Device-Id": DEVICE_ID
    }
    
    path = "/api/upload?" + urlencode({"path": TARGET_PATH})
    status, payload = request("POST", path, file_data, upload_headers)

    if status != 200:
        print(f"Upload failed (Status {status}): {payload.decode('utf-8')}")
        sys.exit(1)

    res = json.loads(payload.decode("utf-8"))
    if res.get("completed"):
        print("\n🎉 SUCCESS! Application uploaded directly to your Nintendo Switch!")
        print("You can close Switch Transfer Hub and immediately open 'Vector Robot Controller' in the Homebrew Menu!")
    else:
        print(f"Upload incomplete: {res}")
        sys.exit(1)

if __name__ == "__main__":
    main()
