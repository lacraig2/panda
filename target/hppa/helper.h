DEF_HELPER_2(excp, noreturn, env, int)
DEF_HELPER_FLAGS_2(tsv, TCG_CALL_NO_WG, void, env, tl)
DEF_HELPER_FLAGS_2(tcond, TCG_CALL_NO_WG, void, env, tl)

DEF_HELPER_FLAGS_3(stby_b, TCG_CALL_NO_WG, void, env, tl, tl)
DEF_HELPER_FLAGS_3(stby_e, TCG_CALL_NO_WG, void, env, tl, tl)

DEF_HELPER_FLAGS_1(probe_r, TCG_CALL_NO_RWG_SE, tl, tl)
DEF_HELPER_FLAGS_1(probe_w, TCG_CALL_NO_RWG_SE, tl, tl)

DEF_HELPER_FLAGS_1(loaded_fr0, TCG_CALL_NO_RWG, void, env)
