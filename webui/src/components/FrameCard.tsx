import { useState } from 'react'
import { Ban, Bird, RotateCcw, Sparkles, Tag } from 'lucide-react'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import { Input } from '@/components/ui/input'
import { classifyWithAI, setState, type Img, type State } from '@/lib/api'

export function stateBadge(img: Img) {
  switch (img.state) {
    case 'no_bird':
      return <Badge variant="secondary" className="text-red-700">no bird</Badge>
    case 'bird_identified':
      return <Badge className="bg-emerald-600 text-white">bird</Badge>
    case 'species_identified':
      return (
        <Badge className="bg-emerald-700 text-white">
          {img.species ?? 'bird'}
        </Badge>
      )
    default:
      return <Badge variant="outline">unclassified</Badge>
  }
}

const fileUrl = (path: string) =>
  '/files/' + path.split('/').map(encodeURIComponent).join('/')

export default function FrameCard({
  img,
  onChanged,
  onError,
}: {
  img: Img
  onChanged: () => void
  onError: (msg: string) => void
}) {
  const [busy, setBusy] = useState(false)
  const [speciesOpen, setSpeciesOpen] = useState(false)
  const [species, setSpecies] = useState(img.species ?? '')

  const act = async (fn: () => Promise<unknown>) => {
    setBusy(true)
    try {
      await fn()
      onChanged()
    } catch (e) {
      onError(String(e instanceof Error ? e.message : e))
    } finally {
      setBusy(false)
    }
  }

  const mark = (state: State) => act(() => setState(img.path, state))

  return (
    <div className="rounded-lg border bg-card text-card-foreground shadow-sm">
      <img
        src={fileUrl(img.path)}
        alt={img.path}
        loading="lazy"
        className="aspect-[4/3] w-full rounded-t-lg object-cover"
      />
      <div className="flex flex-col gap-2 p-3">
        <div className="flex flex-wrap items-center gap-2">
          {stateBadge(img)}
          {img.source && (
            <span className="text-xs text-muted-foreground">
              via {img.source === 'kimi' || img.source === 'ai' ? 'AI' : img.source}
              {img.confidence != null &&
                ` · ${(img.confidence * 100).toFixed(0)}%`}
            </span>
          )}
        </div>
        {img.note && (
          <p className="text-xs text-muted-foreground">{img.note}</p>
        )}
        <div className="flex gap-1">
          <Button size="sm" variant="outline" className="px-2" disabled={busy} onClick={() => mark('no_bird')} title="No bird" aria-label="No bird">
            <Ban className="size-3.5" />
          </Button>
          <Button size="sm" variant="outline" className="px-2" disabled={busy} onClick={() => mark('bird_identified')} title="Bird" aria-label="Bird">
            <Bird className="size-3.5" />
          </Button>
          <Button
            size="sm"
            variant="outline"
            className="px-2"
            disabled={busy}
            title="Mark species"
            aria-label="Mark species"
            onClick={() => {
              setSpecies(img.species ?? '')
              setSpeciesOpen(true)
            }}
          >
            <Tag className="size-3.5" />
          </Button>
          <Button size="sm" className="px-2" disabled={busy} onClick={() => act(() => classifyWithAI(img.path))} title="AI classify" aria-label="AI classify">
            <Sparkles className="size-3.5" />
          </Button>
          {img.state !== 'unclassified' && (
            <Button size="sm" variant="ghost" className="px-2" disabled={busy} onClick={() => mark('unclassified')} title="Clear to unclassified" aria-label="Clear to unclassified">
              <RotateCcw className="size-3.5" />
            </Button>
          )}
        </div>
        <p className="truncate text-[10px] text-muted-foreground" title={img.path}>
          {img.path}
        </p>
      </div>

      <Dialog open={speciesOpen} onOpenChange={setSpeciesOpen}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>Mark species identified</DialogTitle>
          </DialogHeader>
          <form
            onSubmit={(e) => {
              e.preventDefault()
              act(async () => {
                await setState(img.path, 'species_identified', species.trim())
                setSpeciesOpen(false)
              })
            }}
            className="flex flex-col gap-3"
          >
            <Input
              autoFocus
              placeholder="e.g. house sparrow"
              value={species}
              onChange={(e) => setSpecies(e.target.value)}
            />
            {img.note && (
              <p className="text-xs text-muted-foreground">
                AI note: {img.note}
              </p>
            )}
            <DialogFooter>
              <Button type="button" variant="ghost" onClick={() => setSpeciesOpen(false)}>
                cancel
              </Button>
              <Button type="submit" disabled={!species.trim()}>
                save
              </Button>
            </DialogFooter>
          </form>
        </DialogContent>
      </Dialog>
    </div>
  )
}
