"""Lock driver schematics for Lock_Door_RFID (schemdraw).

    python docs/draw_lock_driver.py      -> writes docs/lock_driver_*.png / .svg
"""
import os
import schemdraw
import schemdraw.elements as elm

schemdraw.use('matplotlib')
HERE = os.path.dirname(os.path.abspath(__file__))
NET = '#666666'


def helpers(d):
    A = d.add

    def txt(xy, s, ha='left', va='center', fs=11, color=None):
        kw = dict(halign=ha, valign=va, fontsize=fs)
        if color:
            kw['color'] = color
        A(elm.Label().at(xy).label(s, **kw))

    def wire(*pts):
        for a, b in zip(pts, pts[1:]):
            A(elm.Line().at(a).to(b))

    def dot(xy):
        A(elm.Dot().at(xy))

    return A, txt, wire, dot


def save(d, name):
    for ext in ('png', 'svg'):
        d.save(os.path.join(HERE, f'{name}.{ext}'), transparent=False, dpi=170)


def irf8736():
    """Current hardware: logic-level N-MOSFET, low side, driven directly from GPIO25."""
    TOP, GND = 8.0, 0.0
    d = schemdraw.Drawing(show=False)
    d.config(fontsize=11, unit=2.5, lw=1.4)
    A, txt, wire, dot = helpers(d)

    XV = 15.8
    wire((5.2, TOP), (XV, TOP))
    A(elm.Vdd().at((XV, TOP)))
    txt((XV + 0.3, TOP + 0.55), '+12 V  (PSU ≥ 3 A)')
    wire((0.0, GND), (XV, GND))
    A(elm.Ground().at((5.2, GND)))

    # GPIO25 -> Rg -> gate, pull-up to ESP32 3V3 on the gate
    yg = 2.6
    A(elm.Dot(open=True).at((0.0, yg)))
    txt((-0.25, yg), 'ESP32\nGPIO25', ha='right')
    A(elm.Resistor().endpoints((0.0, yg), (2.6, yg)))
    txt((1.3, yg - 0.75), 'Rg  220 Ω', ha='center')
    xs = 5.2
    q = A(elm.NFet().theta(0).reverse().at((xs, yg + 2.25 - 1.5)))
    wire((2.6, yg), q.gate)
    dot((3.4, yg))
    A(elm.Resistor().endpoints((3.4, 4.8), (3.4, yg)))
    txt((3.1, 3.7), 'R_PU\n10 kΩ', ha='right')
    A(elm.Dot(open=True).at((3.4, 4.8)))
    txt((3.4, 5.2), 'ESP32 3V3', ha='center')
    txt((3.55, yg - 0.4), 'GATE', fs=9, color=NET)
    txt((xs + 0.5, yg), 'Q1  IRF8736\nN-channel, SO-8\n(S = 1,2,3  G = 4  D = 5–8)')
    txt((xs + 0.15, q.drain[1] + 0.3), 'D', fs=9)
    txt((xs + 0.15, q.source[1] - 0.3), 'S', fs=9)
    txt((q.gate[0] + 0.05, yg + 0.35), 'G', fs=9)
    wire(q.source, (xs, GND))
    dot((xs, GND))

    # lock between +12 V and drain, flyback diode across it
    ylp, yln = 7.0, 4.4
    wire((xs, TOP), (xs, ylp)); dot((xs, TOP))
    A(elm.Inductor2(loops=3).endpoints((xs, ylp), (xs, yln)))
    txt((xs - 0.35, ylp - 0.4), '+', ha='right', fs=13)
    txt((xs - 0.35, yln + 0.4), '−', ha='right', fs=13)
    txt((xs + 0.75, (ylp + yln) / 2), 'LOCK\n12 V fail-safe\n2 A peak / 0.2 A')
    wire((xs, yln), q.drain)
    dot((xs, ylp)); dot((xs, yln))
    xd = 9.0
    wire((xs, ylp), (xd, ylp))
    wire((xs, yln), (xd, yln))
    A(elm.Diode().endpoints((xd, yln), (xd, ylp)))
    txt((xd + 0.45, (ylp + yln) / 2), 'D1  1N4007\ncathode → +12 V')

    # decoupling
    for x, lab, pol in ((12.6, 'C1\n1000 µF\n25 V', True), (14.8, 'C2\n100 nF', False)):
        dot((x, TOP))
        wire((x, TOP), (x, 5.2))
        A(elm.Capacitor(polar=pol).endpoints((x, 5.2), (x, 2.8)))
        txt((x + 0.3, 4.0), lab)
        wire((x, 2.8), (x, GND)); dot((x, GND))

    txt((0.0, -1.0), 'GND = 12 V (−) + buck converter GND + ESP32 GND, one common ground', fs=10)
    txt((0.0, -1.6),
        'GPIO25 HIGH or Hi-Z (reset, boot, flashing): gate = 3.3 V → Q1 on → lock powered → LOCKED\n'
        'GPIO25 LOW: gate ≈ 0.07 V → Q1 off → lock unpowered → OPEN\n'
        'Firmware: LOCK_DRIVER_NPN_INVERTER 0',
        va='top', fs=10)
    save(d, 'lock_driver_irf8736')


