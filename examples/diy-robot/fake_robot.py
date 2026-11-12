#!/usr/bin/env python3
"""Robô falso para testar o GLaDOS sem hardware: python3 fake_robot.py [porta]"""
import socketserver, sys

state = "stopped"

class H(socketserver.StreamRequestHandler):
    def handle(self):
        global state
        cmd = self.rfile.readline(64).decode(errors="ignore").strip()
        print("recebi:", cmd, flush=True)
        reply = {"START": "OK", "STOP": "OK", "PAUSE": "OK", "DOCK": "OK", "STATUS": "OK "}.get(cmd, "ERR comando desconhecido")
        if cmd in ("START", "STOP", "PAUSE", "DOCK"):
            state = {"START": "working", "STOP": "stopped", "PAUSE": "paused", "DOCK": "docking"}[cmd]
        if cmd == "STATUS":
            reply += state
        self.wfile.write((reply + "\n").encode())

if __name__ == "__main__":
    with socketserver.ThreadingTCPServer(("0.0.0.0", int(sys.argv[1]) if len(sys.argv) > 1 else 7777), H) as s:
        s.allow_reuse_address = True
        print("robô falso no ar"); s.serve_forever()
