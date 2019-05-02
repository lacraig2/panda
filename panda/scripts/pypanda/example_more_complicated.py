from pypanda import *
from time import sleep

asid_count = {}
kernel_count = 0
user_count = 0

panda = Panda(qcow="/home/luke/ubuntu-14.04-server-cloudimg-i386-disk1.img", mem="2048M")
@panda.callback.before_block_exec
def before_block_execute(cpustate, transblock):
#	progress("before block in python")
#	sleep(1)
	return 0

@panda.callback.asid_changed
def asid_changed(cpustate, old_asid, new_asid):
	progress("asid changed from "+ str(old_asid) +" to "+ str(new_asid))
	return 0

@panda.callback.init
def init(handle):
	progress("init in python. handle="+str(handle))
#	panda.require("osi")
	panda.register_callback(handle, panda.callback.before_block_exec, before_block_execute)
	panda.register_callback(handle, panda.callback.asid_changed, asid_changed)
	return True

panda.load_python_plugin(init,"Cool Plugin")
#panda.begin_replay("/home/luke/recordings/this_is_a_recording")
panda.run()
