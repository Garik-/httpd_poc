import { readFileSync, writeFileSync, statSync } from 'node:fs'
import { brotliCompressSync, constants } from 'node:zlib'
import { resolve } from 'node:path'

const file = resolve('dist/index.html')
const input = readFileSync(file)
const originalSize = statSync(file).size

const brotli = brotliCompressSync(input, {
  params: {
    [constants.BROTLI_PARAM_QUALITY]: 11
  }
})
const brotliFile = file + '.br'
writeFileSync(brotliFile, brotli)
const brotliSize = brotli.length

const ratio = (orig, compressed) =>
  (((orig - compressed) / orig) * 100).toFixed(2) + ' %'

console.log('✔ brotli generated')

console.table({
  original: {
    size: originalSize,
  },
  brotli: {
    size: brotliSize,
    compression: ratio(originalSize, brotliSize)
  }
})
