/* PANDABEGINCOMMENT
 *
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 *  Tom Boning             tboning@mit.edu
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 *
PANDAENDCOMMENT */
#define __STDC_FORMAT_MACROS

#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "panda/rr/rr_log.h"
#include "panda/plugin.h"
#include "panda/plugin_plugin.h"
#include "panda/plog.h"

#include "syscalls2/gen_syscalls_ext_typedefs.h"
#include "syscalls2/syscalls_common.h"

#include "wintrospection.h"
#include "wintrospection_int_fns.h"

char *get_keyname(uint32_t KeyHandle);
bool init_plugin(void *);
void uninit_plugin(void *);

// this stuff only makes sense for win x86 32-bit
#ifdef TARGET_I386

#define KMODE_FS           0x030
#define KPCR_CURTHREAD_OFF 0x124
#define KTHREAD_KPROC_OFF  0x150
#define EPROC_PID_OFF      0x0b4
#define EPROC_NAME_OFF     0x16c

uint32_t get_pid(CPUState *cpu, uint32_t eproc) {
    uint32_t pid;
    panda_virtual_memory_rw(cpu, eproc+EPROC_PID_OFF, (uint8_t *)&pid, 4, false);
    return pid;
}

void get_procname(CPUState *cpu, uint32_t eproc, char *name) {
    panda_virtual_memory_rw(cpu, eproc+EPROC_NAME_OFF, (uint8_t *)name, 16, false);
    name[16] = '\0';
}

uint32_t get_current_proc(CPUState *cpu) {
    CPUArchState *env = (CPUArchState*)cpu->env_ptr;
    // Read the kernel-mode FS segment base
    uint32_t e1, e2;
    uint32_t fs_base, thread, proc;

    // Read out the two 32-bit ints that make up a segment descriptor
    panda_virtual_memory_rw(cpu, env->gdt.base + KMODE_FS, (uint8_t *)&e1, 4, false);
    panda_virtual_memory_rw(cpu, env->gdt.base + KMODE_FS + 4, (uint8_t *)&e2, 4, false);

    // Turn wacky segment into base
    fs_base = (e1 >> 16) | ((e2 & 0xff) << 16) | (e2 & 0xff000000);

    // Read KPCR->CurrentThread->Process
    panda_virtual_memory_rw(cpu, fs_base+KPCR_CURTHREAD_OFF, (uint8_t *)&thread, 4, false);
    panda_virtual_memory_rw(cpu, thread+KTHREAD_KPROC_OFF, (uint8_t *)&proc, 4, false);

    return proc;
}





#define IMAGEPATHNAME_OFF      0x38
#define OBJNAME_OFF            0x8
#define EPROC_OBJTABLE_OFF     0xf4

#define HANDLE_MASK1  0x000007fc
#define HANDLE_SHIFT1  2
#define HANDLE_MASK2  0x001ff800
#define HANDLE_SHIFT2  11
#define HANDLE_MASK3  0x7fe00000
#define HANDLE_SHIFT3  21
#define LEVEL_MASK 0x00000007
#define TABLE_MASK ~LEVEL_MASK
#define ADDR_SIZE 4
#define HANDLE_TABLE_ENTRY_SIZE 8


// Win7 Obj Type Indices
typedef enum {
    OBJ_TYPE_Type = 2,
    OBJ_TYPE_Directory = 3,
    OBJ_TYPE_SymbolicLink = 4,
    OBJ_TYPE_Token = 5,
    OBJ_TYPE_Job = 6,
    OBJ_TYPE_Process = 7,
    OBJ_TYPE_Thread = 8,
    OBJ_TYPE_UserApcReserve = 9,
    OBJ_TYPE_IoCompletionReserve = 10,
    OBJ_TYPE_DebugObject = 11,
    OBJ_TYPE_Event = 12,
    OBJ_TYPE_EventPair = 13,
    OBJ_TYPE_Mutant = 14,
    OBJ_TYPE_Callback = 15,
    OBJ_TYPE_Semaphore = 16,
    OBJ_TYPE_Timer = 17,
    OBJ_TYPE_Profile = 18,
    OBJ_TYPE_KeyedEvent = 19,
    OBJ_TYPE_WindowStation = 20,
    OBJ_TYPE_Desktop = 21,
    OBJ_TYPE_TpWorkerFactory = 22,
    OBJ_TYPE_Adapter = 23,
    OBJ_TYPE_Controller = 24,
    OBJ_TYPE_Device = 25,
    OBJ_TYPE_Driver = 26,
    OBJ_TYPE_IoCompletion = 27,
    OBJ_TYPE_File = 28,
    OBJ_TYPE_TmTm = 29,
    OBJ_TYPE_TmTx = 30,
    OBJ_TYPE_TmRm = 31,
    OBJ_TYPE_TmEn = 32,
    OBJ_TYPE_Section = 33,
    OBJ_TYPE_Session = 34,
    OBJ_TYPE_Key = 35,
    OBJ_TYPE_ALPCPort = 36,
    OBJ_TYPE_PowerRequest = 37,
    OBJ_TYPE_WmiGuid = 38,
    OBJ_TYPE_EtwRegistration = 39,
    OBJ_TYPE_EtwConsumer = 40,
    OBJ_TYPE_FilterConnectionPort = 41,
    OBJ_TYPE_FilterCommunicationPort = 42,
    OBJ_TYPE_PcwObject = 43
} OBJ_TYPES;


