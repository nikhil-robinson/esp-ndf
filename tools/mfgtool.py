#!/usr/bin/env python3
"""Manufacturing utility: one NVS image per device from a config CSV and a
values CSV. Same inputs/outputs as ESP-IDF's mfg_gen.py (images byte-identical).

  mfgtool.py config.csv values.csv PREFIX SIZE [--outdir DIR] [--fileid COLUMN]
             [--keyfile keys.bin | --keygen]

config.csv : key,type,encoding[,REPEAT]  (namespace rows: name,namespace,)
values.csv : header of keys (extra columns such as 'id' allowed), one row per device;
             REPEAT keys take the first row's value. Outputs DIR/csv/PREFIX-ID.csv
             and DIR/bin/PREFIX-ID.bin (ID = --fileid column, else 1, 2, 3...).
"""
import argparse, csv, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvstool

ENCODINGS = {'string', 'binary', 'hex2bin', 'u8', 'i8', 'u16', 'u32', 'i32', 'base64'}


def rows_of(path):
    with open(path, encoding='utf-8') as f:
        return [r for r in csv.reader(l for l in f if not l.isspace())]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('conf'); ap.add_argument('values'); ap.add_argument('prefix'); ap.add_argument('size')
    ap.add_argument('--outdir', default=os.getcwd()); ap.add_argument('--fileid')
    ap.add_argument('--keyfile'); ap.add_argument('--keygen', action='store_true')
    a = ap.parse_args()

    conf = rows_of(a.conf)
    repeat = set()
    for n, r in enumerate(conf, 1):
        if r[1:2] == ['namespace']:
            continue
        if 'REPEAT' in r:
            r.remove('REPEAT'); repeat.add(r[0])
        if len(r) != 3 or r[1] not in ('file', 'data') or r[2] not in ENCODINGS:
            sys.exit('config line %d: need key,type,encoding with a valid type/encoding' % n)
    vals = rows_of(a.values)
    head, devices = vals[0], vals[1:]
    missing = [r[0] for r in conf if r[1] != 'namespace' and r[0] not in head]
    if missing:
        sys.exit('keys missing in values file: %s' % ', '.join(missing))
    if a.fileid and a.fileid not in head:
        sys.exit('--fileid %s is not a column of the values file' % a.fileid)
    for n, d in enumerate(devices, 2):
        if len(d) != len(head):
            sys.exit('values line %d: %d values for %d keys' % (n, len(d), len(head)))

    for sub in ('csv', 'bin'):
        os.makedirs(os.path.join(a.outdir, sub), exist_ok=True)
    key = nvstool.read_keys(a.keyfile) if a.keyfile else None
    size = int(a.size, 0)
    for n, d in enumerate(devices, 1):
        v = dict(zip(head, d))
        for k in repeat:
            v[k] = devices[0][head.index(k)]
        dev_id = v[a.fileid] if a.fileid else str(n)
        rows = [{'key': r[0], 'type': r[1], 'encoding': '' if r[1] == 'namespace' else r[2],
                 'value': '' if r[1] == 'namespace' else v[r[0]]} for r in conf]
        name = '%s-%s' % (a.prefix, dev_id)
        with open(os.path.join(a.outdir, 'csv', name + '.csv'), 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(['key', 'type', 'encoding', 'value'])
            for r in rows:
                w.writerow([r['key'], r['type'], r['encoding'], r['value']])
        dkey = key
        if a.keygen:                                  # one key per device, saved next to the image
            os.makedirs(os.path.join(a.outdir, 'keys'), exist_ok=True)
            kf = os.path.join(a.outdir, 'keys', 'keys-%s.bin' % name)
            nvstool.keygen(kf)
            dkey = nvstool.read_keys(kf)
        img = nvstool.generate_rows(rows, size, dkey)
        open(os.path.join(a.outdir, 'bin', name + '.bin'), 'wb').write(img)
        print('created', os.path.join(a.outdir, 'bin', name + '.bin'))


if __name__ == '__main__':
    main()
