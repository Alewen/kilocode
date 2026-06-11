import whichPkg from "which"
import path from "path"
import { execSync } from "child_process"
import { Global } from "@opencode-ai/core/global"

export function which(cmd: string, env?: NodeJS.ProcessEnv) {
  const base = env?.PATH ?? env?.Path ?? process.env.PATH ?? process.env.Path ?? ""
  const full = base ? base + path.delimiter + Global.Path.bin : Global.Path.bin
  const result = whichPkg.sync(cmd, {
    nothrow: true,
    path: full,
    pathExt: env?.PATHEXT ?? env?.PathExt ?? process.env.PATHEXT ?? process.env.PathExt,
  })
  if (typeof result === "string") return result

  // Windows App Execution Aliases (e.g. pwsh in %LOCALAPPDATA%\Microsoft\WindowsApps)
  // are 0-byte reparse points that the `which` npm package cannot resolve.
  // Fall back to the native `where` command which uses CreateProcess resolution.
  if (process.platform === "win32") {
    try {
      const out = execSync(`where ${cmd} 2>nul`, { encoding: "utf-8", windowsHide: true, timeout: 3000 })
      const line = out.trim().split("\n")[0]?.trim()
      if (line) return line
    } catch {}
  }

  return null
}
