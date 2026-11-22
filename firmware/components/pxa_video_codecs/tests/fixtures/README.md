# Decoder inputs

These fixtures contain generated test patterns, with no third-party media.
`high-640x360.h264` is one second of FFmpeg's testsrc2 at 24 fps, encoded with
High Profile, two B frames, Level 3.0 and access-unit delimiters. The JPEG has
four solid quadrants (red, green, blue, white), 320×180, quality 85, 4:2:0.
`tone-44100-stereo.aac` contains two AAC-LC ADTS frames generated from a
440 Hz sine wave at 44100 Hz, stereo, using FFmpeg's AAC encoder.

Recreate the H.264 fixture with a local FFmpeg tool (not linked into PXA):

```sh
ffmpeg -f lavfi -i 'testsrc2=size=640x360:rate=24:duration=1' \
  -c:v libx264 -profile:v high -level 3.0 -pix_fmt yuv420p -bf 2 -g 24 \
  -x264-params aud=1 -f h264 high-640x360.h264
```

The native decoder test and the device probe use the same H.264 bytes. The
device probe also decodes the JPEG and verifies that a 640×360 SPS is rejected
by a 320×180 session before reference-picture allocation.
