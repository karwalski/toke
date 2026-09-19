#!/usr/bin/env python3
"""fake_llm_endpoint.py -- a minimal OpenAI-shaped chat endpoint, for story 136.18.

Answers POST with one completion whose text names this server's TAG, the model
the request asked for, and the Authorization header it carried, and appends the
same line to LOGFILE.  Two of these on two ports is what distinguishes a client
argument that is honoured from a process-wide singleton: the old glue built one
client from $LLM_BASE_URL and sent every call there no matter which client the
caller passed.

Usage: fake_llm_endpoint.py <port> <tag> <logfile>
"""
import sys, json, http.server, socketserver

PORT, TAG, LOGFILE = int(sys.argv[1]), sys.argv[2], sys.argv[3]


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n).decode("utf-8", "replace")
        try:
            req = json.loads(raw)
        except Exception:
            req = {}
        auth = self.headers.get("Authorization", "none")
        text = "%s|model=%s|auth=%s" % (TAG, req.get("model", "?"), auth)
        tools = req.get("tools") or []
        if tools:
            # Story 136.17: name every tool the request carried, so the log
            # proves llm.withtools actually attached them, then answer with a
            # tool call the way an OpenAI-compatible provider does.
            names = ",".join(
                (t.get("function") or {}).get("name", "?") for t in tools)
            text = "%s|tools=%s" % (text, names)
            message = {"tool_calls": [{
                "id": "call-1",
                "type": "function",
                "function": {"name": "getweather",
                             "arguments": "{\"city\": \"Sydney\"}"},
            }]}
        else:
            message = {"content": text}
        with open(LOGFILE, "a") as fh:
            fh.write(text + "\n")
        body = json.dumps({
            "choices": [{"message": message}],
            "usage": {"prompt_tokens": 11, "completion_tokens": 22},
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("127.0.0.1", PORT), Handler) as srv:
    srv.serve_forever()
