// kilocode_change - new file
import path from "path"
import fs from "fs/promises"
import { StringDecoder } from "string_decoder"
import { Cause, Effect, Exit } from "effect"
import { AppFileSystem } from "@opencode-ai/core/filesystem"
import { InstanceState } from "@/effect/instance-state"
import { SessionID, PartID } from "@/session/schema"
import { MessageV2 } from "@/session/message-v2"
import { Session } from "@/session/session"
import { Instance } from "@/project/instance"
import type { SessionStatus } from "@/session/status"
import { Flag } from "@opencode-ai/core/flag/flag"
import { PlanFollowup } from "@/kilocode/plan-followup"
import { KiloSession } from "@/kilocode/session"
import { Permission } from "@/permission"
import { environmentDetails, type EditorContext } from "@/kilocode/editor-context"
import { Identifier } from "@/id/id"
import { Filesystem } from "@/util/filesystem"
import PROMPT_PLAN from "@/session/prompt/plan.txt"
import CODE_SWITCH from "@/session/prompt/code-switch.txt"

export namespace KiloSessionPrompt {
  const modes = ["ask", "plan"]

  /**
   * Determines whether the plan follow-up prompt should be shown.
   * Checks if the plan_exit tool was called in the last assistant turn.
   * Exported so tests can verify the logic independently.
   */
  export function shouldAskPlanFollowup(input: { messages: MessageV2.WithParts[]; abort: AbortSignal }) {
    if (input.abort.aborted) return false
    if (!["cli", "vscode", "jetbrains"].includes(Flag.KILO_CLIENT)) return false
    const idx = input.messages.findLastIndex((m) => m.info.role === "user")
    return input.messages
      .slice(idx + 1)
      .some((msg) =>
        msg.parts.some((p) => p.type === "tool" && p.tool === "plan_exit" && p.state.status === "completed"),
      )
  }

  /**
   * Checks for plan follow-up and asks the user if needed.
   * Returns "continue" if the loop should continue, "break" otherwise.
   */
  export async function askPlanFollowup(input: {
    sessionID: SessionID
    messages: MessageV2.WithParts[]
    abort: AbortSignal
  }): Promise<"continue" | "break"> {
    if (!shouldAskPlanFollowup({ messages: input.messages, abort: input.abort })) return "break"
    const action = await PlanFollowup.ask({
      sessionID: input.sessionID,
      messages: input.messages,
      abort: input.abort,
    })
    return action === "continue" ? "continue" : "break"
  }

  export function abortPlanFollowup(sessionID: SessionID) {
    return PlanFollowup.abort(sessionID)
  }

  export const recoverDanglingAssistant = Effect.fn("KiloSessionPrompt.recoverDanglingAssistant")(function* (input: {
    sessionID: SessionID
    status: Pick<SessionStatus.Interface, "get">
    sessions: Pick<Session.Interface, "messages" | "removeMessage">
  }) {
    const state = yield* input.status.get(input.sessionID)
    if (state.type !== "idle") return

    const msgs = yield* input.sessions.messages({ sessionID: input.sessionID, limit: 2 })
    const tail = msgs.at(-1)
    if (!tail || tail.info.role !== "assistant") return
    if (tail.parts.length > 0 || tail.info.finish || tail.info.error) return

    const prev = msgs.at(-2)
    if (!prev || prev.info.role !== "user") return
    if (tail.info.parentID !== prev.info.id) return

    yield* input.sessions.removeMessage({ sessionID: input.sessionID, messageID: tail.info.id })
  })

  export const recoverProviderFinishError = Effect.fn("KiloSessionPrompt.recoverProviderFinishError")(
    function* (input: {
      sessionID: SessionID
      status: Pick<SessionStatus.Interface, "get">
      sessions: Pick<Session.Interface, "messages" | "removeMessage">
    }) {
      const state = yield* input.status.get(input.sessionID)
      if (state.type !== "idle") return

      const msgs = yield* input.sessions.messages({ sessionID: input.sessionID, limit: 2 })
      const tail = msgs.at(-1)
      if (!tail || tail.info.role !== "assistant") return
      if (tail.info.finish !== "error" || tail.info.error) return
      if (!tail.parts.some((part) => part.type === "step-finish" && part.reason === "error")) return

      const prev = msgs.at(-2)
      if (!prev || prev.info.role !== "user") return
      if (tail.info.parentID !== prev.info.id) return

      yield* input.sessions.removeMessage({ sessionID: input.sessionID, messageID: tail.info.id })
    },
  )

