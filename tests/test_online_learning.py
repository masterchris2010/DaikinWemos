import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OnlineLearningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get('CXX') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('g++ required for native learner tests')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        source = (ROOT / 'firmware/DaikinWemos/DaikinWemos.ino').read_text(encoding='utf-8')
        record = 'struct OnlineCheckpointHeader {' + source.split('struct OnlineCheckpointHeader {', 1)[1].split('};', 1)[0] + '};'
        magic = re.search(r'static const uint32_t ONLINE_CHECKPOINT_MAGIC = [^;]+;', source).group()
        checksum = 'uint32_t configChecksum(const void *record, size_t size) {' + source.split('uint32_t configChecksum(const void *record, size_t size) {', 1)[1].split('\nbool loadWifi()', 1)[0]
        storage = 'void loadOnlineLearning() {' + source.split('void loadOnlineLearning() {', 1)[1].split('\nvoid handleOnlineLearning()', 1)[0]
        (directory / 'checkpoint_functions.inc').write_text(record + '\n' + magic + '\n' + checksum + '\n' + storage, encoding='utf-8')
        cls.executable = directory / ('online.exe' if os.name == 'nt' else 'online')
        build = subprocess.run([compiler, '-std=c++17', '-O2', '-I', str(ROOT / 'firmware/DaikinWemos'),
                               '-I', str(directory), str(ROOT / 'tests/native/online_learning.cpp'),
                               '-o', str(cls.executable)], text=True, capture_output=True)
        if build.returncode:
            raise AssertionError(build.stderr)

    def run_case(self, name):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([str(self.executable), name], cwd=directory, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_labels_wait_ten_minutes_and_reject_control_changes(self):
        self.run_case('delay')

    def test_gaps_invalidate_pending_examples(self):
        self.run_case('gap')

    def test_invalid_sensors_and_millis_wrap(self):
        self.run_case('invalid')

    def test_pause_restore_and_invalid_weights(self):
        self.run_case('pause')

    def test_no_promotion_against_perfect_baseline(self):
        self.run_case('no_false_promotion')

    def test_frozen_future_validation_and_active_only_acceptance(self):
        self.run_case('frozen_trial')

    def test_gradient_and_memory_bounds(self):
        self.run_case('bounded')

    def test_real_checkpoint_code_recovers_from_corrupt_or_failed_writes(self):
        self.run_case('checkpoint')
