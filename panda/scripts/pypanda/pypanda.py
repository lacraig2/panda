import sys

if sys.version_info[0] < 3:
	print("Please run with Python 3!")
	sys.exit(0)

import socket
import threading
import subprocess # For make_iso
import shlex # for run_guest

from os.path import join as pjoin
from os.path import realpath, exists, abspath, isfile
from os import dup, getenv, devnull
from enum import Enum
from colorama import Fore, Style
from random import randint
from inspect import signature
from tempfile import NamedTemporaryFile

from panda_datatypes import *
from panda_expect import Expect
from asyncthread import AsyncThread

import qcows

import pdb

debug = True
pandas = []

def progress(msg):
	print(Fore.GREEN + '[pypanda.py] ' + Fore.RESET + Style.BRIGHT + msg +Style.RESET_ALL)


# location of panda build dir
panda_build = realpath(pjoin(abspath(__file__), "../../../../build"))
home = getenv("HOME")

def call_with_opt_arg(f, a):
	# Helper function to call a function with an argument only when it expects one
	# To be used for callbacks that may or may not expect a result
	if len(signature(f).parameters) > 0:
		f(a)
	else:
		f()

# Decorator to ensure a function isn't called in the main thread
def blocking(func):
    def wrapper(*args, **kwargs):
        assert (threading.current_thread() is not threading.main_thread()), "Blocking function run in main thread"
        return func(*args, **kwargs)
    wrapper.__blocking__ = True
    return wrapper


def make_iso(directory, iso_path):
    with open(devnull, "w") as DEVNULL:
        if sys.platform.startswith('linux'):
            subprocess.check_call([
                'genisoimage', '-quiet', '-RJ', '-max-iso9660-filenames', '-o', iso_path, directory
            ], stderr=subprocess.STDOUT if debug else DEVNULL)
        elif sys.platform == 'darwin':
            subprocess.check_call([
                'hdiutil', 'makehybrid', '-hfs', '-joliet', '-iso', '-o', iso_path, directory
            ], stderr=subprocess.STDOUT if debug else DEVNULL)
        else:
            raise NotImplementedError("Unsupported operating system!")

# main_loop_wait_cb is called at the start of the main cpu loop in qemu.
# This is a fairly safe place to call into qemu internals but watch out for deadlocks caused
# by your request blocking on the guest's execution

# Functions+args to call when we're next at the main loop. Callbacks to call when they're done
main_loop_wait_fnargs = []
main_loop_wait_cbargs = []


	# At the start of the main cpu loop: run async_callbacks and main_loop_wait_fns
	#progress("main_loop_wait_stuff START")
@pcb.main_loop_wait
def main_loop_wait_cb():
	# Then run any and all requested commands
	global main_loop_wait_fnargs
	global main_loop_wait_cbargs
	for fnargs, cbargs in zip(main_loop_wait_fnargs, main_loop_wait_cbargs):
		(fn, args) = fnargs
		(cb, cb_args) = cbargs
		fnargs = (fn, args)
		progress("main_loop_wait_stuff running : " + (str(fnargs)))
		ret = fn(*args)
		if cb:
			progress("running callback : " + (str(cbargs)))
			try:
				if len(cb_args): # Must take result when cb_args provided
					cb(ret, *cb_args) # callback(result, cb_arg0, cb_arg1...). Note results may be None
				else:
					call_with_opt_arg(cb, ret)
			except Exception as e: # Catch it so we can keep going?
				print("CALLBACK {} RAISED EXCEPTION: {}".format(cb, e))
				raise e
	main_loop_wait_fnargs = []
	main_loop_wait_cbargs = []


@pcb.pre_shutdown
def pre_shutdown_cb():
	print("QEmu has requested to shut down. Gracefully stopping...")
	global pandas
	for p in pandas:
		p.shutdown()

