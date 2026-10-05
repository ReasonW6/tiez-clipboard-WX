import { defineConfig } from "vitest/config";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  test: {
    include: ["src/**/*.test.{ts,tsx}"],
    css: { include: [/\/styles\/themes\/[^/?]+\.css\?raw$/] }
  }
});
