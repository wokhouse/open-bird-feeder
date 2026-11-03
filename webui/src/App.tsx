import { useCallback, useEffect, useRef, useState } from 'react'
import { RefreshCw, Sparkles } from 'lucide-react'
import FrameCard from '@/components/FrameCard'
import { Button } from '@/components/ui/button'
import { Tabs, TabsList, TabsTrigger } from '@/components/ui/tabs'
import {
  classifyPending,
  getImages,
  getStats,
  type Img,
  type Stats,
  type State,
} from '@/lib/api'

type Filter = 'all' | State

const FILTERS: { value: Filter; label: string }[] = [
  { value: 'unclassified', label: 'unclassified' },
  { value: 'no_bird', label: 'no bird' },
  { value: 'bird_identified', label: 'bird' },
  { value: 'species_identified', label: 'species' },
  { value: 'all', label: 'all' },
]

const FILTER_VALUES: Filter[] = FILTERS.map((f) => f.value)

type Sort = 'newest' | 'oldest' | 'p_desc' | 'p_asc'
const SORT_VALUES: Sort[] = ['newest', 'oldest', 'p_desc', 'p_asc']
const SORTS: { value: Sort; label: string }[] = [
  { value: 'newest', label: 'newest first' },
  { value: 'oldest', label: 'oldest first' },
  { value: 'p_desc', label: 'bird p: high→low' },
  { value: 'p_asc', label: 'bird p: low→high' },
]

// active tab + sort live in the URL (?view=...&sort=...) so refresh and
// shared links land on the same state
function urlParam(name: string): string | null {
  return new URLSearchParams(window.location.search).get(name)
}

function writeUrlParam(name: string, value: string) {
  const url = new URL(window.location.href)
  url.searchParams.set(name, value)
  window.history.replaceState(null, '', url)
}

function filterFromLocation(): Filter {
  const v = urlParam('view')
  return FILTER_VALUES.includes(v as Filter) ? (v as Filter) : 'unclassified'
}

function sortFromLocation(): Sort {
  const v = urlParam('sort')
  return SORT_VALUES.includes(v as Sort) ? (v as Sort) : 'newest'
}

function errMsg(e: unknown) {
  return String(e instanceof Error ? e.message : e)
}