  export function guardPermissions(input: {
    agent: { name: string; permission: Permission.Ruleset }
    session: Pick<Session.Info, "permission">
  }) {
    const rules = input.session.permission ?? []
    if (!modes.includes(input.agent.name)) return rules
    return Permission.merge(
      rules,
      input.agent.permission,
      rules.filter((rule) => rule.action === "deny"),
    )
  }

  export function hardPermissions(input: { agent: { name: string; permission: Permission.Ruleset } }) {
    if (!modes.includes(input.agent.name)) return
    return input.agent.permission
  }

  /**
   * Mutable cache for environment details, keyed by user message ID
   * so it recomputes when a new user message arrives.
   */
  export interface EnvCache {
    block?: string
    user?: string
  }

  /**
   * Ephemerally injects dynamic editor context (visible files, open tabs, etc.)
   * into the last user message. Caches the result per user message ID so repeated
   * loop iterations produce byte-identical messages (prompt caching).
   */
  export function injectEditorContext(input: {
    msgs: MessageV2.WithParts[]
    lastUser: MessageV2.User
    sessionID: SessionID
    cache: EnvCache
  }) {
    if (input.cache.user !== input.lastUser.id) {
      const ctx = (() => {
        try {
          return Instance.current
        } catch {
          return undefined
        }
      })()
      input.cache.block = environmentDetails({
        ...input.lastUser.editorContext,
        ...(ctx ? { directory: ctx.directory, worktree: ctx.worktree } : {}),
      })
      input.cache.user = input.lastUser.id
    }
    if (!input.cache.block) return
    const idx = input.msgs.findLastIndex((m) => m.info.role === "user")
    if (idx === -1) return
    input.msgs[idx] = {
      ...input.msgs[idx],
      parts: [
        ...input.msgs[idx].parts,
        {
          id: PartID.make(Identifier.ascending("part")),
          sessionID: input.sessionID,
          messageID: input.msgs[idx].info.id,
          type: "text",
          text: input.cache.block,
          synthetic: true,
        } satisfies MessageV2.TextPart,
      ],
    }
  }

  /**
   * Creates StringDecoder-based helpers for shell stdout/stderr that correctly
   * handle multi-byte UTF-8 characters split across chunks.
   */
  export function createShellDecoders() {
    const stdout = new StringDecoder("utf8")
    const stderr = new StringDecoder("utf8")
    return {
      /** Decode a chunk from the given stream. */
      write(stream: "stdout" | "stderr", chunk: Buffer) {
        return stream === "stdout" ? stdout.write(chunk) : stderr.write(chunk)
      },
      /** Flush any trailing buffered bytes from both decoders. */
      flush() {
        return stdout.end() + stderr.end()
      },
    }
  }

  /**
   * Ensures the plan file directory exists. Pre-checks with `Filesystem.isDir`
   * because `fs.mkdir(recursive: true)` still throws `EEXIST` on Windows
   * OneDrive ReparsePoint directories in some Node versions (kilocode#9755).
   */
  export async function ensurePlanDir(dir: string) {
    if (await Filesystem.isDir(dir)) return
    await fs.mkdir(dir, { recursive: true })
  }

