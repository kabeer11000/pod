// Headless walkthrough of the Pod Board example:
//   bun demo.ts            (POD_URL defaults to ws://localhost:3000/webchannel)
//
// Alice watches the "launch" board live while Bob edits it, and Carol works on
// a different board (a different Pod namespace) that Alice must never see.
import { Board, taskId, type Task } from "./board.ts";
import type { PodDoc } from "./pod-client.ts";

const url = process.env.POD_URL ?? "ws://localhost:3000/webchannel";
const boardName = `launch-${Date.now().toString(36)}`; // fresh namespace per run

const alice = await Board.open(url, boardName, "alice");
const bob = await Board.open(url, boardName, "bob");
const carol = await Board.open(url, `${boardName}-other`, "carol");

let view: PodDoc<Task>[] = [];
let updates = 0;
await alice.watch((tasks) => {
  view = tasks;
  updates++;
  render();
});

function render() {
  console.log(`  alice sees (update #${updates}):`);
  if (view.length === 0) console.log("    (empty)");
  for (const t of view) console.log(`    [${t.data.done ? "x" : " "}] ${t.data.title.padEnd(28)} by ${t.data.author}  v${t._v}`);
}

async function until(label: string, pred: () => boolean) {
  const deadline = Date.now() + 3000;
  while (!pred()) {
    if (Date.now() > deadline) throw new Error(`timed out waiting for: ${label}`);
    await Bun.sleep(5);
  }
}

function step(text: string) {
  console.log(`\n> ${text}`);
}

step("bob adds three tasks");
const ids: string[] = [];
for (const title of ["Write release notes", "Tag v1.0", "Announce on the blog"]) ids.push(await bob.add(title));
await until("3 tasks", () => view.length === 3);

step("alice ticks off 'Tag v1.0'");
await alice.toggle(ids[1]!, true);
await until("toggle", () => view.find((t) => taskId(t) === ids[1])?.data.done === true);

step("bob renames the blog task (merge write)");
await bob.rename(ids[2]!, "Announce on the blog + socials");
await until("rename", () => view.some((t) => t.data.title.endsWith("socials")));

step("carol adds a task on a different board (namespace)");
await carol.add("Secret roadmap item");
const carolsView = await carol.client.query("tasks");
await Bun.sleep(100);
if (view.some((t) => t.data.author === "carol")) throw new Error("namespace leak!");
console.log(`  carol's board has ${carolsView.docs.length} task; alice still sees ${view.length} — namespaces are isolated`);

step("bob completes everything at once (MUTATE_MANY)");
const completed = await bob.completeAll();
console.log(`  server updated ${completed} documents atomically`);
await until("all done", () => view.length === 3 && view.every((t) => t.data.done));

step("alice clears completed tasks (QUERY + DELETE)");
const removed = await alice.clearCompleted();
console.log(`  deleted ${removed} documents`);
await until("empty", () => view.length === 0);

const first = await bob.client.read(`tasks/${ids[0]}`);
if (first !== null) throw new Error("expected task to be gone");

console.log(`\nDemo finished: ${updates} live updates delivered to alice. ✔`);
for (const b of [alice, bob, carol]) b.close();
process.exit(0);
