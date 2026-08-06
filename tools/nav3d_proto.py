import struct, math
from PIL import Image, ImageDraw

RP='maps/allregions.roads'
f=open(RP,'rb'); hdr=f.read(64)
assert hdr[:8]==b'BNAVROAD'
ver,hb,flags,nsect = struct.unpack_from("<HHHH",hdr,8)
lat0,lon0,mlat,mlon = struct.unpack_from("<dddd",hdr,20)
cscale = struct.unpack_from("<I",hdr,52)[0]
f.seek(hb); st=[struct.unpack_from("<II",f.read(8),0) for _ in range(nsect)]
(noff,ncnt),(aoff,acnt),(eoff,ecnt)=st[0],st[1],st[2]
print(f"roads v{ver}: {ncnt} nodes, {ecnt} edges, ref {lat0},{lon0}")
f.seek(noff); NODES=f.read(8*ncnt)
f.seek(aoff); ADJ=f.read(4*acnt)
f.seek(eoff); EDGES=f.read(14*ecnt)
KLON = mlon*math.cos(math.radians(lat0))
def node_en(i):
    la,lo = struct.unpack_from("<ii",NODES,8*i)
    return ((lo/cscale - lon0)*KLON, (la/cscale - lat0)*mlat)
def adj(i): return struct.unpack_from("<I",ADJ,4*i)[0]
def edge(j):
    to,lmm,fl,nm = struct.unpack_from("<IIHI",EDGES,14*j)
    return to, (fl>>1)&0xF

SCR_W,MAPH=400,197
def collect(e0,n0,radius):
    """every edge with an endpoint inside radius, as (e,n)->(e,n) plus class"""
    segs=[]
    r2=radius*radius
    for i in range(ncnt):
        ei,ni = node_en(i)
        de,dn = ei-e0, ni-n0
        if de*de+dn*dn > r2: continue
        for j in range(adj(i), adj(i+1)):
            to,cls = edge(j)
            if to <= i: continue          # one direction only
            ej,nj = node_en(to)
            segs.append((ei,ni,ej,nj,cls))
    return segs

# ---------------------------------------------------------------- provenance
#
# This is the PROTOTYPE that decided DESIGN.md's 3D question, kept because the
# negative result is the valuable part and is not obvious from the positive one.
#
# Sampling the RASTER basemap in perspective does not work on a 1-bit panel. It
# breaks the one property the whole tile path rests on -- "a straight line stays
# connected ... never dotted" (6.5) -- because perspective minifies
# ANISOTROPICALLY: at 100 m out with h=40/f=160 the vertical ground step per
# screen row is z^2/(h*f) = 1.56 m while the horizontal step per column is
# z/f = 0.63 m, a ratio of 2.5. Nearest-neighbour sampling of a 1 px road then
# skips rows and the road dots. Choosing the rung from the horizontal step gives
# NAV3D-RUNGS.png left (dotted); choosing it from the vertical step gives the
# right (connected near, but the horizon is a solid bar, because a coarse rung's
# thick lines compress into a couple of rows). There is no rung between those.
#
# Drawing the VECTOR roads pack through the same projection has neither failure,
# because a line primitive is connected by construction. That needs, in order:
#   - class-based distance culling (an unclassified lane 2 km out is one row of
#     ink and hundreds of them are a black band -- CULL below),
#   - width that tapers with distance (a 3 px trunk at the horizon is the bar
#     again), and
#   - a SPATIAL INDEX, which the roads pack does not have. Six sections and none
#     of them is a grid: router.c scans all 1 738 370 nodes once per route,
#     which is fine, and doing it per frame at 8 Hz on a Pi Zero 2 W is not.
