"""Send a local C5 image through the gate's update API, as its page does."""
import sys, time, requests
host, path = sys.argv[1], sys.argv[2]
print("C5 before:", requests.get(f"http://{host}/api/c5/update", timeout=10).json()["c5"], flush=True)
data = open(path, "rb").read()
print(f"{path}: {len(data)} bytes, version {data[48:80].split(b'\0')[0].decode()}", flush=True)
up = requests.post(f"http://{host}/api/c5/firmware", data=data,
                   headers={"Content-Type": "application/octet-stream"}, timeout=60)
print("stored:", up.status_code, up.text.strip()[:120], flush=True)
if up.status_code != 200: sys.exit(1)
st = requests.post(f"http://{host}/api/c5/update", timeout=10)
print("start:", st.status_code, st.text.strip()[:120], flush=True)
if st.status_code != 200: sys.exit(1)
t0, last = time.time(), None
while time.time() - t0 < 300:
    s = requests.get(f"http://{host}/api/c5/update", timeout=10).json()
    if s["state"] != last:
        print(f"{time.time() - t0:5.1f} s  {s['state']}  {s['sent']}/{s['size']}  {s.get('error', '')}", flush=True)
        last = s["state"]
    if s["state"] in ("done", "failed"): break
    time.sleep(1)
time.sleep(70)  # past the C5's 60 s trial window
print("C5 after trial window:", requests.get(f"http://{host}/api/c5/update", timeout=10).json()["c5"], flush=True)
