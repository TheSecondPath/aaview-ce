#!/usr/bin/env python3
"""Desk test of aarecvd's reader of /boot/firmware/veloaa.txt (src/Config.cpp), through `aarecvd --show-settings`:
--dump for every field, --shell for what aa-ap and install.sh read.

    python3 aarecvd_settings_test.py out/aarecvd-amd64        (build.sh settingstest runs it in the amd64 image)
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = None                      # set from the command line


class Settings(object):
    pass


def show(*args):
    p = subprocess.run([BIN, '--show-settings'] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.returncode, p.stdout.decode('utf-8', 'surrogateescape')     # paths come back byte for byte


def fields(text):
    lines = text.split('\n')
    assert lines.pop() == '', repr(text)
    return dict(line.split('=', 1) for line in lines), lines


def read(path, legacy='/nonexistent/loglevel'):
    code, out = show('--dump', '--settings', path, '--loglevel-file', legacy)
    assert code == 0, (code, out)
    d, _ = fields(out)
    s = Settings()
    s.home_wifi = d['home_wifi'] == 'on'
    s.home_wifi_ssid = d['home_wifi_ssid']
    s.log_level = d['file_log_level']
    s.set_clock = d['set_clock'] == 'on'
    s.given = set(g for g in d['given'].split(',') if g)
    s.warnings = {k[len('warning.'):]: v for k, v in d.items() if k.startswith('warning.')}
    s.level = (d['level'], d['level_source'], d['level_problem'] or None)
    return s


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='veloaa_test_')
        self.path = os.path.join(self.dir, 'veloaa.txt')
        self.legacy = os.path.join(self.dir, 'loglevel')

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def put(self, data, path=None):
        if isinstance(data, str):
            data = data.encode('utf-8')
        with open(path or self.path, 'wb') as f:
            f.write(data)

    def read(self, data=None):
        if data is not None:
            self.put(data)
        return read(self.path)

    def assertDefaults(self, s):
        self.assertIs(s.home_wifi, False)
        self.assertEqual(s.home_wifi_ssid, '')
        self.assertEqual(s.log_level, 'info')
        self.assertIs(s.set_clock, True)
        self.assertEqual(s.given, set())


class Reading(Base):
    def test_missing_file(self):
        s = self.read()
        self.assertDefaults(s)
        self.assertEqual(list(s.warnings), ['file'])
        self.assertIn('using the defaults', s.warnings['file'])

    def test_boot_firmware_not_mounted(self):
        s = read(os.path.join(self.dir, 'no', 'such', 'folder', 'veloaa.txt'))
        self.assertDefaults(s)
        self.assertEqual(list(s.warnings), ['file'])

    def test_path_through_a_file(self):         # ENOTDIR
        self.put('x')
        s = read(os.path.join(self.path, 'veloaa.txt'))
        self.assertDefaults(s)
        self.assertEqual(list(s.warnings), ['file'])

    def test_unreadable(self):                  # a folder where the file should be
        os.mkdir(self.path)
        s = read(self.path)
        self.assertDefaults(s)
        self.assertEqual(list(s.warnings), ['file'])

    def test_empty_file(self):
        for data in (b'', b'\r\n\r\n', b'   \n\t\n', b'\xef\xbb\xbf'):
            s = self.read(data)
            self.assertDefaults(s)
            self.assertEqual(list(s.warnings), ['file'], data)
            self.assertIn('empty', s.warnings['file'])

    def test_only_comments_is_not_a_problem(self):
        s = self.read('# nothing set\n\n   # at all\n')
        self.assertDefaults(s)
        self.assertEqual(s.warnings, {})

    def test_the_shipped_example(self):
        s = read(os.path.join(HERE, '..', '..', 'veloaa.txt.example'))
        self.assertEqual(s.warnings, {})
        self.assertIs(s.home_wifi, False)
        self.assertEqual(s.home_wifi_ssid, '')
        self.assertEqual(s.log_level, 'info')
        self.assertIs(s.set_clock, True)
        self.assertEqual(s.given, {'home_wifi', 'log_level', 'set_clock'})

    def test_garbled_bytes(self):
        blobs = [bytes(range(256)) * 4,
                 b'\x00' * 100,
                 b'\xff\xfe' + bytes(range(1, 255)),          # looks like a UTF-16 mark, then rubbish
                 b'\xfe\xff\x00',
                 b'home_wifi=on\x00\x00\nhome_wifi_ssid=\xff\xfe\xfd\n',
                 os.urandom(3000),
                 b'=' * 500,
                 b'\n'.join(b'=\x01\x02' for _ in range(300))]
        for blob in blobs:
            s = self.read(blob)
            self.assertIs(s.home_wifi, False, blob[:20])
            self.assertEqual(s.log_level, 'info')
            for text in s.warnings.values():
                self.assertNotIn('\n', text)
                self.assertLess(len(text), 400)
                self.assertTrue(all(' ' <= c <= '~' for c in text.replace(self.path, '')), text)

    def test_garbled_lines_do_not_spoil_good_ones(self):
        s = self.read(b'\x00\x01\x02\nlog_level=debug\n\xff\xff\xff\njust words\nhome_wifi=off\n')
        self.assertEqual(s.log_level, 'debug')
        self.assertEqual(s.given, {'log_level', 'home_wifi'})
        self.assertEqual(list(s.warnings), ['file'])
        self.assertIn('lines 1, 3, 4', s.warnings['file'])

    def test_line_without_equals(self):
        s = self.read('home_wifi on\nlog_level=warning\n')
        self.assertIs(s.home_wifi, False)
        self.assertEqual(s.log_level, 'warning')
        self.assertEqual(list(s.warnings), ['file'])
        self.assertIn('line 1 ', s.warnings['file'])

    def test_many_bad_lines_make_one_short_warning(self):
        s = self.read('nonsense\n' * 200)
        self.assertEqual(list(s.warnings), ['file'])
        self.assertIn('and 195 more', s.warnings['file'])
        self.assertLess(len(s.warnings['file']), 300)

    def test_case_spaces_comments_line_ends(self):
        s = self.read('  # a comment\r\n\r\n  Home_WiFi =  ON  \r\nHOME_WIFI_SSID\t=\tMy Net\r\n'
                      'Log_Level= Warning   # quieter\r\n')
        self.assertIs(s.home_wifi, True)
        self.assertEqual(s.home_wifi_ssid, 'My Net')
        self.assertEqual(s.log_level, 'warning')
        self.assertEqual(s.warnings, {})
        self.assertEqual(s.given, {'home_wifi', 'home_wifi_ssid', 'log_level'})

    def test_old_mac_line_ends(self):
        s = self.read('home_wifi=on\rhome_wifi_ssid=Net\rlog_level=error\r')
        self.assertEqual((s.home_wifi, s.home_wifi_ssid, s.log_level), (True, 'Net', 'error'))

    def test_notepad_encodings(self):
        text = 'home_wifi=on\r\nhome_wifi_ssid=Café 5\r\nlog_level=debug\r\n'
        for data in (b'\xef\xbb\xbf' + text.encode('utf-8'), b'\xff\xfe' + text.encode('utf-16-le'),
                     b'\xfe\xff' + text.encode('utf-16-be')):
            s = self.read(data)
            self.assertEqual((s.home_wifi, s.home_wifi_ssid, s.log_level), (True, 'Café 5', 'debug'))
            self.assertEqual(s.warnings, {})

    def test_unknown_keys(self):
        s = self.read('colour=blue\nhome_wifi_password=hunter2\nlog_level=error\nwifi=on\n')
        self.assertEqual(s.log_level, 'error')
        self.assertIs(s.home_wifi, False)
        self.assertEqual(s.warnings, {})
        self.assertEqual(s.given, {'log_level'})

    def test_bad_values(self):
        s = self.read('home_wifi=maybe\nhome_wifi_ssid=Net\nlog_level=loud\n')
        self.assertIs(s.home_wifi, False)
        self.assertEqual(s.log_level, 'info')
        self.assertEqual(s.given, {'home_wifi_ssid'})
        self.assertEqual(sorted(s.warnings), ['home_wifi', 'log_level'])
        self.assertIn('"maybe" is not on or off', s.warnings['home_wifi'])
        self.assertIn('"loud"', s.warnings['log_level'])

    def test_bad_value_is_shown_safely(self):
        s = self.read('log_level=' + 'x' * 500 + '\nhome_wifi=éé\n')
        self.assertLess(len(s.warnings['log_level']), 120)
        self.assertIn('"??"', s.warnings['home_wifi'])

    def test_blank_values_are_defaults_without_a_warning(self):
        s = self.read('home_wifi=\nhome_wifi_ssid=\nlog_level=\n')
        self.assertDefaults(s)
        self.assertEqual(s.warnings, {})

    def test_words_accepted(self):
        for word, want in (('on', True), ('yes', True), ('TRUE', True), ('1', True), ('off', False), ('No', False),
                           ('false', False), ('0', False)):
            s = self.read('home_wifi=%s\nhome_wifi_ssid=n\n' % word)
            self.assertIs(s.home_wifi, want, word)
            self.assertEqual(s.warnings, {})
        for word, want in (('error', 'error'), ('err', 'error'), ('WARNING', 'warning'), ('warn', 'warning'),
                           ('info', 'info'), ('Debug', 'debug')):
            self.assertEqual(self.read('log_level=%s\n' % word).log_level, want)

    def test_ssid_blank_with_home_wifi_on(self):
        for text in ('home_wifi=on\n', 'home_wifi=on\nhome_wifi_ssid=\n', 'home_wifi_ssid=   \nhome_wifi=on\n',
                     'home_wifi=on\nhome_wifi_ssid=""\n'):
            s = self.read(text)
            self.assertIs(s.home_wifi, False, text)
            self.assertEqual(s.home_wifi_ssid, '')
            self.assertEqual(list(s.warnings), ['home_wifi'], text)
            self.assertIn('home_wifi_ssid is blank', s.warnings['home_wifi'])
            self.assertIn('stays off', s.warnings['home_wifi'])

    def test_ssid_given_with_home_wifi_off(self):
        s = self.read('home_wifi=off\nhome_wifi_ssid=Net\n')
        self.assertIs(s.home_wifi, False)
        self.assertEqual(s.home_wifi_ssid, 'Net')
        self.assertEqual(s.warnings, {})

    def test_ssid_kept_as_written(self):
        for line, want in (('home_wifi_ssid=My Home #2 = best', 'My Home #2 = best'),
                           ('home_wifi_ssid="  padded "', '  padded '),
                           ('home_wifi_ssid=MiXeD CaSe', 'MiXeD CaSe'),
                           ('home_wifi_ssid="quoted"', 'quoted'),
                           ('home_wifi_ssid="', '"'),
                           ('home_wifi_ssid=' + 'x' * 32, 'x' * 32)):
            s = self.read('home_wifi=on\n' + line + '\n')
            self.assertEqual(s.home_wifi_ssid, want, line)
            self.assertIs(s.home_wifi, True)
            self.assertEqual(s.warnings, {})

    def test_ssid_too_long(self):
        for name in ('x' * 33, 'é' * 17):
            s = self.read('home_wifi=on\nhome_wifi_ssid=%s\n' % name)
            self.assertIs(s.home_wifi, False)
            self.assertEqual(s.home_wifi_ssid, '')
            self.assertEqual(list(s.warnings), ['home_wifi'])
            self.assertIn('32 bytes', s.warnings['home_wifi'])

    def test_last_line_for_a_key_decides(self):
        s = self.read('log_level=loud\nlog_level=debug\nhome_wifi=on\nhome_wifi_ssid=a\nhome_wifi_ssid=b\n')
        self.assertEqual((s.log_level, s.home_wifi_ssid, s.warnings), ('debug', 'b', {}))
        s = self.read('log_level=debug\nlog_level=loud\n')
        self.assertEqual(s.log_level, 'info')
        self.assertEqual(list(s.warnings), ['log_level'])
        self.assertNotIn('log_level', s.given)

    def test_only_4k_is_read(self):
        body = 'log_level=error\n' + '# padding padding padding\n' * 400
        s = self.read(body + 'log_level=debug\nhome_wifi=on\nhome_wifi_ssid=late\n')
        self.assertEqual(s.log_level, 'error')              # the later lines are past 4 KB
        self.assertIs(s.home_wifi, False)
        self.assertEqual(list(s.warnings), ['file'])
        self.assertIn('longer than 4 KB', s.warnings['file'])

    def test_line_cut_by_the_4k_limit_is_left_out(self):
        head = '#' * 4085 + '\n'                            # 4086 bytes, then a line that crosses 4096
        s = self.read(head + 'log_level=debug\n')
        self.assertEqual(s.log_level, 'info')
        self.assertEqual(list(s.warnings), ['file'])
        s = self.read('x' * 10000)
        self.assertDefaults(s)
        self.assertEqual(list(s.warnings), ['file'])

    def test_exactly_4k_is_fine(self):
        text = 'log_level=debug\n'
        s = self.read('#' * (4096 - len(text) - 1) + '\n' + text)
        self.assertEqual(s.log_level, 'debug')
        self.assertEqual(s.warnings, {})

    def test_odd_paths(self):
        for bad in ('', os.path.join(self.dir, 'bytes\udcff')):
            s = read(bad)
            self.assertIs(s.home_wifi, False)
            self.assertEqual(s.log_level, 'info')
            self.assertEqual(list(s.warnings), ['file'])


class SetClock(Base):
    def test_default_on(self):
        self.assertIs(self.read('log_level=debug\n').set_clock, True)

    def test_words(self):
        for word, want in (('off', False), ('OFF', False), ('no', False), ('0', False), ('on', True),
                           ('yes', True), ('off # not wanted', False)):
            s = self.read('set_clock=%s\n' % word)
            self.assertIs(s.set_clock, want, word)
            self.assertIn('set_clock', s.given)
            self.assertEqual(s.warnings, {})

    def test_bad_word_is_on_with_a_warning(self):
        s = self.read('set_clock=sometimes\n')
        self.assertIs(s.set_clock, True)
        self.assertNotIn('set_clock', s.given)
        self.assertIn('"sometimes"', s.warnings['set_clock'])

    def test_blank_is_on_without_a_warning(self):
        s = self.read('set_clock=\n')
        self.assertIs(s.set_clock, True)
        self.assertEqual(s.warnings, {})


class LogLevel(Base):
    """What aarecvd and aa-ap log at: veloaa.txt's log_level if set, else /etc/aarecv/loglevel, else info."""

    def level(self, settings_text=None, legacy_text=None):
        if settings_text is not None:
            self.put(settings_text)
        if legacy_text is not None:
            self.put(legacy_text, self.legacy)
        return read(self.path, self.legacy).level

    def test_nothing_set_is_info(self):
        self.assertEqual(self.level(), ('info', 'the default', None))
        self.assertEqual(self.level('home_wifi=off\n'), ('info', 'the default', None))

    def test_settings_file_wins(self):
        self.assertEqual(self.level('log_level=warning\n', 'debug\n'), ('warning', 'veloaa.txt', None))

    def test_dev_file_used_when_settings_say_nothing(self):
        self.assertEqual(self.level('home_wifi=off\n', 'debug\n'), ('debug', self.legacy, None))
        os.remove(self.path)
        self.assertEqual(self.level(None, 'error\n'), ('error', self.legacy, None))

    def test_bad_word_in_settings_falls_to_dev_file(self):
        self.assertEqual(self.level('log_level=loud\n', 'debug\n'), ('debug', self.legacy, None))
        self.assertIn('log_level', read(self.path).warnings)

    def test_bad_or_empty_dev_file_is_info(self):
        word, source, warning = self.level(None, 'chatty\n')
        self.assertEqual((word, source), ('info', 'the default'))
        self.assertIn('"chatty"', warning)
        self.assertEqual(self.level(None, ''), ('info', 'the default', None))
        self.put(b'\xff\xfe\x00\x01' * 50, self.legacy)
        self.assertEqual(self.level()[0], 'info')

    def test_dev_file_is_a_folder(self):
        self.assertEqual(read(self.path, self.dir).level[0], 'info')


