"""Independent interior overlap witnesses for the bounded scene3d fixture.

Expectations only: never used to fill a GPU image or replace shader execution.
One-pixel scissor plus full opaque-black GPU clear makes the single nonblack
word identifiable without assuming the intra-tile memory swizzle.
"""
try:
    from .scene_reference import pixel, pixel_without_depth
except ImportError:
    from scene_reference import pixel, pixel_without_depth

# Selected from screen-space overlap, with both possible nearest owners across
# rotation. Coordinates name pixel centers through scene_reference.pixel().
POINTS = ((0,990,440,0,"green"), (45,810,440,0,"blue"),
          (90,970,320,1,"green"), (90,910,375,1,"red"),
          (135,1040,380,1,"red"), (135,950,440,0,"green"))
BGRA = {"red": 0xffff0000,"green": 0xff00ff00,"blue": 0xff0000ff}
DEPTH_OFF_INDICES = (0,1,2,5)


def witnesses():
    result=[]
    for index,(frame,x,y,owner,color) in enumerate(POINTS):
        # A complete 11x11 neighborhood must have the same visible owner/color
        # and overlap both boxes; probe is not chosen on a raster or texel edge.
        for dy in range(-5,6):
            for dx in range(-5,6):
                for precision in (None,8):
                    if pixel(x+dx,y+dy,frame,precision)!=(color,2,owner):
                        raise ValueError("witness lacks interior margin")
        off_color,off_owner=pixel_without_depth(x,y,frame)
        if index in DEPTH_OFF_INDICES and any(pixel_without_depth(x+dx,y+dy,frame)!=(off_color,off_owner)
                for dy in range(-5,6) for dx in range(-5,6)):
            raise ValueError("depth-off witness lacks interior margin")
        result.append(dict(frame=frame,x=x,y=y,owner=owner,color=color,bgra=BGRA[color],
                           off_bgra=BGRA[off_color],off_owner=off_owner))
    return result


def header():
    lines=["/* Owned analytical expectations; not GPU image/shader content. */",
           "#ifndef PS5VK_SCENE_WITNESSES_H", "#define PS5VK_SCENE_WITNESSES_H",
           "#include <stdint.h>",
           "struct ps5vk_scene_witness { unsigned frame,x,y,owner; uint32_t bgra,off_bgra; };",
           "static const struct ps5vk_scene_witness ps5vk_scene_witnesses[] = {"]
    for w in witnesses():
        lines.append("    {%d,%d,%d,%d,UINT32_C(0x%08x),UINT32_C(0x%08x)}," %
                     (w["frame"],w["x"],w["y"],w["owner"],w["bgra"],w["off_bgra"]))
    return "\n".join(lines+["};", "#endif", ""])


if __name__=="__main__":
    print(header(),end="")
