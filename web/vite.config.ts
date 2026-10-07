import { defineConfig, loadEnv } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";

// Builds into ../data, the LittleFS image source. npm run dev proxies the API
// and WebSockets to the bridge; BRIDGE_URL overrides it.
export default defineConfig(({ mode }) => {
    const bridge = loadEnv(mode, ".", "BRIDGE_").BRIDGE_URL || "http://tinkesp32.local";

    return {
        plugins: [svelte()],
        build: {
            outDir: "../data",
            emptyOutDir: true,
        },
        server: {
            proxy: {
                "/api": { target: bridge, changeOrigin: true },
                "/ws": { target: bridge, changeOrigin: true, ws: true },
            },
        },
        css: {
            preprocessorOptions: {
                scss: {
                    // Bootstrap 5.3's Sass still uses @import and legacy color
                    // functions. Drop once it moves to @use.
                    quietDeps: true,
                    silenceDeprecations: ["import", "color-functions", "global-builtin", "if-function"],
                },
            },
        },
    };
});
