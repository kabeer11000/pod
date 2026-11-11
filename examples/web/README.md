# Pod Board — example application

A real-time, collaborative task board built directly on the Pod wire protocol.

* Every board is its own **Pod namespace** (`?board=team-a`), so boards are
  fully isolated from each other.
* The task list is a **live subscription** (`SUBSCRIBE` ordered by
  `createdAt`); every client sees inserts, edits and deletes instantly.
* Toggling/renaming uses **merge writes**, "Complete all" is a single atomic
  **MUTATE_MANY**, and "Clear completed" is a **QUERY** + **DELETE**.

Files:

| File | Purpose |
| --- | --- |
| `pod-client.ts` | Tiny protocol client (request/response + live subscriptions) |
| `board.ts` | Task board logic shared by the web app and the demo |
| `index.html`, `app.ts` | Browser UI |
| `demo.ts` | Headless walkthrough with three users, used to verify the example |

## Run

Start Pod first (from the repo root, after building):

```bash
./build/pod            # listens on ws://localhost:3000/webchannel
```

Then:

```bash
bun install
bun run dev            # open the printed URL in two windows
bun demo.ts            # or run the scripted, headless walkthrough
```

Options: `?board=<name>` picks the board, `?server=ws://host:port/webchannel`
points the page at another Pod server. The demo reads `POD_URL`.
