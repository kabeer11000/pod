// Browser entry point for the Pod Board example.
//   ?board=<namespace>      which board (Pod namespace) to open, default "demo"
//   ?server=<ws url>        Pod server, default ws://localhost:3000/webchannel
import { Board, taskId } from "./board.ts";

const params = new URLSearchParams(location.search);
const boardName = params.get("board") ?? "demo";
const server = params.get("server") ?? "ws://localhost:3000/webchannel";
const author = `guest-${Math.floor(Math.random() * 1000)}`;

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const list = $<HTMLUListElement>("tasks");
const status = $<HTMLSpanElement>("status");
$("board").textContent = boardName;
$("server").textContent = server;

function setStatus(text: string, live: boolean) {
  status.textContent = text;
  status.className = live ? "live" : "down";
}

async function main() {
  let board: Board;
  try {
    board = await Board.open(server, boardName, author);
  } catch (e) {
    setStatus("offline — is the Pod server running?", false);
    list.innerHTML = `<li class="empty">Could not reach ${server}</li>`;
    return;
  }
  setStatus(`live as ${author}`, true);
  board.client.onClose = () => setStatus("disconnected", false);

  await board.watch((tasks) => {
    list.replaceChildren();
    if (tasks.length === 0) {
      list.innerHTML = `<li class="empty">No tasks yet</li>`;
    }
    for (const t of tasks) {
      const li = document.createElement("li");
      li.className = t.data.done ? "done" : "";

      const box = document.createElement("input");
      box.type = "checkbox";
      box.checked = t.data.done;
      box.onchange = () => board.toggle(taskId(t), box.checked);

      const title = document.createElement("span");
      title.className = "title";
      title.textContent = t.data.title;

      const meta = document.createElement("span");
      meta.className = "meta";
      meta.textContent = `${t.data.author} · v${t._v}`;

      const del = document.createElement("button");
      del.className = "x";
      del.title = "Delete";
      del.textContent = "✕";
      del.onclick = () => board.remove(taskId(t));

      li.append(box, title, meta, del);
      list.append(li);
    }
    const open = tasks.filter((t) => !t.data.done).length;
    $("count").textContent = `${open} open · ${tasks.length - open} done`;
  });

  $<HTMLFormElement>("add").onsubmit = async (e) => {
    e.preventDefault();
    const input = $<HTMLInputElement>("title");
    const value = input.value.trim();
    if (!value) return;
    input.value = "";
    await board.add(value);
  };
  $("complete-all").onclick = () => board.completeAll();
  $("clear").onclick = () => board.clearCompleted();
}

void main();
