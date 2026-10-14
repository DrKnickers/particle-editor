# [houdini] static-smoke-puff: one transparent frame, a lit puff of smoke.
#
# A sphere becomes a fog volume (Cloud SOP), billowed by Cloud Noise; the seed
# moves the noise. Mantra renders it with the pyro shader's smoke under a key
# light and an unshadowed fill light, through an orthographic camera that keeps the
# puff inside the frame. Mantra's alpha is the volume's opacity and its colour
# is premultiplied, which is what the transparent profile expects. An edge mask
# then fades the frame's border to clear, so the densest, most billowed puff
# never meets the frame's edge.
#
# Contract (run_generator.py): generate(hou, ctx) writes frames/ under the
# working directory and returns the frame names in index order.
import os
import random

# Per tier: (pixel samples per axis, volume step rate).
QUALITY = {"preview": (2, 0.25), "validate": (3, 0.5), "final": (4, 1.0)}
EDGE = 0.05          # the edge mask's width, as a fraction of the frame


def _parm(node, name):
    parm = node.parm(name)
    if parm is None:
        raise RuntimeError("missing parameter %s.%s" % (node.type().name(), name))
    return parm


def _set(node, values):
    for name, value in values.items():
        _parm(node, name).set(value)


def generate(hou, ctx):
    size, params, seed = ctx["frame"], ctx["params"], ctx["seed"]
    rng = random.Random(seed)
    obj = hou.node("/obj")
    geo = obj.createNode("geo", "puff")
    shape = geo.createNode("sphere", "shape")
    _set(shape, {"type": 2, "rows": 24, "cols": 24, "scale": 0.75 - 0.2 * float(params["billow"])})
    cloud = geo.createNode("cloud", "cloud")
    cloud.setInput(0, shape)
    _set(cloud, {"samplediv": 60, "densitymultiplier": float(params["density"]) * 4.0})
    noise = geo.createNode("cloudnoise", "billow")
    noise.setInput(0, cloud)
    _set(noise, {"noiseamount": float(params["billow"]) * 0.45, "noiseelementsize": 0.55,
                 "noiseoffset1": rng.uniform(0, 50), "noiseoffset2": rng.uniform(0, 50), "noiseoffset3": rng.uniform(0, 50),
                 "advectoffset1": rng.uniform(0, 50), "advectoffset2": rng.uniform(0, 50), "advectoffset3": rng.uniform(0, 50)})
    noise.setDisplayFlag(True)
    noise.setRenderFlag(True)

    shader = hou.node("/mat").createNode("pyroshader", "smoke")
    grey = float(params["brightness"])
    _set(shader, {"s_densityscale": 1.0, "s_colorr": grey, "s_colorg": grey, "s_colorb": grey * 1.02})
    _parm(geo, "shop_materialpath").set(shader.path())

    cam = obj.createNode("cam", "cam")
    _set(cam, {"resx": size, "resy": size, "projection": 1, "orthowidth": 3.6})
    cam.parmTuple("t").set((0.0, 0.0, 5.0))
    # No falloff: a point light's physical falloff over this distance would
    # leave the puff near black.
    key = obj.createNode("hlight::2.0", "key")
    _set(key, {"light_type": 0, "light_intensity": 1.5, "atten_type": 0})
    key.parmTuple("t").set((-4.0, 5.0, 4.0))
    # The fill is a second point light without shadows, not an environment
    # light: Mantra samples an environment light through state its threads
    # build in a racy order, and about one render in four came out different in
    # a few texels. (The pyro shader ignores ambient lights.)
    fill = obj.createNode("hlight::2.0", "fill")
    _set(fill, {"light_type": 0, "light_intensity": 0.6, "atten_type": 0, "shadow_type": "off"})
    fill.parmTuple("t").set((4.0, -2.0, 5.0))

    samples, steprate = QUALITY[ctx["tierName"]]
    rop = hou.node("/out").createNode("ifd", "mantra")
    _set(rop, {"camera": cam.path(), "vobject": "", "forceobject": geo.path(), "alights": key.path() + " " + fill.path(),
               "soho_autoheadlight": 0, "vm_picture": "render/frame_$F4.exr", "vm_samplesx": samples, "vm_samplesy": samples,
               "vm_volumesteprate": steprate, "vm_usemaxthreads": 0, "vm_threadcount": int(ctx["threads"])})
    os.makedirs("render", exist_ok=True)
    rop.render(frame_range=(1, 1), verbose=False)

    # The edge mask scales all four premultiplied channels, so the border only
    # grows more transparent.
    net = hou.node("/img").createNode("img", "edge")
    read = net.createNode("file", "render")
    _parm(read, "filename1").set("render/frame_$F4.exr")
    fade = net.createNode("pixel", "mask")
    fade.setInput(0, read)
    mask = "clamp(min(min($X,%d-$X),min($Y,%d-$Y))/%.4f,0,1)" % (size - 1, size - 1, size * EDGE)
    for channel in "rgba":
        _parm(fade, channel + "expr").setExpression("$%s*%s" % (channel.upper(), mask), hou.exprLanguage.Hscript)
    comp = hou.node("/out").createNode("comp", "frames")
    _parm(comp, "coppath").set(fade.path())
    _parm(comp, "convertcolorspace").set(0)
    _parm(comp, "mkpath").set(1)
    _parm(comp, "copoutput").set("frames/frame_$F4.exr")
    comp.render(frame_range=(1, 1), verbose=False)
    return ["frames/frame_0001.exr"]
