#include "taint_api.h"

extern bool debug_taint;
target_ulong debug_asid = 0;

// Implements taint2:debug plugin arg. Turns on -d llvm_ir,taint_ops,in_asm,exec
// for that specific asid.
int asid_changed_callback(CPUState *env, target_ulong oldval, target_ulong newval) {
    if (debug_asid) {
        if (newval == debug_asid) {
            qemu_loglevel |= CPU_LOG_TAINT_OPS | CPU_LOG_LLVM_IR | CPU_LOG_TB_IN_ASM | CPU_LOG_EXEC;
        } else {
            qemu_loglevel &= ~(CPU_LOG_TAINT_OPS | CPU_LOG_LLVM_IR | CPU_LOG_TB_IN_ASM | CPU_LOG_EXEC);
        }
    }
    return 0;
}

static void start_debugging() {
    extern int qemu_loglevel;
    if (!debug_asid) {
        debug_asid = panda_current_asid(first_cpu);
        printf("taint2: ENABLING DEBUG MODE for asid 0x" TARGET_FMT_lx "\n",
                debug_asid);
    }
    qemu_loglevel |= CPU_LOG_TAINT_OPS | CPU_LOG_LLVM_IR | CPU_LOG_TB_IN_ASM | CPU_LOG_EXEC;
}

// label this phys addr in memory with this label
void taint2_label_ram(uint64_t pa, uint32_t l) {
    if (debug_taint) start_debugging();
    tp_label_ram(pa, l);
}

void taint2_label_reg(int reg_num, int offset, uint32_t l) {
    if (debug_taint) start_debugging();
    tp_label_reg(reg_num, offset, l);
}

uint32_t taint_pos_count = 0;

void label_byte(CPUState *cpu, target_ulong virt_addr, uint32_t label_num) {
    hwaddr pa = panda_virt_to_phys(cpu, virt_addr);
    if (pandalog) {
        Panda__LogEntry ple = PANDA__LOG_ENTRY__INIT;
        ple.has_taint_label_virtual_addr = 1;
        ple.has_taint_label_physical_addr = 1;
        ple.has_taint_label_number = 1;
        ple.taint_label_virtual_addr = virt_addr;
        ple.taint_label_physical_addr = pa;
        ple.taint_label_number = label_num;
        pandalog_write_entry(&ple);
    }
    taint2_label_ram(pa, label_num);
}

// Apply positional taint to a buffer of memory
void taint2_add_taint_ram_pos(CPUState *cpu, uint64_t addr, uint32_t length){
    for (unsigned i = 0; i < length; i++){
        hwaddr pa = panda_virt_to_phys(cpu, addr + i);
        if (pa == (hwaddr)(-1)) {
            printf("can't label addr=0x%lx: mmu hasn't mapped virt->phys, "
                "i.e., it isnt actually there.\n", addr +i);
            continue;
        }
        //taint2_label_ram(pa, i + taint_pos_count);
        printf("taint2: adding positional taint label %d\n", i+taint_pos_count);
        label_byte(cpu, addr+i, i+taint_pos_count);
    }
    taint_pos_count += length;
}

// Apply single label taint to a buffer of memory
void taint2_add_taint_ram_single_label(CPUState *cpu, uint64_t addr,
        uint32_t length, long label){
    for (unsigned i = 0; i < length; i++){
        hwaddr pa = panda_virt_to_phys(cpu, addr + i);
        if (pa == (hwaddr)(-1)) {
            printf("can't label addr=0x%lx: mmu hasn't mapped virt->phys, "
                "i.e., it isnt actually there.\n", addr +i);
            continue;
        }
        //taint2_label_ram(pa, label);
        printf("taint2: adding single taint label %lu\n", label);
        label_byte(cpu, addr+i, label);
    }
}

uint32_t taint2_query(Addr a) {
    LabelSetP ls = tp_query(a);
    return ls_card(ls);
}

// if phys addr pa is untainted, return 0.
// else returns label set cardinality
uint32_t taint2_query_ram(uint64_t pa) {
    LabelSetP ls = tp_query_ram(pa);
    return ls_card(ls);
}

uint32_t taint2_query_reg(int reg_num, int offset) {
    LabelSetP ls = tp_query_reg(reg_num, offset);
    return ls_card(ls);
}

uint32_t taint2_query_llvm(int reg_num, int offset) {
    LabelSetP ls = tp_query_llvm(reg_num, offset);
    return ls_card(ls);
}

uint32_t taint2_query_tcn(Addr a) {
    return tp_query_tcn(a);
}

