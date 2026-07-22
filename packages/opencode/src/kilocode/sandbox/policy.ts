import { readFileSync, statSync } from "node:fs"
import os from "node:os"
import path from "node:path"
import { Effect, Semaphore } from "effect"
import { Global } from "@opencode-ai/core/global"
import * as Log from "@opencode-ai/core/util/log"
import { backendSupport, run as runSandbox, unrestricted, protectedPathsAsync, type Profile } from "@kilocode/sandbox"
import { Bus } from "@/bus"
import { Config } from "@/config/config"
import { InstanceState } from "@/effect/instance-state"
import type { InstanceContext } from "@/project/instance-context"
import type { SessionID } from "@/session/schema"
import { Changed } from "./event"
import * as Environment from "./environment"
import * as Network from "./network"
import { SandboxConfig } from "./config"

const SYMLINK_PATHS = [
  { from: "usr/bin", to: "/bin" },
  { from: "usr/lib", to: "/lib" },
  { from: "usr/lib64", to: "/lib64" },
  { from: "usr/sbin", to: "/sbin" },
  { from: "/tmp/kilo", to: "/tmp" },
]

export type Snapshot = {
  enabled: boolean
  mode: Extract<Profile["network"]["mode"], "allow" | "deny">
  version: number
  readonlyPaths: readonly string[]
  denyPaths: readonly string[]
  writablePaths: readonly string[]
}

const snapshots = new Map<string, Snapshot>()
const locks = new Map<SessionID, { semaphore: Semaphore.Semaphore; refs: number }>()

function key(directory: string, sessionID: SessionID) {
  return directory + "\0" + sessionID
}

function locked<A, E, R>(sessionID: SessionID, effect: Effect.Effect<A, E, R>) {
  return Effect.acquireUseRelease(
    Effect.sync(() => {
      const entry = locks.get(sessionID) ?? { semaphore: Semaphore.makeUnsafe(1), refs: 0 }
      entry.refs++
      locks.set(sessionID, entry)
      return entry
    }),
    (entry) => entry.semaphore.withPermits(1)(effect),
    (entry) =>
      Effect.sync(() => {
        entry.refs--
        if (entry.refs === 0 && locks.get(sessionID) === entry) locks.delete(sessionID)
      }),
  )
}

function root(path: string) {
  return { path, kind: "subtree" as const }
}

function marker(dir: string) {
  try {
    const file = path.join(dir, ".git")
    const entry = statSync(file, { throwIfNoEntry: false })
    if (!entry?.isFile()) return false
    const match = readFileSync(file, "utf8")
      .trim()
      .match(/^gitdir:\s*(.+)$/i)
    if (!match) return true
    const git = path.resolve(dir, match[1])
    if (!statSync(git, { throwIfNoEntry: false })?.isDirectory()) return true
    return statSync(path.join(git, "commondir"), { throwIfNoEntry: false })?.isFile() ?? false
  } catch {
    return true
  }
}

function linked(dir: string, stop: string): boolean {
  if (marker(dir)) return true
  if (dir === stop) return false
  const parent = path.dirname(dir)
  if (parent === dir) return false
  return linked(parent, stop)
}

function isolated(ctx: InstanceContext) {
  if (ctx.worktree === "/") return true
  return linked(path.resolve(ctx.directory), path.resolve(ctx.worktree))
}

type ScanEntry = {
  signal: { aborted: boolean }
  result: string[] | null
  promise: Promise<string[]> | null
}
const scans = new Map<string, ScanEntry>()
const scanLog = Log.create({ service: "sandbox.scan" })

function scanTargets(writable: string[], readonlyPaths?: string[], denyPaths?: string[]): string[] {
  const blocked = [...(readonlyPaths ?? []), ...(denyPaths ?? [])]
  return writable.filter((p) => !blocked.includes(p))
}

function scanKey(directory: string, writablePaths: readonly string[]): string {
  return directory + "\x00" + [...writablePaths].sort().join("\x00")
}

function displayKey(key: string): string {
  return key.replaceAll("\x00", " | ")
}

export function isScanning(directory?: string): boolean {
  for (const [key, entry] of scans) {
    if (entry.result !== null) continue
    if (!directory || key.startsWith(directory + "\x00")) return true
  }
  return false
}

export function scheduleProtectedPathScan(
  directory: string,
  writablePaths: readonly string[],
  readonlyPaths?: readonly string[],
  denyPaths?: readonly string[],
): string {
  const key = scanKey(directory, writablePaths)
  const existing = scans.get(key)
  if (existing) return key

  const targets = scanTargets(scanWritable(directory, writablePaths), readonlyPaths, denyPaths)
  if (targets.length === 0) return ""

  const signal = { aborted: false }
  const rules = targets.map((p) => ({ path: p, kind: "subtree" as const }))
  const profileLike = { filesystem: { denyWrite: [], denyNames: [".git"] } }

  scanLog.info("starting protected path scan", { key: displayKey(key), targets: targets.length })

  const promise = (async () => {
    const result = await protectedPathsAsync(profileLike as Profile, rules, signal)
    if (signal.aborted) {
      scanLog.info("scan aborted", { key: displayKey(key) })
      return []
    }
    scanLog.info("scan completed", { key: displayKey(key), found: result.length, paths: result })
    scans.set(key, { signal, promise: null, result })
    return result
  })()

  scans.set(key, { signal, promise, result: null })
  return key
}

