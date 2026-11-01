#!/usr/bin/env python3
"""Capture Pikachu's pose and animations from the running game for tools/rip_model.py.

Usage: python tools/rip_capture.py <out dir> <action id>=<name> | m<motion id>=<name> ...
  e.g. python tools/rip_capture.py build/rip 145=sit_down m103=sitting 10=panic

Starts build/win/HeyYouPikachuRecompiled.exe with HYP_CONTROL, waits for the title screen
(Pikachu is there and answers the motion player), takes a pose snapshot, then for each
action: plays it, waits until its motion has started (data/voice.log), and captures the
matrices of every display list for the motion's length.
"""
import os
import re
import struct
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_DIR = os.path.join(REPO, 'build', 'win')
VOICE_LOG = os.path.join(GAME_DIR, 'data', 'voice.log')
ROM = os.path.join(REPO, 'rom', 'baserom.us.z64')


def motion_info(action):
    rom = open(ROM, 'rb').read()
    o = 0x8006B590 - 0x80000400 + 0x1000 + action * 0x2C + 0x10
    motion = struct.unpack('>h', rom[o:o + 2])[0]
    lo = 0x800773E0 - 0x80000400 + 0x1000 + motion * 4
    return motion, struct.unpack('>f', rom[lo:lo + 4])[0]


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    wanted = [(a, n) for a, n in (x.split('=', 1) for x in sys.argv[2:])]
    ctl = os.path.join(out, 'control.txt')
    open(ctl, 'w').close()
    env = dict(os.environ, HYP_CONTROL=ctl)
    game = subprocess.Popen([os.path.join(GAME_DIR, 'HeyYouPikachuRecompiled.exe')], cwd=GAME_DIR, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def send(line):
        with open(ctl, 'a') as f:
            f.write(line + '\n')

    def log():
        try:
            return open(VOICE_LOG, encoding='utf-8', errors='replace').read()
        except OSError:
            return ''

    def wait_for(pattern, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, log()):
                return True
            time.sleep(0.05)
        return False

    try:
        time.sleep(17)                       # boot to the title screen
        send('snapshot ' + os.path.join(out, 'pose'))
        send('capture 2 ' + os.path.join(out, 'pose_mtx.txt'))
        time.sleep(1.5)
        for spec, name in wanted:
            if spec.startswith('m'):
                motion = int(spec[1:])
                rom = open(ROM, 'rb').read()
                lo = 0x800773E0 - 0x80000400 + 0x1000 + motion * 4
                length = struct.unpack('>f', rom[lo:lo + 4])[0]
                command, action = 'motion %d' % motion, -1
            else:
                action = int(spec)
                motion, length = motion_info(action)
                command = 'action %d' % action
            before = log().count('motion %d started' % motion)
            send(command)
            end = time.time() + 12
            while time.time() < end and log().count('motion %d started' % motion) <= before:
                time.sleep(0.02)
            if log().count('motion %d started' % motion) <= before:
                print('%s: motion %d never started' % (name, motion))
                continue
            tasks = int(length * 60 * 2) + 8
            send('capture %d %s' % (tasks, os.path.join(out, name + '.txt')))
            time.sleep(length + 1.0)
            print('%s: action %d, motion %d, %.2f s captured' % (name, action, motion, length))
    finally:
        send('quit')
        try:
            game.wait(10)
        except subprocess.TimeoutExpired:
            game.kill()


if __name__ == '__main__':
    main()
