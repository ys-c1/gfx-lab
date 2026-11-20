#!/usr/bin/env python3
# 独立参照实现：纯 Python 暴力重算三段管线与渗色报告，与 C 侧 test.c 的手推值交叉核对。
import sys

def box(src, sw, sh, ox=0, oy=0, dw=None, dh=None):
    # 在 src（sw x sh）上做 2x2 盒式降采样，子矩形原点 (ox,oy)，越界钳制
    if dw is None: dw = max(sw // 2, 1)
    if dh is None: dh = max(sh // 2, 1)
    out = [[[0]*3 for _ in range(dw)] for _ in range(dh)]
    for y in range(dh):
        for x in range(dw):
            gx0 = min(ox + x*2, sw-1); gx1 = min(ox + x*2+1, sw-1)
            gy0 = min(oy + y*2, sh-1); gy1 = min(oy + y*2+1, sh-1)
            for ch in range(3):
                out[y][x][ch] = (src[gy0][gx0][ch] + src[gy0][gx1][ch]
                                 + src[gy1][gx0][ch] + src[gy1][gx1][ch]) // 4
    return out

def block_chain(block_img, levels):
    chain = [block_img]
    w = len(block_img[0]); h = len(block_img)
    for _ in range(1, levels):
        nw, nh = max(w//2, 1), max(h//2, 1)
        chain.append(box(chain[-1], w, h, 0, 0, nw, nh))
        w, h = nw, nh
    return chain

def naive_chain(src, levels):
    chain = [src]; w = len(src[0]); h = len(src)
    for _ in range(1, levels):
        nw, nh = max(w//2, 1), max(h//2, 1)
        chain.append(box(chain[-1], w, h, 0, 0, nw, nh))
        w, h = nw, nh
    return chain

def measure(rects, src, level, block_levels):
    W, H = len(src[0]), len(src)
    chains = [block_chain([[src[y][x] for x in range(r[0], r[0]+r[2])]
                           for y in range(r[1], r[1]+r[3])], block_levels) for r in rects]
    nimg = naive_chain(src, level+1)[level]
    nw, nh = len(nimg[0]), len(nimg)
    pixels = bleed = 0; err_sum = 0; err_max = 0
    for ny in range(nh):
        for nx in range(nw):
            span = 1 << level
            cx = min((2*nx+1)*span // 2, W-1)
            cy = min((2*ny+1)*span // 2, H-1)
            bi = next((i for i, r in enumerate(rects)
                       if r[0] <= cx < r[0]+r[2] and r[1] <= cy < r[1]+r[3]), None)
            if bi is None:
                continue
            r = rects[bi]
            lx = min((cx-r[0]) >> level, len(chains[bi][level][0])-1)
            ly = min((cy-r[1]) >> level, len(chains[bi][level])-1)
            cp = chains[bi][level][ly][lx]; qp = nimg[ny][nx]
            pixels += 1
            es = [abs(qp[ch]-cp[ch]) for ch in range(3)]
            err_sum += sum(es); err_max = max(err_max, max(es))
            if any(es):
                bleed += 1
    return pixels, bleed, err_sum, err_max

def stripes(W, H, bw, colors):
    return [[colors[x//bw] for x in range(W)] for _ in range(H)]

R, G, B, Wc = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)
fails = 0
def ck(name, got, want):
    global fails
    ok = got == want
    if not ok:
        fails += 1
    print(f"{'OK ' if ok else 'BAD'} {name}: got={got} want={want}")

# 算例 A：8x8，4 个 2x8 条，4 层
A = stripes(8, 8, 2, [R, G, B, Wc])
rA = [(i*2, 0, 2, 8) for i in range(4)]
ck("A L1", measure(rA, A, 1, 4), (16, 0, 0, 0))
ck("A L2", measure(rA, A, 2, 4), (4, 4, 1022, 128))
ck("A L3", measure(rA, A, 3, 4), (1, 1, 382, 128))
l2 = naive_chain(A, 3)[2]
ck("A L2 naive px(0,0)", tuple(l2[0][0]), (127, 127, 0))
ck("A L2 naive px(0,1)", tuple(l2[0][1]), (127, 127, 255))

# 算例 B：32x8，4 个 8x8 块，5 层
Bb = stripes(32, 8, 8, [R, G, B, Wc])
rB = [(i*8, 0, 8, 8) for i in range(4)]
ck("B L2", measure(rB, Bb, 2, 5), (16, 0, 0, 0))
ck("B L3", measure(rB, Bb, 3, 5), (4, 0, 0, 0))
ck("B L4", measure(rB, Bb, 4, 5), (2, 2, 511, 128))

# toy 4x1
T = [[R, G, B, Wc]]
rT = [(0, 0, 2, 1), (2, 0, 2, 1)]
ck("toy L1", measure(rT, T, 1, 3), (2, 0, 0, 0))
ck("toy L2", measure(rT, T, 2, 3), (1, 1, 128, 128))
l2t = naive_chain(T, 3)[2]
ck("toy L2 naive px", tuple(l2t[0][0]), (127, 127, 127))   # R,G,B,W 四色均值；中心 x=2 归块1(B/W 均值 127,127,255)

# 贴边扩展手推：10x2，块 (1,0,2,2) 绿色，其余品红；扩展后列0与列9应变绿
def edge_extend(W, H, rects, src):
    out = [row[:] for row in src]
    for y in range(H):
        for x in range(W):
            if any(r[0] <= x < r[0]+r[2] and r[1] <= y < r[1]+r[3] for r in rects):
                continue
            best = None
            for i, r in enumerate(rects):
                gx = min(max(x, r[0]), r[0]+r[2]-1)
                gy = min(max(y, r[1]), r[1]+r[3]-1)
                d = abs(gx-x)+abs(gy-y)
                if best is None or d < best[0]:
                    best = (d, gx, gy)
            out[y][x] = src[best[2]][best[1]]
    return out

MAG = (255, 0, 255)
Gsrc = [[MAG]*10 for _ in range(2)]
for y in range(2):
    Gsrc[y][1] = G; Gsrc[y][2] = G
ext = edge_extend(10, 2, [(1, 0, 2, 2)], Gsrc)
ck("extend left px", tuple(ext[0][0]), G)
ck("extend right px", tuple(ext[0][9]), G)
ck("extend inside kept", tuple(ext[0][1]), G)
ck("no-extend gap kept", tuple(Gsrc[0][0]), MAG)
print("ALL PASS" if fails == 0 else f"{fails} FAILURES")
sys.exit(1 if fails else 0)
