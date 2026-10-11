#!/usr/bin/env python3
"""CDP Runtime.evaluate tool for steamwebhelper (remote-debugging-port 8080).
Usage: cdp_eval.py <target-substr> <expression-file-or-'-'>
Connects to the first /json/list target whose title matches <target-substr>,
evaluates the JS expression (awaitPromise),
prints the JSON result (truncated at 12000 chars)."""
import sys
import json
import urllib.request
import websocket

def main():
    if len(sys.argv) < 3:
        print("Usage: cdp_eval.py <target-substr> <expression-file-or-'-'>")
        return
    tsub, exprf = sys.argv[1], sys.argv[2]
    if exprf == '-':
        expr = sys.stdin.read()
    else:
        with open(exprf) as f:
            expr = f.read()
    needle = tsub.lower()
    targets = json.load(urllib.request.urlopen("http://127.0.0.1:8080/json/list", timeout=5))
    t = next((x for x in targets if needle in x["title"].lower()), None)
    if not t:
        print("NO-TARGET " + tsub + " :: " + ", ".join(x["title"] for x in targets))
        return
    ws = websocket.create_connection(t["webSocketDebuggerUrl"], timeout=15)
    ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                        "params": {"expression": expr, "returnByValue": True,
                                   "awaitPromise": True}}))
    while True:
        msg = json.loads(ws.recv())
        if msg.get("id") == 1:
            # Cap the printed JSON at 12000 chars so a large result can't flood the terminal.
            print(json.dumps(msg.get("result", {}), indent=1)[:12000])
            break
    ws.close()

if __name__ == "__main__":
    main()
