import { defineWorkersConfig } from "@cloudflare/vitest-pool-workers/config";

// Runs the tests inside workerd (the real Workers runtime) with the bindings
// from wrangler.toml — so the Durable Object, its alarm, and SQLite storage
// behave as they will in production.
export default defineWorkersConfig({
  test: {
    poolOptions: {
      workers: {
        wrangler: { configPath: "./wrangler.toml" },
      },
    },
  },
});
