# Screensaver GIF

`screensaver.gif` is retained only as a local reference asset. The firmware no
longer embeds it in the application image.

Compatibility requirements:

- GIF89a format
- 320 x 172 pixels
- Global color table
- No more than 60 frames for full PSRAM predecode

After Wi-Fi connects, OpenWrt supplies `gif_url`, `gif_version` (SHA-256), and
`gif_size`. The firmware downloads the GIF into the `storage` partition,
verifies its length and hash, memory-maps it, then decodes up to 60 RGB565 frames
into PSRAM. The Flash copy is temporary and is replaced at each startup.
While running, a changed `gif_version` triggers another download automatically.
The old GIF continues playing during download and verification; decoding uses
the clock as a temporary fallback because PSRAM cannot hold two decoded GIFs.
Failed versions are retried no more than once per minute.

The OpenWrt response selects the idle screen with `screensaver_type` (`gif` or
`clock`) and sets its delay in seconds with `screensaver_timeout`. A timeout of
`0` disables both screensavers. Until a requested GIF is ready, the clock is
used as a fallback. Backlight dimming remains on its independent 60-second
timer.
