import tailwindcss from '@tailwindcss/vite'
import react from '@vitejs/plugin-react'
import path from 'node:path'
import { defineConfig } from 'vite'

// Dev: vite on :5173 proxies API + frames to the ingest service (:8078).
// Prod: `npm run build` -> webui/dist, which FastAPI serves at /.
export default defineConfig({
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: { '@': path.resolve(import.meta.dirname, './src') },
  },
  server: {
    proxy: {
      '/api': 'http://localhost:8077',
      '/files': 'http://localhost:8077',
      '/upload': 'http://localhost:8077',
      '/export.csv': 'http://localhost:8077',
    },
  },
})
