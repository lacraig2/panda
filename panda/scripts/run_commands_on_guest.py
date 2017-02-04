#!/usr/bin/env python2.7
# import re
import os
from os.path import abspath, join, basename
import sys
import tempfile
import subprocess32
import shutil
import time
import pipes
import json
import colorama
import pexpect

debug = True

def progress(msg):
    print colorama.Fore.RED + msg + colorama.Fore.RESET

# types a command into the qemu monitor and waits for it to complete
def run_monitor(cmd):
    if debug:
        print "monitor cmd: [%s]" % cmd
    print colorama.Style.BRIGHT + "(qemu)" + colorama.Style.RESET_ALL,
    monitor.sendline(cmd)
    monitor.expect_exact("(qemu)")
    print monitor.before.partition("\r\n")[2]

# types a command into the guest os and waits for it to complete
def run_console(cmd, expectation="root@debian-i386:~"):
    if debug:
        print "\n\nconsole cmd: [%s]" % cmd
    print colorama.Style.BRIGHT + "root@debian-i386:~#" + colorama.Style.RESET_ALL,
    console.sendline(cmd)
    try:
        console.expect_exact(expectation)
    except pexpect.TIMEOUT:
        print "\ntimeout"
        print console.before
        raise
    print console.before.partition("\n")[2]

if len(sys.argv) < 2:
    print >>sys.stderr, "Usage: python project.json"
    sys.exit(1)

project_file = abspath(sys.argv[1])
project = json.load(open(project_file, "r"))

# *** Required json fields
# path to qemu exec (correct guest)
assert 'qemu' in project
# name of snapshot from which to revert which will be booted & logged in as root?
assert 'snapshot' in project
# same directory as in add_queries.sh, under which will be the build
assert 'directory' in project
# command line to run the target program (already instrumented with taint and attack queries)
assert 'command' in project
# path to guest qcow
assert 'qcow' in project
# name of project
assert 'name' in project
# path to project on host
assert 'install_dir' in project
# recording name
assert 'recording_name' in project

installdir = project['install_dir']
panda_log_base_dir = project['directory']
if not os.path.isdir(panda_log_base_dir):
    os.makedirs(panda_log_base_dir)
panda_log_loc = os.path.join(project['directory'],"runcommands")
if os.path.isdir(panda_log_loc):
    shutil.rmtree(panda_log_loc)
os.mkdir(panda_log_loc)
progress("Creating panda log directory {}...".format(panda_log_loc))
panda_log_name = os.path.join(panda_log_loc, project['name'])
isoname = os.path.join(panda_log_loc, project['name']) + ".iso"

progress("Creaing ISO {}...".format(isoname))

with open(os.devnull, "w") as DEVNULL:
    if sys.platform.startswith('linux'):
        subprocess32.check_call(['genisoimage', '-RJ', '-max-iso9660-filenames', '-o', isoname, installdir], stderr=DEVNULL)
    elif sys.platform == 'darwin':
        subprocess32.check_call(['hdiutil', 'makehybrid', '-hfs', '-joliet', '-iso', '-o', isoname, installdir], stderr=DEVNULL)
    else:
        raise NotImplementedError("Unsupported operating system!")
tempdir = tempfile.mkdtemp()

monitor_path = os.path.join(tempdir, 'monitor')
serial_path = os.path.join(tempdir, 'serial')
qemu_args = [project['qemu'], project['qcow'], '-loadvm', project['snapshot'],
        '-monitor', 'unix:' + monitor_path + ',server,nowait',
        '-serial', 'unix:' + serial_path + ',server,nowait',
        '-nographic']

progress("Running qemu with args:")
print subprocess32.list2cmdline(qemu_args)

os.mkfifo(monitor_path)
os.mkfifo(serial_path)
qemu = subprocess32.Popen(qemu_args, stderr=subprocess32.STDOUT)
time.sleep(2)

monitor = pexpect.spawn("socat", ["stdin", "unix-connect:" + monitor_path])
monitor.logfile = open(os.path.join(tempdir, 'monitor.txt'), 'w')
console = pexpect.spawn("socat", ["stdin", "unix-connect:" + serial_path])
console.logfile = open(os.path.join(tempdir, 'console.txt'), 'w')

# Make sure monitor/console are in right state.
monitor.expect_exact("(qemu)")
console.sendline("")
console.expect_exact("root@debian-i386:~#")
progress("Inserting CD...")
run_monitor("change ide1-cd0 {}".format(isoname))
run_console("mkdir -p {}".format(installdir))
# Make sure cdrom didn't automount
# Make sure guest path mirrors host path
run_console("while ! mount /dev/cdrom {}; ".format(pipes.quote(installdir)) +
            "do sleep 0.3; umount /dev/cdrom; done")

# start PANDA recording
run_monitor("begin_record {}".format(project['recording_name']))

# run the actual command
progress("Running command inside guest. Panda log to: {}".format(panda_log_name))
expectation = "root@debian-i386:~"

progress("Running command " + project['command'] + " on guest")
run_console(project['command'].format(install_dir=installdir),
            expectation)

print 'back from run_console'
#time.sleep(2)

# end PANDA recording
progress("Ending recording...")
run_monitor("end_record")

monitor.sendline("quit")
shutil.rmtree(tempdir)
