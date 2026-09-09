#!/usr/bin/env python3
"""Regenerate the sequence diagrams in docs/simplepool.html.

The diagrams are inline SVG, drawn in the page's own idiom: no library, no
external requests, and every colour taken from the page's CSS variables so they
follow the reader's theme. They are also ~4 KB each of hand-computed
coordinates, which is why this script exists rather than the SVG being edited in
place: the diagrams have already gone stale once, when the pplns-coinbase forfeit
rule was reversed and the words "FORFEITED to the operator" were baked into the
picture (LayerTwo-Labs/simplepool#76).

Usage:
    python3 docs/sequence-diagrams.py            # rewrite the diagrams in place
    python3 docs/sequence-diagrams.py --check    # fail if they are out of date

To change a diagram, edit DIAGRAMS at the bottom and re-run. Output is
deterministic, so a no-op run leaves the file byte-identical.
"""

import re
import sys
import hashlib
from pathlib import Path

ESC = {'&': '&amp;', '<': '&lt;', '>': '&gt;'}
def e(s): return ''.join(ESC.get(c, c) for c in s)

W_ACTOR   = 132     # actor box width
GAP       = 26      # gap between actor boxes
TOP       = 34      # y of actor box top
H_ACTOR   = 46
FIRST_MSG = 118     # y of first message
STEP      = 34      # vertical distance between messages
NOTE_PAD  = 8


def wrap(text, max_px, px=10.5, adv=0.52):
    """Greedy wrap so a note never runs outside its own box. The generator
    owns this rather than the author: a note that overflows is invisible in
    the source and obvious only once rendered."""
    per = max_px / (px * adv)
    words, lines, cur = text.split(), [], ''
    for w in words:
        trial = (cur + ' ' + w).strip()
        if len(trial) <= per or not cur:
            cur = trial
        else:
            lines.append(cur); cur = w
    if cur: lines.append(cur)
    return lines


