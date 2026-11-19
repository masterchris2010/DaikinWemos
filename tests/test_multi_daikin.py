from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/DaikinWemos'


class MultiDaikinNativeTests(unittest.TestCase):
    def test_peer_authorization_merge_restart_and_real_s21_result_mapping(self):
        compiler = os.environ.get('CXX') or shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required for Multi-Daikin native tests')
        source = (FIRMWARE / 'MultiDaikin.h').read_text(encoding='utf-8')
        start = source.index('uint16_t execute(Operation op) {')
        end = source.index('\nbool applyClock(', start)
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / 'multi_execute.inc').write_text(source[start:end], encoding='utf-8')
            message = source[source.index('struct Message {'):source.index('struct Store {')]
            begin = source.index('void receive(uint32_t now) {')
            end = source.index('\nvoid tickJobs(', begin)
            (folder / 'multi_wire.inc').write_text(message + '\n#include "multi_wire_stubs.inc"\n' +
                                                   source[begin:end], encoding='utf-8')
            begin = source.index('void tickShared() {')
            end = source.index('\n} // namespace MultiDaikin', begin)
            (folder / 'multi_schedule.inc').write_text(source[begin:end], encoding='utf-8')
            exe = folder / ('multi.exe' if os.name == 'nt' else 'multi')
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(FIRMWARE),
                            '-I', tmp, '-I', str(ROOT/'tests/native'), str(ROOT/'tests/native/multi_daikin.cpp'), '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, cwd=tmp, capture_output=True, text=True)
