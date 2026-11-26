"""Black-box tests: input validation, index equivalence, restart and WAL failures."""
import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv)>1 else str(Path('build/quickdb').resolve())
class DatabaseTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.root=Path(self.temp.name)
    def tearDown(self): self.temp.cleanup()
    def run_db(self, commands, env=None):
        return subprocess.run([BINARY,str(self.root)],input=commands,text=True,capture_output=True,env=env)
    def seed(self):
        r=self.run_db("CREATE TABLE users (id INT, name TEXT);\nINSERT INTO users VALUES (1, 'Ada');\n")
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertNotIn('ERROR',r.stdout)
    def test_types_and_syntax_do_not_mutate(self):
        self.seed()
        r=self.run_db("INSERT INTO users VALUES ('2', 'bad');\nINSERT INTO users VALUES (2);\nINSERT INTO users VALUES (9223372036854775808, 'bad');\nINSERT INTO users VALUES (2, 'bad') junk;\nSELECT * FROM users;\n")
        self.assertEqual(r.stdout.count('ERROR'),4)
        self.assertIn('END rows=1',r.stdout)
    def test_index_matches_scan_and_tracks_new_rows(self):
        self.seed()
        r=self.run_db("INSERT INTO users VALUES (+001, 'second');\nCREATE INDEX ON users (id);\nINSERT INTO users VALUES (1, 'third');\nSELECT * FROM users WHERE id = 1;\nSELECT * FROM users WHERE id = 1 SCAN;\nSELECT * FROM users WHERE id = 99;\n")
        parts=r.stdout.split('END')
        self.assertIn('rows=3 examined=3 plan=index',parts[1])
        self.assertIn('rows=3 examined=3 plan=scan',parts[2])
        self.assertIn('rows=0 examined=0 plan=index',parts[3])
        for name in ['Ada','second','third']: self.assertEqual(r.stdout.count('"'+name+'"'),2)
    def test_restart_rebuilds_index(self):
        self.seed()
        self.run_db('CREATE INDEX ON users (id);\n')
        r=self.run_db('SELECT * FROM users WHERE id = 1;\n')
        self.assertIn('"1","Ada"',r.stdout)
        self.assertIn('plan=index',r.stdout)
    def test_csv_quotes_and_empty_strings(self):
        r=self.run_db('CREATE TABLE t (a TEXT, b TEXT);\nINSERT INTO t VALUES (\'\', \'it\'\'s, "quoted"\');\nEXPORT t;\n')
        self.assertNotIn('ERROR',r.stdout)
        with (self.root/'t.csv').open() as f: rows=list(csv.reader(f))
        self.assertEqual(rows,[['a','b'],['', 'it\'s, "quoted"']])
    def test_crash_after_fsync_before_apply(self):
        self.seed()
        env=dict(os.environ,QUICKDB_CRASH_AFTER_WAL='1')
        r=self.run_db("INSERT INTO users VALUES (2, 'Grace');\n",env)
        self.assertEqual(r.returncode,86)
        r=self.run_db('SELECT * FROM users;\n')
        self.assertIn('"2","Grace"',r.stdout)
        self.assertIn('END rows=2',r.stdout)
    def test_torn_tail_is_removed_then_new_writes_recover(self):
        self.seed()
        wal=self.root/'database.wal'
        original=wal.read_bytes()
        for tail in [b'18',b'100 123\nINSERT INTO users']:
            wal.write_bytes(original+tail)
            r=self.run_db('SELECT * FROM users;\n')
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(wal.read_bytes(),original)
        self.run_db("INSERT INTO users VALUES (2, 'Grace');\n")
        self.assertIn('END rows=2',self.run_db('SELECT * FROM users;\n').stdout)
    def test_corruption_fails_without_truncating(self):
        self.seed()
        wal=self.root/'database.wal'
        damaged=wal.read_bytes().replace(b'Ada',b'Zda')
        wal.write_bytes(damaged)
        r=self.run_db('SELECT * FROM users;\n')
        self.assertNotEqual(r.returncode,0)
        self.assertIn('Corrupt WAL',r.stderr)
        self.assertEqual(wal.read_bytes(),damaged)
    def test_second_writer_rejected(self):
        p=subprocess.Popen([BINARY,str(self.root)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            p.stdin.write('CREATE TABLE t (id INT);\n');p.stdin.flush()
            self.assertEqual(p.stdout.readline().strip(),'OK')
            r=self.run_db('SELECT * FROM t;\n')
            self.assertNotEqual(r.returncode,0)
            self.assertIn('already open',r.stderr)
        finally:
            p.communicate('QUIT\n',timeout=5)
if __name__=='__main__': unittest.main()