uint32_t taint2_query_tcn_ram(uint64_t pa) {
    return tp_query_tcn_ram(pa);
}

uint32_t taint2_query_tcn_reg(int reg_num, int offset) {
    return tp_query_tcn_reg(reg_num, offset);
}

uint32_t taint2_query_tcn_llvm(int reg_num, int offset) {
    return tp_query_tcn_llvm(reg_num, offset);
}

uint64_t taint2_query_cb_mask(Addr a, uint8_t size) {
    return tp_query_cb_mask(a, size);
}

uint32_t taint2_num_labels_applied(void) {
    return tp_num_labels_applied();
}

void taint2_delete_ram(uint64_t pa) {
    tp_delete_ram(pa);
}

void taint2_delete_reg(int reg_num, int offset) {
    tp_delete_reg(reg_num, offset);
}

void taint2_labelset_spit(LabelSetP ls) {
    std::set<uint32_t> rendered(label_set_render_set(ls));
    for (uint32_t l : rendered) {
        printf("%u ", l);
    }
    printf("\n");
}

void taint2_labelset_iter(LabelSetP ls,  int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    tp_ls_iter(ls, app, stuff2);
}

void taint2_labelset_addr_iter(Addr *a, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    tp_ls_a_iter(a, app, stuff2);
}

void taint2_labelset_ram_iter(uint64_t pa, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    tp_ls_ram_iter(pa, app, stuff2);
}

void taint2_labelset_reg_iter(int reg_num, int offset, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    tp_ls_reg_iter(reg_num, offset, app, stuff2);
}

void taint2_labelset_llvm_iter(int reg_num, int offset, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    tp_ls_llvm_iter(reg_num, offset, app, stuff2);
}

void taint2_track_taint_state(void) {
    track_taint_state = true;
}

#define MAX_EL_ARR_IND 1000000
static uint32_t el_arr_ind = 0;

// used to pack pandalog array with query result
static int collect_query_labels_pandalog(uint32_t el, void *stuff) {
    uint32_t *label = (uint32_t *) stuff;
    assert (el_arr_ind < MAX_EL_ARR_IND);
    label[el_arr_ind++] = el;
    return 0;
}

/*
  Queries taint on this addr and return a Panda__TaintQuery
  data structure containing results of taint query.

  if there is no taint set associated with that address, return nullptr.

  NOTE: offset is offset into the thing that was queried.
  so, e.g., if that thing was a buffer and the query came
  from guest source code, then offset is where we are in the buffer.
  offset isn't intended to be used in any other way than to
  propagate this to the offset part of the pandalog entry for
  a taint query.
  In other words, this offset is not necessarily related to a.off

  ugh.
*/

Panda__TaintQuery *taint2_query_pandalog (Addr a, uint32_t offset) {
    // used to ensure that we only write a label sets to pandalog once
    static std::set <LabelSetP> ls_returned;

    LabelSetP ls = tp_query(a);
    if (ls) {
        Panda__TaintQuery *tq = (Panda__TaintQuery *) malloc(sizeof(Panda__TaintQuery));
        *tq = PANDA__TAINT_QUERY__INIT;

        // Returns true if insertion took place, i.e. we should plog this LS.
        if (ls_returned.insert(ls).second) {
            // we only want to actually write a particular set contents to pandalog once
            // this ls hasn't yet been written to pandalog
            // write out mapping from ls pointer to labelset contents
            // as its own separate log entry
            Panda__TaintQueryUniqueLabelSet *tquls =
                (Panda__TaintQueryUniqueLabelSet *)
                malloc (sizeof (Panda__TaintQueryUniqueLabelSet));
            *tquls = PANDA__TAINT_QUERY_UNIQUE_LABEL_SET__INIT;
            tquls->ptr = (uint64_t) ls;
            tquls->n_label = ls_card(ls);
            tquls->label = (uint32_t *) malloc (sizeof(uint32_t) * tquls->n_label);
            el_arr_ind = 0;
            tp_ls_iter(ls, collect_query_labels_pandalog, (void *) tquls->label);
            tq->unique_label_set = tquls;
        }
        tq->ptr = (uint64_t) ls;
        tq->tcn = taint2_query_tcn(a);
        // offset within larger thing being queried
        tq->offset = offset;
        return tq;
    }
    return nullptr;
}

void pandalog_taint_query_free(Panda__TaintQuery *tq) {
    if (tq->unique_label_set) {
        if (tq->unique_label_set->label) {
            free(tq->unique_label_set->label);
        }
        free(tq->unique_label_set);
    }
}
