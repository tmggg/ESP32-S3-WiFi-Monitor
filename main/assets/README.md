# Screensaver GIF

`screensaver.gif` is the user-provided animation embedded in the ESP32-S3
application image.

Compatibility requirements:

- GIF89a format
- 320 x 172 pixels
- Global color table
- No more than 60 frames for full PSRAM predecode

At startup the firmware decodes the frames to RGB565 in PSRAM. The OpenWrt
response selects the idle screen with `screensaver_type` (`gif` or `clock`) and
sets its delay in seconds with `screensaver_timeout`. A timeout of `0` disables
both screensavers. Backlight dimming remains on its independent 60-second timer.
