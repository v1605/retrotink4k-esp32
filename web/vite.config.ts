import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";

// Builds into ../data, the LittleFS image source.
export default defineConfig({
    plugins: [svelte()],
    build: {
        outDir: "../data",
        emptyOutDir: true,
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
});
