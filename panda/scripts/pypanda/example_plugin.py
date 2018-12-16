from pypanda import *
from time import sleep
@pyp.callback("bool(void*)")
def init(handle):
	progress("init in python. handle="+str(handle))
	panda.register_callback(handle, "before_block_exec", 3, before_block_execute)
	return True

@pyp.callback("int(CPUState*, TranslationBlock*)")
def before_block_execute(a,b):
	progress("before block in python")
	sleep(sleeptime)
	return 0

sleeptime = 10
panda = Panda(qcow="/home/luke/ubuntu-14.04-server-cloudimg-i386-disk1.img")
panda.load_python_plugin(init,"Cool Plugin")
panda.run()
