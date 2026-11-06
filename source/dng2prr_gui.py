#!/usr/bin/env python3
"""
dng2prr_gui.py: a small cross-platform window for dng2prr (Windows, Linux, macOS).

A starting point for the Windows/Linux ports (see PORTING.md). It uses only
Tkinter from the standard library, plus dng2prr.py and its compiled core.

  python3 dng2prr_gui.py [CLIP_FOLDER_OR_CARD ...]

Paths given on the command line are added to the list, so on Windows you can
drop folders onto the packaged .exe (or a shortcut to it) in Explorer.
If the optional tkinterdnd2 package is installed, folders can also be dropped
onto the window itself.

  dng2prr_gui.py --cli OUT.mov CLIP ...   runs the command-line encoder (used
                                          internally by the packaged .exe)

LGPL-2.1-or-later. Not Apple software. See NOTICE.md.
"""
import os, queue, re, subprocess, sys, threading

HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN = getattr(sys, 'frozen', False)
APP_NAME = 'cDNG Raw Convert'
QUALITIES = [('Near-lossless', 1), ('High', 2), ('Standard (= Ninja V HQ)', 4), ('Compact', 8)]


def run_cli(args):
    if sys.stdout is None:               # windowed .exe started without a pipe
        sys.stdout = sys.stderr = open(os.devnull, 'w')
    sys.path.insert(0, HERE)
    import dng2prr
    sys.argv = ['dng2prr'] + args
    dng2prr.main()


def encoder_cmd():
    """How to start the command-line encoder from here."""
    if FROZEN:
        return [sys.executable, '--cli']
    return [sys.executable, os.path.join(HERE, 'dng2prr.py')]


def default_fallback_dir():
    home = os.path.expanduser('~')
    sub = 'Movies' if sys.platform == 'darwin' else 'Videos'
    return os.path.join(home, sub, 'ProRes RAW')


def output_path_for(clip_dir):
    """<clip name>.mov next to the clip folder; ~/Videos/ProRes RAW if that is not
    writable. Never overwrites: adds _2, _3, ..."""
    clip_dir = os.path.abspath(clip_dir)
    name = os.path.basename(clip_dir.rstrip('\\/')) or 'clip'
    parent = os.path.dirname(clip_dir)
    if not os.access(parent, os.W_OK):
        parent = default_fallback_dir()
        os.makedirs(parent, exist_ok=True)
    out = os.path.join(parent, name + '.mov'); n = 2
    while os.path.exists(out):
        out = os.path.join(parent, f'{name}_{n}.mov'); n += 1
    return out


def find_clips(paths):
    sys.path.insert(0, HERE)
    import dng2prr
    return dng2prr.find_clips(paths)


