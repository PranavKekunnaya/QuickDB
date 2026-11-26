"""Compare scan/index equality queries inside one warmed database process.
Times cover query execution only (not SQL parsing, printing, IPC, or fsync).
"""
import csv
import platform
from pathlib import Path
import random
import re
import statistics
import subprocess
import sys
import tempfile
binary=str(Path(sys.argv[1] if len(sys.argv)>1 else 'build/quickdb').resolve())
results=[]
for size in (1000,10000,50000):
    with tempfile.TemporaryDirectory() as root:
        p=subprocess.Popen([binary,root],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
        def execute(sql):
            p.stdin.write(sql+'\n');p.stdin.flush()
            lines=[]
            while True:
                line=p.stdout.readline().strip()
                if not line: raise RuntimeError('Database exited unexpectedly')
                if line.startswith('ERROR'): raise RuntimeError(line)
                lines.append(line)
                if line=='OK' or line.startswith('END'): return lines
        execute('CREATE TABLE items (id INT, label TEXT);')
        for i in range(size): execute(f"INSERT INTO items VALUES ({i}, 'item_{i}');")
        execute('CREATE INDEX ON items (id);')
        rng=random.Random(42)
        keys=[rng.randrange(size) for _ in range(550)]
        timings={'scan':[],'index':[]}
        # Alternate which plan runs first; use identical keys and check results.
        for i,key in enumerate(keys):
            outputs={}
            for plan in (('scan','index') if i%2 else ('index','scan')):
                lines=execute(f"SELECT * FROM items WHERE id = {key}"+(' SCAN;' if plan=='scan' else ';'))
                outputs[plan]=lines[:-1]
                if i>=50: timings[plan].append(int(re.search(r'query_ns=(\d+)',lines[-1]).group(1)))
            assert outputs['scan']==outputs['index']
        for plan,values in timings.items():
            values.sort()
            results.append({'rows':size,'plan':plan,'queries':len(values),'median_ns':statistics.median(values),'p99_ns':values[int(.99*len(values))-1]})
        p.communicate('QUIT\n',timeout=5)
        assert p.returncode==0
out=Path(__file__).parent/'results.csv'
with out.open('w',newline='') as f:
    writer=csv.DictWriter(f,fieldnames=results[0].keys());writer.writeheader();writer.writerows(results)
print(platform.platform())
print(out.read_text())
