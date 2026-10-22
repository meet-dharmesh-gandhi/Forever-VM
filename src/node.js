const dotenv = require("dotenv");
const ws = require("ws");
const http = require("http");
const cG = require("./build/Release/node.node");

dotenv.config();

console.log("Server starting...", cG.ok);

const server = http.createServer((req, res) => {});

const wss = new ws.WebSocketServer({ server });

wss.on("connection", (ws) => {
	console.log("new connection");

	ws.on("message", (message) => {
		console.log("message");
	});

	ws.on("close", () => {
		console.log("close");
	});
	ws.on("error", () => {
		console.log("error");
	});
});

server.listen(3000, "0.0.0.0", () => {
	console.log("server listening on port 3000");
});
