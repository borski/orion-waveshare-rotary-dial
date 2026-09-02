/*
 * Orion Dial link relay — request handling.
 *
 * This file is the host-agnostic half of the relay: it speaks only the Web
 * platform (Request/Response/URL) and a small storage interface (RelayStore),
 * so moving the service off Cloudflare means rewriting ONE glue file
 * (src/index.ts + src/mailbox.ts) and leaving everything here untouched.
 *
 * The relay is a single-use, short-lived mailbox for a PKCE authorization
 * code. It never sees tokens or PII, and it must never log a `code` or `state`
 * value: `state` doubles as the mailbox bearer key, and `code` is the auth
 * code itself. See relay/README.md for the flow.
 */

// Size caps. `state` is minted by the dial (base64url, >=128-bit) so it stays
// small; `code` is an opaque token from Orion. Anything past these is a client
// that isn't our dial or Orion's redirect — reject rather than store it.
export const STATE_MAX = 128;
// 1024 mirrors the firmware's s_code[1024] buffer, which budgets for long
// (JWT-shaped) auth codes; anything longer isn't a code we can deliver.
export const CODE_MAX = 1024;

// How long a stored code lives before the mailbox self-evicts (see mailbox.ts).
// The phone writes it and the dial has this long to poll it out.
export const TTL_MS = 120_000;

// The result of a single /poll, from the mailbox's point of view.
export type TakeResult =
  | { kind: "code"; code: string }   // a code was waiting; it's now consumed
  | { kind: "pending" }              // nothing here (yet, or already taken)
  | { kind: "ratelimited" };         // caller is polling too fast

// One mailbox, already bound to a single `state`. The concrete implementation
// (a Durable Object on Cloudflare) lives behind this so the routing below never
// names a host-specific type.
export interface MailboxHandle {
  // Deposit the auth code for later polling. Starts/refreshes the TTL.
  store(code: string): Promise<void>;
  // Consume the code exactly once (single-use), or report pending / rate-limited.
  take(): Promise<TakeResult>;
  // Clear the mailbox — used when Orion redirects an error instead of a code.
  fail(): Promise<void>;
}

// Resolves a `state` to its mailbox. On Cloudflare this routes to the Durable
// Object instance keyed by that state (idFromName(state)) so /cb and /poll,
// arriving at different edges, still hit one strongly-consistent object.
export interface RelayStore {
  mailbox(state: string): MailboxHandle;
}

// `state` is our own value, so hold it to a tight URL-safe shape.
function isValidState(s: string | null): s is string {
  return s !== null && s.length >= 1 && s.length <= STATE_MAX && /^[A-Za-z0-9._~-]+$/.test(s);
}

// `code` is opaque and Orion's to shape; accept any printable ASCII with no
// whitespace or control characters, under the cap. This is enough to keep it
// safe to echo back as JSON while rejecting obvious junk.
function isValidCode(c: string | null): c is string {
  return c !== null && c.length >= 1 && c.length <= CODE_MAX && /^[\x21-\x7E]+$/.test(c);
}

export async function handle(request: Request, store: RelayStore): Promise<Response> {
  if (request.method !== "GET") {
    return text(405, "Method Not Allowed");
  }
  const url = new URL(request.url);
  switch (url.pathname) {
    case "/cb":
      return handleCallback(url, store);
    case "/poll":
      return handlePoll(url, store);
    case "/":
      // A bare hit on the base URL — handy to confirm a deploy is live. No
      // secrets, no behaviour.
      return html(200, page("Orion Dial link relay", "Orion Dial link relay",
        "This is the pairing relay for the Orion bedside dial. There's nothing to do here."));
    default:
      return text(404, "Not Found");
  }
}

// GET /cb?code=&state=  — the phone lands here after approving at Orion.
// Also GET /cb?error=&error_description=&state=  — Orion's error redirect.
async function handleCallback(url: URL, store: RelayStore): Promise<Response> {
  const params = url.searchParams;
  const state = params.get("state");

  // `state` is the mailbox key and is required in every case, success or error.
  if (!isValidState(state)) {
    return html(400, page("Link error", "Something's off with this link",
      "This pairing link is missing or malformed. Start again from your dial."));
  }
  const mailbox = store.mailbox(state);

  // Orion redirected an error (e.g. the user declined). Clear any mailbox and
  // show a friendly failure — never a stack trace or raw redirect.
  const error = params.get("error");
  if (error !== null) {
    await mailbox.fail();
    const detail = describeError(error, params.get("error_description"));
    return html(200, page("Link not completed", "Couldn't finish linking", detail));
  }

  const code = params.get("code");
  if (!isValidCode(code)) {
    return html(400, page("Link error", "Something's off with this link",
      "This pairing link didn't carry a valid code. Start again from your dial."));
  }

  await mailbox.store(code);
  return html(200, page("Dial linked", "✓ Dial linked",
    "You can close this tab and return to your dial — it'll finish on its own."));
}

