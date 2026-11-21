import sys

ERR = 0

def bad(msg):
    global ERR
    ERR += 1
    print("MISMATCH:", msg)

def check(path):
    with open(path, "r") as f:
        lines = f.read().splitlines()
    head = lines[0].split()
    n = int(head[head.index("n") + 1])
    ncand = int(head[head.index("cand") + 1])
    visible = int(head[head.index("visible") + 1])
    dl = lines[1].split()
    assert dl[0] == "drawlist"
    draw = [int(x) for x in dl[2:]]
    fr = lines[2].split()
    first = [int(x) for x in fr[2:]]
    cs = lines[3].split()
    assert cs[0] == "candidates"
    cand = [int(x) for x in cs[2:]]

    # 独立参考：唯一对象 = 集合；顺序 = 按首次出现
    seen = set()
    ref, reffirst = [], []
    for i, v in enumerate(cand):
        if v not in seen:
            seen.add(v)
            ref.append(v)
            reffirst.append(i)
    # 合并条数 = 候选数 - 唯一数
    merged = len(cand) - len(ref)

    if len(draw) != len(ref):
        bad("%s drawlist length %d != unique %d" % (path, len(draw), len(ref)))
    if draw != ref:
        bad("%s drawlist order/content differs from reference" % path)
    if first != reffirst:
        bad("%s first-occurrence differs" % path)
    if len(draw) != visible:
        bad("%s draw count %d != visible %d" % (path, len(draw), visible))

    # 逐对象手算：对象 i 出现 k=1+(i%4) 次，且只出现在可见集合里
    ksum = 0
    for v in set(cand):
        k = 1 + (v % 4)
        if cand.count(v) != k:
            bad("%s object %d count %d != hand %d" % (path, v, cand.count(v), k))
        ksum += k
    if ksum != ncand:
        bad("%s candidate total %d != hand %d" % (path, ncand, ksum))
    hand_merged = ncand - len(set(cand))
    if merged != hand_merged:
        bad("%s merge count %d != hand %d" % (path, merged, hand_merged))
    if merged != sum(1 + (v % 4) - 1 for v in set(cand)):
        bad("%s merge count %d != per-object k-1 sum" % (path, merged))

    print("%-44s n=%-7d cand=%-7d unique=%-7d merged=%-7d ok" %
          (path.split("/")[-1], n, ncand, len(ref), merged))

for p in sys.argv[1:]:
    check(p)

print("FAILURES:", ERR)
sys.exit(1 if ERR else 0)
