import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { extname, join, resolve } from "node:path";

const port = Number(process.env.PORT || 4173);
const apiBase = process.env.ROADPULSE_API || "https://09xvgayg27.execute-api.ap-south-1.amazonaws.com";
const root = resolve("dist");

const types = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png"
};

async function proxyApi(req, res) {
  const requestedPath = req.url.replace(/^\/api/, "") || "/";
  const candidates = requestedPath === "/"
    ? [process.env.ROADPULSE_API_PATH || "/", "/detections", "/events", "/potholes", "/latest", "/data"]
    : [requestedPath];

  try {
    let upstream;
    let text = "";
    for (const path of candidates) {
      const target = new URL(path, apiBase);
      upstream = await fetch(target, {
        method: req.method,
        headers: { "content-type": req.headers["content-type"] || "application/json" }
      });
      text = await upstream.text();
      if (upstream.ok) break;
    }
    res.writeHead(upstream.status, {
      "content-type": upstream.headers.get("content-type") || "application/json",
      "access-control-allow-origin": "*"
    });
    res.end(text);
  } catch (err) {
    res.writeHead(502, { "content-type": "application/json" });
    res.end(JSON.stringify({ error: "api_proxy_failed", message: err.message }));
  }
}

createServer(async (req, res) => {
  if (req.url.startsWith("/api")) return proxyApi(req, res);

  const cleanPath = req.url.split("?")[0] === "/" ? "/index.html" : req.url.split("?")[0];
  const file = join(root, cleanPath);
  const safeFile = resolve(file);
  const finalFile = safeFile.startsWith(root) && existsSync(safeFile) ? safeFile : join(root, "index.html");

  try {
    const body = await readFile(finalFile);
    res.writeHead(200, { "content-type": types[extname(finalFile)] || "application/octet-stream" });
    res.end(body);
  } catch {
    res.writeHead(404, { "content-type": "text/plain" });
    res.end("Not found");
  }
}).listen(port, () => {
  console.log(`RoadPulse dashboard running at http://127.0.0.1:${port}`);
});
