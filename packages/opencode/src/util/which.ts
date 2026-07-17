import whichPkg from "which"
import path from "path"
import { execFileSync } from "child_process"
import { existsSync } from "fs"
import { Global } from "@opencode-ai/core/global"

// kilocode_change: Windows App Execution Aliases (0-byte reparse points in WindowsApps)
// are resolvable by where.exe / SearchPathW, but can't be spawned directly by CreateProcess.
// fsutil reparsepoint query extracts the real target so the caller gets a spawnable path.
function resolveAlias(file: string): string | null {
  if (process.platform !== "win32") return null
  try {
    const out = execFileSync("fsutil", ["reparsepoint", "query", file], { windowsHide: true, timeout: 3000 })
    const lines = out.toString().split("\r\n")
    let ascii = ""
    for (const line of lines) {
      const idx = line.lastIndexOf("  ")
      if (idx === -1) continue
      const text = line.slice(idx + 2)
      for (let i = 0; i < text.length; i += 2) ascii += text[i]
    }
    const match = ascii.match(/([A-Za-z]:\\.+?\.exe)/i)
    if (!match) return null
    const target = match[1]
    if (!existsSync(target)) return null
    return target
  } catch {
    return null
  }
}

export function which(cmd: string, env?: NodeJS.ProcessEnv) {
  const base = env?.PATH ?? env?.Path ?? process.env.PATH ?? process.env.Path ?? ""
  const full = base ? base + path.delimiter + Global.Path.bin : Global.Path.bin
  const pathext = env?.PATHEXT ?? env?.PathExt ?? process.env.PATHEXT ?? process.env.PathExt
  const result = whichPkg.sync(cmd, { nothrow: true, path: full, pathExt: pathext })
  if (typeof result === "string") return result
  // kilocode_change: whichPkg uses fs.statSync which gets EACCES for Windows App Execution Aliases
  // (e.g. pwsh.exe in WindowsApps). Fall back to where.exe, then resolve alias to real target
  // so CreateProcess can spawn it directly.
  if (process.platform === "win32") {
    try {
      const whereEnv = env
        ? { ...process.env, PATH: full, Path: full, PATHEXT: pathext, PathExt: pathext }
        : void 0
      const out = execFileSync("where", [cmd], { windowsHide: true, timeout: 3000, env: whereEnv })
      const file = out.toString().split("\r\n")[0]?.trim()
      if (!file) return null
      const target = resolveAlias(file)
      return target ?? file
    } catch {
      // not found
    }
  }
  return null
}