class App:
    def __init__(self, root, initial):
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.ttk, self.root = tk, ttk, root
        root.title(APP_NAME)
        root.minsize(560, 380)
        self.clips, self.proc, self.stop_flag = [], None, False
        self.q = queue.Queue()

        frm = ttk.Frame(root, padding=10); frm.pack(fill='both', expand=True)
        top = ttk.Frame(frm); top.pack(fill='x')
        ttk.Button(top, text='Add clip folder…', command=self.add_folder).pack(side='left')
        ttk.Button(top, text='Add card / parent folder…', command=self.add_folder).pack(side='left', padx=6)
        ttk.Button(top, text='Remove selected', command=self.remove_sel).pack(side='left')

        self.lb = tk.Listbox(frm, height=10, selectmode='extended')
        self.lb.pack(fill='both', expand=True, pady=8)
        self.hint = ttk.Label(frm, foreground='gray',
                              text='Add SIGMA fp clip folders (folders of DNG frames), a folder of clips or a whole card.')
        self.hint.pack(anchor='w')

        opts = ttk.Frame(frm); opts.pack(fill='x', pady=8)
        ttk.Label(opts, text='Quality:').pack(side='left')
        self.quality = tk.StringVar(value=QUALITIES[0][0])
        ttk.Combobox(opts, textvariable=self.quality, state='readonly', width=22,
                     values=[q for q, _ in QUALITIES]).pack(side='left', padx=(4, 16))
        ttk.Label(opts, text='White balance override (R/G,B/G):').pack(side='left')
        self.neutral = tk.StringVar(value='')
        ttk.Entry(opts, textvariable=self.neutral, width=14).pack(side='left', padx=4)
        ttk.Label(opts, text='blank = as shot', foreground='gray').pack(side='left')

        self.status = tk.StringVar(value='Ready.')
        ttk.Label(frm, textvariable=self.status).pack(anchor='w')
        self.bar = ttk.Progressbar(frm, maximum=100); self.bar.pack(fill='x', pady=4)
        bot = ttk.Frame(frm); bot.pack(fill='x')
        self.go = ttk.Button(bot, text='Convert', command=self.start); self.go.pack(side='right')
        self.stop = ttk.Button(bot, text='Stop', command=self.do_stop, state='disabled')
        self.stop.pack(side='right', padx=6)

        self._enable_dnd()
        if initial: self.add_paths(initial)
        root.after(100, self.poll)

    # ---- adding clips
    def _enable_dnd(self):
        try:
            from tkinterdnd2 import DND_FILES          # optional
            self.lb.drop_target_register(DND_FILES)
            self.lb.dnd_bind('<<Drop>>', lambda e: self.add_paths(self.root.tk.splitlist(e.data)))
            self.hint.config(text='Drop clip folders, a folder of clips or a card here, or use the buttons.')
        except Exception:
            pass

    def add_folder(self):
        from tkinter import filedialog
        d = filedialog.askdirectory(title='Choose a clip folder, a folder of clips, or a card')
        if d: self.add_paths([d])

    def add_paths(self, paths):
        found = find_clips(list(paths))
        if not found:
            self.status.set('No DNG clip folders found there.'); return
        for c in found:
            if c not in self.clips:
                self.clips.append(c); self.lb.insert('end', c)
        self.status.set(f'{len(self.clips)} clip(s) ready.')

    def remove_sel(self):
        for i in reversed(self.lb.curselection()):
            self.lb.delete(i); del self.clips[i]

    # ---- converting
    def start(self):
        if not self.clips:
            self.status.set('Add at least one clip folder first.'); return
        nt = self.neutral.get().strip()
        if nt and not re.fullmatch(r'\s*[0-9.]+\s*,\s*[0-9.]+\s*', nt):
            self.status.set('White balance override must look like 0.737,0.367'); return
        scale = dict(QUALITIES)[self.quality.get()]
        self.go.config(state='disabled'); self.stop.config(state='normal'); self.stop_flag = False
        threading.Thread(target=self.worker, args=(list(self.clips), scale, nt), daemon=True).start()

    def worker(self, clips, scale, neutral):
        done = []
        for i, clip in enumerate(clips, 1):
            if self.stop_flag: break
            out = output_path_for(clip)
            cmd = encoder_cmd() + [out, clip, '--scale', str(scale), '--progress-every', '1']
            if neutral: cmd += ['--neutral', neutral.replace(' ', '')]
            self.q.put(('clip', i, len(clips), os.path.basename(clip), 0, 0))
            flags = subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
            self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True, bufsize=1, creationflags=flags)
            tail = []
            for line in self.proc.stdout:
                tail = (tail + [line.rstrip()])[-8:]
                m = re.search(r'frame (\d+)/(\d+)', line)
                if m: self.q.put(('clip', i, len(clips), os.path.basename(clip), int(m[1]), int(m[2])))
            rc = self.proc.wait(); self.proc = None
            if self.stop_flag:
                try: os.remove(out)                      # don't leave a half-written file
                except OSError: pass
                break
            if rc != 0:
                self.q.put(('error', os.path.basename(clip), '\n'.join(tail))); break
            done.append(out)
        self.q.put(('end', done, self.stop_flag))

    def do_stop(self):
        self.stop_flag = True
        p = self.proc
        if p is None: return
        if sys.platform == 'win32':                     # also end the encoder's worker processes
            subprocess.call(['taskkill', '/F', '/T', '/PID', str(p.pid)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            p.terminate()

    def poll(self):
        from tkinter import messagebox
        try:
            while True:
                m = self.q.get_nowait()
                if m[0] == 'clip':
                    _, i, n, name, f, t = m
                    self.status.set(f'Converting clip {i} of {n}: {name}' + (f'  (frame {f} of {t})' if t else ''))
                    self.bar['value'] = (f * 100 // t) if t else 0
                elif m[0] == 'error':
                    messagebox.showerror(APP_NAME, f'{m[1]} failed:\n\n{m[2]}')
                elif m[0] == 'end':
                    done, stopped = m[1], m[2]
                    self.go.config(state='normal'); self.stop.config(state='disabled')
                    self.bar['value'] = 0 if stopped else 100
                    self.status.set(('Stopped. ' if stopped else 'Done. ') + f'{len(done)} file(s) written.')
                    if done and not stopped:
                        messagebox.showinfo(APP_NAME, 'Wrote:\n' + '\n'.join(done))
        except queue.Empty:
            pass
        self.root.after(150, self.poll)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--cli':
        run_cli(sys.argv[2:]); return
    try:
        from tkinterdnd2 import TkinterDnD
        root = TkinterDnD.Tk()
    except Exception:
        import tkinter as tk
        root = tk.Tk()
    App(root, [p for p in sys.argv[1:] if os.path.exists(p)])
    root.mainloop()


if __name__ == '__main__':
    import multiprocessing
    multiprocessing.freeze_support()     # must come first in a frozen .exe
    main()
