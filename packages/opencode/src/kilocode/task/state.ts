import { Effect } from "effect"
import * as Log from "@opencode-ai/core/util/log"
import { InstanceState } from "@/effect/instance-state"

const log = Log.create({ service: "task.state" })
const states = new Map<string, boolean>()

export const status = Effect.fn("TaskState.status")(function* () {
  const directory = yield* InstanceState.directory
  const exists = states.has(directory)
  if (!exists) {
    states.set(directory, true)
    log.info("task state initialized", { directory })
  }
  return { directory, enabled: states.get(directory)!, version: 0 }
})

export const toggle = Effect.fn("TaskState.toggle")(function* () {
  const directory = yield* InstanceState.directory
  if (!states.has(directory)) {
    states.set(directory, true)
    log.info("task state initialized", { directory })
  }
  const current = states.get(directory)!
  const next = !current
  states.set(directory, next)
  log.info("task state toggled", { directory, from: current, to: next })
  return { directory, enabled: next, version: 0 }
})
