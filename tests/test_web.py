import copy
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
from threading import Thread
import time
import unittest
from urllib.parse import parse_qs, urlsplit

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'firmware/DaikinWemos/DaikinWemos.ino').read_text(encoding='utf-8')
HTML = SOURCE.split('R"HTML(', 1)[1].split(')HTML"', 1)[0]
COMPRESSED = bytes(int(byte, 16) for byte in re.findall(r'0x([0-9a-f]{2})', SOURCE.split('// BEGIN GENERATED WEB GZIP')[1].split('// END GENERATED WEB GZIP')[0]))
STATE = dict(online=True, known=True, power=True, mode='4', fan='A', target=20,
             requestedTarget=20, room=18.5, outside=11, internalTarget=20,
             fanRpm=850, rxPin=12, txPin=13, heatOffset=0, archiveReady=True,
             archiveBytes=1200, archiveBufferedRows=2, addressMode='dhcp', swingKnown=True,
             nextSchedule='Oggi · 18:30 · 21.0 °C',
             onlineLearning=dict(enabled=True,phase='collecting',ready=True,examples=500,
                                 activeHours=1.5,activeExamples=120,requiredActiveExamples=2400,
                                 validationActive=0,validationTotal=0,candidate10m=20.2,
                                 mae=.15,baselineMae=.1,trendMae=.12,storage='Salvataggio automatico ogni ora',savedAt=0))


class PageHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Encoding', 'gzip')
        self.send_header('Content-Length', str(len(COMPRESSED)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(COMPRESSED)

    def do_POST(self):
        # Real pending HTTP request: the browser must abort it before the reply.
        time.sleep(.2)
        self.send_response(200)
        self.send_header('Content-Length', '2')
        self.end_headers()
        try:
            self.wfile.write(b'OK')
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def log_message(self, *args):
        pass


class WebTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.playwright = sync_playwright().start()
        options = {'headless': True}
        browser_name = os.environ.get('PLAYWRIGHT_BROWSER', 'chromium')
        if browser_name == 'chromium' and os.environ.get('PLAYWRIGHT_CHROME_CHANNEL'):
            options['channel'] = os.environ['PLAYWRIGHT_CHROME_CHANNEL']
        cls.browser = getattr(cls.playwright, browser_name).launch(**options)
        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), PageHandler)
        cls.server_thread = Thread(target=cls.server.serve_forever, daemon=True)
        cls.server_thread.start()
        cls.base_url = f'http://127.0.0.1:{cls.server.server_port}'

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()
        cls.server.shutdown()
        cls.server.server_close()
        cls.server_thread.join()

    def setUp(self):
        self.context = self.browser.new_context(viewport={'width': 390, 'height': 844},
                                               is_mobile=self.browser.browser_type.name == 'chromium', has_touch=True)
        self.addCleanup(self.context.close)
        self.page = self.context.new_page()
        self.requests = []
        self.errors = []
        self.state = copy.deepcopy(STATE)
        self.humidity_response = {'trace': {'ok': True}, 'payload': '30 35 30'}
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.context.route('**/api/**', self.route)
        self.page.goto(self.base_url + '/')
        self.page.wait_for_function("document.getElementById('basic').disabled === false")

    def route(self, route):
        path = urlsplit(route.request.url).path
        if path == '/api/state':
            route.fulfill(json=self.state)
        elif path == '/api/schedule' and route.request.method == 'GET':
            route.fulfill(json=[])
        elif path == '/api/learning/download':
            route.fulfill(body='timestamp,online,room,outside,target,prediction10m\n1794522600,1,20,10,21,20.5\n1794522630,1,20.1,10,21,\n')
        elif path == '/api/debug/probe':
            self.requests.append((path, parse_qs(route.request.post_data or '')))
            self.assertEqual(route.request.headers.get('x-debug-request'), '1')
            route.fulfill(json=self.humidity_response)
        else:
            data = parse_qs(route.request.post_data or '')
            self.requests.append((path, data))
            if path == '/api/device-name':
                self.state['deviceName'] = data['name'][0]
            elif path == '/api/sleep':
                minutes = int(data['minutes'][0])
                self.state.update(sleepActive=minutes > 0, sleepSeconds=minutes * 60)
            elif path == '/api/set':
                self.state.update(power=data['power'][0] == '1', mode=data['mode'][0],
                                  requestedTarget=float(data['temp'][0]), target=float(data['temp'][0]))
                if 'fan' in data:
                    self.state['fan'] = data['fan'][0]
            elif path == '/api/learning/online' and 'enabled' in data:
                enabled = data['enabled'][0] == '1'
                self.state['onlineLearning'].update(enabled=enabled, phase='collecting' if enabled else 'paused')
            route.fulfill(body='OK')

    def select_section(self, name):
        self.close_sheet()
        self.page.locator('#menuToggle').click()
        self.page.locator('#tab-' + name).click()
        self.assertTrue(self.page.locator('#sectionMenu').is_hidden())

    def close_sheet(self):
        sheet = self.page.locator('dialog[open]')
        if sheet.count():
            sheet.locator('[data-close]').click()

    def open_timer(self):
        self.page.locator('#timerQuick').click()

    def test_navigation_and_no_overflow_at_phone_widths(self):
        for width in [320, 390, 430, 844]:
            self.page.set_viewport_size({'width': width, 'height': 844})
            for name in ['schedule', 'learning', 'preferences', 'firmware', 'debug', 'home']:
                self.select_section(name)
                self.assertTrue(self.page.locator('#panel-' + name).is_visible())
                self.assertTrue(self.page.evaluate('document.documentElement.scrollWidth <= innerWidth'), (width, name))
        self.assertEqual(self.errors, [])

    def test_menu_order_and_wifi_inside_preferences(self):
        self.assertEqual(self.page.locator('.tabs button').all_text_contents(),
                         ['Home', 'Programmazione Orari', 'Apprendimento', 'Preferenze', 'Firmware', 'Debug'])
        self.select_section('preferences')
        self.page.locator('#wifiSettings summary').click()
        self.assertTrue(self.page.locator('#ssid').is_visible())
        self.page.locator('#addressModeChoices label').filter(has=self.page.locator('input[value="static"]')).click()
        self.assertTrue(self.page.locator('#staticIp').is_visible())
        self.page.goto(self.base_url + '/#wifi')
        self.assertTrue(self.page.locator('#panel-preferences').is_visible())
        self.assertTrue(self.page.locator('#ssid').is_visible())

    def test_compressed_page_matches_source_and_initializes(self):
        self.assertEqual(gzip.decompress(COMPRESSED).decode('utf-8'), HTML)
        self.assertLess(len(COMPRESSED), len(HTML.encode('utf-8')) // 2)
        self.assertTrue(self.page.evaluate('window.uiReady'))
        self.assertTrue(self.page.locator('#uiWarning').is_hidden())

    def test_timeout_unlocks_commands_and_menu(self):
        self.page.route('**/api/sleep', lambda route: route.continue_())
        self.page.evaluate('window.request = ((original) => (path,options) => original(path,options,50))(request)')
        self.open_timer()
        self.page.locator('#startSleep').click()
        self.page.wait_for_function("!busy && document.getElementById('sleepMessage').textContent.includes('Connessione interrotta')")
        self.select_section('preferences')
        self.assertTrue(self.page.locator('#deviceName').is_visible())
        self.assertEqual(self.errors, [])

    def test_response_body_stall_also_times_out(self):
        result = self.page.evaluate('''async () => {
          const original = window.fetch;
          let signal;
          window.fetch = async (path, options) => {
            signal=options.signal;
            return new Response(new ReadableStream({start(controller) {
              signal.addEventListener('abort', () => controller.error(new DOMException('Aborted','AbortError')));
            }}));
          };
          try { await request('/stall',{},30); return 'unexpected success'; }
          catch(error) { return {name:error.name,aborted:signal.aborted}; }
          finally { window.fetch = original; }
        }''')
        self.assertTrue(result['aborted'])
        self.assertIn(result['name'], ['AbortError', 'TypeError'])

    def test_startup_failure_is_visible(self):
        self.page.evaluate('window.uiReady=false;showUiError()')
        self.assertTrue(self.page.locator('#uiWarning').is_visible())

    def test_device_name_saved_and_rendered_as_text(self):
        self.select_section('preferences')
        name = '<b>Studio</b>'
        self.page.locator('#deviceName').fill(name)
        self.page.locator('#deviceNameForm button').click()
        self.page.wait_for_function("document.getElementById('deviceBrand').textContent === '<b>Studio</b>'")
        self.assertEqual(self.requests[-1], ('/api/device-name', {'name': [name]}))
        self.assertEqual(self.page.title(), name)
        self.assertEqual(self.page.locator('#deviceBrand b').count(), 0)
        self.page.reload()
        self.page.wait_for_function("document.getElementById('deviceBrand').textContent === '<b>Studio</b>'")
        self.assertEqual(self.page.locator('#deviceName').input_value(), name)

    def test_timer_presets_countdown_and_cancel(self):
        self.open_timer()
        self.page.locator('[data-sleep="60"]').click()
        self.assertIn('1 ora', self.page.locator('#sleepPreview').inner_text())
        self.assertEqual(self.requests, [])
        self.page.locator('#startSleep').click()
        self.page.wait_for_function('sleepActive === true')
        self.assertEqual(self.requests[-1], ('/api/sleep', {'minutes': ['60']}))
        self.assertTrue(self.page.locator('#sleepActiveCard').is_visible())
        self.assertIn('Spegnimento alle', self.page.locator('#sleepEndTime').inner_text())
        self.assertEqual(self.page.locator('#startSleep').inner_text(), 'Aggiorna spegnimento')
        self.close_sheet()
        self.assertTrue(self.page.locator('#homeSleepBanner').is_visible())
        self.assertTrue(self.page.evaluate("document.getElementById('homeSleepBanner').getBoundingClientRect().bottom <= document.querySelector('.next-action').getBoundingClientRect().top"))
        self.page.locator('#homeSleepBanner').click()
        self.assertTrue(self.page.locator('#timerDialog').is_visible())
        self.page.locator('#cancelSleep').click()
        self.page.wait_for_function('sleepActive === false')
        self.assertEqual(self.requests[-1], ('/api/sleep', {'minutes': ['0']}))
        self.assertTrue(self.page.locator('#sleepActiveCard').is_hidden())
        self.assertTrue(self.page.locator('#homeSleepBanner').is_hidden())

    def test_timer_custom_duration_and_invalid_limits(self):
        self.open_timer()
        self.page.locator('[data-sleep="custom"]').click()
        self.page.locator('#sleepHours').fill('1')
        self.page.locator('#sleepMinutes').fill('45')
        self.page.locator('#startSleep').click()
        self.page.wait_for_function('sleepActive === true')
        self.assertEqual(self.requests[-1], ('/api/sleep', {'minutes': ['105']}))
        count = len(self.requests)
        for hours, minutes in [('0', '0'), ('24', '1')]:
            self.page.locator('#sleepHours').fill(hours)
            self.page.locator('#sleepMinutes').fill(minutes)
            self.page.locator('#startSleep').click()
            self.assertEqual(len(self.requests), count)
            self.assertFalse(self.page.locator('#sleepForm').evaluate('(form) => form.checkValidity()'))
        self.page.locator('[data-sleep="15"]').click()
        self.page.locator('#startSleep').click()
        self.page.wait_for_function('busy === false')
        self.assertEqual(self.requests[-1], ('/api/sleep', {'minutes': ['15']}))

    def test_menu_escape_and_backdrop(self):
        self.page.locator('#menuToggle').click()
        self.assertTrue(self.page.evaluate("document.getElementById('mainContent').inert"))
        self.page.keyboard.press('Escape')
        self.assertTrue(self.page.locator('#sectionMenu').is_hidden())
        self.assertFalse(self.page.evaluate("document.getElementById('mainContent').inert"))
        self.page.locator('#menuToggle').click()
        self.page.locator('#menuBackdrop').click(position={'x': 5, 'y': 300})
        self.assertTrue(self.page.locator('#sectionMenu').is_hidden())

    def test_dial_mode_colors_and_quick_power(self):
        self.assertEqual(self.page.locator('#dialMode').inner_text(), 'RISCALDAMENTO')
        hot = self.page.evaluate("getComputedStyle(document.documentElement).getPropertyValue('--accent')")
        self.page.locator('#modeRow').click()
        self.page.locator('#modeChoices label').filter(has=self.page.locator('input[value="3"]')).click()
        self.page.wait_for_function("!busy && !editing && document.documentElement.dataset.mode === '3'")
        self.assertEqual(self.page.locator('#dialMode').inner_text(), 'RAFFRESCAMENTO')
        cold = self.page.evaluate("getComputedStyle(document.documentElement).getPropertyValue('--accent')")
        self.assertNotEqual(hot, cold)
        self.assertTrue(self.page.locator('#modeDialog').is_hidden())
        self.page.locator('#quickPower').click()
        self.page.wait_for_function("!busy && !editing && document.body.dataset.power === 'off'")
        self.assertEqual(self.requests[-1][1]['power'], ['0'])
        self.assertEqual(self.page.locator('#dialMode').inner_text(), 'SPENTO')
        self.assertEqual(self.page.locator('#quickPower').get_attribute('aria-pressed'), 'false')

    def test_sheets_on_small_screens_and_dark_theme(self):
        self.page.evaluate("document.documentElement.dataset.theme='dark'")
        for width in [320, 390, 430]:
            self.page.set_viewport_size({'width': width, 'height': 844})
            for button, sheet in [('modeRow','modeDialog'),('fanRow','fanDialog'),
                                  ('airRow','airDialog'),('timerQuick','timerDialog'),
                                  ('quickFunctions','specialDialog')]:
                self.page.locator('#'+button).click()
                self.assertTrue(self.page.locator('#'+sheet).is_visible())
                self.assertTrue(self.page.locator('#'+sheet).evaluate('(el)=>el.scrollWidth<=el.clientWidth'))
                self.close_sheet()
        self.select_section('preferences')
        self.open_timer()
        self.assertTrue(self.page.locator('#timerDialog').is_visible())

    def test_modal_can_close_after_daikin_disconnects(self):
        self.page.locator('#modeRow').click()
        self.state['online'] = False
        self.page.evaluate('refresh()')
        self.assertTrue(self.page.locator('#modeChoices input').first.is_disabled())
        self.assertTrue(self.page.locator('#modeDialog [data-close]').is_enabled())
        self.close_sheet()
        self.select_section('preferences')
        self.assertTrue(self.page.locator('#panel-preferences').is_visible())

    def test_automatic_changes_preserve_fan_for_temperature_only(self):
        self.assertEqual(self.page.locator('#controls .primary').count(), 0)
        self.page.evaluate('autoApplySeconds = 0')
        self.page.locator('#plus').click()
        self.page.wait_for_function('busy === false && editing === false')
        self.assertEqual(self.requests[-1][1]['temp'], ['20.5'])
        self.assertNotIn('fan', self.requests[-1][1])
        self.page.locator('#fanRow').click()
        self.page.locator('#fanChoices label').filter(has=self.page.locator('input[value="7"]')).click()
        self.page.wait_for_function('busy === false && editing === false')
        self.assertEqual(self.requests[-1][1]['fan'], ['7'])

    def test_temperature_debounce(self):
        self.page.evaluate('autoApplySeconds = 0.4')
        self.page.locator('#plus').click()
        self.page.wait_for_timeout(200)
        self.page.locator('#plus').click()
        self.page.wait_for_timeout(250)
        self.assertEqual(self.requests, [])
        self.page.wait_for_function('busy === false && editing === false')
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.requests[0][1]['temp'], ['21'])

    def test_defaults_and_duplicate_pin_validation(self):
        self.select_section('preferences')
        self.assertTrue(self.page.locator('#pinsForm input[name=tx][value="13"]').is_checked())
        self.assertTrue(self.page.locator('#pinsForm input[name=rx][value="12"]').is_checked())
        self.page.locator('#pinsForm label').filter(has=self.page.locator('input[name=rx][value="13"]')).click()
        self.page.locator('#pinsForm button').click()
        self.assertIn('diversi', self.page.locator('#pinsMessage').inner_text())
        self.assertEqual(self.requests, [])

    def test_humidity_decoder_rejects_invalid_values(self):
        for payload, expected in [('39 37 30', 79), ('30 35 30', 50), ('30 30 31', 100),
                                  ('39 39 39', None), ('', None), ('ZZ 30 30', None), ('41 30 30', None)]:
            self.assertEqual(self.page.evaluate('(payload)=>decodeHumidityPayload(payload)', payload), expected)

    def test_online_learning_status_pause_and_save_are_observation_only(self):
        self.select_section('learning')
        self.assertEqual(self.page.locator('#onlineExamples').inner_text(), '500')
        self.assertEqual(self.page.locator('#onlineHours').inner_text(), '1.5')
        self.assertIn('provvisoria', self.page.locator('#onlineCandidate').inner_text())
        self.page.locator('#onlineToggle').click()
        self.page.wait_for_function('!busy && !lastLiveState.onlineLearning.enabled')
        self.assertEqual(self.requests[-1], ('/api/learning/online', {'enabled': ['0']}))
        self.assertIn('sospeso', self.page.locator('#onlinePhase').inner_text())
        self.page.locator('#onlineToggle').click()
        self.page.wait_for_function('!busy && lastLiveState.onlineLearning.enabled')
        self.page.locator('#onlineSave').click()
        self.page.wait_for_function('!busy')
        self.assertEqual(self.requests[-1], ('/api/learning/online', {'save': ['1']}))
        self.assertFalse(any(path == '/api/set' for path, data in self.requests))

    def test_power_left_menu_right_and_timer_banner_hidden_when_idle(self):
        power = self.page.locator('#quickPower').bounding_box()
        menu = self.page.locator('#menuToggle').bounding_box()
        self.assertLess(power['x'], menu['x'])
        self.assertTrue(self.page.locator('#homeSleepBanner').is_hidden())

    def test_active_timer_banner_fits_narrow_dark_screen(self):
        self.state.update(sleepActive=True, sleepSeconds=6300)
        self.page.set_viewport_size({'width': 320, 'height': 844})
        self.page.evaluate("document.documentElement.dataset.theme='dark';refresh()")
        self.assertTrue(self.page.locator('#homeSleepBanner').is_visible())
        self.assertIn('1:45:', self.page.locator('#homeSleepRemaining').inner_text())
        self.assertTrue(self.page.evaluate('document.documentElement.scrollWidth <= innerWidth'))

    def test_humidity_probe_warns_about_fixed_50_percent(self):
        self.state['debugActive'] = True
        self.page.evaluate('refresh()')
        self.select_section('debug')
        self.page.locator('#probeHumidity').click()
        self.page.wait_for_function("!busy && document.getElementById('humidityProbeResult').textContent.includes('50%')")
        self.assertIn('non confermata', self.page.locator('#humidityProbeResult').inner_text())
        self.assertEqual(self.requests[-1], ('/api/debug/probe', {'command': ['Re']}))

    def test_humidity_unsupported_does_not_show_a_measurement(self):
        self.state['debugActive'] = True
        self.humidity_response = {'trace': {'ok': False, 'error': 'NAK'}, 'payload': ''}
        self.page.evaluate('refresh()')
        self.select_section('debug')
        self.page.locator('#probeHumidity').click()
        self.page.wait_for_function("!busy && document.getElementById('humidityProbeResult').textContent.includes('non ha fornito')")
        self.assertNotIn('%', self.page.locator('#humidityProbeResult').inner_text())

    def test_legacy_csv_and_optional_forecasts(self):
        self.select_section('learning')
        self.page.wait_for_function('chartRows.length === 2')
        self.assertEqual(self.page.evaluate('chartRows.map(row => row.prediction)'), [20.5, None])
        old = self.page.evaluate("parseTemperatureCsv('timestamp,online,room\\n1794522600,1,20')")
        self.assertEqual(old[0]['room'], 20)
        self.assertIsNone(old[0]['prediction'])
        self.assertEqual(self.errors, [])