export async function ensureProtectedScan(key: string): Promise<string[]> {
  const entry = scans.get(key)
  if (!entry) return []
  if (entry.result !== null) return entry.result
  return entry.promise ?? []
}

function scanWritable(directory: string, extra?: readonly string[]): string[] {
  return [
    directory,
    path.join(Global.Path.data, "tool-output"),
    Global.Path.state,
    Global.Path.tmp,
    ...(extra ?? []),
  ]
}

export function computeWritable(ctx: InstanceContext, extra?: readonly string[]) {
  const project = isolated(ctx)
    ? [ctx.directory]
    : ctx.directory === ctx.worktree
      ? [ctx.directory]
      : ctx.directory.toLowerCase().startsWith(ctx.worktree.toLowerCase() + path.sep)
        ? [ctx.directory]
        : [ctx.worktree, ctx.directory]
  return [
    ...project,
    path.join(Global.Path.data, "tool-output"),
    Global.Path.state,
    Global.Path.tmp,
    ...(extra ?? []),
  ]
}

export function profile(
  ctx: InstanceContext,
  mode: Profile["network"]["mode"] = "deny",
  extraWritable?: readonly string[],
  readonlyPaths?: readonly string[],
  denyPaths?: readonly string[],
): Profile {
  const raw = computeWritable(ctx, extraWritable)
  const key = scanKey(ctx.directory, extraWritable ?? [])
  const entry = scans.get(key)
  const preScanned = entry?.result ?? undefined
  const writable = raw.map(root)
  const dbFiles = [
    path.join(Global.Path.data, "kilo.db"),
    path.join(Global.Path.data, "kilo.db-shm"),
    path.join(Global.Path.data, "kilo.db-wal"),
    path.join(Global.Path.data, "session-export.db"),
    path.join(Global.Path.data, "session-export.db-shm"),
    path.join(Global.Path.data, "session-export.db-wal"),
  ]
  const kiloBinDir = path.dirname(process.execPath)
  const isWin = process.platform === "win32"
  return {
    filesystem: {
      allowWrite: writable,
      denyWrite: [],
      denyNames: [".git", ".svn"],
      temporaryDirectory: Global.Path.tmp,
      readonlyPaths: [...(isWin ? [] : ["/usr", "/etc"]), kiloBinDir, ...(readonlyPaths ?? []), ...dbFiles],
      denyPaths: [...new Set([...(denyPaths ?? (isWin ? [] : ["/home", "/root", "/var", "/opt", "/mnt", "/media", "/run", "/srv", "/boot"])), Global.Path.data, Global.Path.cache, Global.Path.config])].filter((p) => p !== "/tmp"),
      symlinkPaths: isWin ? [] : SYMLINK_PATHS,
      protectedPaths: preScanned,
    },
    network: {
      mode,
      allowedHosts: [],
    },
    environment: {
      deny: [...Environment.denied],
      set: {
        TMPDIR: Global.Path.tmp,
        TMP: Global.Path.tmp,
        TEMP: Global.Path.tmp,
      },
    },
  }
}

const read = Effect.fn("SandboxPolicy.read")(function* (directory: string, sessionID: SessionID) {
  return snapshots.get(key(directory, sessionID))
})

const snapshot = Effect.fn("SandboxPolicy.snapshot")(function* (sessionID: SessionID) {
  const directory = yield* InstanceState.directory
  const id = key(directory, sessionID)
  const current = snapshots.get(id)
  if (current) return { directory, state: current }

  const cfg = yield* (yield* Config.Service).get()
  const resolved = SandboxConfig.resolve(cfg)
  const next: Snapshot = {
    enabled: resolved.enabled,
    mode: resolved.mode,
    version: 0,
    readonlyPaths: resolved.readonlyPaths,
    denyPaths: resolved.denyPaths,
    writablePaths: resolved.writablePaths,
  }
  snapshots.set(id, next)
  return { directory, state: next }
})

export const configuredSupport = Effect.fn("SandboxPolicy.configuredSupport")(function* () {
  const cfg = yield* (yield* Config.Service).get()
  return backendSupport({ mode: SandboxConfig.resolve(cfg).mode, allowedHosts: [] })
})

export function fallback(config: Config.Info) {
  return SandboxConfig.resolve(config)
}

export const status = Effect.fn("SandboxPolicy.status")(function* (sessionID: SessionID) {
  const current = yield* snapshot(sessionID)
  const support = backendSupport({ mode: current.state.mode, allowedHosts: [] })
  return {
    directory: current.directory,
    enabled: current.state.enabled && support.available,
    available: support.available,
    reason: support.reason,
    version: current.state.version,
  }
})