class Panda:

	"""
	arch should be "i386" or "x86_64" or ...
	NB: wheezy is debian:3.2.0-4-686-pae
	"""
	def __init__(self, arch="i386", mem="128M",
			expect_prompt = None, os_version="debian:3.2.0-4-686-pae",
			qcow="default", extra_args = "", os="linux", generic=None):
		self.arch = arch
		self.mem = mem
		self.os = os_version
		self.static_var = 0
		self.qcow = qcow
		if extra_args:
			extra_args = extra_args.split()
		else:
			extra_args = []

		# If specified use a generic (x86_64, i386, arm, ppc) qcow from moyix and ignore
		# other args. See details in qcows.py
		if generic:
			q = qcows.get_qcow_info(generic)
			self.arch     = q.arch
			self.os       = q.os
			self.qcow	  = qcows.get_qcow(generic)
			expect_prompt = q.prompt
			if q.extra_args:
				extra_args.extend(q.extra_args.split(" "))

		if self.qcow is None:
			# this means we wont be using a qcow -- replay only presumably
			pass
		else:
			if self.qcow is "default":
				# this means we'll use arch / mem / os to find a qcow
				self.qcow = pjoin(home, ".panda", "%s-%s-%s.qcow" % (self.os, self.arch, mem))
			if not (exists(self.qcow)):
				print("Missing qcow -- %s" % self.qcow)
				print("Please go create that qcow and give it to moyix!")


		self.bindir = pjoin(panda_build, "%s-softmmu" % self.arch)
		self.panda = pjoin(self.bindir, "qemu-system-%s" % self.arch)
		self.libpanda = ffi.dlopen(pjoin(self.bindir, "libpanda-%s.so" % self.arch))
		self.loaded_python = False
		self.qcow="/home/luke/.panda/ubuntu-16.04-server-cloudimg-i386-disk1.img"

		if self.os:
			self.set_os_name(self.os)

		biospath = realpath(pjoin(self.panda,"..", "..",  "pc-bios"))
		bits = None
		if self.arch == "i386":
			bits = 32
		elif self.arch == "x86_64":
			bits = 64
		elif self.arch == "arm":
			bits = 32
		elif self.arch == "aarch64":
			bit = 64
		elif self.arch == "ppc":
			bits = 32
		else:
			print("For arch %s: I need logic to figure out num bits" % self.arch)
		assert (not (bits == None))

		# set os string in line with osi plugin requirements e.g. "linux[-_]64[-_].+"
		self.os_string = "%s-%d-%s" % (os,bits,os_version)

		# note: weird that we need panda as 1st arg to lib fn to init?
		self.panda_args = [self.panda, "-m", self.mem, "-display", "none", "-L", biospath, "-os", self.os_string, self.qcow]
		self.panda_args.extend(extra_args)

		# The "athread" thread manages actions that need to occur outside qemu's CPU loop
		# e.g., interacting with monitor/serial and waiting for results
		
		# Configure serial - Always enabled for now
		self.serial_prompt = expect_prompt
		self.serial_console = None
		self.serial_file = NamedTemporaryFile(prefix="pypanda_s").name
		self.serial_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		self.panda_args.extend(['-serial', 'unix:{},server,nowait'.format(self.serial_file)])

		# Configure monitor - Always enabled for now
		self.monitor_prompt = "(qemu)"
		self.monitor_console = None
		self.monitor_file = NamedTemporaryFile(prefix="pypanda_m").name
		self.monitor_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		self.panda_args.extend(['-monitor', 'unix:{},server,nowait'.format(self.monitor_file)])


		self.running = threading.Event()
		self.started = threading.Event()
		self.athread = AsyncThread(self.started)
		global pandas
		pandas.append(self)

		self.panda_args_ffi = [ffi.new("char[]", bytes(str(i),"utf-8")) for i in self.panda_args]
		cargs = ffi.new("char **")

		# start up panda!
		nulls = ffi.new("char[]", b"")
		cenvp = ffi.new("char **",nulls)
		len_cargs = ffi.cast("int", len(self.panda_args))

		progress ("Panda args: [" + (" ".join(self.panda_args)) + "]")

		self.len_cargs = len_cargs
		self.cenvp = cenvp
		self.libpanda.panda_pre(self.len_cargs, self.panda_args_ffi, self.cenvp)
		self.taint_enabled = False
		self.init_run = False
		self.pcb_list = {}
		self.hook_list = []

		# Setup callbacks and generate self.cb_XYZ functions for cb decorators
		# XXX Don't add any other methods with names starting with 'cb_'
		self.callback = pcb

		for cb_name, pandatype in zip(pcb._fields, pcb):
			def closure(closed_cb_name, closed_pandatype): # Closure on cb_name and pandatype
				def f(*args, **kwargs):
					if debug:
						print("Registering new-style callback: ", closed_cb_name)
					return self._generated_callback(closed_pandatype, *args, **kwargs)
				return f

			setattr(self, 'cb_'+cb_name, closure(cb_name, pandatype))

		self.registered_callbacks = {} # name -> {procname: "bash", enabled: False, callback: None}
		# Register internal callback on init so we can capture 'handle'
		#self.handle = None
		self.handle = ffi.cast('void *', 0xdeadbeef)

		@pcb.asid_changed
		def __asid_changed(cpustate, old_asid, new_asid):
			if old_asid == new_asid:
				return 0

			#NOTE: Fix for case where no procnames are invlolved. This requires use of osi often for unrelated programs.
			current = self.get_current_process(cpustate)
			if current != ffi.NULL:
				current_name = ffi.string(current.name).decode('utf8', 'ignore')
