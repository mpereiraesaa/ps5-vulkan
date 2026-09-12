"""Offline analytical ray/box oracle for the owned scene3d fixture.

Not used to render or populate GPU outputs. Regional histograms avoid guessing
the unknown within-64KiB swizzle. No full-image CPU renderer is implemented.
"""
import argparse
from collections import Counter
import json
import math

CENTERS = ((-.45, 0., 0.), (.55, .12, -.35))
HALVES = (.65, .5)
CORNERS = ((-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),
           (-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1))
FACES = ((0,3,2,1),(4,5,6,7),(0,4,7,3),(1,2,6,5),(0,1,5,4),(3,7,6,2))


def inverse_rotate(p, angle):
    # Inverse of shader Rx(0.35) * Ry(angle).
    cx, sx = .939372713, .342897807
    q = (p[0], cx*p[1]+sx*p[2], -sx*p[1]+cx*p[2])
    c, s = math.cos(angle), math.sin(angle)
    return (c*q[0]-s*q[2], q[1], s*q[0]+c*q[2])


def box_hit(origin, direction, center, half):
    lo, hi = -math.inf, math.inf
    axis, side = None, None
    for a in range(3):
        minimum, maximum = center[a]-half, center[a]+half
        if abs(direction[a]) < 1e-15:
            if not minimum <= origin[a] <= maximum:
                return None
            continue
        t0, t1 = (minimum-origin[a])/direction[a], (maximum-origin[a])/direction[a]
        entering_side = -1 if t0 <= t1 else 1
        near, far = min(t0,t1), max(t0,t1)
        if near > lo:
            lo, axis, side = near, a, entering_side
        hi = min(hi,far)
    if lo > hi or lo < .1 or lo > 20:
        return None
    point = tuple((origin[a]+lo*direction[a]-center[a])/half for a in range(3))
    for face in FACES:
        if all(CORNERS[v][axis] == side for v in face):
            p0, p1, p3 = (CORNERS[face[i]] for i in (0,1,3))
            u = sum((point[a]-p0[a])*(p1[a]-p0[a]) for a in range(3))/4
            v = sum((point[a]-p0[a])*(p3[a]-p0[a]) for a in range(3))/4
            return lo, u, v
    raise AssertionError("face selection")


def nearest_index(coordinate, fraction_bits=None):
    """Ideal nearest by default; optional fixed-point rounding is diagnostic.

    This models a hypothesis, not measured sampler precision or a tolerance.
    Coordinate is normalized for the fixture's two-texel extent.
    """
    scaled = coordinate * 2
    if fraction_bits is not None:
        if not isinstance(fraction_bits, int) or not 4 <= fraction_bits <= 16:
            raise ValueError("diagnostic texel fraction bits must be 4..16")
        factor = 1 << fraction_bits
        scaled = math.floor(scaled * factor + .5) / factor
    return min(1, max(0, math.floor(scaled)))


def pixel(x, y, frame, texel_fraction_bits=None):
    angle=frame*.04
    origin=inverse_rotate((0.,0.,4.),angle)
    direction=inverse_rotate(((2*(x+.5)/1920-1)/1.2,
                              (2*(y+.5)/1080-1)/2.133333333, -1.),angle)
    hits=[(h, i) for i in range(2)
          if (h := box_hit(origin,direction,CENTERS[i],HALVES[i])) is not None]
    if not hits:
        return "black", 0, None
    (distance,u,v), owner=min(hits, key=lambda item:item[0][0])
    del distance
    column=nearest_index(u,texel_fraction_bits)
    row=nearest_index(v,texel_fraction_bits)
    color=("red","green","blue")[(row*2+column+frame//60)%3]
    return color,len(hits),owner


def pixel_without_depth(x, y, frame):
    """Last covering face in the original index order, with culling disabled.

    Intersect geometric face planes independently of the hardware rasterizer.
    Interior witness margins avoid triangle shared-edge fill-rule ambiguity.
    """
    origin=inverse_rotate((0.,0.,4.),frame*.04)
    direction=inverse_rotate(((2*(x+.5)/1920-1)/1.2,
                              (2*(y+.5)/1080-1)/2.133333333,-1.),frame*.04)
    result=("black",None)
    for owner,(center,half) in enumerate(zip(CENTERS,HALVES)):
        for face in FACES:
            axis=next(a for a in range(3) if len({CORNERS[v][a] for v in face})==1)
            if abs(direction[axis])<1e-15:continue
            distance=(center[axis]+half*CORNERS[face[0]][axis]-origin[axis])/direction[axis]
            if not .1<=distance<=20:continue
            point=tuple((origin[a]+distance*direction[a]-center[a])/half for a in range(3))
            if any(abs(point[a])>1+1e-10 for a in range(3) if a!=axis):continue
            p0,p1,p3=(CORNERS[face[i]] for i in (0,1,3))
            u=sum((point[a]-p0[a])*(p1[a]-p0[a]) for a in range(3))/4
            v=sum((point[a]-p0[a])*(p3[a]-p0[a]) for a in range(3))/4
            color=("red","green","blue")[(nearest_index(v)*2+nearest_index(u)+frame//60)%3]
            result=(color,owner)
    return result


def tile(tx, ty, frame, texel_fraction_bits=None):
    if not (0 <= tx < 15 and 0 <= ty < 8 and 0 <= frame < 180):
        raise ValueError("only full visible 128x128 blocks in the 180-frame fixture")
    counts=Counter(black=0,red=0,green=0,blue=0)
    overlap=Counter()
    for y in range(ty*128,(ty+1)*128):
        for x in range(tx*128,(tx+1)*128):
            color,hits,owner=pixel(x,y,frame,texel_fraction_bits)
            counts[color]+=1
            if hits==2:
                overlap[owner]+=1
    return dict(frame=frame, tile_x=tx,tile_y=ty,
                byte_offset=(ty*15+tx)*65536,counts=dict(counts),
                overlap_nearest_cube=dict(overlap))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--frame",type=int,required=True)
    p.add_argument("--tile",type=int,nargs=2,required=True)
    a=p.parse_args()
    print(json.dumps(tile(*a.tile,a.frame)))


if __name__=="__main__":
    main()
