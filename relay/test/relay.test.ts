/*
 * Orion Dial link relay — tests.
 *
 * Runs in workerd via @cloudflare/vitest-pool-workers, so the Durable Object,
 * its TTL alarm, and SQLite storage are the real thing. Most tests drive the
 * Worker end-to-end through SELF.fetch (the /cb write and /poll read as the
 * phone and dial make them); the TTL and rate-cap tests reach into the object
 * directly to fire the alarm and to hammer take() inside one window.
 */

import { env, SELF, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import type { Mailbox } from "../src/mailbox";

// A fresh, valid state per test → a fresh mailbox, so tests don't cross-talk.
function freshState(): string {
  return "st_" + crypto.randomUUID().replace(/-/g, "");
}

function cbUrl(params: Record<string, string>): string {
  const u = new URL("https://relay.test/cb");
  for (const [k, v] of Object.entries(params)) u.searchParams.set(k, v);
  return u.toString();
}

function pollUrl(state: string): string {
  const u = new URL("https://relay.test/poll");
  u.searchParams.set("state", state);
  return u.toString();
}

describe("mailbox delivery", () => {
  it("delivers a stored code exactly once, then reports pending", async () => {
    const state = freshState();

    const cb = await SELF.fetch(cbUrl({ code: "AUTHCODE-123", state }));
    expect(cb.status).toBe(200);
    expect(cb.headers.get("content-type")).toContain("text/html");
    expect(await cb.text()).toContain("Dial linked");

    const first = await SELF.fetch(pollUrl(state));
    expect(first.status).toBe(200);
    expect(await first.json()).toEqual({ code: "AUTHCODE-123" });

    // Single-use: the second poll finds an empty mailbox.
    const second = await SELF.fetch(pollUrl(state));
    expect(second.status).toBe(200);
    expect(await second.json()).toEqual({ status: "pending" });
  });

  it("returns pending when polled before any code is stored", async () => {
    const res = await SELF.fetch(pollUrl(freshState()));
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({ status: "pending" });
  });

  it("carries the code across a separate /cb write and /poll read, once", async () => {
    const state = freshState();
    // Two independent HTTP requests, as the phone and the dial make them.
    await SELF.fetch(cbUrl({ code: "OPAQUE.token_~value", state }));

    const codes: unknown[] = [];
    for (let i = 0; i < 3; i++) {
      codes.push(await (await SELF.fetch(pollUrl(state))).json());
    }
    // Exactly one poll saw the code; the rest are pending.
    expect(codes.filter((c) => JSON.stringify(c) === JSON.stringify({ code: "OPAQUE.token_~value" })))
      .toHaveLength(1);
    expect(codes).toContainEqual({ status: "pending" });
  });
});

describe("TTL eviction", () => {
  it("drops the code when the TTL alarm fires", async () => {
    const state = freshState();
    const stub = env.MAILBOX.get(env.MAILBOX.idFromName(state));

    await runInDurableObject(stub, (inst: Mailbox) => inst.store("EXPIRES-SOON"));

    // store() armed an alarm; run it now instead of waiting out the TTL.
    const fired = await runDurableObjectAlarm(stub);
    expect(fired).toBe(true);

    const res = await SELF.fetch(pollUrl(state));
    expect(await res.json()).toEqual({ status: "pending" });
  });
});

describe("input validation", () => {
  it("rejects a callback with no code (400)", async () => {
    const res = await SELF.fetch(cbUrl({ state: freshState() }));
    expect(res.status).toBe(400);
  });

  it("rejects a callback with no state (400)", async () => {
    const res = await SELF.fetch(cbUrl({ code: "X" }));
    expect(res.status).toBe(400);
  });

  it("rejects a code past the cap (400) but accepts one at the cap", async () => {
    const tooLong = await SELF.fetch(cbUrl({ code: "A".repeat(1025), state: freshState() }));
    expect(tooLong.status).toBe(400);

    // 1024 is the cap (matches the firmware's s_code[1024] budget) — accepted.
    const atCap = await SELF.fetch(cbUrl({ code: "A".repeat(1024), state: freshState() }));
    expect(atCap.status).toBe(200);
  });

  it("rejects an oversized state (400)", async () => {
    const res = await SELF.fetch(cbUrl({ code: "X", state: "s".repeat(129) }));
    expect(res.status).toBe(400);
  });

  it("rejects a malformed code with control characters (400)", async () => {
    const res = await SELF.fetch(cbUrl({ code: "bad\ncode", state: freshState() }));
    expect(res.status).toBe(400);
  });

  it("rejects a malformed state on /poll (400)", async () => {
    const res = await SELF.fetch(pollUrl("has spaces & symbols!"));
    expect(res.status).toBe(400);
  });

  it("rejects /poll with no state (400)", async () => {
    const res = await SELF.fetch("https://relay.test/poll");
    expect(res.status).toBe(400);
  });
});

describe("error redirects", () => {
  it("shows a friendly failure page and clears the mailbox", async () => {
    const state = freshState();
    // A code present first; the error redirect must still empty the mailbox.
    await SELF.fetch(cbUrl({ code: "SHOULD-BE-CLEARED", state }));

    const err = await SELF.fetch(
      cbUrl({ error: "access_denied", error_description: "user declined", state }),
    );
    expect(err.status).toBe(200);
    expect(err.headers.get("content-type")).toContain("text/html");
    // page() escapes the heading, so the ASCII apostrophe renders as an entity.
    expect(await err.text()).toContain("Couldn&#39;t finish linking");

    const poll = await SELF.fetch(pollUrl(state));
    expect(await poll.json()).toEqual({ status: "pending" });
  });

  it("HTML-escapes an unknown error's description", async () => {
    const res = await SELF.fetch(
      cbUrl({ error: "weird_thing", error_description: "<script>alert(1)</script>", state: freshState() }),
    );
    const body = await res.text();
    expect(body).not.toContain("<script>alert(1)</script>");
    expect(body).toContain("&lt;script&gt;");
  });

  it("still requires a valid state on an error redirect (400)", async () => {
    const res = await SELF.fetch(cbUrl({ error: "access_denied" }));
    expect(res.status).toBe(400);
  });
});

describe("poll rate cap", () => {
  it("rate-limits a tight burst of polls on one mailbox", async () => {
    const state = freshState();
    const stub = env.MAILBOX.get(env.MAILBOX.idFromName(state));

    // All within one window (storage reads are sub-millisecond here), so the
    // per-object cap trips.
    const kinds = await runInDurableObject(stub, async (inst: Mailbox) => {
      const out: string[] = [];
      for (let i = 0; i < 30; i++) {
        out.push((await inst.take()).kind);
      }
      return out;
    });

    expect(kinds).toContain("ratelimited");
  });
});
