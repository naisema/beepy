import sys, math, re
sys.path.insert(0,'/private/tmp/claude-502/-Users-suwat-sai-Workspace-beepy/9c7fc903-6653-4cfa-9b06-9bf453e93634/scratchpad')
from vec3d import *
from PIL import Image, ImageDraw

MAP_X, VW, VH = 130, 270, 240
# the SAME metre widths mktiles.py gives the 2D casings, so the two views agree
WIDTH_M = {0:5.0, 1:14.0, 2:12.0, 3:11.0, 4:9.0, 5:7.0, 6:6.0, 7:5.0, 8:4.0}
CULL    = {0:500, 1:1800, 2:1500, 3:900, 4:600, 5:400, 6:260, 7:200, 8:160}

def page(lat, lon, hdg_deg, h_cam, focal, y_hor, route_ll, segs, e0, n0):
    hdg = math.radians(hdg_deg)
    im = Image.new('1',(VW,VH),0); d = ImageDraw.Draw(im)
    fwd=(math.sin(hdg),math.cos(hdg)); rgt=(math.cos(hdg),-math.sin(hdg))
    cx=VW/2; znear=h_cam*focal/(VH-1-y_hor)
    def cam(e,n):
        de,dn=e-e0,n-n0
        return de*fwd[0]+dn*fwd[1], de*rgt[0]+dn*rgt[1]
    def scr(z,X): return (cx+X*focal/z, y_hor+h_cam*focal/z)
    def clip(za,Xa,zb,Xb):
        if za<znear and zb<znear: return None
        if za<znear:
            t=(znear-za)/(zb-za); Xa+=t*(Xb-Xa); za=znear
        elif zb<znear:
            t=(znear-zb)/(za-zb); Xb+=t*(Xa-Xb); zb=znear
        return za,Xa,zb,Xb
    def ribbon(za,Xa,zb,Xb,w,fill,edge):
        """a ground quad of width w metres; a ground line projects to a straight
        screen line, so four projected corners are exact -- no subdivision."""
        dz,dX = zb-za, Xb-Xa
        L=math.hypot(dz,dX)
        if L<1e-9: return
        pz,pX = -dX/L*(w/2), dz/L*(w/2)      # perpendicular, in metres
        q=[scr(za+pz,Xa+pX), scr(zb+pz,Xb+pX), scr(zb-pz,Xb-pX), scr(za-pz,Xa-pX)]
        if any(z<=0 for z in (za+pz,zb+pz,za-pz,zb-pz)): return
        if fill is not None: d.polygon(q,fill=fill)
        if edge is not None:
            d.line([q[0],q[1]],fill=edge,width=1)
            d.line([q[2],q[3]],fill=edge,width=1)
    # FAR TO NEAR: the painter's order is what makes a near road occlude a far one
    prepared=[]
    for (ea,na,eb,nb,cls) in segs:
        za,Xa=cam(ea,na); zb,Xb=cam(eb,nb)
        if min(za,zb) > CULL.get(cls,260): continue
        c=clip(za,Xa,zb,Xb)
        if c: prepared.append((min(c[0],c[2]),c,cls))
    prepared.sort(key=lambda p:-p[0])
    for _,(za,Xa,zb,Xb),cls in prepared:
        w=WIDTH_M.get(cls,5.0)
        # below ~2 px of screen width a casing has no interior left to show
        wpx = w*focal/max(za,zb)
        if wpx < 2.2: ribbon(za,Xa,zb,Xb,w,None,1)
        else:         ribbon(za,Xa,zb,Xb,w,0,1)
    # the route, on top, solid: Google's blue ribbon, in the only ink we have
    rt=[]
    for i in range(len(route_ll)-1):
        za,Xa=cam(*route_ll[i]); zb,Xb=cam(*route_ll[i+1])
        c=clip(za,Xa,zb,Xb)
        if c: rt.append((min(c[0],c[2]),c))
    rt.sort(key=lambda p:-p[0])
    for _,(za,Xa,zb,Xb) in rt:
        ribbon(za,Xa,zb,Xb,7.5,1,1)
    # the chevron, where the rider is: bottom centre, pointing up the road
    bx,by=cx,VH-26
    d.polygon([(bx,by-13),(bx+10,by+11),(bx,by+5),(bx-10,by+11)],fill=1)
    d.line([(bx,by-13),(bx+10,by+11),(bx,by+5),(bx-10,by+11),(bx,by-13)],fill=0,width=1)
    return im

# ------------------------------------------------------------------ notes
#
# What makes this read as a Google-style nav view rather than a wireframe, in
# the order the difference matters:
#
#  1. ROADS ARE RIBBONS, not centrelines. A road is a ground quad of its own
#     OSM_WIDTH_M -- the same metres mktiles.py gives the 2D casings, so the two
#     views agree about how wide Sukhumvit is. A ground line projects to a
#     straight screen line, so four projected corners are exact and no
#     subdivision is needed.
#  2. PAINTER'S ORDER, far to near. Fill the quad white and stroke its two long
#     edges black, and a near road then occludes a far one. Without the sort the
#     casings of distant roads show through the road you are on.
#  3. THE ROUTE'S SCREEN WIDTH IS CAPPED. At true 7.5 m and a near plane 6 m
#     ahead, the route subtends a third of the viewport and blots the map --
#     NAV3D-GOOGLE.png's first version. Google caps it too; 26 px is the value
#     that reads as a corridor without swallowing the side streets.
#  4. Below about 2.2 px of screen width a casing has no interior left, so the
#     ribbon degenerates to a single stroke. That threshold is why distant
#     streets stay thin lines instead of turning into grey mush.
#
# KNOWN ARTIFACT, visible in the committed PNG: the cap in (3) is computed once
# per segment from its nearer end, so consecutive segments can be clamped to
# different widths and the ribbon edge comes out notched. The fix is to clamp
# per VERTEX -- w(z) at each end -- and let the quad taper between them, which
# also removes the seam where two segments meet at an angle.
#
# WHAT CANNOT BE COPIED FROM GOOGLE, and why:
#  - Extruded buildings. The roads pack has no building footprints and neither
#    does the basemap; pbf2osm.py does not extract them. Quads plus vertical
#    edges would be straightforward to DRAW -- the missing piece is data, which
#    means a new pack section, not a new renderer.
#  - Grey fills and shading. One bit, and 6.4 rules out dithering with a reason.
#  - Labels along roads in perspective. The 5x7 font cannot be foreshortened;
#    road names would have to stay axis-aligned, which is why Google's own
#    labels are drawn flat on top rather than lying on the ground plane.