def irf4905():
    """Alternative: P-channel high side with two NPN stages."""
    TOP, GND = 11.0, 0.0
    d = schemdraw.Drawing(show=False)
    d.config(fontsize=11, unit=2.5, lw=1.4)
    A, txt, wire, dot = helpers(d)


    XV = 19.0  # supply symbol x

    # ---- rails
    wire((4.6, TOP), (XV, TOP))
    A(elm.Vdd().at((XV, TOP)))
    txt((XV + 0.3, TOP + 0.55), '+12 V  (PSU ≥ 3 A)')
    wire((0.0, GND), (XV, GND))
    A(elm.Ground().at((8.0, GND)))

    # ---- input stage: GPIO25 -> R1 -> Q1, R2 pull-down
    A(elm.Dot(open=True).at((0.0, 2.0)))
    txt((-0.25, 2.0), 'ESP32\nGPIO25', ha='right')
    A(elm.Resistor().endpoints((0.0, 2.0), (2.6, 2.0)))
    txt((1.3, 2.75), 'R1  4.7 kΩ', ha='center')
    wire((2.6, 2.0), (3.4, 2.0))
    dot((2.6, 2.0))
    A(elm.Resistor().endpoints((2.6, 2.0), (2.6, GND)))
    dot((2.6, GND))
    txt((2.15, 1.0), 'R2\n10 kΩ', ha='right')
    q1 = A(elm.BjtNpn(circle=True).theta(0).at((3.4, 2.0)))
    txt((q1.emitter[0] + 0.45, 2.0), 'Q1\n2N2222 / BC547')
    wire(q1.emitter, (q1.emitter[0], GND)); dot((q1.emitter[0], GND))

    # ---- Q2 base node
    yb = 5.5
    xb2 = 5.6                      # Q2 base x
    xr4 = 4.6
    wire(q1.collector, (q1.collector[0], yb), (xb2, yb))
    dot((xr4, yb))
    A(elm.Resistor().endpoints((xr4, TOP), (xr4, yb)))
    txt((xr4 - 0.5, 8.6), 'R4\n22 kΩ', ha='right')
    dot((xr4, TOP))
    txt((xr4 - 0.1, yb + 0.35), 'Q2_BASE', ha='right', fs=9, color=NET)
    q2 = A(elm.BjtNpn(circle=True).theta(0).at((xb2, yb)))
    txt((q2.emitter[0] + 0.45, yb), 'Q2\n2N2222 / BC547')
    wire(q2.emitter, (q2.emitter[0], GND)); dot((q2.emitter[0], GND))

    # ---- gate node + Q3
    yg = 8.4
    xr3 = 7.6
    xs = 10.2                      # Q3 source/drain x
    q3 = A(elm.PFet().theta(0).reverse().at((xs, yg + 0.75)))
    wire(q2.collector, (q2.collector[0], yg), q3.gate)
    dot((xr3, yg))
    A(elm.Resistor().endpoints((xr3, TOP), (xr3, yg)))
    txt((xr3 - 0.5, 9.7), 'R3\n10 kΩ', ha='right')
    dot((xr3, TOP))
    txt((xr3 + 0.1, yg - 0.4), 'GATE', fs=9, color=NET)
    wire(q3.source, (xs, TOP)); dot((xs, TOP))
    txt((xs + 0.5, yg), 'Q3  IRF4905\nP-channel, TO-220\n(pins: G  D  S)')
    txt((xs + 0.15, q3.source[1] + 0.3), 'S', fs=9)
    txt((xs + 0.15, q3.drain[1] - 0.3), 'D', fs=9)
    txt((q3.gate[0] + 0.05, yg + 0.35), 'G', fs=9)

    # ---- lock + flyback diode
    ylp, yln = 6.0, 2.2
    wire(q3.drain, (xs, ylp)); dot((xs, ylp))
    A(elm.Inductor2(loops=3).endpoints((xs, ylp), (xs, yln)))
    txt((xs - 0.35, ylp - 0.45), '+', ha='right', fs=13)
    txt((xs - 0.35, yln + 0.45), '−', ha='right', fs=13)
    txt((xs - 0.75, (ylp + yln) / 2), 'LOCK\n12 V fail-safe\n2 A peak / 0.2 A', ha='right')
    wire((xs, yln), (xs, GND)); dot((xs, GND)); dot((xs, yln))
    xd = 12.4
    wire((xs, ylp), (xd, ylp))
    wire((xs, yln), (xd, yln))
    A(elm.Diode().endpoints((xd, yln), (xd, ylp)))
    txt((xd + 0.45, (ylp + yln) / 2), 'D1  1N4007\ncathode → LOCK +')

    # ---- 12 V decoupling at the lock
    for x, lab, pol in ((16.0, 'C1\n1000 µF\n25 V', True), (18.0, 'C2\n100 nF', False)):
        dot((x, TOP))
        wire((x, TOP), (x, 7.0))
        A(elm.Capacitor(polar=pol).endpoints((x, 7.0), (x, 4.6)))
        txt((x + 0.3, 5.8), lab)
        wire((x, 4.6), (x, GND)); dot((x, GND))

    txt((0.0, -1.0), 'GND = 12 V (−) + buck converter GND + ESP32 GND, one common ground', fs=10)
    txt((0.0, -1.6),
        'GPIO25 LOW / Hi-Z (reset, boot, flashing):  Q1 off → Q2 on → GATE ≈ 0 V, Vgs ≈ −11.9 V → Q3 on → lock powered → LOCKED\n'
        'GPIO25 HIGH:  Q1 on → Q2 off → GATE = +12 V via R3, Vgs = 0 V → Q3 off → lock unpowered → OPEN\n'
        'Firmware: LOCK_DRIVER_NPN_INVERTER 1.   Optional: 15 V zener GATE → +12 V (cathode to +12 V) if the rail has spikes.',
        va='top', fs=10)

    save(d, 'lock_driver_irf4905')


if __name__ == '__main__':
    irf8736()
    irf4905()
