from contextlib import closing
import json
import socket

AUTOMATION_PROTOCOL_VERSION = 1
CORE_KEYS = ("x", "y", "vel_x", "vel_y", "angle", "direction", "jumped", "hooked_player", "hook_state", "hook_tick", "hook_x", "hook_y", "hook_dx", "hook_dy")


class AutomationError(RuntimeError):
	pass


def free_tcp_port():
	with closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
		sock.bind(("127.0.0.1", 0))
		return sock.getsockname()[1]


class AutomationClient:
	def __init__(self, port, host="127.0.0.1", timeout=60):
		self.socket = socket.create_connection((host, port), timeout=timeout)
		self.socket.settimeout(timeout)
		self.buffer = b""
		self.next_id = 1
		self.last_envelope = None
		info = self.request("ping")
		if info["protocol"] != AUTOMATION_PROTOCOL_VERSION:
			raise AutomationError("automation protocol version {} != {}".format(info["protocol"], AUTOMATION_PROTOCOL_VERSION))

	def close(self):
		self.socket.close()

	def _read_line(self):
		while b"\n" not in self.buffer:
			chunk = self.socket.recv(65536)
			if not chunk:
				raise AutomationError("automation connection closed by the client")
			self.buffer += chunk
		line, self.buffer = self.buffer.split(b"\n", 1)
		return line.decode("utf-8")

	def request(self, cmd, **args):
		request_id = self.next_id
		self.next_id += 1
		self.socket.sendall((json.dumps({"id": request_id, "cmd": cmd, "args": args}, separators=(",", ":")) + "\n").encode("utf-8"))
		reply = json.loads(self._read_line())
		if reply["id"] != request_id:
			raise AutomationError(f"reply id {reply['id']} does not match request id {request_id}")
		self.last_envelope = {k: reply[k] for k in ("frame", "game_tick", "predicted_tick")}
		if not reply["ok"]:
			raise AutomationError("{}: {} (frame {}, tick {})".format(reply["error"]["code"], reply["error"]["message"], reply["frame"], reply["game_tick"]))
		return reply["result"]

	def console(self, line):
		return self.request("console", line=line)

	def get_status(self):
		return self.request("get_status")

	def get_config(self, name):
		return self.request("get_config", name=name)["value"]

	def wait_ticks(self, ticks, timeout_frames=6000):
		return self.request("wait_ticks", ticks=ticks, timeout_frames=timeout_frames)

	def wait_for(self, predicate, timeout_frames=6000, **args):
		return self.request("wait_for", predicate=predicate, timeout_frames=timeout_frames, **args)

	def set_input(self, **args):
		return self.request("set_input", **args)

	def clear_input(self, **args):
		return self.request("clear_input", **args)

	def get_state(self):
		return self.request("get_state")

	def get_parity_history(self, max_entries=400):
		return self.request("get_parity_history", max=max_entries)


def core_mismatches(entry):
	predicted = entry["predicted"]
	snapshot = entry["snapshot"]
	return {key: (predicted[key], snapshot[key]) for key in CORE_KEYS if predicted[key] != snapshot[key]}
