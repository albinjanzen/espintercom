import json
import sys
import urllib.request

ports_left = None
with urllib.request.urlopen("http://localhost:8765/events", timeout=300) as stream:
    for raw in stream:
        line = raw.decode().strip()
        if not line.startswith("data: "):
            continue
        event = json.loads(line[6:])
        if event.get("type") == "build" and event.get("line") in ("compile ok", "compile FAILED"):
            print(event["line"], flush=True)
            if event["line"] == "compile FAILED":
                sys.exit(1)
        if event.get("type") == "port" and event.get("state") in ("flashing",):
            ports_left = (ports_left or set()) | {event["port"]}
        if event.get("type") == "port" and event.get("state") in ("flashed", "flash-failed"):
            print(event["port"], event["state"], flush=True)
            if ports_left is not None:
                ports_left.discard(event["port"])
                if not ports_left:
                    break