  /**
   * Injects plan-specific reminders into the user message when using the plan agent.
   * Ensures the plan file directory exists and tells the agent where to write.
   */
  export async function insertPlanReminders(input: {
    agent: { name: string }
    session: Session.Info
    userMessage: MessageV2.WithParts
  }) {
    if (input.agent.name !== "plan") return
    const plan = Session.plan(input.session, Instance.current)
    const exists = await Filesystem.exists(plan)
    if (!exists) await ensurePlanDir(path.dirname(plan))
    const info = exists
      ? `A plan file already exists at ${plan}. You can read it and make incremental edits using the edit tool.`
      : `No plan file exists yet. You should create your plan at ${plan} using the write tool.`
    input.userMessage.parts.push({
      id: PartID.ascending(),
      messageID: input.userMessage.info.id,
      sessionID: input.userMessage.info.sessionID,
      type: "text",
      text: PROMPT_PLAN + `\n\n## Plan File\n${info}\nThis is the ONLY file you are allowed to write to or edit.`,
      synthetic: true,
    })
  }

  /**
   * Returns the CODE_SWITCH prompt text (plan-to-code transition).
   * Used when switching from plan agent to code agent.
   */
  export const CODE_SWITCH_TEXT = CODE_SWITCH

  /**
   * Determines the close reason for a session turn.
   * Checks for an explicit reason first (e.g. set on error during runLoop),
   * then falls back to inspecting the Effect exit value.
   */
  export function resolveCloseReason(input: {
    sessionID: string
    closeReasons: Map<string, KiloSession.CloseReason>
    exit: Exit.Exit<any, any>
  }): KiloSession.CloseReason {
    const explicit = input.closeReasons.get(input.sessionID)
    input.closeReasons.delete(input.sessionID)
    if (explicit) return explicit
    if (Exit.isFailure(input.exit)) {
      return Cause.hasInterruptsOnly(input.exit.cause) ? "interrupted" : "error"
    }
    return "completed"
  }

  /**
   * Maximum number of compactions attempted within a single turn before we
   * surface an exhaustion error. Three is enough to cover a normal overflow
   * compaction plus a summary-self-overflow retry without spinning forever.
   */
  export const MAX_COMPACTION_ATTEMPTS = 3

  /**
   * Guards a compaction attempt. When the attempt count has already reached
   * `MAX_COMPACTION_ATTEMPTS`, marks the close reason as `"error"`, attaches a
   * `ContextOverflowError` to the assistant message (if provided), and returns
   * `{ exhausted: true }` so callers can break out of the loop. Otherwise
   * returns `{ exhausted: false }`.
   */
  export function guardCompactionAttempt(input: {
    sessionID: string
    attempts: number
    closeReasons: Map<string, KiloSession.CloseReason>
    message?: MessageV2.Assistant
  }) {
    if (input.attempts < MAX_COMPACTION_ATTEMPTS) return { exhausted: false as const }
    const error = new MessageV2.ContextOverflowError({
      message: `Compaction exhausted: context still exceeds model limits after ${MAX_COMPACTION_ATTEMPTS} attempts`,
    }).toObject()
    input.closeReasons.set(input.sessionID, "error")
    if (input.message) {
      // Preserve any pre-existing error/finish the caller already set; only fill in blanks.
      input.message.error ??= error
      input.message.finish ??= "error"
    }
    return { exhausted: true as const, error }
  }

  /**
   * Returns true when `msgs` contains at least one completed, error-free summary
   * assistant.
   */
  export function hasCompletedSummary(msgs: MessageV2.WithParts[]): boolean {
    return msgs.some((m) => m.info.role === "assistant" && m.info.summary === true && !!m.info.finish && !m.info.error)
  }

  /**
   * Returns a possibly-trimmed copy of `msgs` where everything earlier than the
   * newest completed summary's parent user message is dropped. Idempotent — a
   * second call on the already-trimmed list is a no-op.
   *
   * Complements the shared `MessageV2.filterCompacted`, which only breaks when
   * the summary's parent has a `compaction` part. Manual `/compact` and auto-
   * compactions dispatched against a plain text user produce summaries whose
   * parent is a text user; `filterCompacted` keeps the full pre-summary history
   * in that case, which is how the reference session ended up re-shipping
   * multi-MB base-64 images on every turn.
   *
   * If no completed summary is found, or the summary's parent is absent from
   * `msgs`, `msgs` is returned unchanged.
   */
  export function trimBeforeLastSummary(msgs: MessageV2.WithParts[]): MessageV2.WithParts[] {
    for (let i = msgs.length - 1; i >= 0; i--) {
      const info = msgs[i].info
      if (info.role !== "assistant" || info.summary !== true || !info.finish || info.error) continue
      const parentIdx = msgs.findIndex((m) => m.info.id === info.parentID)
      if (parentIdx === -1) return msgs
      return parentIdx === 0 ? msgs : msgs.slice(parentIdx)
    }
    return msgs
  }

