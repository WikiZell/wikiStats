// Packs the Vite output into the firmware's LittleFS image directory.
//
// Every file is gzipped at maximum compression and written as `<name>.gz`.
// ESPAsyncWebServer's static handler prefers a `.gz` sibling automatically and sets
// Content-Encoding, so the panel serves ~4x less data and the 256 KiB partition has
// room to spare.
//
// The uncompressed originals are deliberately not copied: keeping both would double
// the flash cost for no benefit.

import { createReadStream, createWriteStream } from "node:fs";
import { mkdir, readdir, rm, stat } from "node:fs/promises";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { pipeline } from "node:stream/promises";
import { createGzip } from "node:zlib";

const here = dirname(fileURLToPath(import.meta.url));
const distDir = resolve(here, "..", "dist");
const targetDir = resolve(here, "..", "..", "firmware", "data", "www");

// Anything already gzipped or already tiny gains nothing from another pass.
const SKIP = new Set([".gz", ".br", ".png", ".jpg", ".webp", ".woff2"]);

async function* walk(dir) {
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) {
      yield* walk(full);
    } else {
      yield full;
    }
  }
}

function extensionOf(path) {
  const index = path.lastIndexOf(".");
  return index < 0 ? "" : path.slice(index).toLowerCase();
}

async function main() {
  try {
    await stat(distDir);
  } catch {
    console.error(`No build output at ${distDir}. Run "npm run build" first.`);
    process.exit(1);
  }

  await rm(targetDir, { recursive: true, force: true });
  await mkdir(targetDir, { recursive: true });

  let files = 0;
  let raw = 0;
  let packed = 0;

  for await (const source of walk(distDir)) {
    const rel = relative(distDir, source);
    const destination = join(targetDir, rel);
    await mkdir(dirname(destination), { recursive: true });

    const info = await stat(source);
    raw += info.size;

    if (SKIP.has(extensionOf(source))) {
      await pipeline(createReadStream(source), createWriteStream(destination));
      packed += info.size;
    } else {
      const gz = `${destination}.gz`;
      await pipeline(createReadStream(source), createGzip({ level: 9 }), createWriteStream(gz));
      packed += (await stat(gz)).size;
    }
    files += 1;
  }

  const percent = raw > 0 ? Math.round((packed / raw) * 100) : 0;
  console.log(
    `packed ${files} file(s) into ${relative(process.cwd(), targetDir)}: ` +
      `${(raw / 1024).toFixed(1)} KiB -> ${(packed / 1024).toFixed(1)} KiB (${percent}%)`,
  );
  if (packed > 200 * 1024) {
    // The littlefs partition is 256 KiB; leave headroom for config.json and its
    // backup copy or an update will fail at the worst possible moment.
    console.warn("WARNING: the asset bundle is over 200 KiB and may not fit alongside the config.");
    process.exitCode = 1;
  }
}

await main();
