# Screensaver GIF

`screensaver.gif` is the user-provided animation embedded in the ESP32-S3
application image.

Compatibility requirements:

- GIF89a format
- 320 x 172 pixels
- Global color table
- No more than 60 frames for full PSRAM predecode

At startup the firmware decodes the frames to RGB565 in PSRAM. After 60 seconds
without button activity, the display dims and starts the screensaver.
