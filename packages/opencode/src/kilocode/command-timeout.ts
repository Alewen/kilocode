import { Flag } from "@opencode-ai/core/flag/flag"
import { Process } from "@/util/process"
import { Shell } from "@/shell/shell"
import { Effect, Fiber, Stream } from "effect"
import { ChildProcess, ChildProcessSpawner } from "effect/unstable/process"

export namespace CommandTimeout {
  export type Limit = {
    timeout: number
    capped: boolean
  }

  export function clamp(timeout: number): Limit {
    const cap = Flag.KILO_COMMAND_TIMEOUT_MAX_MS
    if (!cap || timeout < cap) return { timeout, capped: false }
    return { timeout: cap, capped: true }
  }

  export function env(): Limit | undefined {
    const cap = Flag.KILO_COMMAND_TIMEOUT_MAX_MS
    if (!cap) return
    return { timeout: cap, capped: true }
  }

  export function note(limit: Limit, text: string) {
    const msg = Flag.KILO_COMMAND_TIMEOUT_MAX_MS_MESSAGE?.trim()
    const base = `${text} after exceeding environment timeout ${limit.timeout} ms.`
    return msg ? `${base} ${msg}` : base
  }

  function make(cmd: string, shell: string) {
    if (process.platform === "win32" && Shell.ps(shell)) {
      return ChildProcess.make(shell, ["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", cmd], {
        stdin: "ignore",
        detached: false,
      })
    }

    return ChildProcess.make(cmd, [], {
      shell,
      stdin: "ignore",
      detached: process.platform !== "win32",
    })
  }

  export function text(cmd: string, shell: string) {
    const limit = env()
    if (!limit) return Effect.promise(async () => (await Process.text([cmd], { shell, nothrow: true })).text)

    return Effect.gen(function* () {
      const spawner = yield* ChildProcessSpawner.ChildProcessSpawner
      const handle = yield* spawner.spawn(make(cmd, shell))
      let text = ""
      const drain = Effect.all(
        [
          Stream.runForEach(Stream.decodeText(handle.stdout), (chunk) =>
            Effect.sync(() => {
              text += chunk
            }),
          ),
          Stream.runDrain(handle.stderr),
        ],
        { concurrency: 2 },
      )
      const reader = yield* drain.pipe(Effect.forkScoped)
      const state = yield* Effect.raceAll([
        handle.exitCode.pipe(Effect.as("exit" as const)),
        Effect.sleep(`${limit.timeout + 100} millis`).pipe(Effect.as("timeout" as const)),
      ])
      if (state !== "timeout") {
        yield* Fiber.join(reader)
        return text
      }

      yield* handle.kill({ forceKillAfter: "3 seconds" }).pipe(Effect.orDie)
      const note = CommandTimeout.note(limit, "shell command terminated")
      return text ? `${text}\n\n${note}` : note
    }).pipe(Effect.scoped, Effect.orDie)
  }
}