function change<E, R>(sessionID: SessionID, guard: Effect.Effect<unknown, E, R>) {
  return Effect.gen(function* () {
    const directory = yield* InstanceState.directory
    return yield* locked(
      sessionID,
      Effect.gen(function* () {
        yield* guard
        const current = yield* snapshot(sessionID)
        const support = backendSupport({ mode: current.state.mode, allowedHosts: [] })
        const status = {
          directory,
          enabled: current.state.enabled && support.available,
          available: support.available,
          reason: support.reason,
          version: current.state.version,
        }
        if (!status.enabled && !status.available) return status
        const next: Snapshot = { ...current.state, enabled: !status.enabled, version: status.version + 1 }
        snapshots.set(key(directory, sessionID), next)
        const value = { ...status, enabled: next.enabled, version: next.version }
        yield* (yield* Bus.Service).publish(Changed, { sessionID, ...value })
        return value
      }),
    )
  })
}

export const toggle = Effect.fn("SandboxPolicy.toggle")((sessionID: SessionID) => change(sessionID, Effect.void))

/** Stored confinement for a session in an explicit directory, without seeding from config. */
export const peek = Effect.fn("SandboxPolicy.peek")(function* (directory: string, sessionID: SessionID) {
  return yield* read(directory, sessionID)
})

export const inherit = Effect.fn("SandboxPolicy.inherit")(function* (
  parentID: SessionID,
  sessionID: SessionID,
  fallback?: Omit<Snapshot, "version">,
) {
  const directory = yield* InstanceState.directory
  yield* locked(
    parentID,
    Effect.gen(function* () {
      const stored = yield* read(directory, parentID)
      const parent: Snapshot | undefined = stored ?? (fallback && { ...fallback, version: 0 })
      if (!parent) return
      yield* locked(
        sessionID,
        Effect.gen(function* () {
          const child = yield* read(directory, sessionID)
          const source = child ?? parent
          const next: Snapshot = child
            ? {
                enabled: parent.enabled || child.enabled,
                mode: parent.mode === "deny" || child.mode === "deny" ? "deny" : "allow",
                version: child.version + 1,
                readonlyPaths: source.readonlyPaths,
                denyPaths: source.denyPaths,
                writablePaths: source.writablePaths,
              }
            : { ...parent, version: 0 }
          if (child && child.enabled === next.enabled && child.mode === next.mode) return
          snapshots.set(key(directory, sessionID), next)
        }),
      )
    }),
  )
})

export function toggleGuarded<E, R>(sessionID: SessionID, guard: Effect.Effect<unknown, E, R>) {
  return change(sessionID, guard)
}

export function retire<A, E, R>(
  sessionID: SessionID,
  directory: string,
  effect: Effect.Effect<A, E, R>,
): Effect.Effect<A, E, R> {
  return locked(
    sessionID,
    Effect.gen(function* () {
      const result = yield* effect
      snapshots.delete(key(directory, sessionID))
      return result
    }),
  )
}

export function dispose<A, E, R>(sessionID: SessionID, effect: Effect.Effect<A, E, R>): Effect.Effect<A, E, R> {
  return locked(
    sessionID,
    Effect.gen(function* () {
      const result = yield* effect
      const suffix = "\0" + sessionID
      for (const id of snapshots.keys()) {
        if (id.endsWith(suffix)) snapshots.delete(id)
      }
      return result
    }),
  )
}

function execute<A, E, R>(sessionID: SessionID, effect: Effect.Effect<A, E, R>) {
  return Effect.gen(function* () {
    const current = yield* snapshot(sessionID)
    const cfg = yield* (yield* Config.Service).get()
    const enabled = current.state.enabled
    const mode = cfg.sandbox?.network ?? "deny"
    const support = backendSupport({ mode, allowedHosts: [] })
    const extraWritable = (cfg.sandbox?.writable_paths ?? []).map((p) =>
      p.startsWith("~") ? path.join(os.homedir(), p.slice(1)) : p,
    )
    const readonlyPaths = cfg.sandbox?.readonly_paths
    const denyPaths = cfg.sandbox?.deny_paths

    if (!enabled || !support.available) return yield* unrestricted(effect)

    const ctx = yield* InstanceState.context
    const key = scanKey(ctx.directory, extraWritable)
    if (key) {
      yield* Effect.promise(() => ensureProtectedScan(key))
    }

    return yield* runSandbox(
      profile(ctx, mode, extraWritable, readonlyPaths, denyPaths),
      effect,
    )
  })
}

export function executeTool<A, E, R>(sessionID: SessionID, tool: { id: string }, effect: Effect.Effect<A, E, R>) {
  return execute(sessionID, Network.tool(tool, effect))
}

export function executeMcp<A, E, R>(sessionID: SessionID, tool: object, effect: Effect.Effect<A, E, R>) {
  return execute(sessionID, Network.mcp(tool, effect))
}
