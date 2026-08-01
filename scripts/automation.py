from contextlib import closing
import json
import socket

AUTOMATION_PROTOCOL_VERSION = 1
HOOK_GRABBED = 5
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

	# Convenience layer. Everything below is built from the commands above; it exists because
	# tests otherwise spend most of their lines on the same four shapes.

	def snapshot(self):
		"""The authoritative character core the server last sent."""
		return self.get_state()["snapshot"]

	def predicted(self):
		"""The client's predicted character core, or None while prediction is not valid."""
		return self.get_state()["predicted"]

	def connect(self, server_port, host="127.0.0.1", settle_ticks=25, timeout_frames=12000):
		"""Connect, wait for a local character, and let prediction settle.

		Also drains the parity history, since the ticks around a connect are not comparable.
		"""
		self.console(f"connect {host}:{server_port}")
		self.wait_for("state", value="online", timeout_frames=timeout_frames)
		self.wait_for("has_local_character", timeout_frames=timeout_frames)
		if settle_ticks:
			self.wait_ticks(settle_ticks)
		self.get_parity_history()

	def hold(self, ticks, **inputs):
		"""Apply an input, hold it for a number of ticks, and return the resulting snapshot."""
		self.set_input(**inputs)
		self.wait_ticks(ticks)
		return self.snapshot()

	def sample(self, ticks, every=1):
		"""Return one snapshot every `every` ticks, over `ticks` ticks.

		For observing a transient, such as the apex of a jump or a hook that latches and lets go
		again, where only the endpoints would miss it.
		"""
		samples = []
		for _ in range(max(1, ticks // every)):
			self.wait_ticks(every)
			samples.append(self.snapshot())
		return samples

	def parity_mismatches(self, max_entries=400):
		"""Drain the parity history and return [(tick, {field: (predicted, snapshot)})] for
		every tick where the two disagree. Empty means the client predicted the server exactly.
		"""
		entries = self.get_parity_history(max_entries)["entries"]
		return [(entry["tick"], core_mismatches(entry)) for entry in entries if core_mismatches(entry)]


def core_mismatches(entry):
	predicted = entry["predicted"]
	snapshot = entry["snapshot"]
	return {key: (predicted[key], snapshot[key]) for key in CORE_KEYS if predicted[key] != snapshot[key]}
