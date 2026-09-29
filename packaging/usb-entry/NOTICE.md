# Diagnostic terminal entry

`js/run.js` and the MP3 title format come from
[Bijan-A/oem-aa-mod-installer](https://github.com/Bijan-A/oem-aa-mod-installer/tree/cee70a47d5ea786059742cb959b402e32408550b).
This is the entry mechanism used by the user's successful AA touch and km/L
installers. Upstream has no LICENSE file; attribution does not assert an
upstream license or relicense this JavaScript.

The four MP3 streams are generated silence, without upstream audio or artwork.
Rebuild with `python3 packaging/usb-entry/generate.py` (host Python 3 + ffmpeg).
The ID3v2.3 titles load `js/run.js` from sda1, sdb1, sdc1 or sdd1.
The script opens the OEM diagnostic terminal. It does not execute the DR
installer or install/reinstall the AA touch modification.
