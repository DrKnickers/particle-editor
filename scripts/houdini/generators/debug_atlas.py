# [houdini] debug-atlas: the aesthetics-free frame-order oracle (plan §3.4).
#
# Writes grid*grid frames, each one flat colour that encodes its own index:
# red = (i % grid) / (grid - 1), green = (i // grid) / (grid - 1), blue = 0.5.
# Read back through the packer and the editor, the colours over time must
# decode to 0, 1, ..., grid*grid - 1; a flip, a transposition or an
# off-by-one each give a wrong sequence.
#
# Each frame also carries an orientation mark: its top-left quarter-by-quarter
# corner has blue = 1. A frame flipped or mirrored inside its cell moves the
# mark, which a cell's flat colour can't show. The mark stays clear of the
# frame's centre, where the editor validation reads the colour.
#
# The frames come from a compositing network, not a renderer: on the dev box a
# Mantra frame costs ~10 s of fixed overhead, and the Karma ROP samples an
# animated material once. An oracle needs exact flat colour fast; the
# generators that simulate exercise the renderers.
#
# Contract (run_generator.py): generate(hou, ctx) writes frames/ under the
# working directory and returns the frame names in index order. Paths are
# relative and fixed here; nothing from the job reaches a string parameter.


def _parm(node, name):
    parm = node.parm(name)
    if parm is None:
        raise RuntimeError("missing parameter %s.%s" % (node.type().name(), name))
    return parm


def index_colour(index, grid):
    span = float(grid - 1)
    return ((index % grid) / span, (index // grid) / span, 0.5)


def generate(hou, ctx):
    # Frames fill the cell inside its guard band; the packer places them.
    grid, size = ctx["grid"], ctx["frame"]
    count = grid * grid
    net = hou.node("/img").createNode("img", "atlas")
    colour = net.createNode("color", "flat")
    _parm(colour, "overridesize").set(1)
    _parm(colour, "size1").set(size)
    _parm(colour, "size2").set(size)
    _parm(colour, "length").set(count)
    _parm(colour, "colora").set(1.0)
    # One constant keyframe per frame: one render call covers the range.
    for component, channel in enumerate("rgb"):
        parm = _parm(colour, "color" + channel)
        for index in range(count):
            key = hou.Keyframe()
            key.setFrame(index + 1)
            key.setValue(index_colour(index, grid)[component])
            key.setExpression("constant()", hou.exprLanguage.Hscript)
            parm.setKeyframe(key)

    # COP pixel coordinates start at the bottom left, so the top-left corner is
    # low X and high Y. The bounds are integers from the tier, never job text.
    mark = net.createNode("pixel", "mark")
    mark.setInput(0, colour)
    _parm(mark, "bexpr").setExpression("if($X < %d && $Y >= %d, 1, $B)" % (size // 4, size - size // 4),
                                       hou.exprLanguage.Hscript)

    rop = hou.node("/out").createNode("comp", "frames")
    _parm(rop, "coppath").set(mark.path())
    _parm(rop, "convertcolorspace").set(0)
    _parm(rop, "mkpath").set(1)
    _parm(rop, "copoutput").set("frames/frame_$F4.exr")
    rop.render(frame_range=(1, count), verbose=False)
    return ["frames/frame_%04d.exr" % (index + 1) for index in range(count)]
