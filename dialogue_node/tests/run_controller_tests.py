#!/usr/bin/env python3
"""Run production business components against an isolated HTTP model server."""
import os
import subprocess
import sys
import threading
from http.server import ThreadingHTTPServer

sys.dont_write_bytecode = True
from mock_deepseek import Handler


def Main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: run_controller_tests.py WEATHER_TEST CONVERSATION_TEST")
    subprocess.run([sys.argv[1]], check=True)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        environment = dict(os.environ, DEEPSEEK_API_KEY="isolated-test-placeholder")
        subprocess.run([sys.argv[2], str(server.server_port)], check=True,
                       env=environment, timeout=60)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == "__main__":
    Main()