static uint32_t handle_table_code(CPUState *cpu, uint32_t table_vaddr) {
    uint32_t tableCode;
    // HANDLE_TABLE.TableCode is offest 0
    panda_virtual_memory_rw(cpu, table_vaddr, (uint8_t *)&tableCode, 4, false);
    return (tableCode & TABLE_MASK);
}


static uint32_t handle_table_L1_addr(CPUState *cpu, uint32_t table_vaddr, uint32_t entry_num) {
    return handle_table_code(cpu, table_vaddr) + ADDR_SIZE * entry_num;
}


static uint32_t handle_table_L2_addr(uint32_t L1_table, uint32_t L2) {
    if (L1_table != 0x0) {
        uint32_t L2_entry = L1_table + ADDR_SIZE * L2;
        return L2_entry;
    }
    return 0;
}


static uint32_t handle_table_L1_entry(CPUState *cpu, uint32_t table_vaddr, uint32_t entry_num) {
    return (handle_table_code(cpu, table_vaddr) +    
            HANDLE_TABLE_ENTRY_SIZE * entry_num);
}


static uint32_t handle_table_L2_entry(uint32_t table_vaddr, uint32_t L1_table, uint32_t L2) {
    if (L1_table == 0) return 0;
    return L1_table + HANDLE_TABLE_ENTRY_SIZE * L2;
}


static uint32_t handle_table_L3_entry(uint32_t table_vaddr, uint32_t L2_table, uint32_t L3) {
    if (L2_table == 0) return 0;
    return L2_table + HANDLE_TABLE_ENTRY_SIZE * L3;
}

// i.e. return pointer to the object represented by this handle
uint32_t get_handle_table_entry(CPUState *cpu, uint32_t pHandleTable, uint32_t handle) {
    uint32_t tableCode, tableLevels;
    // get tablecode
    panda_virtual_memory_rw(cpu, pHandleTable, (uint8_t *)&tableCode, 4, false);
    //printf ("tableCode = 0x%x\n", tableCode);
    // extract levels
    tableLevels = tableCode & LEVEL_MASK;
    //printf("tableLevels = %d\n", tableLevels);
    //  assert (tableLevels <= 2);
    if (tableLevels > 2) {
        return 0;
    }
    uint32_t pEntry=0;
    if (tableLevels == 0) {
        uint32_t index = (handle & HANDLE_MASK1) >> HANDLE_SHIFT1;
        pEntry = handle_table_L1_entry(cpu, pHandleTable, index);
    }
    if (tableLevels == 1) {
        uint32_t L1_index = (handle & HANDLE_MASK2) >> HANDLE_SHIFT2;
        uint32_t L1_table_off = handle_table_L1_addr(cpu, pHandleTable, L1_index);
        uint32_t L1_table;
        panda_virtual_memory_rw(cpu, L1_table_off, (uint8_t *) &L1_table, 4, false);
        uint32_t index = (handle & HANDLE_MASK1) >> HANDLE_SHIFT1;
        pEntry = handle_table_L2_entry(pHandleTable, L1_table, index);
    }
    if (tableLevels == 2) {
        uint32_t L1_index = (handle & HANDLE_MASK3) >> HANDLE_SHIFT3;
        uint32_t L1_table_off = handle_table_L1_addr(cpu, pHandleTable, L1_index);
        uint32_t L1_table;
        panda_virtual_memory_rw(cpu, L1_table_off, (uint8_t *) &L1_table, 4, false);
        uint32_t L2_index = (handle & HANDLE_MASK2) >> HANDLE_SHIFT2;
        uint32_t L2_table_off = handle_table_L2_addr(L1_table, L2_index);
        uint32_t L2_table;
        panda_virtual_memory_rw(cpu, L2_table_off, (uint8_t *) &L2_table, 4, false);
        uint32_t index = (handle & HANDLE_MASK1) >> HANDLE_SHIFT1;
        pEntry = handle_table_L3_entry(pHandleTable, L2_table, index);
    }
    uint32_t pObjectHeader;
    if ((panda_virtual_memory_rw(cpu, pEntry, (uint8_t *) &pObjectHeader, 4, false)) == -1) {
        return 0;
    }
    //  printf ("processHandle_to_pid pObjectHeader = 0x%x\n", pObjectHeader);
    pObjectHeader &= ~0x00000007;

    return pObjectHeader;
}

// Hack
static void unicode_to_ascii(char *uni, char *ascii, int len) {
    int i;
    for (i = 0; i < len; i++) {
        ascii[i] = uni[i*2];
    }
}

