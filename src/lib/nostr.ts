import { SimplePool, Event, Filter } from 'nostr-tools'
import { getDefaultRelays } from './relays'

export class NostrClient {
  private pool: SimplePool
  private relays: string[]

  constructor(relays: string[] = getDefaultRelays()) {
    this.pool = new SimplePool()
    this.relays = relays
  }

  // Placeholder method - to be implemented with proper nostr-tools usage
  async getEvents(filters: Filter[], timeout = 5000): Promise<Event[]> {
    // This is a basic implementation - in a real app you'd use proper nostr-tools methods
    console.log('Getting events with filters:', filters)
    return []
  }

  // Placeholder method - to be implemented with proper nostr-tools usage  
  async publishEvent(event: Event): Promise<void> {
    console.log('Publishing event:', event)
    // Implementation would use this.pool.publish
  }

  close() {
    this.pool.close(this.relays)
  }
}

// Create a client instance that can be imported
let clientInstance: NostrClient | null = null

export const getNostrClient = (): NostrClient => {
  if (!clientInstance) {
    clientInstance = new NostrClient()
  }
  return clientInstance
}