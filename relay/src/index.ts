/*
 * Orion Dial link relay — Cloudflare Worker entry point.
 *
 * Thin: it wires the Cloudflare storage glue (DurableObjectStore over the
 * MAILBOX binding) into the host-agnostic router in handler.ts, and re-exports
 * the Durable Object class so the runtime can find it. All routing, validation,
 * and response shaping lives in handler.ts.
 */

import { handle } from "./handler";
import { DurableObjectStore, type Env } from "./mailbox";

export { Mailbox } from "./mailbox";

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      return await handle(request, new DurableObjectStore(env.MAILBOX));
    } catch {
      // Never surface internals (and never log code/state). A generic 500 is
      // all a caller ever needs to see.
      return new Response("Internal Server Error", {
        status: 500,
        headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" },
      });
    }
  },
} satisfies ExportedHandler<Env>;
