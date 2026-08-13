import { defineConfig } from "vite";

// Dev-server proxy so the dashboard can use plain relative /api paths in
// both dev and the built static output (served alongside the backend in
// production - see PLAN.md §9 step 7). Default backend port per
// deploy/docker-compose.yml; override with BACKEND_URL if running the
// backend elsewhere.
const backendUrl = process.env.BACKEND_URL ?? "http://localhost:8080";

export default defineConfig({
  server: {
    proxy: {
      "/api": {
        target: backendUrl,
        ws: true,
      },
    },
  },
});
