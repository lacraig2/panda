// pulled from osi plugin
//typedef void OsiProc;
//typedef void OsiProcs;
//typedef void OsiModules;
//typedef void CPUState;
//typedef void OsiThread;
typedef void target_pid_t;
typedef target_ulong target_ptr_t;

typedef struct osi_page_struct {
    target_ptr_t start;
    target_ulong len;
} OsiPage;

typedef struct osi_proc_struct {
    target_ptr_t offset;
    char *name;
    target_ptr_t asid;
    OsiPage *pages;
    target_ptr_t pid;
    target_ptr_t ppid;
} OsiProc;

typedef struct osi_procs_struct {
    uint32_t num;
    uint32_t capacity;
    OsiProc *proc;
} OsiProcs;

typedef struct osi_module_struct {
    target_ptr_t offset;
    char *file;
    target_ptr_t base;
    target_ptr_t size;
    char *name;
} OsiModule;

typedef struct osi_modules_struct {
    uint32_t num;
    uint32_t capacity;
    OsiModule *module;
} OsiModules;

typedef struct osi_thread_struct {
    target_pid_t tid;
    target_pid_t pid;
} OsiThread;
