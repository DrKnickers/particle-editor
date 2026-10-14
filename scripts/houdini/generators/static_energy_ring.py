# [houdini] static-energy-ring: one additive frame, a glowing ring of energy.
#
# A compositing network, no renderer: one Pixel COP computes the ring's
# intensity per pixel (a sharp band and a wide halo at `radius`, its edge
# warped and its brightness broken up by noise), a second takes it through the
# palette's tone curve, so dim light takes the tint and the hot core runs to
# white. Additive: RGB on black; the profile writes alpha 255.
#
# The seed moves the noise; `turbulence` scales the warp and the breakup. The
# expressions are built here from the checked parameters, formatted as
# numbers, and the seed's offsets; nothing from the job reaches them as text.
#
# Contract (run_generator.py): generate(hou, ctx) writes frames/ under the
# working directory and returns the frame names in index order.
import random

PALETTES = {
    "blue": (0.25, 0.55, 1.0),
    "green": (0.3, 1.0, 0.45),
    "orange": (1.0, 0.5, 0.12),
    "purple": (0.72, 0.35, 1.0),
}


def _parm(node, name):
    parm = node.parm(name)
    if parm is None:
        raise RuntimeError("missing parameter %s.%s" % (node.type().name(), name))
    return parm


def _f(value):
    return "%.6f" % float(value)


def intensity_expression(size, params, seed):
    """The ring's brightness at pixel ($X, $Y): COP pixels count from the bottom left."""
    rng = random.Random(seed)
    ox, oy, oz = (rng.uniform(0.0, 97.0) for _ in range(3))
    fx, fy, fz = (rng.uniform(0.0, 97.0) for _ in range(3))
    c = size / 2.0
    dist = "(sqrt(pow($X+0.5-%s,2)+pow($Y+0.5-%s,2))/%s)" % (_f(c), _f(c), _f(c))
    freq = 4.0 / size
    warp = "noise(($X)*%s+%s,($Y)*%s+%s,%s)" % (_f(freq), _f(ox), _f(freq), _f(oy), _f(oz))
    flick = "noise(($X)*%s+%s,($Y)*%s+%s,%s)" % (_f(freq * 7), _f(fx), _f(freq * 7), _f(fy), _f(fz))
    turb, radius, width = float(params["turbulence"]), float(params["radius"]), float(params["thickness"])
    warped = "(%s+%s*(%s-0.5))" % (dist, _f(turb * 0.05), warp)
    band = "exp(-pow((%s-%s)/%s,2))" % (warped, _f(radius), _f(width))
    halo = "0.35*exp(-pow((%s-%s)/%s,2))" % (warped, _f(radius), _f(width * 3.5))
    flicker = "(1-%s*clamp(%s*1.8-0.4,0,1))" % (_f(turb * 0.7), flick)
    # Falls to black before the frame's edge, so the ring never meets it: zero
    # from 0.97 of the half-width, which every edge texel's centre is beyond.
    window = "clamp((0.97-%s)/0.12,0,1)" % dist
    return "(%s+%s)*%s*%s" % (band, halo, flicker, window)


def generate(hou, ctx):
    size, params, seed = ctx["frame"], ctx["params"], ctx["seed"]
    net = hou.node("/img").createNode("img", "ring")
    base = net.createNode("color", "black")
    _parm(base, "overridesize").set(1)
    _parm(base, "size1").set(size)
    _parm(base, "size2").set(size)
    for channel in "rgb":
        _parm(base, "color" + channel).set(0.0)
    _parm(base, "colora").set(1.0)
    shape = net.createNode("pixel", "intensity")
    shape.setInput(0, base)
    _parm(shape, "rexpr").setExpression(intensity_expression(size, params, seed), hou.exprLanguage.Hscript)
    colour = net.createNode("pixel", "colour")
    colour.setInput(0, shape)
    for channel, tint in zip("rgb", PALETTES[params["palette"]]):
        _parm(colour, channel + "expr").setExpression(
            "clamp($R*%s+pow($R,4)*0.3,0,1)" % _f(tint * 1.3), hou.exprLanguage.Hscript)

    rop = hou.node("/out").createNode("comp", "frames")
    _parm(rop, "coppath").set(colour.path())
    _parm(rop, "convertcolorspace").set(0)
    _parm(rop, "mkpath").set(1)
    _parm(rop, "copoutput").set("frames/frame_$F4.exr")
    rop.render(frame_range=(1, 1), verbose=False)
    return ["frames/frame_0001.exr"]