export default function App() {
  const [images, setImages] = useState<Img[]>([])
  const [stats, setStats] = useState<Stats | null>(null)
  const [filter, setFilter] = useState<Filter>(filterFromLocation)
  const [sort, setSort] = useState<Sort>(sortFromLocation)
  const [error, setError] = useState<string | null>(null)
  const [classifying, setClassifying] = useState(false)

  const reload = useCallback(async () => {
    try {
      const [imgs, st] = await Promise.all([getImages(), getStats()])
      setImages(imgs)
      setStats(st)
    } catch (e) {
      setError(errMsg(e))
    }
  }, [])

  useEffect(() => {
    reload()
    // auto-classification lands verdicts in the background: keep the queue live
    const t = setInterval(reload, 8000)
    return () => clearInterval(t)
  }, [reload])

  const runAIBatch = async () => {
    setClassifying(true)
    setError(null)
    try {
      const res = await classifyPending(100)
      if (res.failed > 0) {
        setError(
          `classified ${res.classified}, failed ${res.failed}: ${res.error ?? 'AI call failed'}`,
        )
      }
      await reload()
    } catch (e) {
      setError(
        errMsg(e),
      )
    } finally {
      setClassifying(false)
    }
  }

  useEffect(() => {
    const onPop = () => {
      setFilter(filterFromLocation())
      setSort(sortFromLocation())
    }
    window.addEventListener('popstate', onPop)
    return () => window.removeEventListener('popstate', onPop)
  }, [])

  const tabsRef = useRef<HTMLDivElement | null>(null)
  const [tabFade, setTabFade] = useState({ l: false, r: false })
  useEffect(() => {
    const el = tabsRef.current
    if (!el) return
    const update = () => {
      const max = el.scrollWidth - el.clientWidth
      setTabFade({ l: el.scrollLeft > 4, r: el.scrollLeft < max - 4 })
    }
    update()
    el.addEventListener('scroll', update, { passive: true })
    // watch the content child too: web-font swap can make the row overflow after mount
    const ro = new ResizeObserver(update)
    ro.observe(el)
    const first = el.firstElementChild
    if (first) ro.observe(first)
    document.fonts?.ready.then(update)
    return () => {
      el.removeEventListener('scroll', update)
      ro.disconnect()
    }
  }, [])
  const edgeFade = `linear-gradient(90deg, ${
    tabFade.l ? 'transparent 0, #000 16px' : '#000 0'
  }, ${tabFade.r ? '#000 calc(100% - 16px), transparent 100%' : '#000 100%'})`

  // paths embed date+time, so their lexical order is chronological
  const byNewest = (a: Img, b: Img) => (a.path < b.path ? 1 : a.path > b.path ? -1 : 0)
  const shown = (filter === 'all' ? images : images.filter((m) => m.state === filter))
    .slice()
    .sort((a, b) => {
      switch (sort) {
        case 'oldest':
          return -byNewest(a, b)
        case 'p_desc':
        case 'p_asc': {
          // frames without a p (manual/legacy/unclassified) always go last
          if (a.p_bird == null && b.p_bird == null) return byNewest(a, b)
          if (a.p_bird == null) return 1
          if (b.p_bird == null) return -1
          const d = sort === 'p_desc' ? b.p_bird - a.p_bird : a.p_bird - b.p_bird
          return d !== 0 ? d : byNewest(a, b)
        }
        default:
          return byNewest(a, b)
      }
    })

  return (
    <div className="mx-auto max-w-7xl p-4 md:p-6">
      <header className="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h1 className="text-lg font-semibold">open-bird-feeder review</h1>
          <p className="text-sm text-muted-foreground">
            {stats
              ? `${stats.total} frames · ${stats.unclassified} awaiting AI/review`
              : 'loading…'}
          </p>
        </div>
        <div className="flex gap-2">
          <select
            aria-label="Sort frames"
            value={sort}
            onChange={(e) => {
              const s = e.target.value as Sort
              setSort(s)
              writeUrlParam('sort', s)
            }}
            className="h-9 rounded-md border border-input bg-background px-2 text-sm"
          >
            {SORTS.map((s) => (
              <option key={s.value} value={s.value}>
                {s.label}
              </option>
            ))}
          </select>
          <Button onClick={runAIBatch} disabled={classifying}>
            <Sparkles className="size-4" />
            {classifying ? 'classifying…' : 'Bulk Classify'}
          </Button>
          <Button variant="outline" onClick={reload}>
            <RefreshCw className="size-4" />
          </Button>
        </div>
      </header>

      {error && (
        <div className="mb-4 rounded-md border border-red-300 bg-red-50 p-3 text-sm text-red-800">
          {error}
        </div>
      )}

      <Tabs value={filter} onValueChange={(v) => { const f = v as Filter; setFilter(f); writeUrlParam('view', f) }}>
        <TabsList
          ref={tabsRef}
          style={{ WebkitMask: edgeFade, mask: edgeFade }}
          className="no-scrollbar mb-4 w-full max-w-full justify-start overflow-x-auto overscroll-x-contain sm:w-fit"
        >
          {FILTERS.map((f) => (
            <TabsTrigger key={f.value} value={f.value}>
              {f.label}
              {stats && f.value !== 'all' && (
                <span className="ml-1 text-muted-foreground">
                  {stats[f.value as State]}
                </span>
              )}
            </TabsTrigger>
          ))}
        </TabsList>
      </Tabs>

      {shown.length === 0 ? (
        <p className="py-16 text-center text-sm text-muted-foreground">
          nothing here — frames land at /upload as the XIAO posts them.
        </p>
      ) : (
        <div className="grid grid-cols-1 gap-4 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-4">
          {shown.map((img) => (
            <FrameCard
              key={img.path}
              img={img}
              onChanged={reload}
              onError={setError}
            />
          ))}
        </div>
      )}
    </div>
  )
}
