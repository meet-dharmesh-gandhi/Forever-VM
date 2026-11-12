const dotenv = require("dotenv");
const websocket = require("ws");
const http = require("http");
const cG = require("./build/Release/node.node");
const net = require("net");
const express = require("express");
const cors = require("cors");

dotenv.config();

console.log("Server starting...", cG.ok);

const app = express();

app.use(
	cors({
		origin: "*",
	}),
);

app.use(express.json());

app.use((req, res, next) => {
	console.log(`[HTTP ${req.method}] ${req.url}`);
	next();
});

app.get("/cloud-init/:username/:password/:instanceId/user-data", (req, res) => {
	const { username, password, instanceId } = req.params;
	console.log(
		`[Cloud-Init] Serving user-data for user=${username}, instance=${instanceId}`,
	);

	if (!username || !password) {
		res.type("text/plain").send("");
		return;
	}

	let customUserSection = "";
	if (username !== "ubuntu") {
		customUserSection = `
  - name: ${username}
    plain_text_passwd: ${password}
    lock_passwd: false
    sudo: ALL=(ALL) NOPASSWD:ALL
    groups: [sudo, adm]
    shell: /bin/bash`;
	}

	const content = `#cloud-config
users:
  - default${customUserSection}

chpasswd:
  list: |
    ${username}:${password}
    ubuntu:${password}
    root:${password}
  expire: False

ssh_pwauth: True

runcmd:
  - mkdir -p /home/${username}/.config
  - touch /home/${username}/.config/gnome-initial-setup-done
  - chown -R ${username}:${username} /home/${username}/.config || true
`;

	res.type("text/plain").send(content);
});

app.get("/cloud-init/:username/:password/:instanceId/meta-data", (req, res) => {
	const { instanceId } = req.params;
	console.log(`[Cloud-Init] Serving meta-data for instance=${instanceId}`);

	if (!instanceId) {
		res.type("text/plain").send("");
		return;
	}

	res.type("text/plain").send(
		`instance-id: ${instanceId}
local-hostname: forever-vm
`,
	);
});

app.get("/", (req, res) => {
	const ip = cG.getNodeIP();
	console.log("ip?", ip);
	if (!ip) {
		console.log("error while getting new node ip");
		res.send("Could not get IP");
	} else {
		console.log("got ip:", ip);
		res.send(ip);
	}
});

app.post("/", (req, res) => {
	try {
		const jsonData = req.body;
		console.log("jsonData:", jsonData);
		const port = ports.indexOf(0);

		if (
			port != -1 &&
			jsonData.memory &&
			jsonData.vcpus &&
			jsonData.disk &&
			jsonData.username &&
			jsonData.password
		) {
			ports[port] = 1;
			const domainName = `VM-${port + START_PORT}`;
			console.log("domainName:", domainName);
			const created = cG.createVM(
				domainName,
				jsonData.vcpus,
				jsonData.memory,
				jsonData.disk,
				port + START_PORT,
				jsonData.username,
				jsonData.password,
			);

			if (!created) {
				console.log(
					"Could not create VM with memory:",
					jsonData.memory,
					"and vcpus:",
					jsonData.vcpus,
					"and vdisk:",
					jsonData.disk,
					"and username:",
					jsonData.username,
					"and password:",
					jsonData.password,
				);
				res.send({
					status: "vm creation error",
				});
				return;
			}

			res.send({
				status: "ok",
				port: port + START_PORT,
			});
			return;
		}

		res.send({
			status: "unknown error",
		});
		return;
	} catch (error) {
		console.log("Invalid JSON data", error);
		res.send({
			status: "structure error",
		});
		return;
	}
});

app.use(express.static(__dirname));

const server = http.createServer(app);

const wss = new websocket.WebSocketServer({ server });
const MAX_PORTS = 10;
const START_PORT = 5900;
const ports = Array(MAX_PORTS).fill(0);

wss.on("connection", (ws, req) => {
	console.log("new connection");

	const url = new URL(req.url, `http://${req.headers.host}`);
	const port = url.pathname.replace("/", "");
	console.log("url:", url, "port:", port);

	const ind = port - START_PORT;
	console.log("ind:", ind);
	if (ind < 0 || ind >= ports.length || ports[ind] != 1) {
		console.log("Invalid port: %d", port);
		ports[ind] = 0;
		ws.terminate();
		return;
	}

	const spiceSocket = net.createConnection(
		{
			host: "127.0.0.1",
			port,
		},
		() => {
			console.log("Connected to QEMU Spice port!!");
		},
	);

	const wsStream = websocket.createWebSocketStream(ws, {
		binary: true,
	});

	wsStream.pipe(spiceSocket).pipe(wsStream);

	spiceSocket.on("close", () => {
		console.log("Spice connection closed.");
		ports[ind] = 0;
		ws.terminate();
	});

	ws.on("close", () => {
		console.log("Websocket connection closed.");
		ports[ind] = 0;
		spiceSocket.end();
	});

	ws.on("error", () => {
		console.log("error");
	});
});

server.listen(3000, "0.0.0.0", () => {
	console.log("server listening on port 3000");
});
