import sys, math, re
sys.path.insert(0,'/private/tmp/claude-502/-Users-suwat-sai-Workspace-beepy/9c7fc903-6653-4cfa-9b06-9bf453e93634/scratchpad')
from vec3d import *
from PIL import Image, ImageDraw

MAP_X, VW, VH = 130, 270, 240
WIDTH_M = {0:5.0, 1:14.0, 2:12.0, 3:11.0, 4:9.0, 5:7.0, 6:6.0, 7:5.0, 8:4.0}
CULL    = {0:900, 1:4000, 2:3000, 3:2000, 4:1400, 5:900, 6:600, 7:450, 8:350}

class Cam:
    """A real pitched pinhole. pitch=90 deg is EXACTLY the top-down 2D view --
    that is the property that makes 2D and 3D one code path with one knob."""
    def __init__(self, pitch_deg, focal, nadir_mpp, hdg_deg, chevron_frac=0.72):
        self.p = math.radians(pitch_deg)
        self.f = focal
        self.h = nadir_mpp * focal          # h/f is the metres-per-pixel at nadir
        self.sp, self.cp = math.sin(self.p), math.cos(self.p)
        self.cx, self.cy = VW/2.0, VH/2.0
        hdg = math.radians(hdg_deg)
        self.F = (math.sin(hdg), math.cos(hdg))
        self.R = (math.cos(hdg), -math.sin(hdg))
        # put the chevron at the same screen row for every pitch, so the tilts
        # are actually comparable instead of also being different zooms
        v_r = self.cy - chevron_frac*VH
        den = self.f*self.sp - v_r*self.cp
        t_r = self.h/den
        self.back = t_r*(v_r*self.sp + self.f*self.cp)
        self.horizon_v = self.f*self.sp/self.cp if self.cp > 1e-9 else None
    def ground(self, x, y):
        """screen -> (forward, lateral) metres relative to the RIDER"""
        u = x - self.cx; v = self.cy - y
        den = self.f*self.sp - v*self.cp
        if den <= 1e-9: return None
        t = self.h/den
        return (-self.back + t*(v*self.sp + self.f*self.cp), t*u)
    def screen(self, fwd, lat):
        """(forward, lateral) metres relative to the rider -> screen, or None
        if the point is behind the camera plane."""
        Y = fwd + self.back                     # forward from the CAMERA
        # invert: u = lat/t, v from Y = t*(v sp + f cp), t = h/(f sp - v cp)
        # => Y*(f sp - v cp) = h*(v sp + f cp)
        # => Y f sp - Y v cp = h v sp + h f cp
        # => v (-Y cp - h sp) = h f cp - Y f sp
        den = -(Y*self.cp + self.h*self.sp)
        if abs(den) < 1e-9: return None
        v = (self.h*self.f*self.cp - Y*self.f*self.sp)/den
        t = self.h/(self.f*self.sp - v*self.cp)
        if t <= 0: return None
        return (self.cx + lat/t, self.cy - v)

def screen_t(c, fwd, lat):
    Y = fwd + c.back
    den = -(Y*c.cp + c.h*c.sp)
    if abs(den) < 1e-9: return None
    v = (c.h*c.f*c.cp - Y*c.f*c.sp)/den
    if c.cp > 1e-9 and v >= c.horizon_v - 1e-6: return None
    t = c.h/(c.f*c.sp - v*c.cp)
    if t <= 0: return None
    return (c.cx + lat/t, c.cy - v, t)