class ForAaAp(Base):
    def shell(self):
        code, out = show('--shell', '--settings', self.path, '--loglevel-file', self.legacy)
        got, lines = fields(out)
        return code, got, lines

    def test_iw_escape(self):
        for name, want in (('Plain-Net_5', 'Plain-Net_5'), ('My Net', 'My Net'), (' edge ', '\\x20edge\\x20'),
                           ('a\\b', 'a\\x5cb'), ('Café', 'Caf\\xc3\\xa9'), ('', '')):
            self.put('home_wifi=on\nhome_wifi_ssid="%s"\n' % name)
            code, got, lines = self.shell()
            self.assertEqual((got['home_wifi_ssid'], got['home_wifi_ssid_iw']), (name, want), name)

    def test_shell_output(self):
        self.put('home_wifi=on\nhome_wifi_ssid=Café = #1\nlog_level=warning\n')
        code, got, lines = self.shell()
        self.assertEqual(code, 0)
        self.assertEqual(got, {'home_wifi': 'on', 'home_wifi_ssid': 'Café = #1',
                               'home_wifi_ssid_iw': 'Caf\\xc3\\xa9 = #1', 'log_level': 'warning',
                               'log_level_num': '4', 'set_clock': 'on', 'warn_file': '', 'warn_home': '',
                               'warn_log': ''})
        self.assertEqual([line.split('=', 1)[0] for line in lines],
                         ['home_wifi', 'home_wifi_ssid', 'home_wifi_ssid_iw', 'log_level', 'log_level_num',
                          'set_clock', 'warn_file', 'warn_home', 'warn_log'])

    def test_shell_output_with_nothing_there(self):
        code, got, lines = self.shell()
        self.assertEqual(code, 0)
        self.assertEqual((got['home_wifi'], got['home_wifi_ssid'], got['log_level'], got['log_level_num']),
                         ('off', '', 'info', '6'))
        self.assertIn('using the defaults', got['warn_file'])

    def test_shell_output_warnings(self):
        self.put('home_wifi=on\nlog_level=loud\n')
        self.put('chatty\n', self.legacy)
        code, got, lines = self.shell()
        self.assertEqual(got['home_wifi'], 'off')
        self.assertIn('blank', got['warn_home'])
        self.assertIn('"loud"', got['warn_log'])
        self.assertIn('"chatty"', got['warn_log'])
        self.assertEqual(len(lines), 9)

    def test_in_words(self):
        self.put('home_wifi=on\nhome_wifi_ssid=Net\nset_clock=maybe\n')
        code, out = show('--settings', self.path, '--loglevel-file', self.legacy)
        self.assertEqual(code, 0)
        self.assertEqual(out.split('\n')[:5], ['settings file: %s' % self.path, 'home Wi-Fi:    on, network "Net"',
                                               'log level:     info (from the default)', 'set clock:     on',
                                               'warning: set_clock "maybe" in veloaa.txt is not on or off: on'])

    def test_bad_arguments(self):
        p = subprocess.run([BIN, '--show-settings', '--frobnicate'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(p.returncode, 2)
        p = subprocess.run([BIN, '--shell'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(p.returncode, 2)


class QuitFlag(Base):
    """--quit-aaview / --unquit-aaview: bit 0 of the header's flags (byte 36), nothing else touched."""

    def test_set_and_clear(self):
        img = os.path.join(self.dir, 'stream.img')
        for arg, want in (('--quit-aaview', 1), ('--unquit-aaview', 0)):
            p = subprocess.run([BIN, arg, '--stream', img], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(p.returncode, 0, p.stderr)
            with open(img, 'rb') as f:
                b = f.read()
            self.assertEqual(len(b), 8192 * 512)
            self.assertEqual(b[36], want)
        with open(img, 'r+b') as f:            # the other flag bits stay
            f.seek(36)
            f.write(b'\x06\x01\x00\x00')
        subprocess.run([BIN, '--quit-aaview', '--stream', img], stdout=subprocess.PIPE, check=True)
        with open(img, 'rb') as f:
            f.seek(36)
            self.assertEqual(f.read(4), b'\x07\x01\x00\x00')


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    BIN = os.path.realpath(args[0] if args else os.path.join(HERE, '..', 'out', 'aarecvd-amd64'))
    unittest.main(argv=[sys.argv[0]] + [a for a in sys.argv[1:] if a.startswith('-')], verbosity=1)
