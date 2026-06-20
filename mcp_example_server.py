#!/usr/bin/env python3
# mcp_example_server.py - server MCP stdio minimale di esempio/test per mcp_bridge.py.
# Espone due tool: 'eco' e 'somma'. Protocollo: JSON-RPC 2.0 newline-delimited su stdio.
import sys, json

def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()

TOOLS = [
    {"name": "eco", "description": "Ripete il testo fornito.",
     "inputSchema": {"type": "object", "properties": {"testo": {"type": "string"}}, "required": ["testo"]}},
    {"name": "somma", "description": "Somma due numeri a e b.",
     "inputSchema": {"type": "object", "properties": {"a": {"type": "number"}, "b": {"type": "number"}},
                     "required": ["a", "b"]}},
]

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        msg = json.loads(line)
    except Exception:
        continue
    mid, method = msg.get("id"), msg.get("method")
    if method == "initialize":
        send({"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
            "serverInfo": {"name": "esempio", "version": "1.0"}}})
    elif method == "notifications/initialized":
        pass
    elif method == "tools/list":
        send({"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}})
    elif method == "tools/call":
        p = msg.get("params", {}); name = p.get("name"); a = p.get("arguments", {})
        if name == "eco":
            txt = str(a.get("testo", ""))
        elif name == "somma":
            try:
                txt = str(float(a.get("a", 0)) + float(a.get("b", 0)))
            except Exception as e:
                txt = "errore: " + str(e)
        else:
            txt = "tool sconosciuto"
        send({"jsonrpc": "2.0", "id": mid, "result": {"content": [{"type": "text", "text": txt}], "isError": False}})
    elif mid is not None:
        send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}})