  /**
   * Returns a shallow-modified copy of `msgs` where every message before the
   * last real user turn has its media stripped:
   *   - `file` parts with an image/PDF MIME become placeholder `text` parts
   *     (same placeholder shape as `toModelMessagesEffect({ stripMedia: true })`).
   *   - Completed assistant `tool` parts keep their non-media attachments but
   *     drop image/PDF attachments.
   *
   * The cutoff anchors on the newest user message that carries at least one
   * non-synthetic part. Synthetic-only user turns — e.g. the `"Summarize the
   * task tool output above…"` message emitted by `handleSubtask` when a task
   * command continues a turn, or the auto-compaction continue prompt in
   * `compaction.process` — do not count as the current turn, so attachments
   * the user just sent before that handoff are preserved.
   *
   * Media in and after the cutoff is left alone so the model can still
   * analyse attachments the user just sent. Shallow copies only — input is
   * never mutated.
   */
  export function stripHistoricalMedia(msgs: MessageV2.WithParts[]): MessageV2.WithParts[] {
    const cutoff = msgs.findLastIndex(
      (m) => m.info.role === "user" && m.parts.some((p) => p.type !== "text" || !p.synthetic),
    )
    if (cutoff <= 0) return msgs
    return msgs.map((msg, idx) => {
      if (idx >= cutoff) return msg
      const parts = msg.parts.map((part) => {
        if (part.type === "file" && MessageV2.isMedia(part.mime)) {
          return {
            id: part.id,
            sessionID: part.sessionID,
            messageID: part.messageID,
            type: "text" as const,
            text: `[Attached ${part.mime}: ${part.filename ?? "file"}]`,
          } satisfies MessageV2.TextPart
        }
        if (part.type === "tool" && part.state.status === "completed" && part.state.attachments?.length) {
          const kept = part.state.attachments.filter((a) => !MessageV2.isMedia(a.mime))
          if (kept.length === part.state.attachments.length) return part
          return { ...part, state: { ...part.state, attachments: kept } }
        }
        return part
      })
      return { ...msg, parts }
    })
  }

  /**
   * Convenience wrapper: calls `stripHistoricalMedia` only when `msgs` contains
   * a completed summary. Keeps the main-prompt call site to a single line.
   */
  export function maybeStripHistoricalMedia(msgs: MessageV2.WithParts[]): MessageV2.WithParts[] {
    return hasCompletedSummary(msgs) ? stripHistoricalMedia(msgs) : msgs
  }

