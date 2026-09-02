/*
 * Orion Dial link relay — Cloudflare storage glue.
 *
 * This is the host-specific half: the Durable Object that holds one code, and
 * the RelayStore adapter that routes a `state` to its object. Swapping the
 * relay to another host means replacing this file (and the entry in index.ts);
 * handler.ts stays as-is.
 *
 * Why a Durable Object and not KV: KV is eventually consistent across PoPs, so
 * the /cb write (from the phone's edge) and the /poll read (from the dial's
 * edge) can land on different replicas and the dial would miss its own code.
 * A DO keyed by `state` (idFromName(state)) forces both requests onto one
 * strongly-consistent instance.
 */

import { DurableObject } from "cloudflare:workers";
import { TTL_MS, type MailboxHandle, type RelayStore, type TakeResult } from "./handler";

export interface Env {
  MAILBOX: DurableObjectNamespace<Mailbox>;
}

// Per-object poll rate cap. The object is keyed by `state`, so this caps one
// link attempt's polling — a best-effort, in-memory guard (a hibernated object
// resets it, which is fine for the purpose).
const POLL_WINDOW_MS = 1_000;
const POLL_MAX_PER_WINDOW = 8;

const CODE_KEY = "code";

export class Mailbox extends DurableObject<Env> {
  // In-memory sliding window for the poll rate cap.
  #windowStart = 0;
  #windowCount = 0;

  // Deposit the code and arm the self-eviction alarm. Persisted (not just held
  // in memory) so it survives the object hibernating between the phone's write
  // and the dial's poll.
  async store(code: string): Promise<void> {
    await this.ctx.storage.put(CODE_KEY, code);
    await this.ctx.storage.setAlarm(Date.now() + TTL_MS);
  }

  // Consume the code exactly once, or report why not.
  async take(): Promise<TakeResult> {
    if (this.#rateLimited()) {
      return { kind: "ratelimited" };
    }
    const code = await this.ctx.storage.get<string>(CODE_KEY);
    if (code === undefined) {
      return { kind: "pending" };
    }
    await this.#clear(); // single-use: gone the moment it's read
    return { kind: "code", code };
  }

  // Orion redirected an error instead of a code — drop anything we're holding.
  async fail(): Promise<void> {
    await this.#clear();
  }

  // TTL reached: evict. Idempotent, so a race with take()/fail() is harmless.
  async alarm(): Promise<void> {
    await this.#clear();
  }

  async #clear(): Promise<void> {
    await this.ctx.storage.deleteAll();
    await this.ctx.storage.deleteAlarm();
  }

  #rateLimited(): boolean {
    const now = Date.now();
    if (now - this.#windowStart >= POLL_WINDOW_MS) {
      this.#windowStart = now;
      this.#windowCount = 0;
    }
    this.#windowCount += 1;
    return this.#windowCount > POLL_MAX_PER_WINDOW;
  }
}

// Routes a `state` to its Durable Object. This is the only place the relay
// names a Cloudflare binding.
export class DurableObjectStore implements RelayStore {
  constructor(private readonly ns: DurableObjectNamespace<Mailbox>) {}

  mailbox(state: string): MailboxHandle {
    const stub = this.ns.get(this.ns.idFromName(state));
    return {
      store: (code) => stub.store(code),
      take: () => stub.take(),
      fail: () => stub.fail(),
    };
  }
}
