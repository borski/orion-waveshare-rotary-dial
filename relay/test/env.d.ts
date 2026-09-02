// Teach `cloudflare:test` about the Worker's bindings (the MAILBOX Durable
// Object), so `env` in the tests is typed the same as it is in the Worker.
import type { Env } from "../src/mailbox";

declare module "cloudflare:test" {
  interface ProvidedEnv extends Env {}
}
