import { useCallback, useEffect, useRef, useState } from 'react'
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import { armLive, liveUrl } from '@/lib/api'

const ARM_EVERY_MS = 60_000 // keeps the Pi-side 2-minute live window open
const ROTATE_MS = 20_000 // the relay ends a stream after ~20 s without frames
const RETRY_MS = 1_000

// Watch-Live player: frames come straight from the Pi's MJPEG relay, so the
// browser never reaches the ESP32; the dialog unmounts the instant it closes
export default function LivePanel({
  device,
  onClose,
  onError,
}: {
  device: string
  onClose: () => void
  onError: (msg: string) => void
}) {
  const [src, setSrc] = useState(() => liveUrl(device))
  const [live, setLive] = useState(false)
  const imgRef = useRef<HTMLImageElement | null>(null)
  const retry = useRef<number | null>(null)

  // arm on open, then keep re-arming while mounted; closing the panel lets the
  // window expire on its own
  useEffect(() => {
    const arm = () =>
      armLive(device).catch((e) =>
        onError(String(e instanceof Error ? e.message : e)),
      )
    arm()
    const t = window.setInterval(arm, ARM_EVERY_MS)
    return () => window.clearInterval(t)
  }, [device, onError])

  // a fresh request: cache-bust so the ended stream is not served from cache
  const reopen = useCallback(() => {
    if (retry.current) {
      window.clearTimeout(retry.current)
      retry.current = null
    }
    setSrc(liveUrl(device, Date.now()))
  }, [device])

  // the relay hangs up ~20 s after the device stops sending; reopen while the
  // user is still watching, which also picks up the newest buffered frame
  useEffect(() => {
    const t = window.setInterval(reopen, ROTATE_MS)
    return () => window.clearInterval(t)
  }, [reopen])

  useEffect(
    () => () => {
      if (retry.current) window.clearTimeout(retry.current)
    },
    [],
  )

  // an MJPEG image never "finishes" and its load event is not dependable, so
  // naturalWidth is the real signal that a frame has been decoded. Going blank
  // is debounced: reopening the stream briefly drops the old image, and that
  // must not flash the waiting note over a frame that is still on screen.
  useEffect(() => {
    let blank = 0
    const t = window.setInterval(() => {
      if (imgRef.current?.naturalWidth) {
        blank = 0
        setLive(true)
      } else if ((blank += 500) >= 2_000) {
        setLive(false)
      }
    }, 500)
    return () => window.clearInterval(t)
  }, [])

  return (
    <Dialog open onOpenChange={(open) => !open && onClose()}>
      <DialogContent className="sm:max-w-3xl">
        <DialogHeader>
          <DialogTitle className="flex items-center gap-2 text-sm font-medium">
            <span
              aria-hidden
              className={
                live
                  ? 'size-1.5 rounded-full bg-emerald-500'
                  : 'size-1.5 animate-pulse rounded-full bg-muted-foreground'
              }
            />
            Live
            <span className="truncate text-xs font-normal text-muted-foreground">
              {device}
            </span>
          </DialogTitle>
        </DialogHeader>
        <div className="relative aspect-[4/3] max-h-[70vh] w-full overflow-hidden rounded-md bg-black">
          <img
            ref={imgRef}
            src={src}
            alt={`live view of ${device}`}
            className="size-full object-contain"
            onError={() => {
              if (retry.current) return
              retry.current = window.setTimeout(() => {
                retry.current = null
                reopen()
              }, RETRY_MS)
            }}
          />
          {!live && (
            <p className="absolute inset-0 flex items-center justify-center text-xs text-white/60">
              waiting for the camera to stream…
            </p>
          )}
        </div>
      </DialogContent>
    </Dialog>
  )
}