def build(title, desc, actors, steps, accent="pplns", min_width=0):
    """actors: [(key, label, sub)] ; steps: list of tuples, see below."""
    n = len(actors)
    width = max(n * W_ACTOR + (n - 1) * GAP + 20, min_width)
    span = (width - 20 - W_ACTOR) / max(n - 1, 1)
    x = {}
    for i, (k, _, _) in enumerate(actors):
        x[k] = 10 + i * span + W_ACTOR / 2

    body, y = [], FIRST_MSG
    for st in steps:
        kind = st[0]
        if kind == 'msg':
            _, a, b, label = st[:4]
            dashed = len(st) > 4 and 'dashed' in st[4]
            x1, x2 = x[a], x[b]
            back = x2 < x1
            x1 += (-4 if back else 4); x2 += (5 if back else -5)
            cls = 'ln dash' if dashed else 'ln'
            body.append(f'<line class="{cls}" x1="{x1:.0f}" y1="{y}" '
                        f'x2="{x2:.0f}" y2="{y}" marker-end="url(#sqar)"/>')
            body.append(f'<text class="lbl" x="{(x1+x2)/2:.0f}" y="{y-6}" '
                        f'text-anchor="middle">{e(label)}</text>')
            y += STEP
        elif kind == 'self':
            _, a, label = st[:3]
            x1 = x[a]
            # A self-message on the RIGHTMOST lifeline has nowhere to put a
            # left-anchored label, so mirror the loop and the text inward.
            # Cheaper than shortening every label to fit the worst case.
            mirror = x1 > width * 0.62
            d = -1 if mirror else 1
            body.append(f'<path class="ln" d="M{x1+4*d:.0f},{y} h{26*d} v16 h{-26*d}" '
                        f'marker-end="url(#sqar)"/>')
            body.append(f'<text class="lbl" x="{x1+36*d:.0f}" y="{y+4}" '
                        f'text-anchor="{"end" if mirror else "start"}">{e(label)}</text>')
            y += STEP + 4
        elif kind == 'note':
            _, text = st[:2]
            tone = st[2] if len(st) > 2 else 'plain'
            lines = wrap(text, width - 44)
            h = 12 + 14 * len(lines)
            body.append(f'<rect class="note {tone}" x="14" y="{y-14}" '
                        f'width="{width-28}" height="{h}" rx="5"/>')
            for j, ln in enumerate(lines):
                body.append(f'<text class="notet" x="{width/2:.0f}" '
                            f'y="{y - 14 + 17 + j*14}" '
                            f'text-anchor="middle">{e(ln)}</text>')
            y += h + 12
        elif kind == 'gap':
            y += st[1]

    bottom = y - STEP + 22
    life = []
    for k, label, sub in actors:
        cx = x[k]
        life.append(f'<line class="life" x1="{cx:.0f}" y1="{TOP+H_ACTOR}" '
                    f'x2="{cx:.0f}" y2="{bottom}"/>')
    heads = []
    for i, (k, label, sub) in enumerate(actors):
        bx = 10 + i * span
        cx = bx + W_ACTOR / 2
        cls = 'abx accent' if k == 'pool' else 'abx'
        heads.append(f'<rect class="{cls}" x="{bx}" y="{TOP}" '
                     f'width="{W_ACTOR}" height="{H_ACTOR}" rx="7"/>')
        heads.append(f'<text class="at" x="{cx:.0f}" y="{TOP+20}" '
                     f'text-anchor="middle">{e(label)}</text>')
        if sub:
            heads.append(f'<text class="as" x="{cx:.0f}" y="{TOP+36}" '
                         f'text-anchor="middle">{e(sub)}</text>')

    # Deterministic: Python's hash() is randomised per process, so the ids
    # moved on every run and a no-op regeneration produced a diff.
    tid = 'sq' + hashlib.sha1(title.encode()).hexdigest()[:6]
    return f'''<figure>
  <div class="svgbox">
    <svg viewBox="0 0 {width} {bottom+14}" role="img"
         aria-labelledby="{tid}t {tid}d" style="max-width: {width}px; margin: 0 auto;">
      <title id="{tid}t">{e(title)}</title>
      <desc id="{tid}d">{e(desc)}</desc>
      <style>
        .abx  {{ fill: var(--bg-sunk); stroke: var(--rule-firm); stroke-width: 1.25; }}
        .abx.accent {{ fill: var(--{accent}-bg); stroke: var(--{accent}); }}
        .at   {{ fill: var(--fg); font: 600 12px ui-sans-serif, system-ui, sans-serif; }}
        .as   {{ fill: var(--fg-faint); font: 400 10px ui-monospace, monospace; }}
        .life {{ stroke: var(--rule); stroke-width: 1.25; stroke-dasharray: 3 4; }}
        .ln   {{ stroke: var(--rule-firm); stroke-width: 1.5; fill: none; }}
        .ln.dash {{ stroke-dasharray: 5 4; }}
        .lbl  {{ fill: var(--fg-muted); font: 400 10.5px ui-monospace, monospace; }}
        .note {{ fill: var(--bg-sunk); stroke: var(--rule); stroke-width: 1; }}
        .note.win  {{ fill: var(--ok-bg, var(--pplns-bg)); stroke: var(--ok); }}
        .note.warn {{ fill: var(--warn-bg); stroke: var(--warn); }}
        .notet {{ fill: var(--fg-muted); font: 400 10.5px ui-sans-serif, system-ui, sans-serif; }}
      </style>
      <defs>
        <marker id="sqar" viewBox="0 0 10 10" refX="9" refY="5"
                markerWidth="6" markerHeight="6" orient="auto-start-reverse">
          <path d="M0,0 L10,5 L0,10 z" fill="var(--rule-firm)"/>
        </marker>
      </defs>
      {''.join(heads)}
      {''.join(life)}
      {''.join(body)}
    </svg>
  </div>
</figure>'''



# ---- the diagrams ---------------------------------------------------------

DIAGRAMS = {}

