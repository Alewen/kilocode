// kilocode_change - new file
import chardet from "chardet"
import iconv from "iconv-lite"

const LEGACY = "gb18030"
const SAMPLE = 32
const MIN_UTF8 = 4

type Decoder = {
  write(bytes: Buffer): string
  end(): string | undefined
}

type State =
  | { type: "detect"; pending: Buffer }
  | { type: "utf8"; decoder: TextDecoder }
  | { type: "legacy"; decoder: Decoder }

const ext = iconv as typeof iconv & { getDecoder(encoding: string): Decoder }

/**
 * Detects whether bytes are valid UTF-8 before falling back to Windows GB18030.
 */
function utf8(bytes: Buffer): boolean {
  try {
    new TextDecoder("utf-8", { fatal: true }).decode(bytes)
    return true
  } catch {
    return false
  }
}

function ascii(bytes: Buffer): boolean {
  return bytes.every((byte) => byte < 0x80)
}

function detected(bytes: Buffer): "utf8" | "legacy" | undefined {
  if (!utf8(bytes)) return "legacy"
  if (bytes.length < MIN_UTF8) return undefined
  if (chardet.detect(bytes) === "UTF-8") return "utf8"
  if (bytes.length >= SAMPLE) return "utf8"
}

/**
 * Creates a streaming decoder for shell output bytes.
 */
export function create() {
  let state: State = { type: "detect", pending: Buffer.alloc(0) }

  return {
    write(input: Uint8Array): string {
      const bytes = Buffer.from(input)
      if (bytes.length === 0) return ""

      if (state.type === "utf8") return state.decoder.decode(bytes, { stream: true })
      if (state.type === "legacy") return state.decoder.write(bytes)
      if (state.pending.length === 0 && ascii(bytes)) return bytes.toString("utf8")

      const pending = Buffer.concat([state.pending, bytes])
      const enc = detected(pending)
      if (!enc) {
        state = { type: "detect", pending }
        return ""
      }

      if (enc === "utf8") {
        const decoder = new TextDecoder("utf-8", { fatal: true })
        state = { type: "utf8", decoder }
        return decoder.decode(pending, { stream: true })
      }

      const decoder = ext.getDecoder(LEGACY)
      state = { type: "legacy", decoder }
      return decoder.write(pending)
    },
    end(): string {
      if (state.type === "utf8") return state.decoder.decode()
      if (state.type === "legacy") return state.decoder.end() ?? ""
      if (state.pending.length === 0) return ""
      const enc = detected(state.pending)
      if (enc === "utf8") return new TextDecoder("utf-8", { fatal: true }).decode(state.pending)
      return iconv.decode(state.pending, LEGACY)
    },
  }
}