char *read_unicode_string(CPUState *cpu, uint32_t pUstr) {
    uint16_t fileNameLen;
    uint32_t fileNamePtr;
    char *fileName = (char *)calloc(1, 260);
    char fileNameUnicode[260*2] = {};

    panda_virtual_memory_rw(cpu, pUstr,
            (uint8_t *) &fileNameLen, 2, false);
    panda_virtual_memory_rw(cpu, pUstr+4,
            (uint8_t *) &fileNamePtr, 4, false);

    if (fileNameLen > 259*2) {
        fileNameLen = 259*2;
    }
    panda_virtual_memory_rw(cpu, fileNamePtr, (uint8_t *)fileNameUnicode, fileNameLen, false);
    unicode_to_ascii(fileNameUnicode, fileName, fileNameLen/2);

    return fileName;
}


char * get_objname(CPUState *cpu, uint32_t obj) {
  uint32_t pObjectName;

  panda_virtual_memory_rw(cpu, obj+OBJNAME_OFF,
              (uint8_t *) &pObjectName, 4, false);
  return read_unicode_string(cpu, pObjectName);
}

#define FILE_OBJECT_NAME_OFF 0x30
char *get_file_obj_name(CPUState *cpu, uint32_t fobj) {
    return read_unicode_string(cpu, fobj+FILE_OBJECT_NAME_OFF);
}

#define FILE_OBJECT_POS_OFF 0x38
int64_t get_file_obj_pos(CPUState *cpu, uint32_t fobj) {
    int64_t file_pos = -1;
    if (-1 == panda_virtual_memory_rw(cpu, fobj+FILE_OBJECT_POS_OFF, (uint8_t *)&file_pos, 8, false))
        return -1;
    else
        return file_pos;
}

HandleObject *get_handle_object(CPUState *cpu, uint32_t eproc, uint32_t handle) {
    uint32_t pObjectTable;
    if (-1 == panda_virtual_memory_rw(cpu, eproc+EPROC_OBJTABLE_OFF, (uint8_t *)&pObjectTable, 4, false)) {
        return NULL;
    }
    uint32_t pObjHeader = get_handle_table_entry(cpu, pObjectTable, handle);
    if (pObjHeader == 0) return NULL;
    uint32_t pObj = pObjHeader + 0x18;
    uint8_t objType = 0;
    if (-1 == panda_virtual_memory_rw(cpu, pObjHeader+0xc, &objType, 1, false)) {
        return NULL;
    }
    HandleObject *ho = (HandleObject *) malloc(sizeof(HandleObject));
    ho->objType = objType;
    ho->pObj = pObj;
    return ho;
}

/*
HandleObject *get_handle_object_current(CPUState *cpu, uint32_t HandleVariable) {
  uint32_t eproc = get_current_proc(cpu);
  uint32_t handle;
  if (-1 == panda_virtual_memory_rw(cpu, HandleVariable, (uint8_t *)&handle, 4, false)) {
    return NULL;
  }
  return get_handle_object(cpu, eproc, handle);
}
*/

char *get_handle_object_name(CPUState *cpu, HandleObject *ho) {
    if (ho == NULL){
        char *procName = (char *) calloc(8, 1);
        sprintf(procName, "unknown");
        return procName;
    }
    switch (ho->objType) {
    case OBJ_TYPE_File:
        return get_file_obj_name(cpu, ho->pObj);
    case OBJ_TYPE_Key: {
        char *fileName = (char *) calloc(100, 1);
        sprintf(fileName, "_CM_KEY_BODY@%08x", ho->pObj);
        return fileName;
        break;
    }
    case OBJ_TYPE_Process: {
        char *procName = (char *) calloc(17, 1);
        //char procExeName[16] = {};
        //uint32_t procPid = get_pid(cpu, ho->pObj);
        get_procname(cpu, ho->pObj, procName);
        //sprintf(procName, "[%d] %s", procPid, procExeName);
        return procName;
        break;
    }
    default: {
        char *procName = (char *) calloc(8, 1);
        sprintf(procName, "unknown");
        return procName;
    }
    }
}


char * get_handle_name(CPUState *cpu, uint32_t eproc, uint32_t handle) {
    HandleObject *ho = get_handle_object(cpu, eproc, handle);
    return get_handle_object_name(cpu, ho);
}

int64_t get_file_handle_pos(CPUState *cpu, uint32_t eproc, uint32_t handle) {
    HandleObject *ho = get_handle_object(cpu, eproc, handle);
    if (!ho) {
        return -1;
    }
    else {
        return get_file_obj_pos(cpu, ho->pObj);
    }
}

#endif


bool init_plugin(void *self) {
    printf("Initializing plugin wintrospection\n");

#ifdef TARGET_I386
    // this stuff only currently works for win7 32-bit
    assert (panda_os_type == OST_WINDOWS);
    assert (panda_os_bits == 32);
    assert (0 == strcmp(panda_os_details, "7"));
    return true;
#else
    fprintf(stderr, "Plugin is not supported on this platform.\n");
    return false;
#endif

}

void uninit_plugin(void *self) {
    printf("Unloading wintrospection plugin\n");
}
