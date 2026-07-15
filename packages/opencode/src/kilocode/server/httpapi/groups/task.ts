import { Schema } from "effect"
import { HttpApi, HttpApiEndpoint, HttpApiGroup, OpenApi } from "effect/unstable/httpapi"
import { SessionID } from "@/session/schema"
import { Authorization } from "@/server/routes/instance/httpapi/middleware/authorization"
import { InstanceContextMiddleware } from "@/server/routes/instance/httpapi/middleware/instance-context"
import {
  WorkspaceRoutingMiddleware,
  WorkspaceRoutingQuery,
} from "@/server/routes/instance/httpapi/middleware/workspace-routing"
import { described } from "@/server/routes/instance/httpapi/groups/metadata"
import { ApiNotFoundError } from "@/server/routes/instance/httpapi/errors"

const root = "/session/:sessionID/task"

export const TaskStatus = Schema.Struct({
  directory: Schema.String,
  enabled: Schema.Boolean,
  version: Schema.Int,
})

export const TaskApi = HttpApi.make("task")
  .add(
    HttpApiGroup.make("task")
      .add(
        HttpApiEndpoint.get("status", root, {
          params: { sessionID: SessionID },
          query: WorkspaceRoutingQuery,
          success: described(TaskStatus, "Session task status"),
          error: ApiNotFoundError,
        }).annotateMerge(
          OpenApi.annotations({
            identifier: "task.status",
            summary: "Get session task status",
            description: "Get the task enabled state for one session.",
          }),
        ),
        HttpApiEndpoint.post("toggle", `${root}/toggle`, {
          params: { sessionID: SessionID },
          query: WorkspaceRoutingQuery,
          success: described(TaskStatus, "Updated session task status"),
          error: ApiNotFoundError,
        }).annotateMerge(
          OpenApi.annotations({
            identifier: "task.toggle",
            summary: "Toggle session task",
            description: "Toggle the task enabled state for one session.",
          }),
        ),
      )
      .annotateMerge(OpenApi.annotations({ title: "task", description: "Kilo session task routes." }))
      .middleware(InstanceContextMiddleware)
      .middleware(WorkspaceRoutingMiddleware)
      .middleware(Authorization),
  )
  .annotateMerge(
    OpenApi.annotations({
      title: "kilo HttpApi",
      version: "0.0.1",
      description: "Kilo HttpApi surface.",
    }),
  )