  /**
   * Guards tool execution by checking if file operations are within workspace.
   */
  export const guardToolExecution = Effect.fn("KiloSessionPrompt.guardToolExecution")(function* (input: {
    toolName: string
    args: any
  }) {
    const instance = yield* InstanceState.context

    // Check write/edit tools
    if (input.toolName === "write" || input.toolName === "edit") {
      const targetPath = input.args.filePath
      if (targetPath) {
        const normalizedDir = AppFileSystem.normalizePath(instance.directory)
        const normalizedWorktree = AppFileSystem.normalizePath(instance.worktree)
        
        // 先把相对路径转换为绝对路径，再 normalize
        const absoluteTarget = path.isAbsolute(targetPath) 
          ? targetPath 
          : path.join(instance.directory, targetPath)
        const normalizedTarget = AppFileSystem.normalizePath(absoluteTarget)

        const isInWorkspace =
          AppFileSystem.contains(normalizedDir, normalizedTarget) ||
          AppFileSystem.contains(normalizedWorktree, normalizedTarget)

        if (!isInWorkspace) {
          throw new Error(
            `The "${input.toolName}" tool does not allow operations on files outside the workspace, and workspace is "${instance.directory}"`
          )
        }
      }
    }

    // Check bash tool for file system operations
    if (input.toolName === "bash" && input.args.command) {
      const command = input.args.command
      const normalizedDir = AppFileSystem.normalizePath(instance.directory)
      const normalizedWorktree = AppFileSystem.normalizePath(instance.worktree)

      /**
       * 从命令字符串中提取路径，支持引号包裹的路径
       */
      const extractPaths = (cmd: string): string[] => {
        const paths: string[] = []
        let i = 0
        
        while (i < cmd.length) {
          // 跳过空白字符
          while (i < cmd.length && /\s/.test(cmd[i])) i++
          if (i >= cmd.length) break
          
          let pathStart = i
          let pathEnd: number
          
          // 检查是否是引号包裹的路径
          if (cmd[i] === '"' || cmd[i] === "'") {
            const quote = cmd[i]
            pathStart = i + 1
            i++
            while (i < cmd.length && cmd[i] !== quote) i++
            pathEnd = i
            if (i < cmd.length) i++ // 跳过结束引号
          } else {
            // 非引号包裹，读取到下一个空白字符
            while (i < cmd.length && !/\s/.test(cmd[i])) i++
            pathEnd = i
          }
          
          if (pathEnd > pathStart) {
            const extractedPath = cmd.slice(pathStart, pathEnd)
            // 只添加看起来像路径的内容（排除选项如 -p, -r 等）
            if (!/^-/.test(extractedPath)) {
              paths.push(extractedPath)
            }
          }
        }
        
        return paths
      }

      /**
       * 从 PowerShell 命令中提取 -Path 参数的值
       */
      const extractPowerShellPath = (cmd: string, paramName: string = 'Path'): string | null => {
        // 匹配 -Path 参数，支持各种写法：-Path, -path, -p, -Destination 等
        const pathRegex = new RegExp(`-${paramName}\\s*(?::\\s*)?(['"])(.*?)\\1|-${paramName}\\s*(?::\\s*)?([^\\s-]+)`, 'i')
        const match = cmd.match(pathRegex)
        if (match) {
          return match[2] || match[3]
        }
        return null
      }

      /**
       * 检查单个路径是否在工作区内
       */
      const checkPath = (targetPath: string, cmdName: string) => {
        // 先把相对路径转换为绝对路径，再 normalize
        const absoluteTarget = path.isAbsolute(targetPath) 
          ? targetPath 
          : path.join(instance.directory, targetPath)
        const normalizedTarget = AppFileSystem.normalizePath(absoluteTarget)

        const isInWorkspace =
          AppFileSystem.contains(normalizedDir, normalizedTarget) ||
          AppFileSystem.contains(normalizedWorktree, normalizedTarget)

        if (!isInWorkspace) {
          throw new Error(
            `The "bash" tool does not allow "${cmdName}" operations on paths outside the workspace. Workspace is "${instance.directory}", target path is "${targetPath}"`
          )
        }
      }

      /**
       * 检查命令是否包含嵌套执行，如果是则递归检查内部命令
       */
      const checkNestedCommand = (cmd: string) => {
        // 检测 powershell -Command / -c
        const powershellNestedRegex = /^\s*powershell(?:\.exe)?\s*(?:-Command|-c)\s*(['"])(.*?)\1/i
        const psMatch = cmd.match(powershellNestedRegex)
        if (psMatch && psMatch[2]) {
          checkCommand(psMatch[2])
          return true
        }
        
        // 检测 cmd /c /k
        const cmdNestedRegex = /^\s*cmd(?:\.exe)?\s*(?:\/c|\/k)\s*(['"])?(.*?)\1?$/i
        const cmdMatch = cmd.match(cmdNestedRegex)
        if (cmdMatch && cmdMatch[2]) {
          checkCommand(cmdMatch[2])
          return true
        }
        
        return false
      }
      
      /**
       * 递归检查命令是否包含危险操作
       */
      const checkCommand = (cmd: string) => {
        const cmdLower = cmd.toLowerCase().trimStart()
        
        // 先检查是否是嵌套命令
        if (checkNestedCommand(cmd)) {
          return
        }
        
        // ========== .NET 方法调用检查 ==========
        
        // 检测 .NET 的 Directory 和 File 方法调用
        const dotNetMethodRegex = /\[(system\.io\.(directory|file))\]::(createdirectory|delete|create|move|copy|writealltext|writeallbytes|writealllines)\s*\(\s*(['"])(.*?)\4/i
        const dotNetMatch = cmd.match(dotNetMethodRegex)
        if (dotNetMatch) {
          const className = dotNetMatch[1]
          const methodName = dotNetMatch[3]
          const targetPath = dotNetMatch[5]
          checkPath(targetPath, `${className}::${methodName}`)
          return
        }
        
        // ========== PowerShell 命令检查 ==========
        
        // New-Item - 创建文件/文件夹
        if (/^new-item\s/i.test(cmdLower)) {
          const targetPath = extractPowerShellPath(cmd, 'Path')
          if (targetPath) {
            checkPath(targetPath, 'New-Item')
          }
          return
        }
        // Remove-Item - 删除文件/文件夹
        else if (/^remove-item\s/i.test(cmdLower)) {
          const targetPath = extractPowerShellPath(cmd, 'Path')
          if (targetPath) {
            checkPath(targetPath, 'Remove-Item')
          }
          return
        }
        // Move-Item - 移动文件/文件夹
        else if (/^move-item\s/i.test(cmdLower)) {
          const destPath = extractPowerShellPath(cmd, 'Destination') || extractPowerShellPath(cmd, 'Path')
          if (destPath) {
            checkPath(destPath, 'Move-Item')
          }
          return
        }
        // Copy-Item - 复制文件/文件夹
        else if (/^copy-item\s/i.test(cmdLower)) {
          const destPath = extractPowerShellPath(cmd, 'Destination') || extractPowerShellPath(cmd, 'Path')
          if (destPath) {
            checkPath(destPath, 'Copy-Item')
          }
          return
        }
        
        // ========== 传统 CMD/Bash 命令检查 ==========
        
        // 创建文件夹 (mkdir/md)
        if (/^(mkdir|md)\s/i.test(cmdLower)) {
          const paths = extractPaths(cmd.replace(/^(mkdir|md)\s/i, ''))
          for (const p of paths) {
            checkPath(p, 'mkdir')
          }
          return
        }
        // 删除文件夹 (rmdir/rd)
        else if (/^(rmdir|rd)\s/i.test(cmdLower)) {
          const paths = extractPaths(cmd.replace(/^(rmdir|rd)\s/i, ''))
          for (const p of paths) {
            checkPath(p, 'rmdir')
          }
          return
        }
        // 删除文件 (del/erase/rm)
        else if (/^(del|erase|rm)\s/i.test(cmdLower)) {
          const paths = extractPaths(cmd.replace(/^(del|erase|rm)\s/i, ''))
          for (const p of paths) {
            checkPath(p, 'rm')
          }
          return
        }
        // 移动文件/文件夹 (move/mv) - 检查目标路径
        else if (/^(move|mv)\s/i.test(cmdLower)) {
          const paths = extractPaths(cmd.replace(/^(move|mv)\s/i, ''))
          if (paths.length >= 2) {
            checkPath(paths[paths.length - 1], 'mv') // 最后一个是目标路径
          }
          return
        }
        // 复制文件/文件夹 (copy/xcopy/cp) - 检查目标路径
        else if (/^(copy|xcopy|cp)\s/i.test(cmdLower)) {
          const paths = extractPaths(cmd.replace(/^(copy|xcopy|cp)\s/i, ''))
          if (paths.length >= 2) {
            checkPath(paths[paths.length - 1], 'cp') // 最后一个是目标路径
          }
          return
        }
       }
       
       // 开始检查命令
       checkCommand(command)
    }
  })
}