MINER = ('miner', 'Miner', 'ASIC')
POOL  = ('pool',  'simplepool', ':3334')
NODE  = ('node',  'bitcoind', '+ enforcer')
CHAIN = ('chain', 'Bitcoin L1', 'the chain')
DB    = ('db',    'shares.db', 'SQLite')
WORK  = ('worker','payout worker', 'systemd')
THUN  = ('thun',  'Thunder', 'sidechain #9')

# ---------------------------------------------------------------- solo -----
DIAGRAMS['solo'] = build(
    'solo mode: the finder is paid in the block it found',
    'A miner subscribes, simplepool builds a coinbase paying that miner and '
    'hands out work. When the miner finds a block the coinbase already pays it, '
    'so no ledger and no payout step exist.',
    [MINER, POOL, NODE, CHAIN],
    [('msg', 'miner', 'pool', 'authorize  <bitcoin-address>'),
     ('msg', 'pool', 'node', 'getblocktemplate'),
     ('msg', 'node', 'pool', 'template', 'dashed'),
     ('self', 'pool', 'build coinbase paying THIS miner'),
     ('msg', 'pool', 'miner', 'notify  (cb1 / cb2)'),
     ('msg', 'miner', 'pool', 'submit  (a block!)'),
     ('msg', 'pool', 'node', 'submitblock'),
     ('msg', 'node', 'chain', 'block accepted'),
     ('note', 'The miner is already paid — the coinbase is the payment. No ledger, no payout worker, no wait.', 'win'),
    ], accent='solo')

# --------------------------------------------------------- pps-classic -----
DIAGRAMS['pps'] = build(
    'pps-classic: every share is priced on arrival, the pool carries the risk',
    'Each accepted share is credited immediately at a rate derived from the '
    'template. The coinbase pays the pool wallet, and a payout worker settles '
    'balances over Thunder on a daily batch.',
    [MINER, POOL, DB, WORK, THUN],
    [('msg', 'miner', 'pool', 'authorize  <thunder-address>'),
     ('msg', 'miner', 'pool', 'submit  (an ordinary share)'),
     ('self', 'pool', 'price it: rate x difficulty'),
     ('msg', 'pool', 'db', 'credit NOW, block or not'),
     ('note', 'The pool owes this miner before it has earned anything. That gap is the operator reserve.', 'warn'),
     ('msg', 'miner', 'pool', 'submit  (a block)'),
     ('note', 'The coinbase pays the POOL wallet, not the miner.'),
     ('gap', 6),
     ('msg', 'worker', 'db', 'daily: who is owed?'),
     ('msg', 'worker', 'thun', 'one batched transfer'),
     ('msg', 'worker', 'db', 'credit paid_sats on CONFIRMATION', 'dashed'),
    ], accent='pps')

# ------------------------------------------------- pplns-thunder / btc -----
DIAGRAMS['pplns_custodial'] = build(
    'pplns-thunder and pplns-btc: a matured block is split across the work that found it',
    'Shares are recorded but not priced. When a block reaches 100 confirmations '
    'it is divided across the window of shares that produced it, and the payout '
    'worker settles the resulting balances over Thunder or on Bitcoin L1.',
    [MINER, POOL, DB, WORK],
    [('msg', 'miner', 'pool', 'submit  (an ordinary share)'),
     ('msg', 'pool', 'db', 'record it — credited 0'),
     ('note', 'Nothing is promised. The pool never owes more than it has just been paid.', 'win'),
     ('msg', 'miner', 'pool', 'submit  (a block)'),
     ('msg', 'pool', 'db', 'row: hash + window size'),
     ('gap', 4),
     ('note', '...100 confirmations later, on a new tip...'),
     ('self', 'pool', 'reconcile: still in the chain?'),
     ('msg', 'pool', 'db', 'split the block across the window'),
     ('gap', 4),
     ('msg', 'worker', 'db', 'daily: who is owed?'),
     ('self', 'worker', 'pay: Thunder, or L1 via the enforcer'),
     ('msg', 'worker', 'db', 'paid_sats on confirmation', 'dashed'),
    ])