def draw_page(c, segs_g, route_g, cap_px=20):
    im = Image.new('1',(VW,VH),0); d = ImageDraw.Draw(im)
    def ribbon(a, b, w, fill, edge, ew=1, cap=None):
        """a,b are (fwd,lat). Half-width is capped PER VERTEX, so the quad
        tapers smoothly instead of stepping between segments -- that notch is
        the artifact the centreline mockup had."""
        (fa,la),(fb,lb) = a,b
        df,dl = fb-fa, lb-la
        L = math.hypot(df,dl)
        if L < 1e-9: return
        pf,pl = -dl/L, df/L
        ends=[]
        for (f0,l0) in ((fa,la),(fb,lb)):
            s = screen_t(c,f0,l0)
            if s is None: return
            hw = w/2.0
            if cap: hw = min(hw, cap/2.0*s[2])
            p1 = screen_t(c, f0+pf*hw, l0+pl*hw)
            p2 = screen_t(c, f0-pf*hw, l0-pl*hw)
            if p1 is None or p2 is None: return
            ends.append((p1,p2))
        q=[(ends[0][0][0],ends[0][0][1]),(ends[1][0][0],ends[1][0][1]),
           (ends[1][1][0],ends[1][1][1]),(ends[0][1][0],ends[0][1][1])]
        if fill is not None: d.polygon(q,fill=fill)
        if edge is not None:
            d.line([q[0],q[1]],fill=edge,width=ew); d.line([q[2],q[3]],fill=edge,width=ew)
    prep=[]
    for (a,b,cls) in segs_g:
        sa,sb = screen_t(c,*a), screen_t(c,*b)
        if sa is None or sb is None: continue
        prep.append((max(sa[2],sb[2]), a, b, cls, min(sa[2],sb[2])))
    prep.sort(key=lambda p:-p[0])
    for _,a,b,cls,tnear in prep:
        w=WIDTH_M.get(cls,5.0)
        ribbon(a,b,w,None if (w/tnear)<2.2 else 0,1)
    for i in range(len(route_g)-1):
        ribbon(route_g[i],route_g[i+1],9.0,1,1,1,cap_px)
    # chevron in a white disc, as the reference has it
    s=screen_t(c,0.0,0.0)
    if s:
        bx,by=s[0],s[1]
        d.ellipse([bx-13,by-13,bx+13,by+13],fill=0,outline=1)
        d.polygon([(bx,by-9),(bx+7,by+8),(bx,by+3),(bx-7,by+8)],fill=1)
    return im

def speedo(d, x, y, limit, cur, over):
    """The subject of the reference shot: a speed-LIMIT box beside the speed."""
    d.rectangle([x,y,x+58,y+26],fill=0,outline=1)
    d.rectangle([x+2,y+2,x+25,y+23],fill=0,outline=1)
    d.rectangle([x+3,y+3,x+24,y+22],fill=0,outline=1)
    from PIL import ImageFont
    f=ImageFont.load_default()
    d.text((x+7,y+9),str(limit),fill=1,font=f)
    d.text((x+31,y+4),str(cur),fill=1,font=f)
    d.text((x+31,y+15),"KM/H",fill=1,font=f)
    if over:                      # the red "56" becomes an inversion on 1 bit
        d.rectangle([x+28,y+2,x+56,y+13],fill=1)
        d.text((x+31,y+4),str(cur),fill=0,font=f)

def draw_map(c, segs_g, route_g, cap_px=20, min_seg_px=3.0, min_w_px=0.8):
    """The map plane only. No speedometer, no new furniture -- the page keeps
    every overlay it already has (nav.c draws those and this does not touch them).

    min_seg_px is the fix for the smeared top edge at low pitch: a segment whose
    projection is shorter than a couple of pixels contributes a dot, not
    information, and thousands of them piling into the top rows is the smear.
    Distance culling alone cannot do this -- a trunk road legitimately runs to
    4 km, and at pitch 50 the top screen row is 5347 m out."""
    im = Image.new('1',(VW,VH),0); d = ImageDraw.Draw(im)
    def ribbon(a, b, w, fill, edge, ew=1, cap=None):
        (fa,la),(fb,lb) = a,b
        df,dl = fb-fa, lb-la
        L = math.hypot(df,dl)
        if L < 1e-9: return
        pf,pl = -dl/L, df/L
        ends=[]
        for (f0,l0) in ((fa,la),(fb,lb)):
            s = screen_t(c,f0,l0)
            if s is None: return
            hw = w/2.0
            if cap: hw = min(hw, cap/2.0*s[2])
            p1 = screen_t(c, f0+pf*hw, l0+pl*hw)
            p2 = screen_t(c, f0-pf*hw, l0-pl*hw)
            if p1 is None or p2 is None: return
            ends.append((p1,p2))
        q=[(ends[0][0][0],ends[0][0][1]),(ends[1][0][0],ends[1][0][1]),
           (ends[1][1][0],ends[1][1][1]),(ends[0][1][0],ends[0][1][1])]
        if fill is not None: d.polygon(q,fill=fill)
        if edge is not None:
            d.line([q[0],q[1]],fill=edge,width=ew); d.line([q[2],q[3]],fill=edge,width=ew)
    prep=[]
    for (a,b,cls) in segs_g:
        sa,sb = screen_t(c,*a), screen_t(c,*b)
        if sa is None or sb is None: continue
        if math.hypot(sb[0]-sa[0], sb[1]-sa[1]) < min_seg_px: continue
        # a ribbon thinner than a pixel is noise, not information. Measured at
        # pitch 50: this rule alone takes the ink in the top 40 rows from 3009
        # to 395, which is the smeared horizon going away.
        if WIDTH_M.get(cls,5.0)/max(sa[2],sb[2]) < min_w_px: continue
        prep.append((max(sa[2],sb[2]), a, b, cls, min(sa[2],sb[2])))
    prep.sort(key=lambda p:-p[0])
    for _,a,b,cls,tnear in prep:
        w=WIDTH_M.get(cls,5.0)
        ribbon(a,b,w,None if (w/tnear)<2.2 else 0,1)
    for i in range(len(route_g)-1):
        ribbon(route_g[i],route_g[i+1],9.0,1,1,1,cap_px)
    s=screen_t(c,0.0,0.0)
    if s:
        bx,by=s[0],s[1]
        d.ellipse([bx-13,by-13,bx+13,by+13],fill=0,outline=1)
        d.polygon([(bx,by-9),(bx+7,by+8),(bx,by+3),(bx-7,by+8)],fill=1)
    return im

