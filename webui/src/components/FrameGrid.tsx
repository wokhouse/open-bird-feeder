import { useEffect, useRef, useState } from 'react'
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
}: {
  items: Img[]
  onChanged: () => void
  onError: (msg: string) => void
}) {
  const [cols, setCols] = useState(readCols)
  useEffect(() => {
    const onResize = () => setCols(readCols())
    window.addEventListener('resize', onResize)
    return () => window.removeEventListener('resize', onResize)
  }, [])

  const listRef = useRef<HTMLDivElement | null>(null)
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
              />
            ))}
          </div>
        </div>
      ))}
    </div>
  )
}
