import { useEffect, useState } from 'react'
import { Ban, Bird, RotateCcw, Sparkles, Tag, Trash2, X } from 'lucide-react'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import { Input } from '@/components/ui/input'
import { bulk, classifyBulk } from '@/lib/api'

// Fixed floating bar shown while frames are selected: the same manual
// controls the per-card row offers (no bird / bird / species / clear / AI),
// plus bulk delete and selection management. Destructive delete arms on the
// first click and needs a second click within 3 s, like the card's delete.
export default function SelectionBar({
  paths,
  shownCount,
  onApplied,
  onSelectAll,
  onClearSelection,
  onError,
}: {
  paths: string[]
  shownCount: number
  onApplied: () => void
  onSelectAll: () => void
  onClearSelection: () => void
  onError: (msg: string) => void
}) {
  const [busy, setBusy] = useState<string | null>(null)
  const [armedDelete, setArmedDelete] = useState(false)
  const [speciesOpen, setSpeciesOpen] = useState(false)
  const [species, setSpecies] = useState('')

  useEffect(() => {
    if (!armedDelete) return
    const t = setTimeout(() => setArmedDelete(false), 3000)
    return () => clearTimeout(t)
  }, [armedDelete])

  const run = (label: string, fn: () => Promise<{ failed: unknown }>) => {
    setBusy(label)
    void (async () => {
      try {
        const r = await fn()
        const failed = Array.isArray(r.failed) ? r.failed.length : Number(r.failed ?? 0)
        if (failed > 0) onError(`${failed} of ${paths.length} failed`)
        onApplied()
      } catch (e) {
        onError(String(e instanceof Error ? e.message : e))
      } finally {
        setBusy(null)
        setArmedDelete(false)
      }
    })()
  }

  const disabled = busy !== null
  const n = paths.length

  return (
    <>
      <div className="pointer-events-none fixed inset-x-0 bottom-3 z-40 flex justify-center px-2">
        <div className="pointer-events-auto flex max-w-full flex-wrap items-center justify-center gap-1.5 rounded-xl border bg-background/95 px-3 py-2 shadow-lg backdrop-blur">
          <span className="mr-1 text-sm font-medium tabular-nums">
            {busy ? `${busy}…` : `${n} selected`}
          </span>
          <Button size="sm" variant="outline" disabled={disabled} title="Mark selected as no bird" aria-label="Bulk: no bird" onClick={() => run('no bird', () => bulk(paths, { op: 'set_state', state: 'no_bird' }))}>
            <Ban className="size-3.5" /> <span className="hidden sm:inline">no bird</span>
          </Button>
          <Button size="sm" variant="outline" disabled={disabled} title="Mark selected as bird" aria-label="Bulk: bird" onClick={() => run('bird', () => bulk(paths, { op: 'set_state', state: 'bird_identified' }))}>
            <Bird className="size-3.5" /> <span className="hidden sm:inline">bird</span>
          </Button>
          <Button size="sm" variant="outline" disabled={disabled} title="Mark selected as a species" aria-label="Bulk: species" onClick={() => { setSpecies(''); setSpeciesOpen(true) }}>
            <Tag className="size-3.5" /> <span className="hidden sm:inline">species</span>
          </Button>
          <Button size="sm" variant="outline" disabled={disabled} title="Clear selected labels to unclassified" aria-label="Bulk: clear to unclassified" onClick={() => run('clear', () => bulk(paths, { op: 'set_state', state: 'unclassified' }))}>
            <RotateCcw className="size-3.5" /> <span className="hidden sm:inline">clear</span>
          </Button>
          <Button size="sm" disabled={disabled} title="Run AI on selected frames" aria-label="Bulk: AI classify" onClick={() => run('AI', () => classifyBulk(paths))}>
            <Sparkles className="size-3.5" /> <span className="hidden sm:inline">AI</span>
          </Button>
          <Button size="sm" variant={armedDelete ? 'destructive' : 'outline'} disabled={disabled} aria-label={armedDelete ? `Confirm bulk delete ${n}` : 'Bulk delete'} title={armedDelete ? `Click again to delete ${n} frames for good` : 'Delete selected frames (file and all labels)'} onClick={() => { if (!armedDelete) { setArmedDelete(true); return } run('delete', () => bulk(paths, { op: 'delete' })) }}>
            <Trash2 className="size-3.5" /> <span className={armedDelete ? '' : 'hidden sm:inline'}>{armedDelete ? `delete ${n}?` : 'delete'}</span>
          </Button>
          <span className="mx-1 h-5 w-px bg-border" aria-hidden />
          {shownCount > n && (
            <Button size="sm" variant="ghost" disabled={disabled} onClick={onSelectAll} aria-label="Select all shown frames">
              select all {shownCount}
            </Button>
          )}
          <Button size="sm" variant="ghost" disabled={disabled} onClick={onClearSelection} aria-label="Clear selection">
            <X className="size-3.5" />
          </Button>
        </div>
      </div>

      <Dialog open={speciesOpen} onOpenChange={setSpeciesOpen}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>Mark {n} frames as species identified</DialogTitle>
          </DialogHeader>
          <form
            onSubmit={(e) => {
              e.preventDefault()
              setSpeciesOpen(false)
              run('species', () => bulk(paths, { op: 'set_state', state: 'species_identified', species: species.trim() }))
            }}
            className="flex flex-col gap-3"
          >
            <Input
              autoFocus
              placeholder="e.g. house sparrow"
              value={species}
              onChange={(e) => setSpecies(e.target.value)}
            />
            <DialogFooter>
              <Button type="button" variant="ghost" onClick={() => setSpeciesOpen(false)}>
                cancel
              </Button>
              <Button type="submit" disabled={!species.trim()}>
                apply to {n}
              </Button>
            </DialogFooter>
          </form>
        </DialogContent>
      </Dialog>
    </>
  )
}
