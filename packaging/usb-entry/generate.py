#!/usr/bin/env python3
"""Generate silent diagnostic-entry MP3s; host only, requires ffmpeg.

The title format is from Bijan-A/oem-aa-mod-installer. No upstream recordings,
artwork, Mazda binaries or touch-mod payloads are copied.
"""
from pathlib import Path
import subprocess


def main():
    output = Path(__file__).resolve().parent / 'mp3'
    output.mkdir(exist_ok=True)
    for letter in 'abcd':
        title = ("</span><iframe onload=\"utility.loadScript('../../../mnt/sd" +
                 letter + "1/js/run.js')\"></iframe><span>")
        subprocess.run([
            'ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
            '-f', 'lavfi', '-i', 'anullsrc=r=44100:cl=mono', '-t', '5',
            '-map_metadata', '-1', '-c:a', 'libmp3lame', '-b:a', '32k',
            '-id3v2_version', '3', '-write_id3v1', '0', '-metadata',
            'title=' + title, str(output / (letter + '.mp3')),
        ], check=True)


if __name__ == '__main__':
    main()
