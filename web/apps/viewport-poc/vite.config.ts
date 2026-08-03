import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  base: "./", // relative paths so the built index.html works from any directory (WebView2's file:// or app.local virtual host)
  build: { outDir: "dist", emptyOutDir: true },
});
