#!/usr/bin/env bun
import { $ } from "bun"
import fs from "fs"
import path from "path"

const VERSION = "15.1.0"
const CACHE_DIR = path.join(process.env.HOME || "/tmp", ".cache", "kilo", "rg")

function cachePath(filename: string): string {
  return path.join(CACHE_DIR, filename)
}

export async function downloadRipgrep(target: string, outputDir: string): Promise<string> {
  const rgName = target.includes("win32") ? "rg.exe" : "rg"
  const outPath = path.join(outputDir, rgName)

  if (fs.existsSync(outPath)) {
    console.log(`ripgrep already at ${outPath}`)
    return outPath
  }

  const platformMap: Record<string, string> = {
    "linux-x64": "x86_64-unknown-linux-musl",
    "linux-arm64": "aarch64-unknown-linux-gnu",
    "darwin-x64": "x86_64-apple-darwin",
    "darwin-arm64": "aarch64-apple-darwin",
    "win32-x64": "x86_64-pc-windows-msvc",
    "win32-arm64": "aarch64-pc-windows-msvc",
  }
  const platform = platformMap[target]
  if (!platform) throw new Error(`unsupported platform for ripgrep: ${target}`)

  const ext = target.startsWith("win32") ? "zip" : "tar.gz"
  const filename = `ripgrep-${VERSION}-${platform}.${ext}`
  const url = `https://github.com/BurntSushi/ripgrep/releases/download/${VERSION}/${filename}`

  fs.mkdirSync(CACHE_DIR, { recursive: true })
  fs.mkdirSync(outputDir, { recursive: true })

  const cached = cachePath(filename)
  if (!fs.existsSync(cached)) {
    console.log(`downloading ripgrep from ${url}`)
    const resp = await fetch(url)
    if (!resp.ok) throw new Error(`failed to download ripgrep: ${resp.status}`)
    const buf = await resp.arrayBuffer()
    fs.writeFileSync(cached, new Uint8Array(buf))
  }

  const tmpDir = fs.mkdtempSync(path.join(outputDir, ".rg-tmp-XXXXXX"))
  try {
    const archive = path.join(tmpDir, filename)
    fs.cpSync(cached, archive)

    if (ext === "zip") {
      await $`unzip -o ${archive} -d ${tmpDir}`.nothrow()
    } else {
      await $`tar -xzf ${archive} -C ${tmpDir}`.nothrow()
    }

    const extracted = path.join(tmpDir, `ripgrep-${VERSION}-${platform}`, rgName)
    if (!fs.existsSync(extracted)) throw new Error(`rg not found in extracted archive: ${extracted}`)

    fs.cpSync(extracted, outPath)
    fs.chmodSync(outPath, 0o755)
    console.log(`ripgrep extracted to ${outPath}`)
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true })
  }

  return outPath
}
