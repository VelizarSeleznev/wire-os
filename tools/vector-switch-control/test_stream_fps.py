import urllib.request
import hashlib
import time
import sys

URL = "http://192.168.1.89:8080/v1/camera/snapshot"
print("Starting 20-frame camera streaming test...")
times = []
hashes = set()

for i in range(20):
    start = time.time()
    try:
        with urllib.request.urlopen(URL, timeout=3.0) as f:
            data = f.read()
    except Exception as e:
        print(f"Frame {i}: ERROR: {e}")
        sys.exit(1)
    
    elapsed = time.time() - start
    md5 = hashlib.md5(data).hexdigest()
    times.append(elapsed)
    hashes.add(md5)
    print(f"Frame {i:02d}: size={len(data)} bytes, time={elapsed:.3f}s, md5={md5}")
    time.sleep(0.05)

print("\n--- Test Results ---")
print(f"Average frame time: {sum(times)/len(times):.3f}s")
print(f"Unique frames: {len(hashes)} out of 20")
if len(hashes) == 20:
    print("🎉 SUCCESS! No stale or frozen frames, camera is streaming continuously at full speed!")
else:
    print("⚠️ WARNING: Some frames were duplicate or duplicate hashes found!")