#				print("Got process", current_name)
				for cb_name, cb in self.registered_callbacks.items():
					if not cb["procname"]:
						continue
					if current_name == cb["procname"] and not cb['enabled']:
						self.enable_callback_by_name(cb_name)
					if current_name != cb["procname"] and cb['enabled']:
						self.disable_callback_by_name(cb_name)

			for h in self.hook_list:
				if not h.is_kernel and ffi.NULL != current:
					if h.program_name:
						if h.program_name == curent_name and not h.is_enabled:
							print("Enabling hook at", hex(h.target_addr), "because program is", current_name)
							self.enable_hook(h)
						elif hook.program_name != current_name and hook.is_enabled:
							print("Disabling hook at", hex(h.target_addr), "because program is", current_name)
							self.disable_hook(h)
						else:
							pass
					libs = self.get_libraries(cpustate,current)
					if h.library_name:
						lowest_matching_lib = None
						if libs != ffi.NULL:
							for i in range(libs.num):
								lib = libs.module[i]
								if lib.file != ffi.NULL: 
									filename = ffi.string(lib.file).decode()
									if h.library_name in filename:
										if lowest_matching_lib:
											lowest_matching_lib = lib if lib.base < lowest_matching_lib.base else lowest_matching_lib
										else:
											lowest_matching_lib = lib
						if lowest_matching_lib:
							print("Hook on lib", lowest_matching_lib.name, hex(lowest_matching_lib.base), hex(lowest_matching_lib.base+h.target_library_offset))
							self.update_hook(h, lowest_matching_lib.base + h.target_library_offset)
							print("Updating hook for libname", h.library_name, "to match for program", current_name)
						else:
							if h.is_enabled:
								self.disable_hook(h)
								print("Disabling hook because library", h.library_name, "was not found on process", current_name)

			return 0

		@pcb.init
		def __panda_loaded(newhandle): # Local function with a reference to this instance's self.handle
			self.handle = newhandle
			self.register_callback(self.handle, self.callback.asid_changed, __asid_changed)
			return True

		# Register init which sets up asid_changed as well
		self.load_python_plugin(__panda_loaded, "__internal_python_init")
	
	def update_hook(self, hook, addr):
		if addr != hook.target_addr:
			print("Updating hook", hook, hex(addr))
			hook.target_addr = addr
			self.enable_hook(hook)
		print("Hook is not updated", hook, hex(addr))

	def enable_hook(self, hook):
		if not hook.is_enabled:
			hook.is_enabled = True
			progress("Enabling hook")
			self.libpanda_hooks.enable_hook(hook.hook_cb, hook.target_addr)

	def disable_hook(self,hook):
		if hook.is_enabled:
			hook.is_enabled = False
			progress("Disabling hook")
			self.libpanda_hooks.disable_hook(hook.hook_cb)

	# /__init__

	def unload(self):
		print("calling dlclose on {}".format(self.libpanda))
		ffi.dlclose(self.libpanda)
		self.running.clear()
		self.init_run = False
		self.libpanda = None

	def _reload_libpanda_if_necessary(self):
		# Reopen the libpanda dll after it closed (e.g. from monitor quit)
		# XXX: Make this interface more clean
		#raise NotImplemented("Panda has already exited. Can't cleanly restart library.")
		# XXX: Cffi won't let us reopen a new libpanda to do the replay. We should fix this someday

		if not hasattr(self, "libpanda") or self.libpanda is None:
			# Initialize libpanda as necessary
			print("Reloading libpanda handle")

			self.libpanda = ffi.dlopen(pjoin(self.bindir, "libpanda-%s.so" % self.arch))
			self.running.clear()

	def shutdown(self): # Cleanup panda object. XXX can't then re-initialize new python
		del self.libpanda

	def init(self):
		self.init_run = True
		self.libpanda.panda_init(self.len_cargs, self.panda_args_ffi, self.cenvp)

		# Connect to serial socket and setup serial_console if necessary
		if not self.serial_console:
			self.serial_socket.connect(self.serial_file)
			self.serial_console = Expect(self.serial_socket, expectation=self.serial_prompt, quiet=True,
										consume_first=False)

		# Connect to monitor socket and setup monitor_console if necessary
		if not self.monitor_console:
			self.monitor_socket.connect(self.monitor_file)
			self.monitor_console = Expect(self.monitor_socket, expectation=self.monitor_prompt, quiet=True,
										consume_first=True)
		# Register main_loop_wait_callback
		self.register_callback(self.handle,
				self.callback.main_loop_wait, main_loop_wait_cb)

		# Register callback to cleanup when qemu shuts down
		global panda
		panda = self
		self.register_callback(self.handle,
				self.callback.pre_shutdown, pre_shutdown_cb)

	# fnargs is a pair (fn, args)
	# fn is a function we want to run
	# args is args (an array)
	def queue_main_loop_wait_fn(self, fn, args=[], callback=None, cb_args=[]):
		#progress("queued up a fnargs")
		fnargs = (fn, args)
		main_loop_wait_fnargs.append(fnargs)
		cbargs = (callback, cb_args)
		main_loop_wait_cbargs.append(cbargs)
		
	def exit_emul_loop(self):
		self.libpanda.panda_exit_emul_loop()

	def revert(self, snapshot_name, now=False, finished_cb=None): # In the next main loop, revert
		# XXX: now=True might be unsafe. Causes weird 30s hangs sometimes
		if debug:
			progress ("Loading snapshot " + snapshot_name)
		if now:
			charptr = ffi.new("char[]", bytes(snapshot_name, "utf-8"))
			self.libpanda.panda_revert(charptr)
		else:
			# vm_stop(). so stop executing guest code right now
			self.stop()
			# queue up revert then continue
			charptr = ffi.new("char[]", bytes(snapshot_name, "utf-8"))
			self.queue_main_loop_wait_fn(self.libpanda.panda_revert, [charptr])
			# if specified, finished_cb will run after we revert and have started to continue
			# but before guest has a chance to execute anything
			self.queue_main_loop_wait_fn(self.libpanda.panda_cont, [], callback=finished_cb)
		
	def cont(self): # Call after self.stop()
