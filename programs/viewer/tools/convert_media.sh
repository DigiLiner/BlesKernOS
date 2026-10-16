#!/bin/sh
# Host-side compatibility converter for BlesKernOS Viewer.
# MPEG-2 Video, MP3/Layer III, AC-3, H.264, Xvid and other host-supported
# codecs are TRANSCODED to MPEG-1 Video + MP2, not decoded natively by Viewer.
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 INPUT OUTPUT.mpg [retro|quality]" >&2
    exit 2
fi
command -v ffmpeg >/dev/null 2>&1 || {
    echo "Error: ffmpeg must be installed on the host computer." >&2
    exit 1
}
input=$1
output=$2
profile=${3:-retro}
[ -f "$input" ] || { echo "Error: input file not found." >&2; exit 1; }
case "$profile" in
    retro)
        # 352x288 PAL / 25 fps. MPEG-1 codec accepts even dimensions.
        width=352; height=288; fps=25; bitrate=900k; abr=96k ;;
    quality)
        width=640; height=480; fps=30; bitrate=1800k; abr=128k ;;
    *) echo "Unknown profile '$profile' (retro|quality)." >&2; exit 2 ;;
esac
# ffmpeg pads instead of stretching the source. Video is fully MPEG-1 PS,
# which the pl_mpeg implementation in Viewer can actually decode.
ffmpeg -hide_banner -y -i "$input" \
    -map 0:v:0 -map '0:a:0?' -sn -dn \
    -vf "scale=${width}:${height}:force_original_aspect_ratio=decrease:flags=bicubic,pad=${width}:${height}:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=${fps}" \
    -c:v mpeg1video -b:v "$bitrate" -maxrate "$bitrate" -bufsize 512k \
    -c:a mp2 -b:a "$abr" -ar 44100 -ac 2 \
    -f mpeg "$output"
echo "Conversion complete: $output (MPEG-1 PS + optional MP2)"
