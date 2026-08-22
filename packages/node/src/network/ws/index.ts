/** Pod wireprotocol definations */
interface IEvent {
    type: "READ" | "QUERY" | "WRITE" | "DELETE" | "MUTATE_MANY" | "SUBSCRIBE",
    
}
