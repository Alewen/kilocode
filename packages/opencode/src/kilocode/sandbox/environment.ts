const providerKeys = new Set([
  "ANTHROPIC_API_KEY",
  "OPENAI_API_KEY",
  "GOOGLE_GENERATIVE_AI_API_KEY",
  "GOOGLE_API_KEY",
  "GROQ_API_KEY",
  "DEEPSEEK_API_KEY",
  "MISTRAL_API_KEY",
  "XAI_API_KEY",
  "OPENROUTER_API_KEY",
  "COHERE_API_KEY",
  "TOGETHER_API_KEY",
  "AZURE_OPENAI_API_KEY",
  "REPLICATE_API_KEY",
  "PERPLEXITY_API_KEY",
  "FIREWORKS_API_KEY",
  "VOYAGE_API_KEY",
  "KILO_API_KEY",
  "APERTIS_API_KEY",
])

const internal = new Set(["KILO_SERVER_PASSWORD", "KILO_SERVER_USERNAME"])

export const denied = new Set([...providerKeys, ...internal])

export function strip(env: Record<string, string | undefined>) {
  for (const key of denied) {
    delete env[key]
  }
}

export const isDenied = (name: string) => denied.has(name.toUpperCase())
