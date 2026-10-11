#!/usr/bin/env python3
"""Protocol regression server. No real credentials or external requests."""
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def call(name, args, index):
    return {"id": f"call_{index}", "type": "function",
            "function": {"name": name, "arguments": json.dumps(args, ensure_ascii=False)}}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            assert body["thinking"] == {"type": "disabled"}
            assert len(body["tools"]) == 9
            messages = body["messages"]
            user_index = max(i for i, m in enumerate(messages) if m["role"] == "user")
            case = messages[user_index]["content"]
            results = [json.loads(m["content"]) for m in messages[user_index + 1:] if m["role"] == "tool"]
            message = {"role": "assistant", "content": None}
            if case == "all_tools":
                if not results:
                    specs = [("get_current_time", {}), ("get_volume", {}),
                             ("set_volume", {"action": "set", "percent": 73}),
                             ("save_memory", {"content": "测试记忆"}),
                             ("forget_memory", {"content": "测试记忆"}),
                             ("list_memories", {}),
                             ("create_reminder", {"content": "喝水", "delay_seconds": 60}),
                             ("get_music_status", {})]
                    message["tool_calls"] = [call(n, a, i) for i, (n, a) in enumerate(specs)]
                else:
                    assert len(results) == 8 and all(r["ok"] for r in results)
                    assert results[0]["timezone"] == "Asia/Shanghai"
                    assert results[1]["percent"] == 60 and results[2]["percent"] == 73
                    assert results[3]["content"] == "测试记忆" and results[4]["deleted"] == 1
                    assert results[6]["content"] == "喝水"
                    message["content"] = "all_tools_ok"
            elif case == "invalid_tools":
                if not results:
                    bad = call("get_volume", {}, 2)
                    bad["function"]["arguments"] = "{broken"
                    message["tool_calls"] = [call("run_shell", {"command": "date"}, 0),
                                             call("set_volume", {"action": "set", "percent": 101}, 1), bad]
                else:
                    assert len(results) == 3 and all(not r["ok"] for r in results)
                    message["content"] = "invalid_tools_ok"
            elif case == "duplicate_call":
                if len(results) < 2:
                    message["tool_calls"] = [call("set_volume", {"action": "up"}, 0)]
                else:
                    assert results[0] == results[1] and results[0]["percent"] == 83
                    message["content"] = "duplicate_call_ok"
            elif case == "music_tool":
                if not results:
                    message["tool_calls"] = [call("control_music", {"action": "play", "query": "晴天"}, 0)]
                else:
                    assert results[0]["ok"] and results[0]["title"] == "晴天"
                    message["content"] = "music_tool_ok"
            elif case == "hush_cancel":
                time.sleep(2)
                message["tool_calls"] = [call("set_volume", {"action": "down"}, 0)]
            else:
                message["content"] = "recovered_ok"
            response = json.dumps({"choices": [{"message": message}]}, ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)
        except BrokenPipeError:
            pass
        except Exception as exc:
            print("FAIL", repr(exc), flush=True)
            self.send_error(500)


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", 18763), Handler).serve_forever()
