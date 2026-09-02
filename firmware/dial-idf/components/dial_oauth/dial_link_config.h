#pragma once

/*
 * Hosted PKCE code-relay ("mailbox") configuration.
 *
 * Linking used to redirect the phone back to an HTTP server on the dial ITSELF
 * (http://orion-dial-xxxx.local/callback), which required the phone to reach
 * the dial INBOUND on the LAN. That is impossible on cellular, on guest /
 * client-isolated SSIDs, and on weak/roamed Wi-Fi (seen live at -79 dBm with
 * power-save on: 100% inbound packet loss) — so linking just never completed,
 * leaving an infinite loader. RFC 8628 device grant would be the textbook fix
 * but Orion's server does not support it (confirmed).
 *
 * The relay moves the OAuth redirect off the dial to a tiny hosted "mailbox":
 * Orion redirects the phone to <RELAY>/cb (outbound internet, always
 * reachable), the relay stashes the single-use, PKCE-bound auth code keyed by
 * `state`, and the dial COLLECTS it by polling <RELAY>/poll?state=… (outbound
 * HTTPS — the same direction the MCP client already talks, which we know
 * works). Tokens NEVER touch the relay: the dial does the PKCE token exchange
 * directly with Orion. The relay only ever holds a short-lived, single-use code
 * that is useless without the dial's verifier, which never leaves the dial.
 *
 * ORION_DIAL_RELAY_BASE is the relay's origin, no trailing slash. It is a
 * single stable constant for the WHOLE FLEET — the DCR redirect_uri, the QR's
 * authorize URL, and the token exchange all derive from it — so every dial
 * converges on the same registered client instead of a per-device .local/IP
 * URI.
 *
 * Set to the deployed relay origin (relay/, Cloudflare Worker) — see the relay
 * deploy runbook. The relay's edge-cert root CA must ALSO be present in
 * orion_root_ca.pem, or the dial's HTTPS poll fails TLS verification (respect
 * the cert-sentinel CI). Cloudflare's *.workers.dev edge chains to Google Trust
 * Services / ISRG (Let's Encrypt) roots, both already in the bundle.
 */
#define ORION_DIAL_RELAY_BASE "https://orion-dial-relay.chris-meyer023.workers.dev"

// The OAuth redirect_uri: presented in DCR, in the authorize URL/QR, and in the
// token exchange — all three must be byte-identical, so they all come from
// here. Orion validated an https://…/cb redirect_uri live (307 -> /login with
// it preserved, not "invalid redirect_uri").
#define ORION_DIAL_RELAY_REDIRECT ORION_DIAL_RELAY_BASE "/cb"
