import json
import os
from pathlib import Path
import unittest
from urllib.parse import parse_qs

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
HTML = (ROOT / 'firmware/DaikinWemos/DaikinWemos.ino').read_text(encoding='utf-8').split('R"HTML(', 1)[1].split(')HTML"', 1)[0]
STATE = dict(online=True, known=True, power=True, mode='4', fan='A', target=20,
             requestedTarget=20, room=18.5, outside=11, internalTarget=20,
             fanRpm=850, rxPin=12, txPin=13, heatOffset=0, archiveReady=True,
             archiveBytes=1200, archiveBufferedRows=2, addressMode='dhcp')


class WebTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.playwright = sync_playwright().start()
        options = {'headless': True}
        if os.environ.get('PLAYWRIGHT_CHROME_CHANNEL'):
            options['channel'] = os.environ['PLAYWRIGHT_CHROME_CHANNEL']
        cls.browser = cls.playwright.chromium.launch(**options)

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()

    def setUp(self):
        self.context = self.browser.new_context(viewport={'width': 390, 'height': 844}, is_mobile=True, has_touch=True)
        self.addCleanup(self.context.close)
        self.page = self.context.new_page()
        self.requests = []
        self.errors = []
        self.state = dict(STATE)
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))
        self.context.route('**/*', self.route)
        self.page.goto('http://wemos.test/')
        self.page.wait_for_function("document.getElementById('basic').disabled === false")

    def route(self, route):
        path = route.request.url.split('wemos.test', 1)[-1]
        if path == '/':
            route.fulfill(content_type='text/html', body=HTML)
        elif path == '/api/state':
            route.fulfill(json=self.state)
        elif path == '/api/schedule' and route.request.method == 'GET':
            route.fulfill(json=[])
        elif path == '/api/learning/download':
            route.fulfill(body='timestamp,online,room,outside,target,prediction10m\n1794522600,1,20,10,21,20.5\n1794522630,1,20.1,10,21,\n')
        else:
            data = parse_qs(route.request.post_data or '')
            self.requests.append((path, data))
            if path == '/api/device-name':
                self.state['deviceName'] = data['name'][0]
            elif path == '/api/sleep':
                minutes = int(data['minutes'][0])
                self.state.update(sleepActive=minutes > 0, sleepSeconds=minutes * 60)
            route.fulfill(body='OK')

    def select_section(self, name):
        self.page.locator('#menuToggle').click()
        self.page.locator('#tab-' + name).click()
        self.assertTrue(self.page.locator('#sectionMenu').is_hidden())

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
        self.page.goto('http://wemos.test/#wifi')
        self.assertTrue(self.page.locator('#panel-preferences').is_visible())
        self.assertTrue(self.page.locator('#ssid').is_visible())

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
        self.page.locator('[data-sleep="60"]').click()
        self.assertIn('1 ora', self.page.locator('#sleepPreview').inner_text())
        self.assertEqual(self.requests, [])
        self.page.locator('#startSleep').click()
        self.page.wait_for_function('sleepActive === true')
        self.assertEqual(self.requests[-1], ('/api/sleep', {'minutes': ['60']}))
        self.assertTrue(self.page.locator('#sleepActiveCard').is_visible())
        self.assertIn('Spegnimento alle', self.page.locator('#sleepEndTime').inner_text())
        self.assertEqual(self.page.locator('#startSleep').inner_text(), 'Aggiorna spegnimento')
        self.page.locator('#cancelSleep').click()
        self.page.wait_for_function('sleepActive === false')
        self.assertEqual(self.requests[-1], ('/api/sleep', {'minutes': ['0']}))
        self.assertTrue(self.page.locator('#sleepActiveCard').is_hidden())

    def test_timer_custom_duration_and_invalid_limits(self):
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

    def test_automatic_changes_preserve_fan_for_temperature_only(self):
        self.assertEqual(self.page.locator('#controls .primary').count(), 0)
        self.page.evaluate('autoApplySeconds = 0')
        self.page.locator('#plus').click()
        self.page.wait_for_function('busy === false && editing === false')
        self.assertEqual(self.requests[-1][1]['temp'], ['20.5'])
        self.assertNotIn('fan', self.requests[-1][1])
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

    def test_legacy_csv_and_optional_forecasts(self):
        self.select_section('learning')
        self.page.wait_for_function('chartRows.length === 2')
        self.assertEqual(self.page.evaluate('chartRows.map(row => row.prediction)'), [20.5, None])
        old = self.page.evaluate("parseTemperatureCsv('timestamp,online,room\\n1794522600,1,20')")
        self.assertEqual(old[0]['room'], 20)
        self.assertIsNone(old[0]['prediction'])
        self.assertEqual(self.errors, [])
