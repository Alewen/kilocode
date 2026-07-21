import { HttpApiBuilder } from "effect/unstable/httpapi"
import * as TaskState from "@/kilocode/task/state"
import { TaskApi } from "../groups/task"

export const taskHandlers = HttpApiBuilder.group(TaskApi, "task", (handlers) =>
  handlers
    .handle("status", () => TaskState.status())
    .handle("toggle", () => TaskState.toggle()),
)
