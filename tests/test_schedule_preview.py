"""Exercise the actual firmware function with a synthetic clock and schedule."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SchedulePreviewTests(unittest.TestCase):
    def test_upcoming_rules_clock_and_week_rollover(self):
        compiler = os.environ.get('CXX') or shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required for the native firmware-function check')
        source = (ROOT / 'firmware/DaikinWemos/DaikinWemos.ino').read_text(encoding='utf-8')
        function = source.split('String nextScheduleText() {', 1)[1].split('\nvoid handleStatus()', 1)[0]
        preamble = r'''
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cassert>
struct String : std::string {
  using std::string::string;
  String(float value, int digits) { std::ostringstream s; s << std::fixed << std::setprecision(digits) << value; assign(s.str()); }
};
struct ScheduleRule { uint16_t minute; uint8_t days, halfDegrees; char mode; uint8_t enabled; };
struct { ScheduleRule rules[8]; } schedule = {};
std::tm clockValue = {};
bool synchronized = true;
bool localClock(std::tm &out) { out = clockValue; return synchronized; }
String nextScheduleText() {
'''
        cases = r'''
int main() {
  clockValue.tm_wday = 1; clockValue.tm_hour = 18;
  schedule.rules[0] = {1110,2,42,'4',1};
  assert(nextScheduleText() == "Oggi · 18:30 · 21.0 °C");
  schedule.rules[1] = {1095,2,0,'0',1};
  assert(nextScheduleText() == "Oggi · 18:15 · Spegnimento");
  schedule.rules[1].enabled = 0;
  clockValue.tm_min = 30;
  assert(nextScheduleText() == "Lun · 18:30 · 21.0 °C");
  clockValue.tm_wday = 6; clockValue.tm_hour = 23; clockValue.tm_min = 59;
  schedule.rules[0].minute = 420;
  assert(nextScheduleText() == "Lun · 07:00 · 21.0 °C");
  schedule.rules[0].enabled = 0;
  assert(nextScheduleText() == "Nessun orario attivo");
  synchronized = false;
  assert(nextScheduleText() == "Orologio da sincronizzare");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'preview.cpp'
            executable = Path(directory) / ('preview.exe' if os.name == 'nt' else 'preview')
            cpp.write_text(preamble + function + cases, encoding='utf-8')
            subprocess.run([compiler, '-std=c++17', str(cpp), '-o', str(executable)], check=True, capture_output=True)
            subprocess.run([str(executable)], check=True, capture_output=True)
