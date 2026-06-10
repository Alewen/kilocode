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
import { Patch } from "@/patch"
import { environmentDetails, type EditorContext } from "@/kilocode/editor-context"
import { Identifier } from "@/id/id"
import { Filesystem } from "@/util/filesystem"
import { containsPath } from "@/project/instance-context" // kilocode_change
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
        const absoluteTarget = path.isAbsolute(targetPath)
          ? targetPath
          : path.join(instance.directory, targetPath)
        const normalizedTarget = AppFileSystem.resolve(absoluteTarget)

        if (!containsPath(normalizedTarget, instance)) {
          throw new Error(
            `The "${input.toolName}" tool does not allow operations on files outside the workspace, and workspace is "${instance.directory}"`
          )
        }
      }
    }

    // Check apply_patch tool
    if (input.toolName === "apply_patch" && input.args.patchText) {
      const { hunks } = Patch.parsePatch(input.args.patchText)
      for (const hunk of hunks) {
        const absoluteTarget = path.isAbsolute(hunk.path)
          ? hunk.path
          : path.join(instance.directory, hunk.path)
        const normalizedTarget = AppFileSystem.resolve(absoluteTarget)

        if (!containsPath(normalizedTarget, instance)) {
          throw new Error(
            `The "apply_patch" tool does not allow operations on files outside the workspace, and workspace is "${instance.directory}"`
          )
        }
        if (hunk.type === "update" && hunk.move_path) {
          const absoluteMove = path.isAbsolute(hunk.move_path)
            ? hunk.move_path
            : path.join(instance.directory, hunk.move_path)
          const normalizedMove = AppFileSystem.resolve(absoluteMove)

          if (!containsPath(normalizedMove, instance)) {
            throw new Error(
              `The "apply_patch" tool does not allow operations on files outside the workspace, and workspace is "${instance.directory}"`
            )
          }
        }
      }
    }

    // Check bash tool for file system operations
    if (input.toolName === "bash" && input.args.command) {
      const command = input.args.command
      const normalizedDir = AppFileSystem.resolve(instance.directory)
      const normalizedWorktree = AppFileSystem.resolve(instance.worktree)

        /**
         * 从命令中提取命令名（支持绝对路径如 /bin/echo）
         */
        const extractCmdName = (cmdLower: string): string | null => {
          const match = cmdLower.match(/^\s*(\/[\w-]+\/[\w]+|\w+)/)
          return match ? match[1] : null
        }

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
       * 从 PowerShell 命令中提取位置参数路径（用于处理 Copy-Item <src> <dest> 等位置参数写法）
       */
      const extractPowerShellPositionalPaths = (cmd: string, cmdName: string): string[] => {
        // 移除命令名，提取剩余参数部分
        const afterCmd = cmd.replace(new RegExp(`^\\s*${cmdName}\\s+`, 'i'), '')
        return extractPaths(afterCmd)
      }

      /**
       * 检查单个路径是否在工作区内
       */
      const checkPath = (targetPath: string, cmdName: string) => {
        // 先把相对路径转换为绝对路径，再 resolve
        const absoluteTarget = path.isAbsolute(targetPath)
          ? targetPath
          : path.join(instance.directory, targetPath)
        const normalizedTarget = AppFileSystem.resolve(absoluteTarget)

        if (!containsPath(normalizedTarget, instance)) {
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
          const targetPath = extractPowerShellPath(cmd, 'Path') ||
            extractPowerShellPositionalPaths(cmd, 'New-Item')[0]
          if (targetPath) {
            checkPath(targetPath, 'New-Item')
          }
          return
        }
        // Remove-Item - 删除文件/文件夹
        else if (/^remove-item\s/i.test(cmdLower)) {
          const targetPath = extractPowerShellPath(cmd, 'Path') ||
            extractPowerShellPositionalPaths(cmd, 'Remove-Item')[0]
          if (targetPath) {
            checkPath(targetPath, 'Remove-Item')
          }
          return
        }
        // Move-Item - 移动文件/文件夹
        else if (/^move-item\s/i.test(cmdLower)) {
          const positionalPaths = extractPowerShellPositionalPaths(cmd, 'Move-Item')
          const destPath = extractPowerShellPath(cmd, 'Destination') ||
            extractPowerShellPath(cmd, 'Path') ||
            positionalPaths[positionalPaths.length - 1]
          if (destPath) {
            checkPath(destPath, 'Move-Item')
          }
          return
        }
        // Copy-Item - 复制文件/文件夹
        else if (/^copy-item\s/i.test(cmdLower)) {
          const positionalPaths = extractPowerShellPositionalPaths(cmd, 'Copy-Item')
          const destPath = extractPowerShellPath(cmd, 'Destination') ||
            extractPowerShellPath(cmd, 'Path') ||
            positionalPaths[positionalPaths.length - 1]
          if (destPath) {
            checkPath(destPath, 'Copy-Item')
          }
          return
        }
        
        // ========== 传统 CMD/Bash 命令检查 ==========

        // 获取命令名（支持绝对路径如 /bin/sed）
        const cmdName = extractCmdName(cmdLower)
        const cmdBaseName = cmdName ? cmdName.replace(/.*\//, '') : null

        // 创建文件夹 (mkdir/md) - 支持绝对路径
        if (cmdBaseName === 'mkdir' || cmdBaseName === 'md') {
          const paths = extractPaths(cmd.replace(/^\s*(\/[\w-]+\/[\w]+|\w+)\s*/, ''))
          for (const p of paths) {
            checkPath(p, 'mkdir')
          }
          return
        }
        // 删除文件夹 (rmdir/rd) - 支持绝对路径
        if (cmdBaseName === 'rmdir' || cmdBaseName === 'rd') {
          const paths = extractPaths(cmd.replace(/^\s*(\/[\w-]+\/[\w]+|\w+)\s*/, ''))
          for (const p of paths) {
            checkPath(p, 'rmdir')
          }
          return
        }
        // 删除文件 (del/erase/rm) - 支持绝对路径
        if (cmdBaseName === 'del' || cmdBaseName === 'erase' || cmdBaseName === 'rm') {
          const paths = extractPaths(cmd.replace(/^\s*(\/[\w-]+\/[\w]+|\w+)\s*/, ''))
          for (const p of paths) {
            checkPath(p, 'rm')
          }
          return
        }
        // 移动文件/文件夹 (move/mv) - 支持绝对路径，检查目标路径
        if (cmdBaseName === 'move' || cmdBaseName === 'mv') {
          const paths = extractPaths(cmd.replace(/^\s*(\/[\w-]+\/[\w]+|\w+)\s*/, ''))
          if (paths.length >= 2) {
            checkPath(paths[paths.length - 1], 'mv')
          }
          return
        }
        // 复制文件/文件夹 (copy/xcopy/cp) - 支持绝对路径，检查目标路径
        if (cmdBaseName === 'copy' || cmdBaseName === 'xcopy' || cmdBaseName === 'cp') {
          const paths = extractPaths(cmd.replace(/^\s*(\/[\w-]+\/[\w]+|\w+)\s*/, ''))
          if (paths.length >= 2) {
            checkPath(paths[paths.length - 1], 'cp')
          }
          return
        }
        // touch - 创建文件 - 支持绝对路径
        if (cmdBaseName === 'touch') {
          const paths = extractPaths(cmd.replace(/^\s*(\/[\w-]+\/[\w]+|\w+)\s*/, ''))
          for (const p of paths) {
            checkPath(p, 'touch')
          }
          return
        }
        // sed -i 直接修改文件 - 支持绝对路径
        if (cmdBaseName === 'sed') {
          const inPlaceMatch = cmd.match(/-i\b/)
          if (inPlaceMatch) {
            const args = cmd.replace(/^\s*(\/[\w-]+\/[\w]+|\w+)\s*/, '').replace(/-i\b/, '').trim().split(/\s+/)
            const filePath = args[args.length - 1]
            if (filePath) {
              checkPath(filePath, 'sed -i')
            }
          }
          return
        }
        // perl -i 直接修改文件 - 支持绝对路径
        if (cmdBaseName === 'perl') {
          const inPlaceMatch = cmd.match(/-i\b/)
          if (inPlaceMatch) {
            const fileMatch = cmd.match(/-i(?:\s+\S+)?\s+(\S+)\s*$/)
            if (fileMatch) {
              checkPath(fileMatch[1], 'perl -i')
            }
          }
          return
        }
        // awk 处理文件（写入模式）- 支持绝对路径
        if (cmdBaseName === 'awk') {
          if (/\{.*\}/.test(cmd) && />>?/.test(cmd)) {
            const redirectMatch = cmd.match(/>{1,2}\s*(\S+)/)
            if (redirectMatch) {
              checkPath(redirectMatch[1], 'awk')
            }
          }
          return
        }
        // echo/printf 重定向写入 (支持 > 和 >>，绝对路径)
        if (cmdBaseName === 'echo' || cmdBaseName === 'printf') {
          const redirectMatch = cmd.match(/>{1,2}\s*(\S+)/)
          if (redirectMatch) {
            checkPath(redirectMatch[1], cmdBaseName)
            return
          }
        }
        // tee 无重定向写入 (直接跟文件路径)
        if (cmdBaseName === 'tee') {
          const redirectMatch = cmd.match(/>{1,2}\s*(\S+)/)
          if (redirectMatch) {
            checkPath(redirectMatch[1], 'tee')
            return
          }
          const paths = extractPaths(cmd.replace(/^\s*(\/[\w-]+\/[\w]+|\w+)\s*/, ''))
          for (const p of paths) {
            checkPath(p, 'tee')
          }
          return
        }
        // cat 重定向写入 (支持 > 和 >>，绝对路径)
        if (cmdBaseName === 'cat') {
          const redirectMatch = cmd.match(/>{1,2}\s*(\S+)/)
          if (redirectMatch) {
            checkPath(redirectMatch[1], 'cat')
            return
          }
        }
        // 管道 tee 写入 (| tee) - 支持带重定向和不带重定向
        if (/\|\s*tee\b/i.test(cmd)) {
          // 先检测带重定向的情况: | tee > file 或 | tee >> file
          const pipeMatch = cmd.match(/\|\s*tee\s+(>>?)\s*(\S+)/)
          if (pipeMatch) {
            checkPath(pipeMatch[2], 'tee')
            return
          }
          // 检测不带重定向的情况: echo | tee file
          const teeMatch = cmd.match(/\|\s*tee\s+(\S+)/)
          if (teeMatch) {
            checkPath(teeMatch[1], 'tee')
          }
          return
        }
       }
       
       // 开始检查命令
       checkCommand(command)
    }
  })
}
