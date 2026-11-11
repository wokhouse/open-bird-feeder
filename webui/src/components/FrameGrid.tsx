import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { useWindowVirtualizer } from '@tanstack/react-virtual'
import FrameCard from '@/components/FrameCard'
import type { Img } from '@/lib/api'

const GAP = 16 // grid gap-4

// mirror the viewport breakpoints the plain grid used: 1 col, then 2/3/4
// at sm/lg/xl — the only way row slicing can match what fits on screen
function readCols() {
  const vw = window.innerWidth
  return vw >= 1280 ? 4 : vw >= 1024 ? 3 : vw >= 640 ? 2 : 1
}

// rough row height: aspect-[4/3] image + meta strip; measureElement
// replaces this per real row as it mounts
function estimateRow(cols: number) {
  const vw = Math.min(window.innerWidth, 1280) // max-w-7xl
  const pad = window.innerWidth >= 768 ? 24 : 16 // p-4 / md:p-6
  const w = (vw - pad * 2 - GAP * (cols - 1)) / cols
  return w * 0.75 + 150
}

// windowed rendering: only rows near the viewport exist in the DOM, so a
// multi-thousand-frame list scrolls like a short one. Transparent to the
// user: no pages, no sentinel, sort and URL state live above it untouched.
export function FrameGrid({
  items,
  onChanged,
  onError,
  selected,
  onToggleSelect,
}: {
  items: Img[]
  onChanged: () => void
  onError: (msg: string) => void
  selected: Set<string>
  onToggleSelect: (path: string, shift: boolean) => void
}) {
  const [cols, setCols] = useState(readCols)
  const colsRef = useRef(cols)
  const listRef = useRef<HTMLDivElement | null>(null)

  // A zoom or rotation that crosses a readCols() breakpoint re-slices every row,
  // and the pixel scroll offset then points at different frames — the view the
  // user was looking at is lost. Track the first visible card every frame
  // (rAF sample = what the user actually saw, before any reflow moves it), so
  // the layout effect below can put that same card back at the same position.
  useEffect(() => {
    const viewRef = { current: null as { path: string; top: number } | null }
    let raf = 0
    const sample = () => {
      viewRef.current = null
      const els = listRef.current?.querySelectorAll<HTMLElement>('[data-frame-path]')
      for (const el of els ?? []) {
        const top = el.getBoundingClientRect().top
        if (top >= 0 && top < window.innerHeight) {
          viewRef.current = { path: el.dataset.framePath as string, top }
          break
        }
      }
      raf = requestAnimationFrame(sample)
    }
    raf = requestAnimationFrame(sample)
    const onResize = () => {
      const next = readCols()
      if (next !== colsRef.current && viewRef.current) {
        anchorRef.current = viewRef.current
      }
      colsRef.current = next
      setCols(next)
    }
    window.addEventListener('resize', onResize)
    return () => {
      cancelAnimationFrame(raf)
      window.removeEventListener('resize', onResize)
    }
  }, [])

  // After the new cols commit: scroll so the anchor card sits at its old
  // viewport offset. The card may be outside the freshly-mounted virtual
  // window, so step toward its row in index space (estimate error cannot
  // compound) and fine-tune from the real rect once it mounts.
  const anchorRef = useRef<{ path: string; top: number } | null>(null)
  const itemsRef = useRef(items)
  itemsRef.current = items
  useLayoutEffect(() => {
    const a = anchorRef.current
    if (!a) return
    anchorRef.current = null
    const idx = itemsRef.current.findIndex((it) => it.path === a.path)
    if (idx < 0) return
    const targetRow = Math.floor(idx / cols)
    const pitch = estimateRow(cols) + GAP
    const restore = (tries: number) => {
      const el = listRef.current?.querySelector<HTMLElement>(
        `[data-frame-path="${CSS.escape(a.path)}"]`,
      )
      if (el) {
        // Chrome re-measures freshly-mounted rows and applies its own scroll
        // anchoring after the swap, both of which shift content later — keep
        // re-correcting from the real rect for a bounded window after the
        // swap, not just until the first zero delta.
        const d = el.getBoundingClientRect().top - a.top
        if (Math.abs(d) > 1) window.scrollBy(0, d)
      } else {
        const rows = [...(listRef.current?.querySelectorAll<HTMLElement>('[data-index]') ?? [])]
          .map((r) => Number(r.dataset.index))
          .filter((n) => Number.isFinite(n))
        if (rows.length) {
          const lo = Math.min(...rows)
          const hi = Math.max(...rows)
          if (targetRow < lo) window.scrollBy(0, -(lo - targetRow + 3) * pitch)
          else if (targetRow > hi) window.scrollBy(0, (targetRow - hi + 3) * pitch)
        }
      }
      if (tries < 45) requestAnimationFrame(() => restore(tries + 1))
    }
    restore(0)
  }, [cols])
  const scrollMargin =
    (listRef.current?.getBoundingClientRect().top ?? 0) + window.scrollY

  const virtualizer = useWindowVirtualizer({
    count: Math.ceil(items.length / cols),
    gap: GAP,
    overscan: 3,
    estimateSize: () => estimateRow(cols),
    // rows keep the height they were measured at across data reloads;
    // key by the row's first frame so sorting re-keys cleanly
    getItemKey: (i) => items[i * cols]?.path ?? `${cols}:${i}`,
    scrollMargin,
  })

  return (
    <div
      ref={listRef}
      style={{ height: virtualizer.getTotalSize(), position: 'relative' }}
    >
      {virtualizer.getVirtualItems().map((row) => (
        <div
          key={row.key}
          data-index={row.index}
          ref={virtualizer.measureElement}
          style={{
            position: 'absolute',
            top: 0,
            left: 0,
            width: '100%',
            transform: `translateY(${row.start - virtualizer.options.scrollMargin}px)`,
          }}
        >
          <div
            className="grid"
            style={{
              gap: GAP,
              gridTemplateColumns: `repeat(${cols}, minmax(0, 1fr))`,
            }}
          >
            {items.slice(row.index * cols, (row.index + 1) * cols).map((img) => (
              <FrameCard
                key={img.path}
                img={img}
                onChanged={onChanged}
                onError={onError}
                selected={selected.has(img.path)}
                onToggleSelect={(shift) => onToggleSelect(img.path, shift)}
              />
            ))}
          </div>
        </div>
      ))}
    </div>
  )
}
