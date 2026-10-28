#!/usr/bin/env python3
"""Protocol regression server. No real credentials or external requests."""
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def Call(name, args, index):
    return {"id": f"call_{index}", "type": "function",
            "function": {"name": name, "arguments": json.dumps(args, ensure_ascii=False)}}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            assert body["thinking"] == {"type": "disabled"}
            repair = body.get("tool_choice") == "none"
            if repair:
                assert "tools" not in body
                assert body["response_format"]["type"] in {"json_schema", "json_object"}
            else:
                assert "response_format" not in body
                assert {t["function"]["name"] for t in body["tools"]} == {
                    "get_current_time", "get_weather", "get_volume", "set_volume",
                    "control_music", "get_music_status", "save_memory",
                    "forget_memory", "list_memories", "create_reminder"}
            messages = body["messages"]
            request_messages = messages[:-1] if repair else messages
            user_index = max(i for i, m in enumerate(request_messages) if m["role"] == "user")
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
                    message["tool_calls"] = [Call(n, a, i) for i, (n, a) in enumerate(specs)]
                else:
                    assert len(results) == 8 and all(r["ok"] for r in results)
                    assert results[0]["timezone"] == "Asia/Shanghai"
                    assert results[1]["percent"] == 60 and results[2]["percent"] == 73
                    assert results[3]["content"] == "测试记忆" and results[4]["deleted"] == 1
                    assert results[6]["content"] == "喝水"
                    message["content"] = "all_tools_ok"
            elif case == "invalid_tools":
                if not results:
                    bad = Call("get_volume", {}, 2)
                    bad["function"]["arguments"] = "{broken"
                    message["tool_calls"] = [Call("run_shell", {"command": "date"}, 0),
                                             Call("set_volume", {"action": "set", "percent": 151}, 1), bad]
                else:
                    assert len(results) == 3 and all(not r["ok"] for r in results)
                    message["content"] = "invalid_tools_ok"
            elif case == "duplicate_call":
                if len(results) < 2:
                    message["tool_calls"] = [Call("set_volume", {"action": "up"}, 0)]
                else:
                    assert results[0] == results[1] and results[0]["percent"] == 83
                    message["content"] = "duplicate_call_ok"
            elif case == "music_tool":
                if not results:
                    message["tool_calls"] = [Call("control_music", {"action": "play", "query": "晴天"}, 0)]
                else:
                    assert results[0]["ok"] and results[0]["title"] == "晴天"
                    message["content"] = "music_tool_ok"
            elif case == "hush_cancel":
                time.sleep(2)
                message["tool_calls"] = [Call("set_volume", {"action": "down"}, 0)]
            elif case == "weather_location":
                if not results:
                    message["tool_calls"] = [Call("get_weather", {}, 0)]
                else:
                    assert results == [{"ok": False, "error_code": "location_required",
                                        "error": "请先询问用户要查询哪个城市。"}]
                    message["content"] = json.dumps(
                        {"reply": "请问你想查询哪个城市？", "expect_reply": True}, ensure_ascii=False)
            elif case == "深圳":
                previous = [m for m in messages[:user_index] if m["role"] == "assistant"]
                assert len(previous) == 1
                assert json.loads(previous[0]["content"]) == {
                    "reply": "请问你想查询哪个城市？", "expect_reply": True}
                message["content"] = "city_context_ok"
            elif case in {"new_session", "after_timeout", "after_invalid"}:
                assert [m["role"] for m in messages] == ["system", "user"]
                message["content"] = case + "_ok"
            elif case == "timeout_question":
                message["content"] = json.dumps(
                    {"reply": "还有哪个城市？", "expect_reply": True}, ensure_ascii=False)
            elif case == "invalid_response":
                if self.path == "/local":
                    message["content"] = '不能回答。{"reply":"错误历史","expect_reply":false}'
                else:
                    assert self.path == "/cloud"
                    message["content"] = "format_fallback_ok"
            elif case == "repair_response":
                message["content"] = ("format_repair_ok" if repair else
                                      '普通正文。{"reply":"格式整理","expect_reply":false}')
            else:
                message["content"] = "recovered_ok"
            malformed = ((case == "invalid_response" and self.path == "/local") or
                         (case == "repair_response" and not repair))
            if message["content"] is not None and not malformed:
                try:
                    structured = json.loads(message["content"])
                except (ValueError, TypeError):
                    structured = None
                if not isinstance(structured, dict):
                    message["content"] = json.dumps(
                        {"reply": message["content"], "expect_reply": False}, ensure_ascii=False)
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
