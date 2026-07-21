import { existsSync, mkdirSync } from "node:fs"
import path from "node:path"
import { Effect, PlatformError } from "effect"
import type { Backend, Launch, Support } from "./backend"
import type { Profile } from "./profile"

function executable(): string | undefined {
  if (process.env.KILO_BWRAP_PATH) return process.env.KILO_BWRAP_PATH
  const bundled = path.join(path.dirname(process.execPath), "bwrap.exe")
  if (existsSync(bundled)) return bundled
  return undefined
}

function command(launch: Launch): [string, ...string[]] {
  if (!launch.shell) return [launch.command, ...launch.args]
  const cmd = [launch.command, ...launch.args].join(" ")
  if (typeof launch.shell === "string") {
    const lower = launch.shell.toLowerCase()
    if (lower.endsWith("cmd.exe") || lower === "cmd") return ["cmd.exe", "/d", "/s", "/c", cmd]
    return [launch.shell, "-c", cmd]
  }
  return ["cmd.exe", "/d", "/s", "/c", cmd]
}

function depth(p: string) {
  return p.split(/[\\/]/).length
}

type Entry = { depth: number; args: string[] }

export function generate(profile: Profile, launch: Launch, exe: string): Launch {
  const entries: Entry[] = []

  for (const p of profile.filesystem.denyPaths ?? []) {
    entries.push({ depth: depth(p), args: ["--tmpfs", p] })
  }

  for (const p of profile.filesystem.readonlyPaths ?? []) {
    entries.push({ depth: depth(p), args: ["--ro-bind", p] })
  }

  for (const rule of profile.filesystem.allowWrite) {
    entries.push({ depth: depth(rule.path), args: ["--bind", rule.path] })
  }

  for (const rule of profile.filesystem.denyWrite) {
    entries.push({ depth: depth(rule.path), args: ["--ro-bind", rule.path] })
  }

  entries.sort((a, b) => a.depth - b.depth)

  const args = entries.flatMap((e) => e.args)
  if (launch.cwd) args.push("--cwd", launch.cwd)
  const ses = launch.environment?.KILO_SESSION_ID
  if (ses) args.push("--ses", ses)
  args.push("--", ...command(launch))

  return {
    ...launch,
    command: exe,
    args,
  }
}

function probe(exe: string): Support {
  if (!existsSync(exe)) return { available: false, reason: `${exe} is not available` }
  return { available: true }
}

let cached: Support | undefined

function support(): Support {
  if (cached) return cached
  const exe = executable()
  if (!exe) {
    cached = { available: false, reason: "bwrap.exe is not available" }
    return cached
  }
  cached = probe(exe)
  return cached
}

function failure(cause: unknown, launch: Launch) {
  return PlatformError.systemError({
    _tag: "PermissionDenied",
    module: "Sandbox",
    method: "prepareCommand",
    pathOrDescriptor: launch.command,
    description: cause instanceof Error ? cause.message : "Could not construct the Windows sandbox",
    cause,
  })
}

export const winbwrap: Backend = {
  support: () => support(),
  prepare: (profile, launch) =>
    Effect.try({
      try: () => {
        const tmp = profile.filesystem.temporaryDirectory
        if (tmp) mkdirSync(tmp, { recursive: true })
        const exe = executable()
        return exe ? generate(profile, launch, exe) : launch
      },
      catch: (cause) => failure(cause, launch),
    }),
}
