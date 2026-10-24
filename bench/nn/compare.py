"""Compare examples/nn_lib (ndf_result.txt) with bench/idf_nn (../idf_nn/result.txt): cycles + checksum equality."""
import re, os
d = os.path.dirname(os.path.abspath(__file__))
def load(f, pre):
    r = {}
    for l in open(f):
        m = re.match(pre + r" (.+?)\s+(\d+) cycles\s+sum (-?\d+)", l)
        if m: r[m.group(1).strip()] = (int(m.group(2)), int(m.group(3)))
    return r
a, b = load(os.path.join(d, "ndf_result.txt"), "ndf"), load(os.path.join(d, "..", "idf_nn", "result.txt"), "idf")
print("| layer | esp-ndf cycles | ESP-NN 1.4.1 cycles | gain | bit-exact |\n|---|---|---|---|---|")
for k in a:
    if k in b: print(f"| {k} | {a[k][0]} | {b[k][0]} | {b[k][0] / a[k][0]:.2f}x | {'yes' if a[k][1] == b[k][1] else 'NO'} |")
