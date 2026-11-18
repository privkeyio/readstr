// Fired on window when another account's per-user data was wiped from this
// device, so providers holding a copy in memory (e.g. the AI config with its
// API key) reload from the now-cleared storage instead of keeping it.
export const USER_STATE_CLEARED_EVENT = 'readstr:user-state-cleared'
