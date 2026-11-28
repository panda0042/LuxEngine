"""Bake original soft-edged surface marks into a padded PBR atlas for projected geometry."""
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

SIDE = 384
TILES = (4, 2)
DEST = Path(__file__).resolve().parents[2] / "Assets" / "FrontierMarks"
rng = np.random.default_rng(4805)
v, u = np.mgrid[-1:1:complex(SIDE), -1:1:complex(SIDE)]


def noise(cells):
    field = Image.fromarray(rng.integers(0, 256, (cells, cells), dtype=np.uint8))
    return np.asarray(field.resize((SIDE, SIDE), Image.Resampling.BICUBIC)) / 255.


def ramp(x, lo, hi):
    t = np.clip((x - lo) / (hi - lo), 0, 1)
    return t * t * (3 - 2 * t)


def ellipse(x, y, cx, cy, rx, ry):
    return np.sqrt(((x-cx)/rx)**2 + ((y-cy)/ry)**2)


def fracture():
    sketch = Image.new("L", (SIDE, SIDE))
    pen = ImageDraw.Draw(sketch)
    line = [(24, 116)]
    for i in range(1, 31):
        x = 24 + 11*i
        y = int(np.clip(line[-1][1]+rng.integers(-14, 15), 40, SIDE-50))
        pen.line([line[-1], (x, y)], fill=235, width=int(rng.integers(2, 6)))
        line.append((x, y))
        if i % 4 == 1:
            start = (x, y)
            side = 1 if i % 3 else -1
            for k in range(int(rng.integers(3, 10))):
                stop = (start[0]+int(rng.integers(-4, 14)), start[1]+side*int(rng.integers(4, 12)))
                pen.line([start, stop], fill=178 - 11*k, width=2 if k<3 else 1)
                start = stop
    return np.asarray(sketch.filter(ImageFilter.GaussianBlur(.45))) / 255.


def tile(index):
    fine = noise(112) * .5 + noise(36) * .5
    broad = noise(8) * .65 + noise(18) * .35
    edge = 1-ramp(np.maximum(abs(u), abs(v)), .84, .97)
    if index == 0:  # Staggered lugs with two circumferential grooves.
        lane = abs(v)
        phase = (u*5.8 + .65*abs(v) + .16*(v>0) + .13*broad) % 1
        lugs = (1-ramp(phase, .43, .58))*ramp(phase, .03, .10)
        grooves = ramp(abs(lane-.29), .04, .09)*ramp(lane,.03,.08)
        mask = (1-ramp(lane, .58, .78))*(.19+.81*lugs*grooves)
        color, rough, relief = (67,53,40), .61, -.35
    elif index in (1, 2):  # Boot with rounded toe, narrow waist and separate heel.
        y = v if index==1 else -v
        fore = ellipse(u,y,.29,-.06,.58,.73)
        heel = ellipse(u,y,-.52,.09,.28,.61)
        outline = 1-ramp(np.minimum(fore,heel), .88+.04*broad,1.0)
        phase = (u*6.5 + .54*abs(y+.08)) % 1
        ribs = (1-ramp(phase,.51,.65))*ramp(phase,.08,.19)
        mask = outline*(.27+.73*ribs*ramp(abs(y),.06,.15))
        color, rough, relief = (67,52,39), .64, -.55
    elif index == 3:
        mask=fracture()
        color,rough,relief=(44,42,39),.85,-.85
    else:
        distance = ellipse(u,v,0,0,.92,.78) + (broad-.5)*.62
        if index == 4:  # Fluvial sediment with thin downstream streaks.
            mask=(1-ramp(distance,.46,.99))*(.3+.7*broad)*(.5+.5*noise(26))
            color,rough,relief=(108,91,68),.83,.22
        elif index == 5:  # Damp ground, diffuse shoreline and glossy centre.
            mask=(1-ramp(distance,.53,.96))*(.72+.28*broad)
            color,rough,relief=(75,68,51),.19,-.15
        elif index == 6:  # Broken dark soil/nonuniform moss at a rock/soil boundary.
            mask=(1-ramp(distance,.55,1.05))*(.24+.76*fine)
            color,rough,relief=(77,76,51),.93,.30
        else:  # Thin erosion vein from concentrated runoff.
            channel = abs(v - (.27*np.sin(3*u+.7)+.21*(broad-.5)))
            mask = (1-ramp(channel,.03,.19))*(.35+.65*fine)*(1-ramp(abs(u),.5,.95))
            color,rough,relief=(99,76,52),.78,-.45
    mask=np.clip(mask*edge*(.77+.23*fine),0,1)
    rgb=np.array(color)[None,None,:]*(.68+.39*fine[:,:,None]+.17*broad[:,:,None])
    depth=relief*mask*(.68+.32*fine)
    dy,dx=np.gradient(depth)
    n=np.stack((-3*dx,3*dy,np.ones_like(dx)),axis=-1)
    n/=np.linalg.norm(n,axis=-1)[:,:,None]
    quant=lambda x:np.rint(np.clip(x,0,1)*255).astype(np.uint8)
    return (Image.fromarray(quant(np.dstack((rgb/255.,mask)))),
            Image.fromarray(quant(n*.5+.5)),
            Image.fromarray(quant(rough+(.5-fine)*.16)),
            Image.fromarray(quant(1.+np.minimum(depth,0)*.43)))


def main():
    DEST.mkdir(exist_ok=True)
    atlases=[Image.new(mode,(SIDE*TILES[0],SIDE*TILES[1])) for mode in ("RGBA","RGB","L","L")]
    for i in range(8):
        corner=(i%4*SIDE,i//4*SIDE)
        for dst,part in zip(atlases,tile(i)):
            dst.paste(part,corner)
    for image,name in zip(atlases,("albedo","normal","roughness","occlusion")):
        image.save(DEST/(name+".png"), optimize=True)


if __name__ == "__main__":
    main()
