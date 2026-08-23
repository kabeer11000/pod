import { pod, collection, doc, setDoc, getDoc } from "@pod-db/node";

function render(text: string) {
    const output = document.getElementById("output");
    if (output) output.textContent += text + "\n";
    console.log(text);
}

async function main() {
    const db = pod({
        server: {
            uri: "ws://localhost:3000/webchannel",
            token: "",
        },
        config: {
            upgradeToBackgroundSync: false,
        },
        cache: {
            adapter: undefined as any,
        },
    });

    const users = collection(db, "users");
    const alice = doc(users, "alice");

    render(`Writing document at ${alice._path.join("/")} ...`);
    await setDoc(alice, { name: "Alice", role: "admin" });
    render("Write acknowledged.");

    render(`Reading document at ${alice._path.join("/")} ...`);
    const snapshot = await getDoc(alice);
    render(`exists=${snapshot.exists} data=${JSON.stringify(snapshot.data)}`);
}

main().catch((err) => render(`Error: ${err instanceof Error ? err.message : String(err)}`));