#		print ("executing panda_start (vm_start)\n");
		self.libpanda.panda_cont()
		self.running.set()

	def snap(self, snapshot_name):
		if debug:
			progress ("Creating snapshot " + snapshot_name)
		# vm_stop(), so stop executing guest code
		self.stop()  

		# queue up snapshot for when monitor gets a turn
		charptr = ffi.new("char[]", bytes(snapshot_name, "utf-8"))
		self.queue_main_loop_wait_fn(self.libpanda.panda_snap, [charptr])
		# and right after that we will do a vm_start
		self.queue_main_loop_wait_fn(self.libpanda.panda_cont, []) # so this 


	def delvm(self, snapshot_name, now):
		if debug:
			progress ("Deleting snapshot " + snapshot_name)
		if now:
			charptr = ffi.new("char[]", bytes(snapshot_name, "utf-8"))
			self.libpanda.panda_delvm(charptr)
		else:
			self.exit_emul_loop()
			charptr = ffi.new("char[]", bytes(snapshot_name, "utf-8"))
			self.queue_main_loop_wait_fn(self.libpanda.panda_delvm, [charptr])

		
	def enable_tb_chaining(self):
		if debug:
			progress("Enabling TB chaining")
		self.libpanda.panda_enable_tb_chaining()

	def disable_tb_chaining(self):
		if debug:
			progress("Disabling TB chaining")
#		print("!@!@!!!!!! Disabling TB chaining\n")
		self.libpanda.panda_disable_tb_chaining()

	def run(self):
		if debug:
			progress ("Running")
		if not self.init_run:
			self.init()

		assert not self.running.is_set()
		self.running.set()

		assert not self.started.is_set()
		self.started.set()

		self.libpanda.panda_run()

	def stop(self):
		if debug:
		    progress ("Stopping guest")
		if self.init_run:
			self.running.clear()
			self.libpanda.panda_stop()
		else:
		    raise RuntimeError("Guest not running- can't be stopped")

	def begin_replay(self, replaypfx):
		if debug:
			progress ("Replaying %s" % replaypfx)
		charptr = ffi.new("char[]",bytes(replaypfx,"utf-8"))
		self.libpanda.panda_replay(charptr)

	def load_plugin(self, name, args=[]): # TODO: this doesn't work yet
		if debug:
			progress ("Loading plugin %s" % name),
