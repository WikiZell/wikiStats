import { defineConfig } from "vite";

// The output goes onto a 256 KiB LittleFS partition and is served by an ESP32, so
// the build is tuned for "few files, small files" rather than for HTTP/2 parallelism:
//
//  * fixed asset names (app.js / app.css) - no content hashes, because the panel has
//    no CDN in front of it and hashed names would leave orphans on the filesystem
//    after every update;
//  * everything in one chunk - three round trips to an ESP32 cost more than one
//    slightly larger file;
//  * inline anything under 8 KiB, so there is no third request for a small asset.
export default defineConfig({
  base: "/",
  build: {
    target: "es2020",
    outDir: "dist",
    emptyOutDir: true,
    assetsInlineLimit: 8192,
    cssCodeSplit: false,
    modulePreload: { polyfill: false },
    reportCompressedSize: true,
    rollupOptions: {
      output: {
        entryFileNames: "app.js",
        chunkFileNames: "app.js",
        assetFileNames: (info) =>
          info.name && info.name.endsWith(".css") ? "app.css" : "[name][extname]",
        manualChunks: undefined,
      },
    },
  },
  server: {
    port: 5173,
    proxy: {
      // `npm run dev` against a real panel: point at its address.
      "/api": {
        target: process.env.WIKISTATS_PANEL ?? "http://wikistats-XXXX.local",
        changeOrigin: true,
      },
    },
  },
});
