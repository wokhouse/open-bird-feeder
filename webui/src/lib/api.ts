export type State =
  | 'unclassified'
  | 'no_bird'
  | 'bird_identified'
  | 'species_identified'

export interface Img {
  path: string
  state: State
  species: string | null
  source: string | null // manual | ai (legacy rows: 'kimi')
  confidence: number | null
  p_bird: number | null // raw P(bird) from the newest AI verdict (null: manual/legacy/unclassified)
  note: string | null
  box: [number, number, number, number] | null // AI bbox, normalized 0..1 [x1,y1,x2,y2]
  taken_at: string | null // server-local wall clock, e.g. 2026-11-01T19:27:45
  device?: string | null // newest servers send it; deviceOf() parses the path otherwise
}

export interface Stats {
  total: number
  unclassified: number
  no_bird: number
  bird_identified: number
  species_identified: number
}

async function j(resp: Response) {
  if (!resp.ok) {
    let detail = ''
    try {
      detail = (await resp.json()).detail ?? ''
    } catch {
      /* non-JSON error body */
    }
    throw new Error(detail || `HTTP ${resp.status}`)
  }
  return resp.json()
}

const post = (url: string, body: unknown) =>
  fetch(url, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
  }).then(j)

export const getImages = (): Promise<Img[]> => fetch('/api/images').then(j)
export const getStats = (): Promise<Stats> => fetch('/api/stats').then(j)
export const setState = (path: string, state: State, species?: string) =>
  post('/api/set_state', { path, state, species })
export const deletePhoto = (path: string) => post('/api/delete_photo', { path })
export const classifyWithAI = (path: string) => post('/api/classify', { path })
export interface BatchResult {
  classified: number
  failed: number
  error: string | null
  results: unknown[]
}
export const classifyPending = (limit: number): Promise<BatchResult> =>
  post('/api/classify_pending', { limit })

export interface BulkResult {
  ok: boolean
  done: number
  failed: { path: string; error: string }[]
}
export const bulk = (
  paths: string[],
  opts: { op: 'delete' } | { op: 'set_state'; state: State; species?: string },
): Promise<BulkResult> => post('/api/bulk', { paths, ...opts })
export const classifyBulk = (paths: string[]): Promise<BatchResult> =>
  post('/api/classify_bulk', { paths })

// photo/<date>/<HHMMSS>_<device>[_2].jpg — the storage path has always carried
// the device; same parse as the ingest side (CAP_PARSE)
export function deviceOf(img: Img): string | null {
  if (img.device) return img.device
  const m = /\d{6}_([\w.-]+?)(?:_\d+)?\.jpg$/.exec(img.path)
  return m ? m[1] : null
}

export interface LiveArm {
  ok: boolean
  device: string
  seconds: number
}
// while the window is open the device is told to stream (X-Live on its uploads)
export const armLive = (device: string, minutes = 2): Promise<LiveArm> =>
  post('/api/live_arm', { device, minutes })

// `bust` re-requests the stream: the relay itself ends after ~20 s of quiet
export const liveUrl = (device: string, bust?: number) =>
  `/live.mjpg?device=${encodeURIComponent(device)}${bust ? `&t=${bust}` : ''}`
