#!/usr/bin/env python3
"""cDNG Raw Convert app icon (0.6.3): a colour-wheel iris over "DNG" / "CONVERT", in the app's dark theme.
Writes icon.html; render it at 1024x1024 with a transparent background (e.g. Chromium/Playwright) to AppIcon.png."""
import math

R, G, B = '#e5483a', '#4fbf63', '#3f86e0'
ORANGE = '#ec8a2f'

def squircle_defs(i, top='#34343a', bottom='#141417'):
    return f'''
  <linearGradient id="bg{i}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/></linearGradient>
  <filter id="sh{i}" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="14" stdDeviation="16" flood-color="#000" flood-opacity=".45"/></filter>
  <clipPath id="sq{i}"><rect x="100" y="100" width="824" height="824" rx="185"/></clipPath>'''

def bayer(x0, y0, cols, rows, cell, gap, rad, opacity=1.0, fade=None):
    """RGGB tiles. fade(cx, cy) -> opacity multiplier."""
    out = []
    for r in range(rows):
        for c in range(cols):
            col = (R if c % 2 == 0 else G) if r % 2 == 0 else (G if c % 2 == 0 else B)
            x, y = x0 + c * (cell + gap), y0 + r * (cell + gap)
            o = opacity * (fade(x + cell / 2, y + cell / 2) if fade else 1)
            if o <= 0.02: continue
            out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{cell}" height="{cell}" rx="{rad}" fill="{col}" opacity="{o:.2f}"/>')
    return '\n'.join(out)

def iris(cx, cy, r_out, r_in, n, colours, rot=0.0, gap=6, gap_col='#141417', shade=True, uid='x'):
    """Aperture blades tiling the ring between a regular n-gon (radius r_in) and a circle (r_out)."""
    v = [(cx + r_in * math.cos(rot + 2 * math.pi * k / n), cy + r_in * math.sin(rot + 2 * math.pi * k / n)) for k in range(n)]
    def hit(px, py, dx, dy):              # where the ray p + t d meets the outer circle
        fx, fy = px - cx, py - cy; a = dx * dx + dy * dy; b = 2 * (fx * dx + fy * dy); c = fx * fx + fy * fy - r_out * r_out
        t = (-b + math.sqrt(b * b - 4 * a * c)) / (2 * a); return px + t * dx, py + t * dy
    P = []
    for k in range(n):
        pv, cv = v[k - 1], v[k]
        P.append(hit(cv[0], cv[1], cv[0] - pv[0], cv[1] - pv[1]))
    defs, out = [], []
    for k in range(n):
        a, b2, pb, pa = v[k], v[(k + 1) % n], P[(k + 1) % n], P[k]
        col = colours[k % len(colours)]
        # arc from pb back to pa (counter-clockwise in screen space = sweep 0)
        d = f'M{a[0]:.1f},{a[1]:.1f} L{b2[0]:.1f},{b2[1]:.1f} L{pb[0]:.1f},{pb[1]:.1f} A{r_out},{r_out} 0 0 0 {pa[0]:.1f},{pa[1]:.1f} Z'
        if shade:
            gid = f'bl{uid}{k}'
            mx, my = (a[0] + b2[0]) / 2, (a[1] + b2[1]) / 2
            defs.append(f'<linearGradient id="{gid}" gradientUnits="userSpaceOnUse" x1="{mx:.1f}" y1="{my:.1f}" x2="{(pa[0]+pb[0])/2:.1f}" y2="{(pa[1]+pb[1])/2:.1f}">'
                        f'<stop offset="0" stop-color="{col}" stop-opacity="1"/><stop offset="1" stop-color="{col}" stop-opacity=".62"/></linearGradient>')
            fill = f'url(#{gid})'
        else: fill = col
        out.append(f'<path d="{d}" fill="{fill}" stroke="{gap_col}" stroke-width="{gap}" stroke-linejoin="round"/>')
    hole = ' '.join(f'{x:.1f},{y:.1f}' for x, y in v)
    return '\n'.join(defs), '\n'.join(out), hole

def icon(i, body, defs=''):
    return f'''<div class="ic" id="{i}"><svg width="1024" height="1024" viewBox="0 0 1024 1024">
 <defs>{squircle_defs(i)}{defs}</defs>
 <rect x="100" y="100" width="824" height="824" rx="185" fill="url(#bg{i})" filter="url(#sh{i})"/>
 <g clip-path="url(#sq{i})">
  <rect x="100" y="100" width="824" height="5" fill="#fff" opacity=".10"/>
{body}
 </g>
</svg></div>'''

spec = ['#e5483a', '#ec8a2f', '#e6c33a', '#9bcf45', '#4fbf63', '#36b8a4', '#3fa6e0', '#3f86e0', '#6a62e0', '#a35ad8', '#d6519f', '#e5486a']
cx, cy, R = 512, 404, 206
gg, blades, hole = iris(cx, cy, R, R * 0.28, 12, spec, rot=0.2, gap=6, shade=False, uid='H')
body = f'''  <circle cx="{cx}" cy="{cy}" r="{R + 22}" fill="#101013"/>
{blades}
  <polygon points="{hole}" fill="#0d0d10"/>
  <circle cx="{cx}" cy="{cy}" r="{R + 2}" fill="none" stroke="#000" stroke-opacity=".35" stroke-width="6"/>
  <path d="M{cx - 21},{cy - 31} L{cx + 36},{cy} L{cx - 21},{cy + 31} Z" fill="#f2f2f2"/>
  <text x="512" y="795" text-anchor="middle" font-family="Inter Display, Inter, sans-serif" font-weight="800" font-size="168" letter-spacing="4" fill="#f2f2f4">cDNG</text>
  <text x="512" y="862" text-anchor="middle" font-family="Inter, sans-serif" font-weight="600" font-size="46" letter-spacing="11" fill="#9a9aa4">RAW CONVERT</text>'''
html = '''<!doctype html><html><head><meta charset="utf-8"><style>
html,body{margin:0;background:transparent}.ic{width:1024px;height:1024px}svg{display:block}</style></head><body>
''' + icon('H', body, gg) + '</body></html>'
open(__file__.replace('make_icon.py', 'icon.html'), 'w').write(html)