#			print("plugin args: [" + (" ".join(args)) + "]")
		n = len(args)
		cargs = []
		assert(len(args)==0), "TODO: support arguments"

		# First set qemu_path so plugins can load (may be unnecessary after the first time)
		panda_name_ffi = ffi.new("char[]", bytes(self.panda,"utf-8"))
		self.libpanda.panda_set_qemu_path(panda_name_ffi)

		name_ffi = ffi.new("char[]", bytes(name,"utf-8"))
		self.libpanda.panda_init_plugin(name_ffi, cargs, n)
		self.load_plugin_library(name)

	def load_python_plugin(self, init_function, name):
		if not self.loaded_python: # Only cdef this once
			ffi.cdef("""
			extern "Python" bool init(void*);
			""")
			self.loaded_python = True
		init_ffi = init_function
		name_ffi = ffi.new("char[]", bytes(name, "utf-8"))
		filename_ffi = ffi.new("char[]", bytes(name, "utf-8"))
		uid_ffi = ffi.cast("void*",randint(0,0xffffffff)) # XXX: Unlikely but possible for collisions here
		self.libpanda.panda_load_external_plugin(filename_ffi, name_ffi, uid_ffi, init_ffi)

	def hook(self, addr, enabled=True, kernel=True,libraryname=None,procname=None):
		'''
		Decorate a function to setup a hook: when a guest goes to execute a basic block beginning with addr,
		the function will be called with args (CPUState, TranslationBlock)
		'''
		def decorator(fun):
			assert(self.handle is not None)

			# Ultimately, our hook resolves as a before_block_exec_invalidate_opt callback so we must match its args
			hook_cb_type = self.callback.before_block_exec_invalidate_opt # (CPUState, TranslationBlock)

			if not hasattr(self, 'libpanda_hooks'):
				# Enable hooks plugin on first request
				self.load_plugin("hooks")

			if debug:
				print("Registering breakpoint at 0x{:x} -> {} == {}".format(addr, fun, 'cdata_cb'))

			# Inform the plugin that it has a new breakpoint at addr
			hook_cb_passed = hook_cb_type(fun)
			self.libpanda_hooks.add_hook(addr, hook_cb_passed)
			hook_to_add = Hook(is_enabled=enabled,is_kernel=kernel,target_addr=addr,library_name=libraryname,program_name=procname,hook_cb=None, target_library_offset=None)
			if libraryname: 
					hook_to_add.target_library_offset = addr
					hook_to_add.target_addr = 0
					hook_to_add.hook_cb = hook_cb_passed
			else:
					hook_to_add.hook_cb = hook_cb_passed
			self.hook_list.append(hook_to_add)
			if libraryname or procname:
				self.disable_hook(hook_to_add)

			@hook_cb_type # Make CFFI know it's a callback. Different from _generated_callback for some reason?
			def wrapper(*args, **kw):
				return fun(*args, **kw)

			return wrapper
		return decorator

	def _generated_callback(self, pandatype, name=None, procname=None, enabled=True):
		'''
		Actual implementation of self.cb_XXX. pandatype is pcb.XXX
		name must uniquely describe a callback
		if procname is specified, callback will only be enabled when that asid is running (requires OSI support)
		'''
		if name == None:
			name = str(pandatype)
		if procname:
			enabled = False # Process won't be running at time 0 (probably)

		if debug:
			print("Registering callback {}, for proc={}, enabled={}".format(pandatype, procname, enabled))

		def decorator(fun):
			assert(self.handle is not None)
			self.registered_callbacks[name] = {"procname": procname, "enabled": enabled, "callback": pandatype}
			self.register_callback(self.handle, pandatype, pandatype(fun))
			if not enabled: # Disable if necessary
				self.disable_callback_by_name(name)
			def wrapper(*args, **kw):
				return fun(*args, **kw)
			return wrapper
		return decorator

	def register_callback(self, handle, callback, function):
		cb = callback_dictionary[callback]
		pcb = ffi.new("panda_cb *", {cb.name:function})
		self.libpanda.panda_register_callback_helper(handle, cb.number, pcb)
		self.pcb_list[callback] = (function,pcb, handle) # XXX: Should be append?
		if "block" in cb.name:
			print("Warning: disabling TB chaining to support {} callback".format(cb.name))
			self.disable_tb_chaining()

		if debug:
			progress("registered callback for type: %s" % cb.name)

	def enable_callback_by_name(self, name):
		if name not in self.registered_callbacks.keys():
			raise RuntimeError("Cannot enable callback with unknown name {}".format(name))
		self.registered_callbacks[name]['enabled'] = True
		# XXX This should enable/disable by name, but for now we enable/disable the whole type
		print("Warning, enabling all callbacks of type {}, not just {}".format(self.registered_callbacks[name]["callback"], name))
		self.enable_callback(self.registered_callbacks[name]["callback"])

	def disable_callback_by_name(self, name):
		if name not in self.registered_callbacks.keys():
			raise RuntimeError("Cannot disable callback with unknown name {}".format(name))
		self.registered_callbacks[name]['enabled'] = False
		# XXX This should enable/disable by name, but for now we enable/disable the whole type
		print("Warning, disabling all callbacks of type {}, not just {}".format(self.registered_callbacks[name]["callback"], name))
		self.disable_callback(self.registered_callbacks[name]["callback"])

	def enable_callback(self, callback):
		if self.pcb_list[callback]:
			function, pcb, handle = self.pcb_list[callback]
			print("Enable ", function)
			cb = callback_dictionary[callback]
			progress("enabled callback %s" % cb.name)
			self.libpanda.panda_enable_callback_helper(handle, cb.number, pcb)
			if "block" in cb.name:
				self.disable_tb_chaining()
		else:
			progress("ERROR: plugin not registered");

	def disable_callback(self,  callback):
		if self.pcb_list[callback]:
			function,pcb,handle = self.pcb_list[callback]
			cb = callback_dictionary[callback]
			progress("disabled callback %s" % cb.name)
			self.libpanda.panda_disable_callback_helper(handle, cb.number, pcb)
			#if "block" in cb.name: # XXX can't simply reenable without more checks
			#	self.enable_tb_chaining()
		else:
			progress("ERROR: plugin not registered");


	def unload_plugin(self, name):
		if debug:
			progress ("Unloading plugin %s" % name),
		name_ffi = ffi.new("char[]", bytes(name,"utf-8"))
		self.libpanda.panda_unload_plugin_by_name(name_ffi)

	def unload_plugins(self):
		if debug:
			progress ("Unloading all panda plugins")
		self.libpanda.panda_unload_plugins()

	def rr_get_guest_instr_count(self):
		return self.libpanda.rr_get_guest_instr_count_external()

	def require(self, plugin):
		if not self.init_run:
			self.init()
		charptr = pyp.new("char[]", bytes(plugin,"utf-8"))
		self.libpanda.panda_require(charptr)
		self.load_plugin_library(plugin)

	def enable_plugin(self, handle):
		self.libpanda.panda_enable_plugin(handle)

	def disable_plugin(self, handle):
		self.libpanda.panda_disable_plugin(handle)

	def enable_memcb(self):
		self._memcb = True
		self.libpanda.panda_enable_memcb()

	def disable_memcb(self):
		self._memcb = False
		self.libpanda.panda_disable_memcb()

	def enable_llvm(self):
		self.libpanda.panda_enable_llvm()

	def disable_llvm(self):
		self.libpanda.panda_disable_llvm()

	def enable_llvm_helpers(self):
		self.libpanda.panda_enable_llvm_helpers()

	def disable_llvm_helpers(self):
		self.libpanda.panda_disable_llvm_helpers()

	def enable_tb_chaining(self):
		self.libpanda.panda_enable_tb_chaining()

	def disable_tb_chaining(self):
		self.libpanda.panda_disable_tb_chaining()

	def flush_tb(self):
		return self.libpanda.panda_flush_tb()

	def enable_precise_pc(self):
		self.libpanda.panda_enable_precise_pc()

	def disable_precise_pc(self):
		self.libpanda.panda_disable_precise_pc()

	def memsavep(self, file_out):
		newfd = dup(f_out.fileno())
		self.libpanda.panda_memsavep(newfd)
		self.libpanda.fclose(newfd)

	def in_kernel(self, cpustate):
		return self.libpanda.panda_in_kernel_external(cpustate)

	def current_sp(self, cpustate): # under construction
		if self.arch == "i386":
			if self.in_kernel(cpustate):
				'''
				probably an enum at some point here.
				#define R_EAX 0
				#define R_ECX 1
				#define R_EDX 2
				#define R_EBX 3
				#define R_ESP 4
				#define R_EBP 5
				#define R_ESI 6
				#define R_EDI 7
				'''
				R_ESP = 4
				return cpustate.env_ptr.regs[R_ESP]
	#		else:
	#			esp0 = 4
	#			tss_base = env.tr.base + esp0
	#			kernel_esp = 0
	#			self.virtual_memory_rw(cpustate, tss_base,
		return 0

	
	def g_malloc0(self, size):
		return self.libpanda.g_malloc0(size)

	def drive_get(self, blocktype, bus, unit):
		return self.libpanda.drive_get(blocktype,bus,unit)

	def sysbus_create_varargs(self, name, addr):
		return self.libpanda.sysbus_create_varargs(name,addr,ffi.NULL)

	def cpu_class_by_name(self, name, cpu_model):
		return self.libpanda.cpu_class_by_name(name, cpu_model)
	
	def object_class_by_name(self, name):
		return self.libpanda.object_class_by_name(name)
	
	def object_property_set_bool(self, obj, value, name):
		return self.libpanda.object_property_set_bool(obj,value,name,self.libpanda.error_abort)

	def object_class_get_name(self, objclass):
		return self.libpanda.object_class_get_name(objclass)

	def object_new(self, name):
		return self.libpanda.object_new(name)

	def object_property_get_bool(self, obj, name):
		return self.libpanda.object_property_get_bool(obj,name,self.libpanda.error_abort)
	
	def object_property_set_int(self,obj, value, name):
		return self.libpanda.object_property_set_int(obj, value, name, self.libpanda.error_abort)

	def object_property_get_int(self, obj, name):
		return self.libpanda.object_property_get_int(obj, name, self.libpanda.error_abort)
	
	def object_property_set_link(self, obj, val, name):
		return self.libpanda.object_property_set_link(obj,val,name,self.libpanda.error_abort)

	def object_property_get_link(self, obj, name):
		return self.libpanda.object_property_get_link(obj,name,self.libpanda.error_abort)

	def object_property_find(self, obj, name):
		return self.libpanda.object_property_find(obj,name,ffi.NULL)

	def memory_region_allocate_system_memory(self, mr, obj, name, ram_size):
		return self.libpanda.memory_region_allocate_system_memory(mr, obj, name, ram_size)

	def memory_region_add_subregion(self, mr, offset, sr):
		return self.libpanda.memory_region_add_subregion(mr,offset,sr)
	
	def memory_region_init_ram_from_file(self, mr, owner, name, size, share, path):
		return self.libpanda.memory_region_init_ram_from_file(mr, owner, name, size, share, path, self.libpanda.error_fatal)

	def create_internal_gic(self, vbi, irqs, gic_vers):
		return self.libpanda.create_internal_gic(vbi, irqs, gic_vers)
	
	def create_one_flash(self, name, flashbase, flashsize, filename, mr):
		return self.libpanda.create_one_flash(name, flashbase, flashsize, filename, mr)

	def create_external_gic(self, vbi, irqs, gic_vers, secure):
		return self.libpanda.create_external_gic(vbi, irqs, gic_vers, secure)

	def create_virtio_devices(self, vbi, pic):
		return self.libpanda.create_virtio_devices(vbi, pic)

	def arm_load_kernel(self, cpu, bootinfo):
		return self.libpanda.arm_load_kernel(cpu, bootinfo)

	def error_report(self, s):
		return self.libpanda.error_report(s)

	def get_system_memory(self):
		return self.libpanda.get_system_memory()

	def lookup_gic(self,n):
		return self.libpanda.lookup_gic(n)

	def current_sp(self, cpustate):
		return self.libpanda.panda_current_sp_external(cpustate)

	def current_pc(self, cpustate):
		return self.libpanda.panda_current_pc(cpustate)

	def current_asid(self, cpustate):
		return self.libpanda.panda_current_asid(cpustate)

	def disas(self, fout, code, size):
		newfd = dup(fout.fileno())
		return self.libpanda.panda_disas(newfd, code, size)

	def set_os_name(self, os_name):
		os_name_new = ffi.new("char[]", bytes(os_name, "utf-8"))
		self.libpanda.panda_set_os_name(os_name_new)

	def cleanup(self):
		self.libpanda.panda_cleanup()

	def virtual_memory_read(self, env, addr, buf, length):
		if not hasattr(self,"_memcb"):
			self.enable_memcb()
		self.libpanda.panda_virtual_memory_read_external(env, addr, buf, length)

	def virtual_memory_write(self, env, addr, buf, length):
		if not hasattr(self,"_memcb"):
			self.enable_memcb()
		return self.libpanda.panda_virtual_memory_write_external(env, addr, buf, length)

	def taint_enable(self):
		if not self.taint_enabled:
			progress("taint not enabled -- enabling")
			if not self.taint_plugin_loaded:
				progress("taint2 plugin not loaded -- loading")
				self.load_plugin("taint2")
				self.taint_plugin_loaded = True
			self.libpanda.panda_taint_enable()
			self.taint_enabled = True

	def taint_reg(reg_num, label):
		self.stop()
		self.taint_enable()
		self.queue_main_loop_wait_fn(self.libpanda.panda_taint_label_reg, [reg_num, label])
		return self.libpanda.panda_virtual_memory_read_external(env, addr, buf, length)

	def send_monitor_async(self, cmd, finished_cb=None, finished_cb_args=[]):
		if debug:
			progress ("Sending monitor command async: %s" % cmd),

		buf = ffi.new("char[]", bytes(cmd,"UTF-8"))
		n = len(cmd)

		self.queue_main_loop_wait_fn(self.libpanda.panda_monitor_run,
				[buf], self.monitor_command_cb, [finished_cb, finished_cb_args])

	def monitor_command_cb(self, result, finished_cb=None, finished_cb_args=[]):
		if result == ffi.NULL:
			r = None
		else:
			r = ffi.string(result).decode("utf-8", "ignore")
		if finished_cb:
			if len(finished_cb_args):
				finished_cb(r, *finished_cb_args)
			else:
				finished_cb(r)
		elif debug and r:
			print("(Debug) Monitor command result: {}".format(r))

	def load_plugin_library(self, name):
		libname = "libpanda_%s" % name
		if not hasattr(self, libname):
			assert(isfile(pjoin(self.bindir, "panda/plugins/panda_%s.so"% name)))
			library = ffi.dlopen(pjoin(self.bindir, "panda/plugins/panda_%s.so"% name))
			self.__setattr__(libname, library)

	def load_osi(self):
		self.require("osi")
		if "linux" in self.os_string:
			self.require("osi_linux")
