import { Effect } from "effect"
import { HttpApiBuilder } from "effect/unstable/httpapi"
import * as TaskState from "@/kilocode/task/state"
import { Session } from "@/session/session"
import type { SessionID } from "@/session/schema"
import * as SessionError from "@/server/routes/instance/httpapi/handlers/session-errors"
import { TaskApi } from "../groups/task"

export const taskHandlers = HttpApiBuilder.group(TaskApi, "task", (handlers) =>
  Effect.gen(function* () {
    const session = yield* Session.Service
    const exists = (sessionID: SessionID) => SessionError.mapStorageNotFound(session.get(sessionID))
    return handlers
      .handle("status", (ctx: { params: { sessionID: SessionID } }) =>
        exists(ctx.params.sessionID).pipe(Effect.andThen(() => TaskState.status())),
      )
      .handle("toggle", () => TaskState.toggle())
  }),
)
