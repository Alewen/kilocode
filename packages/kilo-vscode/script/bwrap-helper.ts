export function currentBwrapTarget(): string {
  const os = process.platform === "win32" ? "win32" : process.platform
  return `${os}-${process.arch}`
}

export async function ensureBwrapForTarget(_target: string, _root?: string): Promise<string | undefined> {
  return undefined
}