#			self.require("osi_test")
		else:
			print("Not supported yet for os: %s" % self.os_string)
	
	def get_current_process(self, cpustate):
		if not hasattr(self, "libpanda_osi"):
			self.load_osi()	
		process = self.libpanda_osi.get_current_process(cpustate)
		if process == ffi.NULL:
			progress("[ERROR] returned process is NULL")
		return process

	def get_processes(self, cpustate):
		if not hasattr(self, "libpanda_osi"):
			self.load_osi()	
		return self.libpanda_osi.get_processes(cpustate)
	
	def get_libraries(self, cpustate, current):
		if not hasattr(self, "libpanda_osi"):
			self.load_osi()	
		return self.libpanda_osi.get_libraries(cpustate,current)
	
	def get_modules(self, cpustate):
		if not hasattr(self, "libpanda_osi"):
			self.load_osi()	
		return self.libpanda_osi.get_modules(cpustate)
	
	def get_current_thread(self, cpustate):
		if not hasattr(self, "libpanda_osi"):
			self.load_osi()	
		return self.libpanda_osi.get_current_thread(cpustate)
	
	def ppp_reg_cb(self):
		pass

	def get_cpu(self,cpustate):
		if self.arch == "arm":
			return self.get_cpu_arm(cpustate)
		elif self.arch == "x86":
			return self.get_cpu_x86(cpustate)
		elif self.arch == "x64" or self.arch == "x86_64":
			return self.get_cpu_x64(cpustate)
		elif self.arch == "ppc":
			return self.get_cpu_ppc(cpustate)
		else:
			return self.get_cpu_x86(cpustate)

	# note: should add something to check arch in self.arch
	def get_cpu_x86(self,cpustate):
		# we dont do this because x86 is the assumed arch
		# ffi.cdef(open("./include/panda_x86_support.h")) 
		return ffi.cast("CPUX86State*", cpustate.env_ptr)
	
	def get_cpu_x64(self,cpustate):
		# we dont do this because x86 is the assumed arch
		if not hasattr(self, "x64_support"):
			self.x64_support = ffi.cdef(open("./include/panda_x64_support.h").read()) 
		return ffi.cast("CPUX64State*", cpustate.env_ptr)

	def get_cpu_arm(self,cpustate):
		if not hasattr(self, "arm_support"):
			self.arm_support = ffi.cdef(open("./include/panda_arm_support.h").read())
		return ffi.cast("CPUARMState*", cpustate.env_ptr)
	
	def get_cpu_ppc(self,cpustate):
		if not hasattr(self, "ppc_support"):
			self.ppc_support = ffi.cdef(open("./include/panda_ppc_support.h").read())
		return ffi.cast("CPUPPCState*", cpustate.env_ptr)

	def queue_async(self, f):
		self.athread.queue(f)

	# XXX: Do not call any of the following from the main thread- they depend on the CPU loop running
	@blocking
	def run_serial_cmd(self, cmd, no_timeout=False):
		self.running.wait() # Can only run serial when guest is running
		self.serial_console.sendline(cmd.encode("utf8"))
		if no_timeout:
			result = self.serial_console.expect(timeout=9999)
		else:
			result = self.serial_console.expect()
		return result

	@blocking
	def type_serial_cmd(self, cmd):
		#Can send message into socket without guest running (no self.running.wait())
		self.serial_console.send(cmd.encode("utf8")) # send, not sendline

	def finish_serial_cmd(self):
		result = self.serial_console.send_eol()
		result = self.serial_console.expect()
		return result

	@blocking
	def run_monitor_cmd(self, cmd):
		self.monitor_console.sendline(cmd.encode("utf8"))
		result = self.monitor_console.expect(self.monitor_prompt)
		return result

	@blocking
	def revert_sync(self, snapshot_name):
		self.run_monitor_cmd("loadvm {}".format(snapshot_name))

	@blocking
	def record_cmd(self, guest_command, copy_directory=None, iso_name=None, recording_name="recording"):
		self.revert_sync("root") # Can't use self.revert because that would would run async and we'd keep going before the revert happens

		if copy_directory: # If there's a directory, build an ISO and put it in the cddrive
			# Make iso
			if not iso_name: iso_name = copy_directory + '.iso'

			progress("Creating ISO {}...".format(iso_name))
			make_iso(copy_directory, iso_name)

			# 1) we insert the CD drive
			self.run_monitor_cmd("change ide1-cd0 \"{}\"".format(iso_name))

			# 2) run setup script
			# setup_sh: 
			#   Make sure cdrom didn't automount
			#   Make sure guest path mirrors host path
			#   if there is a setup.sh script in the directory,
			#   then run that setup.sh script first (good for scripts that need to
			#   prep guest environment before script runs)
		
			# TODO XXX: guest filesystem is read only so this could hang forever if it can't mount. Instead change the command to run out of /mnt/.
			guest_cmd = guest_cmd.replace(copy_directory, "/mnt/")
			copy_directory="/mnt/" # for now just mount to an existing directory

			# XXX: fix this copy directory hack

			setup_sh = "mkdir -p {mount_dir}; while ! mount /dev/cdrom {mount_dir}; do sleep 0.3; " \
						" umount /dev/cdrom; done; {mount_dir}/setup.sh &> /dev/null || true " \
							.format(mount_dir = (shlex.quote(copy_directory)))
			self.run_serial_cmd(setup_sh)

		# 3) type commmand (note we type command, start recording, finish command)
		self.type_serial_cmd(guest_command)

		# 3) start recording
		self.run_monitor_cmd("begin_record {}".format(recording_name))

		# 4) finish command
		result = self.finish_serial_cmd()

		if debug:
			progress("Result of `{}`:".format(guest_command))
			print("\t"+"\n\t".join(result.split("\n"))+"\n")

		# 5) End recording
		self.run_monitor_cmd("end_record")

		print("Finished recording")
# vim: noexpandtab:tabstop=4:
