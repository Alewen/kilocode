import { describe, expect, it } from "bun:test"
import path from "node:path"
import { currentBwrapTarget, ensureBwrapForTarget } from "../../script/bwrap-helper"
import { resolveLocalBwrapEnv, sanitizeSandboxResources } from "../../src/services/cli-backend/cli-resources"

describe("Bubblewrap helpers", () => {
  it("returns the current platform target", () => {
    const target = currentBwrapTarget()
    expect(target).toMatch(/^(linux|darwin|win32)-(x64|arm64)$/)
  })

  it("ensureBwrapForTarget returns undefined (system bwrap is used)", async () => {
    const dest = await ensureBwrapForTarget("linux-x64", "/tmp")
    expect(dest).toBeUndefined()
  })

  it("resolveLocalBwrapEnv always returns empty (system bwrap is used)", () => {
    expect(resolveLocalBwrapEnv("/fake", true, "linux-x64", "/tmp")).toEqual({})
    expect(resolveLocalBwrapEnv("/fake", false, "linux-x64", "/tmp")).toEqual({})
    expect(resolveLocalBwrapEnv("/fake", true, "darwin-arm64", "/tmp")).toEqual({})
  })

  it("sanitizeSandboxResources always returns false", async () => {
    expect(await sanitizeSandboxResources(path.join("/tmp", "bin"), true)).toBe(false)
    expect(await sanitizeSandboxResources(path.join("/tmp", "bin"), false)).toBe(false)
  })
})
