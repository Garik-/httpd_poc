import { defineConfig } from "vite"
import { viteSingleFile } from "vite-plugin-singlefile"
import simpleHtmlPlugin from 'vite-plugin-simple-html';

export default defineConfig({
	plugins: [
        viteSingleFile(),
        simpleHtmlPlugin({
      minify: true,
    }),],
    build: {
    cssCodeSplit: false,
    rollupOptions: {
      output: {
        inlineDynamicImports: true
      }
    }
  }
})