# TUNNEL FM-1: each preset's VOLUME so the bank plays at one loudness. Renders every factory preset for 8 seconds at
# VOLUME 100 (host/render.c, built here), measures its loudness and writes tools/levels.json; then
# tools/gen_presets.py writes the volumes into app/factory.h. Run from tunnel-fm1/ after gen_presets.py.
import array, json, math, os, re, subprocess, tempfile, wave

d = tempfile.mkdtemp()
exe = os.path.join(d, "render")
if os.path.exists("tools/levels.json"):
    os.remove("tools/levels.json")
subprocess.check_call(["python3", "tools/gen_presets.py"])
subprocess.check_call(["cc", "-O2", "-DPLAT_HOST", "-I", "../fm1-base/include", "-I", "app", "host/render.c", "app/tun.c", "-lm", "-o", exe])
names = re.findall(r'^\{"([^"]+)"', open("app/factory.h").read(), re.M)
db = {}
for i, n in enumerate(names):
    out = os.path.join(d, "%03d.wav" % i)
    subprocess.check_call([exe, str(i), "8", out])
    w = wave.open(out)
    a = array.array("h", w.readframes(w.getnframes()))[::2]
    k = 2205
    win = sorted(math.sqrt(sum(x * x for x in a[j:j + k]) / k) for j in range(0, len(a) - k, k))
    top = win[len(win) // 4:]                     # the loud three quarters: what plays, not the gaps
    db[n] = 20 * math.log10(sum(top) / len(top) / 32768 + 1e-9)
target = sorted(db.values())[len(db) // 2]
levels = {n: max(70, min(127, round(100 + (target - v) / 0.45))) for n, v in db.items()}
json.dump(levels, open("tools/levels.json", "w"), indent=0, sort_keys=True)
subprocess.check_call(["python3", "tools/gen_presets.py"])
print("target %.1f dB; volumes %d..%d" % (target, min(levels.values()), max(levels.values())))
