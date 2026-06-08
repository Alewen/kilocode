import { describe, expect, test } from "bun:test"
import iconv from "iconv-lite"
import * as ShellOutput from "../../src/kilocode/tool/shell-output"

const collect = (chunks: Uint8Array[]) => {
  const decoder = ShellOutput.create()
  return chunks.map((chunk) => decoder.write(chunk)).join("") + decoder.end()
}

/**
 * Verifies shell output byte decoding before text reaches tool metadata/output.
 */
describe("kilocode shell output", () => {
  test("keeps utf-8 output unchanged", async () => {
    const bytes = new TextEncoder().encode("hello 默认\n")
    expect(collect([bytes])).toBe("hello 默认\n")
  })

  test("decodes Windows legacy Chinese output split across chunks", async () => {
    const bytes = iconv.encode("    (默认)    REG_SZ    value\r\n", "gb18030")
    const chunks = [bytes.subarray(0, 5), bytes.subarray(5, 7), bytes.subarray(7, 9), bytes.subarray(9)]

    expect(collect(chunks)).toBe("    (默认)    REG_SZ    value\r\n")
  })
})
