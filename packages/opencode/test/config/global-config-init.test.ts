import { test, expect, describe, beforeEach, afterEach } from "bun:test"
import { Effect, Layer, Option } from "effect"
import { NodeFileSystem, NodePath } from "@effect/platform-node"
import { Config } from "@/config/config"
import { EffectFlock } from "@opencode-ai/core/util/effect-flock"
import { AppFileSystem } from "@opencode-ai/core/filesystem"
import { Env } from "@/env"
import { tmpdir } from "../fixture/fixture"
import path from "path"
import fs from "fs/promises"
import { existsSync } from "fs"
import { Global } from "@opencode-ai/core/global"
import { Auth } from "@/auth"
import { Account } from "@/account/account"
import { Npm } from "@opencode-ai/core/npm"
import { CrossSpawnSpawner } from "@opencode-ai/core/cross-spawn-spawner"
import { InstanceRuntime } from "@/project/instance-runtime"

// 模拟必要的服务
const emptyAccount = Layer.mock(Account.Service)({
  active: () => Effect.succeed(Option.none()),
  activeOrg: () => Effect.succeed(Option.none()),
})

const emptyAuth = Layer.mock(Auth.Service)({
  all: () => Effect.succeed({}),
})

const testFlock = EffectFlock.defaultLayer

const noopNpm = Layer.mock(Npm.Service)({
  install: () => Effect.void,
  add: () => Effect.die("not implemented"),
  which: () => Effect.succeed(Option.none()),
})

const infra = CrossSpawnSpawner.defaultLayer.pipe(
  Layer.provideMerge(Layer.mergeAll(NodeFileSystem.layer, NodePath.layer)),
)

const layer = Config.layer.pipe(
  Layer.provide(testFlock),
  Layer.provide(AppFileSystem.defaultLayer),
  Layer.provide(Env.defaultLayer),
  Layer.provide(emptyAuth),
  Layer.provide(emptyAccount),
  Layer.provideMerge(infra),
  Layer.provide(noopNpm),
)

// 辅助函数
const loadGlobal = () =>
  Effect.runPromise(Config.Service.use((svc) => svc.getGlobal()).pipe(Effect.scoped, Effect.provide(layer)))

const clear = async (wait = false) => {
  await Effect.runPromise(Config.Service.use((svc) => svc.invalidate()).pipe(Effect.scoped, Effect.provide(layer)))
  if (wait) await InstanceRuntime.disposeAllInstances()
}

describe("全局配置初始化功能", () => {
  let originalGlobalPathConfig: string
  let testTempDir: string

  beforeEach(async () => {
    // 保存原始路径
    originalGlobalPathConfig = Global.Path.config
    // 创建临时目录
    const tmp = await tmpdir()
    testTempDir = tmp.path
    // 替换 Global.Path.config
    ;(Global.Path as { config: string }).config = testTempDir
    await clear(true)
  })

  afterEach(async () => {
    // 恢复原始路径
    ;(Global.Path as { config: string }).config = originalGlobalPathConfig
    await clear(true)
  })

  test("测试场景 1: 配置文件不存在，应该创建它并包含默认 bwrap 配置", async () => {
    const testConfigPath = path.join(testTempDir, "kilo.jsonc")
    
    // 确保文件不存在
    expect(existsSync(testConfigPath)).toBe(false)

    // 调用 getGlobal，这会触发初始化
    await loadGlobal()

    // 验证文件已创建
    expect(existsSync(testConfigPath)).toBe(true)

    // 读取并验证内容
    const content = await fs.readFile(testConfigPath, "utf-8")
    const parsed = JSON.parse(content)
    
    expect(parsed.$schema).toBe("https://app.kilo.ai/config.json")
    expect(parsed.bwrap).toBeDefined()
    expect(parsed.bwrap.tmpfs).toEqual([
      "/tmp", "/root", "/var", "/opt", "/mnt", "/media", "/run", "/srv", "/boot"
    ])
    expect(parsed.bwrap.symlink).toEqual([
      { from: "usr/bin", to: "/bin" },
      { from: "usr/lib", to: "/lib" },
      { from: "usr/lib64", to: "/lib64" },
      { from: "usr/sbin", to: "/sbin" }
    ])
  })

  test("测试场景 2: 配置文件存在但没有 bwrap 节点，应该添加它", async () => {
    const testConfigPath = path.join(testTempDir, "kilo.jsonc")
    
    // 创建一个没有 bwrap 的配置文件
    await fs.mkdir(testTempDir, { recursive: true })
    await fs.writeFile(
      testConfigPath,
      JSON.stringify({
        $schema: "https://app.kilo.ai/config.json",
        model: "test/model"
      }, null, 2)
    )

    // 验证初始状态
    expect(existsSync(testConfigPath)).toBe(true)
    const initialContent = await fs.readFile(testConfigPath, "utf-8")
    expect(JSON.parse(initialContent).bwrap).toBeUndefined()

    // 先 invalidate 确保不会有缓存
    await clear(true)
    
    // 调用 getGlobal，这会触发初始化
    await loadGlobal()

    // 验证 bwrap 已添加
    const updatedContent = await fs.readFile(testConfigPath, "utf-8")
    const updatedParsed = JSON.parse(updatedContent)
    
    expect(updatedParsed.model).toBe("test/model")
    expect(updatedParsed.bwrap).toBeDefined()
  })

  test("测试场景 3: 配置文件存在且有 bwrap 节点，应该保持原样", async () => {
    const testConfigPath = path.join(testTempDir, "kilo.jsonc")
    
    // 创建一个有自定义 bwrap 的配置文件
    await fs.mkdir(testTempDir, { recursive: true })
    const customConfig = {
      $schema: "https://app.kilo.ai/config.json",
      model: "test/model",
      bwrap: { tmpfs: ["/custom/tmp"], ro_bind: ["/custom/path"] }
    }
    await fs.writeFile(testConfigPath, JSON.stringify(customConfig, null, 2))

    // 调用 getGlobal
    await loadGlobal()

    // 验证配置保持不变
    const finalContent = await fs.readFile(testConfigPath, "utf-8")
    const finalParsed = JSON.parse(finalContent)
    
    expect(finalParsed.model).toBe("test/model")
    expect(finalParsed.bwrap.tmpfs).toEqual(["/custom/tmp"])
    expect(finalParsed.bwrap.ro_bind).toEqual(["/custom/path"])
  })
})
