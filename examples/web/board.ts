// Task board domain logic shared by the browser app and the CLI demo.
//
// Data model (inside the board's namespace):
//   tasks/<id> = { title, done, createdAt, author }
import { PodClient, type PodDoc } from "./pod-client.ts";

export interface Task {
  title: string;
  done: boolean;
  createdAt: number;
  author: string;
}

export const TASKS = "tasks";

export class Board {
  constructor(
    readonly client: PodClient,
    readonly author: string,
  ) {}

  static async open(serverUrl: string, board: string, author: string): Promise<Board> {
    return new Board(await PodClient.connect(serverUrl, board), author);
  }

  /** Live, ordered view of every task on the board. */
  watch(onChange: (tasks: PodDoc<Task>[]) => void) {
    return this.client.subscribe<Task>(TASKS, { orderBy: [{ field: "createdAt" }] }, (docs) => onChange(docs));
  }

  async add(title: string): Promise<string> {
    const id = crypto.randomUUID();
    await this.client.write(`${TASKS}/${id}`, {
      title,
      done: false,
      createdAt: Date.now(),
      author: this.author,
    } satisfies Task);
    return id;
  }

  toggle(id: string, done: boolean) {
    return this.client.write(`${TASKS}/${id}`, { done }, true);
  }

  rename(id: string, title: string) {
    return this.client.write(`${TASKS}/${id}`, { title }, true);
  }

  remove(id: string) {
    return this.client.delete(`${TASKS}/${id}`);
  }

  /** Marks every open task as done in one atomic server-side operation. */
  completeAll() {
    return this.client.mutateMany(TASKS, {
      where: [{ field: "done", op: "EQUAL", value: false }],
      set: { done: true },
    });
  }

  /** Deletes every completed task. */
  async clearCompleted(): Promise<number> {
    const { docs } = await this.client.query<Task>(TASKS, {
      where: [{ field: "done", op: "EQUAL", value: true }],
      project: [],
    });
    let n = 0;
    for (const d of docs) n += await this.client.delete(d.path);
    return n;
  }

  close() {
    this.client.close();
  }
}

export const taskId = (doc: PodDoc) => doc.path.split("/").pop()!;
