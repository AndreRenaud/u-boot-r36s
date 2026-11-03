#!/bin/sh
# Regenerate r36s_splash.bmp from splash.svg: 320x240 (r36s.c centres it on
# the 640x480 panel), 256 colours without dithering, RLE8 compressed.
# Needs rsvg-convert (librsvg) and ImageMagick.
set -e
cd "$(dirname "$0")"
rsvg-convert -w 320 -h 240 splash.svg -o splash.png
convert splash.png -dither None -colors 256 -compress RLE BMP3:r36s_splash.bmp
rm splash.png
