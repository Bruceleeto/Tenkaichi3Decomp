#!/usr/bin/env bash
# The disc's movies (PSS: MPEG-2, 512 x 448 at 30 pictures a second) as pl_mpeg plays them (port/src/plat_movie.c):
# MPEG-1 video in an MPEG program stream at 320 x 240 (the Dreamcast's pl_mpeg's recommended size: the SH-4 decodes
# it; the picture is stretched over the screen) and the same 30 pictures a second (the game shows one each frame), at a
# constant rate (no decoding spikes), without sound (the game plays the movie's ADX file itself). On the Dreamcast and
# in the PC's simulation of its renderer.
#
#     dc_movie.sh IN.PSS OUT.mpg [kbit/s]
set -euo pipefail
in=$1
out=$2
rate=${3:-1500}
command -v ffmpeg >/dev/null || { echo "dc_movie.sh: ffmpeg is needed (apt install ffmpeg)" >&2; exit 1; }
ffmpeg -hide_banner -loglevel error -y -i "$in" -an \
    -vf "fps=30,scale=320:240" -c:v mpeg1video -b:v "${rate}k" -minrate "${rate}k" -maxrate "${rate}k" -bufsize "${rate}k" \
    -f mpeg -packet_size 2048 "$out.tmp.mpg"
mv "$out.tmp.mpg" "$out"
