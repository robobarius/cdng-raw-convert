#!/usr/bin/env python3
"""
resolve_export_stills.py: imports the wedge_q*.mov clips into the CURRENT
DaVinci Resolve project, makes one timeline per clip and exports a TIFF still
of each, ready for measure_still.py. Resolve Studio must be running, with
external scripting enabled (Preferences > System > General > External
scripting using: Local).

  python3 resolve_export_stills.py CLIP_FOLDER

Set up the project first (see README.md): DaVinci YRGB Color Managed,
Rec.709 timeline and output colour space with Linear gamma, 1920x1080.
Writes CLIP_FOLDER/stills/wedge_q*.tif.

Finding Resolve's scripting module: this tries the standard install paths.
If it fails, set RESOLVE_SCRIPT_API / RESOLVE_SCRIPT_LIB as described in
Resolve's Help > Documentation > Developer > Scripting README.
"""
import glob, os, sys, time


def get_resolve():
    try:
        import DaVinciResolveScript as dvr
    except ImportError:
        cands = {
            'win32': os.path.expandvars(r'%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting\Modules'),
            'darwin': '/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/Scripting/Modules',
        }
        sys.path.append(cands.get(sys.platform, '/opt/resolve/Developer/Scripting/Modules'))
        import DaVinciResolveScript as dvr
    r = dvr.scriptapp('Resolve')
    if r is None: sys.exit('Could not connect to Resolve. Is it running, with external scripting set to Local?')
    return r


def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    d = os.path.abspath(sys.argv[1])
    clips = sorted(glob.glob(os.path.join(d, 'wedge_q*.mov')))
    if not clips: sys.exit(f'no wedge_q*.mov in {d} (run make_validation_clips.py first)')
    resolve = get_resolve()
    p = resolve.GetProjectManager().GetCurrentProject()
    print('project:', p.GetName())
    for k in ('colorScienceMode', 'colorSpaceTimeline', 'colorSpaceTimelineGamma',
              'colorSpaceOutput', 'colorSpaceOutputGamma', 'timelineResolutionWidth', 'timelineResolutionHeight'):
        print(f'  {k} = {p.GetSetting(k)}')
    mp = p.GetMediaPool()
    mp.SetCurrentFolder(mp.AddSubFolder(mp.GetRootFolder(), 'cDNG Raw Convert validation'))
    out = os.path.join(d, 'stills'); os.makedirs(out, exist_ok=True)
    for c in clips:
        name = os.path.splitext(os.path.basename(c))[0]
        item = mp.ImportMedia([c])[0]
        tl = mp.CreateTimelineFromClips(name, [item])
        p.SetCurrentTimeline(tl)
        resolve.OpenPage('color'); time.sleep(1.5)
        path = os.path.join(out, name + '.tif')
        ok = p.ExportCurrentFrameAsStill(path)
        print(('wrote ' if ok else 'FAILED ') + path)


if __name__ == '__main__':
    main()
