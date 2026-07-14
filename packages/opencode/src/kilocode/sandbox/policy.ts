import { readFileSync, statSync } from "node:fs"
import os from "node:os"
import path from "node:path"
import { Effect, Semaphore } from "effect"
import { Global } from "@opencode-ai/core/global"
import { backendSupport, run as runSandbox, unrestricted, type Profile } from "@kilocode/sandbox"
import { Bus } from "@/bus"
import { Config } from "@/config/config"
import { InstanceState } from "@/effect/instance-state"
import type { InstanceContext } from "@/project/instance-context"
import type { SessionID } from "@/session/schema"
import { Changed } from "./event"
import * as Network from "./network"
import { SandboxConfig } from "./config"

export type Snapshot = {
  enabled: boolean
  mode: Extract<Profile["network"]["mode"], "allow" | "deny">
  version: number
  readonlyPaths: readonly string[]
  denyPaths: readonly string[]
  symlinkPaths: readonly { from: string; to: string }[]
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

export function profile(
  ctx: InstanceContext,
  mode: Profile["network"]["mode"] = "deny",
  extraWritable?: readonly string[],
  readonlyPaths?: readonly string[],
  denyPaths?: readonly string[],
  symlinkPaths?: readonly { from: string; to: string }[],
): Profile {
  const project = isolated(ctx)
    ? [ctx.directory]
    : ctx.directory === ctx.worktree
      ? [ctx.directory]
      : [ctx.worktree, ctx.directory]
  const writable = [
    ...project,
    Global.Path.state,
    Global.Path.tmp,
    ...(extraWritable ?? []),
  ].map(root)
  const dbFiles = [
    path.join(Global.Path.data, "kilo.db"),
    path.join(Global.Path.data, "kilo.db-shm"),
    path.join(Global.Path.data, "kilo.db-wal"),
    path.join(Global.Path.data, "session-export.db"),
    path.join(Global.Path.data, "session-export.db-shm"),
    path.join(Global.Path.data, "session-export.db-wal"),
  ]
  return {
    filesystem: {
      allowWrite: writable,
      denyWrite: [],
      denyNames: [".git"],
      temporaryDirectory: Global.Path.tmp,
      readonlyPaths: [...(readonlyPaths ?? ["/usr", "/etc"]), ...dbFiles],
      denyPaths: [...new Set([...(denyPaths ?? ["/home", "/tmp", "/root", "/var", "/opt", "/mnt", "/media", "/run", "/srv", "/boot"]), Global.Path.data, Global.Path.cache, Global.Path.config])],
      symlinkPaths: symlinkPaths ?? [
        { from: "usr/bin", to: "/bin" },
        { from: "usr/lib", to: "/lib" },
        { from: "usr/lib64", to: "/lib64" },
        { from: "usr/sbin", to: "/sbin" },
      ],
    },
    network: {
      mode,
      allowedHosts: [],
    },
    environment: {
      deny: ["KILO_SERVER_PASSWORD", "KILO_SERVER_USERNAME"],
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
    symlinkPaths: resolved.symlinkPaths,
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
                symlinkPaths: source.symlinkPaths,
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
    const enabled = cfg.sandbox?.enabled ?? false
    const mode = cfg.sandbox?.network ?? "deny"
    const support = backendSupport({ mode, allowedHosts: [] })
    if (!enabled || !support.available) return yield* unrestricted(effect)
    const extraWritable = current.state.writablePaths.map((p) =>
      p.startsWith("~") ? path.join(os.homedir(), p.slice(1)) : p,
    )
    return yield* runSandbox(
      profile(
        yield* InstanceState.context,
        mode,
        extraWritable,
        current.state.readonlyPaths,
        current.state.denyPaths,
        current.state.symlinkPaths,
      ),
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
