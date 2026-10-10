"""Complete wiring schematic for Lock_Door_RFID (schemdraw).

    python docs/draw_wiring.py      -> writes docs/wiring.png / .svg
"""
import schemdraw
import schemdraw.elements as elm

from draw_lock_driver import helpers, save

schemdraw.use('matplotlib')


def wiring():
    d = schemdraw.Drawing(show=False)
    d.config(fontsize=11, unit=2.0, lw=1.3)
    A, txt, wire, dot = helpers(d)

    def rail(xy, name):
        A(elm.Vdd().at(xy).label(name, fontsize=10))

    def gnd(xy):
        A(elm.Ground().at(xy))

    # ================================================================ ESP32
    # Ic pins are listed bottom -> top on each side.
    esp = A(elm.Ic(
        pins=[
            # slot 3/7 left empty: room for the door filter cap below GPIO27
            elm.IcPin(name='GPIO33', side='left', slot='1/7', anchorname='G33'),
            elm.IcPin(name='GPIO32', side='left', slot='2/7', anchorname='G32'),
            elm.IcPin(name='GPIO27', side='left', slot='4/7', anchorname='G27'),
            elm.IcPin(name='GND', side='left', slot='5/7', anchorname='GNDL'),
            elm.IcPin(name='3V3', side='left', slot='6/7', anchorname='V33'),
            elm.IcPin(name='VIN (5 V)', side='left', slot='7/7', anchorname='VIN'),
            elm.IcPin(name='GPIO26', side='right', anchorname='G26'),
            elm.IcPin(name='GPIO25', side='right', anchorname='G25'),
            elm.IcPin(name='GPIO22', side='right', anchorname='G22'),
            elm.IcPin(name='GPIO5', side='right', anchorname='G5'),
            elm.IcPin(name='GPIO23', side='right', anchorname='G23'),
            elm.IcPin(name='GPIO19', side='right', anchorname='G19'),
            elm.IcPin(name='GPIO18', side='right', anchorname='G18'),
        ],
        pinspacing=1.2, edgepadH=0.7, edgepadW=0.6, leadlen=0.8, size=(4.4, 9.8),
    ).theta(0).at((0, 0)).label('ESP32-WROOM-32 DevKit', loc='top', fontsize=12))

    # ================================================================ RC522
    rc = A(elm.Ic(
        pins=[
            elm.IcPin(name='GND', side='left', anchorname='GND'),
            elm.IcPin(name='3.3V', side='left', anchorname='V33'),
            elm.IcPin(name='RST', side='left', anchorname='RST'),
            elm.IcPin(name='SDA', side='left', anchorname='SDA'),
            elm.IcPin(name='MOSI', side='left', anchorname='MOSI'),
            elm.IcPin(name='MISO', side='left', anchorname='MISO'),
            elm.IcPin(name='SCK', side='left', anchorname='SCK'),
        ],
        pinspacing=1.2, edgepadH=0.6, edgepadW=0.6, leadlen=0.8, size=(3.0, 8.4),
    ).theta(0).anchor('SCK').at((esp.G18[0] + 5.5, esp.G18[1] + 3.6))
      .label('RC522 RFID reader (3.3 V only)', loc='top', fontsize=11))

    # SPI + RST, jogged so the lines never cross
    for i, (a, b) in enumerate((('G18', 'SCK'), ('G19', 'MISO'), ('G23', 'MOSI'),
                                ('G5', 'SDA'), ('G22', 'RST'))):
        pa, pb = getattr(esp, a), getattr(rc, b)
        xm = pa[0] + 0.6 + 0.6 * i      # highest pin jogs first
        wire(pa, (xm, pa[1]), (xm, pb[1]), pb)
    xv = rc.V33[0] - 0.7
    wire(rc.V33, (xv, rc.V33[1]))
    rail((xv, rc.V33[1]), '3V3')
    xgn = rc.GND[0] - 0.7
    wire(rc.GND, (xgn, rc.GND[1]))
    gnd((xgn, rc.GND[1]))

    # ================================================================ lock driver (option B)
    p25 = esp.G25
    x0 = rc.SCK[0] + 5.0               # driver to the right of the RC522
    wire(p25, (x0, p25[1]))
    A(elm.Resistor().endpoints((x0, p25[1]), (x0 + 2.4, p25[1])))
    txt((x0 + 1.2, p25[1] - 0.6), 'Rg 220 Ω', ha='center', fs=10)
    xgate = x0 + 2.4
    dot((xgate, p25[1]))
    q = A(elm.NFet().theta(0).reverse().anchor('gate').at((xgate + 0.9, p25[1])))
    wire((xgate, p25[1]), q.gate)
    A(elm.Resistor().endpoints((xgate, p25[1]), (xgate, p25[1] + 2.4)))
    txt((xgate - 0.3, p25[1] + 1.2), 'R_PU\n10 kΩ', ha='right', fs=10)
    rail((xgate, p25[1] + 2.4), '3V3')
    txt((q.source[0] + 0.4, p25[1] - 0.1), 'Q1  IRF8736\nN-ch, SO-8', fs=10)
    ys = q.source[1] - 0.6
    wire(q.source, (q.source[0], ys))
    gnd((q.source[0], ys))

    xs = q.drain[0]
    yln = q.drain[1] + 0.8
    ylp = yln + 3.0
    wire(q.drain, (xs, yln)); dot((xs, yln))
    A(elm.Inductor2(loops=3).endpoints((xs, yln), (xs, ylp)))
    dot((xs, ylp))
    wire((xs, ylp), (xs, ylp + 0.5))
    rail((xs, ylp + 0.5), '+12 V')
    txt((xs - 0.3, ylp - 0.35), '+', ha='right', fs=12)
    txt((xs - 0.3, yln + 0.35), '−', ha='right', fs=12)
    txt((xs + 0.5, (ylp + yln) / 2 + 0.6), 'LOCK', fs=11)
    xd = xs + 2.6
    wire((xs, ylp), (xd, ylp)); wire((xs, yln), (xd, yln))
    A(elm.Diode().endpoints((xd, yln), (xd, ylp)))
    txt((xd + 0.5, (ylp + yln) / 2), 'D1\n1N4007', fs=10)

    # ================================================================ buzzer
    p26 = esp.G26
    xj = p26[0] + 0.6
    yb = p26[1] - 3.4
    wire(p26, (xj, p26[1]), (xj, yb))
    A(elm.Resistor().endpoints((xj, yb), (xj + 2.4, yb)))
    txt((xj + 1.2, yb - 0.6), '1 kΩ', ha='center', fs=10)
    qb = A(elm.BjtNpn(circle=True).theta(0).anchor('base').at((xj + 2.4, yb)))
    txt((qb.emitter[0] + 0.45, yb), 'Q2\n2N2222 / BC547', fs=10)
    wire(qb.emitter, (qb.emitter[0], qb.emitter[1] - 0.4))
    gnd((qb.emitter[0], qb.emitter[1] - 0.4))
    ybz = qb.collector[1] + 1.8
    A(elm.RBox().endpoints(qb.collector, (qb.collector[0], ybz)))
    txt((qb.collector[0] - 0.45, (qb.collector[1] + ybz) / 2), 'BZ1\nactive buzzer', ha='right', fs=10)
    rail((qb.collector[0], ybz), '+5 V / +12 V')

    # ================================================================ power
    pv = esp.VIN
    yt = pv[1] + 2.0
    xbk = pv[0] - 1.0
    wire(pv, (xbk, pv[1]), (xbk, yt))
    bk = A(elm.Ic(
        pins=[elm.IcPin(name='IN−', side='left', anchorname='INN'),
              elm.IcPin(name='IN+', side='left', anchorname='INP'),
              elm.IcPin(name='OUT+', side='right', anchorname='OUTP'),
              elm.IcPin(name='OUT−', side='right', anchorname='OUTN')],
        pinspacing=1.0, edgepadH=0.5, edgepadW=0.5, leadlen=0.6, size=(3.2, 2.0),
    ).theta(0).anchor('OUTP').at((xbk, yt)).label('Buck 12 V → 5 V\nMP1584 / LM2596', loc='top', fontsize=10))
    wire(bk.OUTN, (bk.OUTN[0] + 0.8, bk.OUTN[1]))
    gnd((bk.OUTN[0] + 0.8, bk.OUTN[1]))
    xps = bk.INP[0] - 7.4
    wire(bk.INP, (xps, bk.INP[1]))
    wire(bk.INN, (bk.INN[0] - 0.6, bk.INN[1]))
    gnd((bk.INN[0] - 0.6, bk.INN[1]))
    rail((xps, bk.INP[1]), '+12 V  PSU ≥ 3 A')
    for i, (lab, pol) in enumerate((('C1\n1000 µF\n25 V', True), ('C2\n100 nF', False))):
        xc = xps + 1.6 + 2.4 * i
        dot((xc, bk.INP[1]))
        A(elm.Capacitor(polar=pol).endpoints((xc, bk.INP[1]), (xc, bk.INP[1] - 1.8)))
        gnd((xc, bk.INP[1] - 1.8))
        txt((xc + 0.35, bk.INP[1] - 0.9), lab, fs=9)

    # 3V3 and GND pins
    p3 = esp.V33
    wire(p3, (p3[0] - 2.6, p3[1]))
    rail((p3[0] - 2.6, p3[1]), '3V3')
    pg = esp.GNDL
    wire(pg, (pg[0] - 0.6, pg[1]))
    gnd((pg[0] - 0.6, pg[1]))

    # ================================================================ door sensor + buttons
    # 100 nF down into the empty slot below GPIO27, 10 k pull-up up to the 3V3 pin row
    p27 = esp.G27
    xcap, xn = p27[0] - 1.4, p27[0] - 2.4
    dot((xcap, p27[1])); dot((xn, p27[1]))
    A(elm.Capacitor().endpoints((xcap, p27[1]), (xcap, p27[1] - 1.2)))
    gnd((xcap, p27[1] - 1.2))
    txt((xcap + 0.3, p27[1] - 0.6), '100 nF', fs=9)
    A(elm.Resistor().endpoints((xn, p27[1]), (xn, p3[1])))
    dot((xn, p3[1]))
    txt((xn - 0.3, (p27[1] + p3[1]) / 2), '10 kΩ', ha='right', fs=9)
    xsw = p27[0] - 4.2
    wire(p27, (xsw, p27[1]))
    A(elm.SwitchReed().endpoints((xsw, p27[1]), (xsw - 2.6, p27[1])))
    gnd((xsw - 2.6, p27[1]))
    txt((xsw - 1.3, p27[1] + 0.9), 'COM         NO', ha='center', fs=9)
    txt((xsw - 1.3, p27[1] + 1.6), 'Door sensor (in the lock)\nclosed = door closed', ha='center', fs=10)

    for pin, lab in ((esp.G32, 'EXIT  (inside)'), (esp.G33, 'ENROLL  (inside)')):
        xbtn = pin[0] - 1.4
        wire(pin, (xbtn, pin[1]))
        A(elm.Button().endpoints((xbtn, pin[1]), (xbtn - 2.0, pin[1])))
        gnd((xbtn - 2.0, pin[1]))
        txt((xbtn - 2.5, pin[1]), lab, ha='right', fs=10)

    # ================================================================ notes
    yn = min(esp.G33[1], yb) - 2.2
    notes = (
        'One common ground: 12 V (−), buck GND, ESP32 GND, Q1 source, RC522 GND.',
        'GPIO25 HIGH or Hi-Z (reset/boot) → Q1 on → lock powered → LOCKED.   GPIO25 LOW → OPEN.',
        'Inputs use INPUT_PULLUP; add 10 kΩ + 100 nF on long button cables. 1N4148 across BZ1 if it is a magnetic buzzer.',
        'Recommended: NC emergency-release button in series with the lock +12 V, beside the exit button.',
    )
    for k, line in enumerate(notes):    # one label per line: matplotlib's line spacing is too tight
        txt((esp.G33[0] - 10, yn - 0.5 * k), line, va='top', fs=10)

    save(d, 'wiring')


if __name__ == '__main__':
    wiring()