// GET /poll?state=  — the dial asks whether its code has arrived. Returns the
// code exactly once, then the mailbox is empty again.
async function handlePoll(url: URL, store: RelayStore): Promise<Response> {
  const state = url.searchParams.get("state");
  if (!isValidState(state)) {
    return json(400, { error: "bad_state" });
  }
  const result = await store.mailbox(state).take();
  switch (result.kind) {
    case "ratelimited":
      return json(429, { status: "slow_down" }, { "Retry-After": "1" });
    case "code":
      return json(200, { code: result.code });
    case "pending":
      return json(200, { status: "pending" });
  }
}

// Map Orion's OAuth error code to a sentence. page() escapes the returned text,
// so a hostile or odd `error_description` can never break out into markup.
function describeError(error: string, description: string | null): string {
  const known: Record<string, string> = {
    access_denied: "The request was declined in your Orion account. Start again from your dial to try once more.",
    invalid_request: "Orion rejected the pairing request. Start again from your dial.",
    server_error: "Orion hit a problem completing the link. Give it a moment, then start again from your dial.",
    temporarily_unavailable: "Orion is briefly unavailable. Wait a moment, then start again from your dial.",
  };
  // Own-property guard: `known[error]` would otherwise resolve `__proto__`,
  // `constructor`, etc. to inherited Object.prototype members.
  if (Object.hasOwn(known, error)) {
    return known[error];
  }
  // Unknown code: show it (capped) so a report is possible. Slice the raw input
  // before page() escapes it, so a cut never lands mid-entity.
  const shown = error.slice(0, 120);
  const extra = description ? " (" + description.slice(0, 200) + ")" : "";
  return "Orion returned an error: " + shown + extra + ". Start again from your dial.";
}

// --- response helpers --------------------------------------------------------

const HTML_SECURITY_HEADERS: Record<string, string> = {
  // The success page carries the code in its own URL; keep it from leaking
  // onward in a Referer header, and lock the page down — it loads nothing.
  "Referrer-Policy": "no-referrer",
  "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'",
  "X-Content-Type-Options": "nosniff",
  "Cache-Control": "no-store",
};

function html(status: number, body: string): Response {
  return new Response(body, {
    status,
    headers: { "Content-Type": "text/html; charset=utf-8", ...HTML_SECURITY_HEADERS },
  });
}

function json(status: number, body: unknown, extra?: Record<string, string>): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store", ...extra },
  });
}

function text(status: number, body: string): Response {
  return new Response(body, {
    status,
    headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" },
  });
}

function escapeHtml(s: string): string {
  return s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

// A small self-contained page — no external CSS, fonts, or scripts, so it
// renders instantly and the strict CSP above allows it. It escapes `title`,
// `heading`, and `body` itself, so callers pass plain text (never pre-escaped).
function page(title: string, heading: string, body: string): string {
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escapeHtml(title)}</title>
<style>
  :root { color-scheme: light dark; }
  html, body { height: 100%; margin: 0; }
  body {
    display: flex; align-items: center; justify-content: center;
    font: 17px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    background: #f6f6f7; color: #1a1a1a; padding: 24px;
  }
  .card {
    max-width: 22rem; text-align: center;
    background: #fff; border-radius: 18px; padding: 32px 28px;
    box-shadow: 0 1px 3px rgba(0,0,0,.08), 0 8px 24px rgba(0,0,0,.06);
  }
  h1 { font-size: 1.35rem; margin: 0 0 .5rem; }
  p  { margin: 0; color: #55565a; }
  @media (prefers-color-scheme: dark) {
    body { background: #111214; color: #f2f2f3; }
    .card { background: #1c1d20; box-shadow: none; }
    p { color: #a8a9ad; }
  }
</style>
</head>
<body>
  <main class="card">
    <h1>${escapeHtml(heading)}</h1>
    <p>${escapeHtml(body)}</p>
  </main>
</body>
</html>`;
}