# ------------------------------------------------------ pplns-coinbase -----
DIAGRAMS['cbwin'] = build(
    'pplns-coinbase: the block pays the whole window, directly',
    'The window is snapshotted onto the job when the template is built, so the '
    'coinbase carries one output per miner it has room for. There is no pool '
    'wallet, no ledger and no maturity wait. What one block cannot fit is '
    'shared among the miners it could pay, and those left out go first in the '
    'queue for the next block.',
    [MINER, POOL, DB, CHAIN],
    [('msg', 'pool', 'db', 'who is in the window NOW?'),
     ('msg', 'db', 'pool', 'claims + who has waited longest', 'dashed'),
     ('self', 'pool', 'order: biggest claims, plus reserved slots'),
     ('note', 'A coinbase fits only so many outputs. What it cannot pay is shared among the miners it can — never the operator, who takes only its fee.'),
     ('msg', 'pool', 'miner', 'notify — pays the whole window'),
     ('msg', 'miner', 'pool', 'submit  (a block)'),
     ('msg', 'pool', 'chain', 'submitblock'),
     ('msg', 'pool', 'db', 'stage who was skipped (sums to zero)'),
     ('note', 'Staged, not applied: an orphaned block paid nobody and rotates nobody. The confirmation pass decides.', 'warn'),
     ('note', 'Nobody is owed money — only a turn. Skipped miners go first in the next block.', 'win'),
    ])

# ------------------------------------------------------------- payouts -----
DIAGRAMS['payout'] = build(
    'the payout worker: never pay twice, never claim to have paid',
    'A write-ahead in-flight row is written before the transaction is sent, so '
    'a crash mid-payout is recoverable; paid_sats is credited only once the '
    'transaction confirms.',
    [WORK, DB, ('rail', 'the rail', 'Thunder / L1')],
    [('msg', 'worker', 'db', 'who clears PAYOUT_MIN_SATS?'),
     ('msg', 'worker', 'db', 'write in-flight row FIRST'),
     ('note', 'Written before the money moves. A crash here is recoverable; the reverse order is not.', 'warn'),
     ('msg', 'worker', 'rail', 'ONE transaction for the whole batch'),
     ('msg', 'rail', 'worker', 'txid', 'dashed'),
     ('msg', 'worker', 'db', 'store txid against the in-flight row'),
     ('gap', 4),
     ('note', '...later ticks, until it confirms...'),
     ('self', 'worker', 'is the txid confirmed yet?'),
     ('msg', 'worker', 'db', 'NOW credit paid_sats, clear in-flight'),
     ('note', 'Crediting on confirmation, not on send, is what makes a lost transaction a retry rather than a theft.', 'win'),
    ], accent='pps', min_width=560)



# ---- splicing -------------------------------------------------------------
#
# Each figure sits between HTML comment markers so a regeneration replaces
# exactly the drawing and nothing around it. Prose about a diagram lives
# outside the markers and is written by hand.

HTML = Path(__file__).resolve().parent / "simplepool.html"


def splice(src: str) -> str:
    out = src
    for key, svg in DIAGRAMS.items():
        begin, end = f"<!-- seq:{key} -->", f"<!-- /seq:{key} -->"
        i, j = out.find(begin), out.find(end)
        if i < 0 or j < 0:
            sys.exit(f"marker {begin} missing from {HTML.name}; add it around "
                     f"the figure this diagram belongs to")
        out = out[:i + len(begin)] + "\n" + svg + "\n" + out[j:]
    return out


def main() -> int:
    check = "--check" in sys.argv[1:]
    src = HTML.read_text()
    new = splice(src)
    if new == src:
        print(f"{HTML.name}: {len(DIAGRAMS)} diagram(s) already up to date")
        return 0
    if check:
        print(f"{HTML.name}: diagrams are STALE — run "
              f"`python3 docs/{Path(__file__).name}` and commit the result",
              file=sys.stderr)
        return 1
    HTML.write_text(new)
    print(f"{HTML.name}: rewrote {len(DIAGRAMS)} diagram(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
