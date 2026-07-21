import { Schema } from "effect"
import { HttpApi, HttpApiEndpoint, HttpApiGroup, OpenApi } from "effect/unstable/httpapi"
import { Authorization } from "@/server/routes/instance/httpapi/middleware/authorization"
import { InstanceContextMiddleware } from "@/server/routes/instance/httpapi/middleware/instance-context"
import {
  WorkspaceRoutingMiddleware,
  WorkspaceRoutingQuery,
} from "@/server/routes/instance/httpapi/middleware/workspace-routing"
import { described } from "@/server/routes/instance/httpapi/groups/metadata"

const root = "/task"

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
          query: WorkspaceRoutingQuery,
          success: described(TaskStatus, "Task status"),
        }).annotateMerge(
          OpenApi.annotations({
            identifier: "task.status",
            summary: "Get task status",
            description: "Get the task enabled state for a workspace directory.",
          }),
        ),
        HttpApiEndpoint.post("toggle", `${root}/toggle`, {
          query: WorkspaceRoutingQuery,
          success: described(TaskStatus, "Toggled task status"),
        }).annotateMerge(
          OpenApi.annotations({
            identifier: "task.toggle",
            summary: "Toggle task",
            description: "Toggle the task enabled state for a workspace directory.",
          }),
        ),
      )
      .annotateMerge(OpenApi.annotations({ title: "task", description: "Kilo task routes." }))
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
