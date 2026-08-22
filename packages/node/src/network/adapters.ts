// export class ImplementableNetworkAdapter {
//     constructor() {}
//     onNetworkUpdate(updateFn: (change: "CONNECTED" | "DISCONNECTED") => any) {}
// }
// export class BrowserNetwork implements ImplementableNetworkAdapter {
//     state = {
//         online: typeof window !== "undefined" ? window.navigator.onLine : true,
//     };
//     private listeners: Array<(change: "CONNECTED" | "DISCONNECTED") => any> =
//         [];
//     private connectionCheckInterval?: NodeJS.Timeout;

//     constructor() {}

//     onNetworkUpdate(
//         updateFn: (change: "CONNECTED" | "DISCONNECTED") => any,
//     ): void {
//         this.listeners.push(updateFn);

//         if (typeof window === "undefined") {
//             // Node.js environment - assume connected
//             updateFn("CONNECTED");
//             return;
//         }

//         const offlineHandler = () => {
//             this.state.online = false;
//             this.listeners.forEach((fn) => fn("DISCONNECTED"));
//         };

//         const onlineHandler = () => {
//             this.state.online = true;
//             this.listeners.forEach((fn) => fn("CONNECTED"));
//         };

//         window.addEventListener("offline", offlineHandler);
//         window.addEventListener("online", onlineHandler);

//         // Additional connection checks for more reliability
//         if (typeof window.fetch !== "undefined") {
//             const checkConnection = async () => {
//                 try {
//                     await fetch("/", { method: "HEAD", cache: "no-cache" });
//                     if (!this.state.online) {
//                         this.state.online = true;
//                         this.listeners.forEach((fn) => fn("CONNECTED"));
//                     }
//                 } catch {
//                     if (this.state.online) {
//                         this.state.online = false;
//                         this.listeners.forEach((fn) => fn("DISCONNECTED"));
//                     }
//                 }
//             };

//             // Check connection every 30 seconds
//             this.connectionCheckInterval = setInterval(checkConnection, 30000);
//         }

//         // Initial state
//         updateFn(this.state.online ? "CONNECTED" : "DISCONNECTED");
//     }

//     removeListener(
//         updateFn: (change: "CONNECTED" | "DISCONNECTED") => any,
//     ): void {
//         const index = this.listeners.indexOf(updateFn);
//         if (index > -1) {
//             this.listeners.splice(index, 1);
//         }
//     }

//     isOnline(): boolean {
//         return this.state.online;
//     }

//     destroy(): void {
//         if (this.connectionCheckInterval) {
//             clearInterval(this.connectionCheckInterval);
//             this.connectionCheckInterval = undefined;
//         }
//         this.listeners = [];
//     }
// }
