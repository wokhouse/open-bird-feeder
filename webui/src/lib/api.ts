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
  note: string | null
  box: [number, number, number, number] | null // AI bbox, normalized 0..1 [x1,y1,x2,y2]
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