# ------------------------------------------------------------------- notes
#
# This is the renderer that matches the reference the product owner supplied
# (Google-Maps-adds-speedometer-...jpg): a GENTLE oblique with the horizon off
# the top of the screen, not the windscreen view the first two prototypes drew.
# NAV3D-GMAP.png is its output at pitch 75/70/60/50.
#
# THE PROPERTY THE WHOLE DESIGN RESTS ON: pitch 90 degrees is EXACTLY the
# top-down view. Verified numerically -- at nadir_mpp 4.0 and focal 120, a point
# 100 m forward lands 25 px up and 100 m lateral lands 25 px right, both 4 m/px.
# So 2D and 3D are ONE code path with ONE knob, and the toggle sets a pitch
# rather than choosing a renderer. Two renderers would drift; this cannot.
#
# The earlier Mode-7 prototypes (nav3d_proto.py, nav3d_ribbon.py) cannot express
# the reference at all -- they assume a horizontal camera axis with the horizon
# ON screen, so their "tilt" knob was really a camera height. They are kept for
# the raster negative result recorded in nav3d_proto.py, not for this.
#
# WHAT THE PAGE KEEPS: everything. draw_map() returns the map plane only. The
# turn panel, compass, speed badge and scale bar are nav.c's and are untouched --
# the product owner asked for the existing design kept, and there is no
# speedometer and no speed-limit box here.
#
# Two things this needs that a top-down view does not:
#
#  - A KNOCKOUT behind each overlay. Over a dense oblique street grid the
#    compass and badge are legible only by luck. The 2D view gets away without
#    one because a raster basemap at 4 m/px is sparser than this.
#  - A SUB-PIXEL WIDTH CULL. Distance culling alone cannot work, because a trunk
#    road legitimately runs to 4 km and the top screen row is 905 m out at pitch
#    75, 1739 m at 60 and 5347 m at 50 -- the far field grows faster than any
#    fixed table can track. Culling a ribbon whose projected WIDTH is under
#    0.8 px is the rule that scales by itself, because it asks the question that
#    matters: is there anything left to draw? Measured at pitch 50, ink in the
#    top 40 rows: 3009 with a length rule alone, 395 with the width rule added.
#    The smeared horizon in the first NAV3D-GMAP.png is that 3009.
#
# The route ribbon's width is capped PER VERTEX (20 px), which is both what stops
# a 9 m route from eating the near field and the fix for the notched edge in
# NAV3D-GOOGLE.png, where clamping once per segment made consecutive segments
# step between widths.
#
# WHY THERE IS NO SPEED LIMIT, beyond the product owner not wanting one: the data
# does not exist. Measured on maps/thailand-latest.osm.pbf -- 2 923 134 highway
# ways in Thailand, 29 126 with `maxspeed`, 1.00%. Best classes are secondary
# 11.0%, trunk 9.7%, tertiary 4.7%; residential is 1.0% and service 0.1%. A limit
# box would be blank on essentially every road, which is worse than absent. Note
# this is a DATA gap, not a format one: EDGES flags is a u16 with 10 bits spare,
# so a 4-bit index into the eight common values would cost nothing. (An earlier
# count of the extract said 0% -- that was pbf2osm.py, which drops `maxspeed`.)
#
# STILL NOT POSSIBLE: extruded buildings (no footprints in either pack -- a
# pipeline job), grey fills and shading (one bit, and 6.4 rules out dithering),
# and labels lying on the road surface (the 5x7 font cannot be foreshortened,
# which is why Google draws its own labels flat on top).
