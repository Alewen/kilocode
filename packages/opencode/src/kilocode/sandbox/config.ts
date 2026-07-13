import { Schema } from "effect"

export namespace SandboxConfig {
  export const Network = Schema.Literals(["allow", "deny"])
  export type Network = Schema.Schema.Type<typeof Network>

  export const Info = Schema.Struct({
    enabled: Schema.optional(
      Schema.Boolean.annotate({ description: "Enable sandbox confinement for new sessions (default: false)" }),
    ),
    network: Schema.optional(
      Network.annotate({ description: "Control outbound network access from sandboxed tools (default: deny)" }),
    ),
    writable_paths: Schema.optional(
      Schema.mutable(Schema.Array(Schema.String)).annotate({
        description: "Additional filesystem paths that sandboxed tools may write to",
      }),
    ),
    readonly_paths: Schema.optional(
      Schema.mutable(Schema.Array(Schema.String)).annotate({
        description: "Paths to expose read-only inside the sandbox",
      }),
    ),
    deny_paths: Schema.optional(
      Schema.mutable(Schema.Array(Schema.String)).annotate({
        description: "Paths to exclude from the sandbox (mounted as empty tmpfs)",
      }),
    ),
    symlink_paths: Schema.optional(
      Schema.mutable(
        Schema.Array(
          Schema.Struct({
            from: Schema.String.annotate({ description: "Symlink target (relative to sandbox root)" }),
            to: Schema.String.annotate({ description: "Symlink location inside sandbox" }),
          }),
        ),
      ).annotate({
        description: "Symlinks to create inside the sandbox",
      }),
    ),
  }).annotate({ description: "Sandbox configuration for agent tools" })
  export type Info = Schema.Schema.Type<typeof Info>

  const DEFAULT_READONLY = ["/usr", "/etc"]
  const DEFAULT_DENY = ["/home", "/tmp", "/root", "/var", "/opt", "/mnt", "/media", "/run", "/srv", "/boot"]
  const DEFAULT_SYMLINK = [
    { from: "usr/bin", to: "/bin" },
    { from: "usr/lib", to: "/lib" },
    { from: "usr/lib64", to: "/lib64" },
    { from: "usr/sbin", to: "/sbin" },
  ]

  export function resolve(config: { sandbox?: Info }) {
    return {
      enabled: config.sandbox?.enabled ?? false,
      mode: config.sandbox?.network ?? "deny",
      readonlyPaths: config.sandbox?.readonly_paths ?? DEFAULT_READONLY,
      denyPaths: config.sandbox?.deny_paths ?? DEFAULT_DENY,
      symlinkPaths: config.sandbox?.symlink_paths ?? DEFAULT_SYMLINK,
      writablePaths: config.sandbox?.writable_paths ?? [],
    }
  }

  export function scope<T extends { sandbox?: Info }>(config: T, source: "global" | "local"): T {
    if (source === "global" || config.sandbox === undefined) return config
    const scoped = { ...config }
    const sandbox: Info = {
      ...(config.sandbox.enabled === true ? { enabled: true } : {}),
      ...(config.sandbox.network === "deny" ? { network: "deny" as const } : {}),
    }
    if (Object.keys(sandbox).length > 0) scoped.sandbox = sandbox
    else delete scoped.sandbox
    return scoped
  }
}
