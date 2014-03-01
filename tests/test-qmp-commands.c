#include <glib.h>
#include "qemu-common.h"
#include "qapi/qmp/types.h"
#include "test-qmp-commands.h"
#include "qapi/qmp/dispatch.h"
#include "qemu/module.h"
#include "qapi/qmp-input-visitor.h"
#include "tests/test-qapi-types.h"
#include "tests/test-qapi-visit.h"

void qmp_user_def_cmd(Error **errp)
{
}

void qmp_user_def_cmd1(UserDefOne * ud1, Error **errp)
{
}

UserDefTwo *qmp_user_def_cmd2(UserDefOne *ud1a,
                              bool has_udb1, UserDefOne *ud1b,
                              Error **errp)
{
    UserDefTwo *ret;
    UserDefOne *ud1c = g_malloc0(sizeof(UserDefOne));
    UserDefOne *ud1d = g_malloc0(sizeof(UserDefOne));

    ud1c->string = strdup(ud1a->string);
    ud1c->integer = ud1a->integer;
    ud1d->string = strdup(has_udb1 ? ud1b->string : "blah0");
    ud1d->integer = has_udb1 ? ud1b->integer : 0;

    ret = g_malloc0(sizeof(UserDefTwo));
    ret->string = strdup("blah1");
    ret->dict.string = strdup("blah2");
    ret->dict.dict.userdef = ud1c;
    ret->dict.dict.string = strdup("blah3");
    ret->dict.has_dict2 = true;
    ret->dict.dict2.userdef = ud1d;
    ret->dict.dict2.string = strdup("blah4");

    return ret;
}

int64_t qmp_user_def_cmd3(int64_t a, bool has_b, int64_t b, Error **errp)
{
    return a + (has_b ? b : 0);
}

/* test commands with no input and no return value */
static void test_dispatch_cmd(void)
{
    QDict *req = qdict_new();
    QObject *resp;

    qdict_put_obj(req, "execute", QOBJECT(qstring_from_str("user_def_cmd")));

    resp = qmp_dispatch(QOBJECT(req));
    assert(resp != NULL);
    assert(!qdict_haskey(qobject_to_qdict(resp), "error"));

    qobject_decref(resp);
    QDECREF(req);
}

/* test commands that return an error due to invalid parameters */
static void test_dispatch_cmd_error(void)
{
    QDict *req = qdict_new();
    QObject *resp;

    qdict_put_obj(req, "execute", QOBJECT(qstring_from_str("user_def_cmd2")));

    resp = qmp_dispatch(QOBJECT(req));
    assert(resp != NULL);
    assert(qdict_haskey(qobject_to_qdict(resp), "error"));

    qobject_decref(resp);
    QDECREF(req);
}

static QObject *test_qmp_dispatch(QDict *req)
{
    QObject *resp_obj;
    QDict *resp;
    QObject *ret;

    resp_obj = qmp_dispatch(QOBJECT(req));
    assert(resp_obj);
    resp = qobject_to_qdict(resp_obj);
    assert(resp && !qdict_haskey(resp, "error"));
    ret = qdict_get(resp, "return");
    assert(ret);
    qobject_incref(ret);
    qobject_decref(resp_obj);
    return ret;
}

/* test commands that involve both input parameters and return values */
static void test_dispatch_cmd_io(void)
{
    QDict *req = qdict_new();
    QDict *args = qdict_new();
    QDict *args3 = qdict_new();
    QDict *ud1a = qdict_new();
    QDict *ud1b = qdict_new();
    QDict *ret, *ret_dict, *ret_dict_dict, *ret_dict_dict_userdef;
    QDict *ret_dict_dict2, *ret_dict_dict2_userdef;
    QInt *ret3;

    qdict_put_obj(ud1a, "integer", QOBJECT(qint_from_int(42)));
    qdict_put_obj(ud1a, "string", QOBJECT(qstring_from_str("hello")));
    qdict_put_obj(ud1b, "integer", QOBJECT(qint_from_int(422)));
    qdict_put_obj(ud1b, "string", QOBJECT(qstring_from_str("hello2")));
    qdict_put_obj(args, "ud1a", QOBJECT(ud1a));
    qdict_put_obj(args, "ud1b", QOBJECT(ud1b));
    qdict_put_obj(req, "arguments", QOBJECT(args));
    qdict_put_obj(req, "execute", QOBJECT(qstring_from_str("user_def_cmd2")));

    ret = qobject_to_qdict(test_qmp_dispatch(req));

    assert(!strcmp(qdict_get_str(ret, "string"), "blah1"));
    ret_dict = qdict_get_qdict(ret, "dict");
    assert(!strcmp(qdict_get_str(ret_dict, "string"), "blah2"));
    ret_dict_dict = qdict_get_qdict(ret_dict, "dict");
    ret_dict_dict_userdef = qdict_get_qdict(ret_dict_dict, "userdef");
    assert(qdict_get_int(ret_dict_dict_userdef, "integer") == 42);
    assert(!strcmp(qdict_get_str(ret_dict_dict_userdef, "string"), "hello"));
    assert(!strcmp(qdict_get_str(ret_dict_dict, "string"), "blah3"));
    ret_dict_dict2 = qdict_get_qdict(ret_dict, "dict2");
    ret_dict_dict2_userdef = qdict_get_qdict(ret_dict_dict2, "userdef");
    assert(qdict_get_int(ret_dict_dict2_userdef, "integer") == 422);
    assert(!strcmp(qdict_get_str(ret_dict_dict2_userdef, "string"), "hello2"));
    assert(!strcmp(qdict_get_str(ret_dict_dict2, "string"), "blah4"));
    QDECREF(ret);

    qdict_put(args3, "a", qint_from_int(66));
    qdict_put(req, "arguments", args3);
    qdict_put(req, "execute", qstring_from_str("user_def_cmd3"));

    ret3 = qobject_to_qint(test_qmp_dispatch(req));
    assert(qint_get_int(ret3) == 66);
    QDECREF(ret);

    QDECREF(req);
}

/* test generated dealloc functions for generated types */
static void test_dealloc_types(void)
{
    UserDefOne *ud1test, *ud1a, *ud1b;
    UserDefOneList *ud1list;

    ud1test = g_malloc0(sizeof(UserDefOne));
    ud1test->integer = 42;
    ud1test->string = g_strdup("hi there 42");

    qapi_free_UserDefOne(ud1test);

    ud1a = g_malloc0(sizeof(UserDefOne));
    ud1a->integer = 43;
    ud1a->string = g_strdup("hi there 43");

    ud1b = g_malloc0(sizeof(UserDefOne));
    ud1b->integer = 44;
    ud1b->string = g_strdup("hi there 44");

    ud1list = g_malloc0(sizeof(UserDefOneList));
    ud1list->value = ud1a;
    ud1list->next = g_malloc0(sizeof(UserDefOneList));
    ud1list->next->value = ud1b;

    qapi_free_UserDefOneList(ud1list);
}

/* test generated deallocation on an object whose construction was prematurely
 * terminated due to an error */
static void test_dealloc_partial(void)
{
    static const char text[] = "don't leak me";

    UserDefTwo *ud2 = NULL;
    Error *err = NULL;

    /* create partial object */
    {
        QDict *ud2_dict;
        QmpInputVisitor *qiv;

        ud2_dict = qdict_new();
        qdict_put_obj(ud2_dict, "string", QOBJECT(qstring_from_str(text)));

        qiv = qmp_input_visitor_new(QOBJECT(ud2_dict));
        visit_type_UserDefTwo(qmp_input_get_visitor(qiv), &ud2, NULL, &err);
        qmp_input_visitor_cleanup(qiv);
        QDECREF(ud2_dict);
    }

    /* verify partial success */
    assert(ud2 != NULL);
    assert(ud2->string != NULL);
    assert(strcmp(ud2->string, text) == 0);
    assert(ud2->dict.dict.userdef == NULL);

    /* confirm & release construction error */
    assert(err != NULL);
    error_free(err);

    /* tear down partial object */
    qapi_free_UserDefTwo(ud2);
}


int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/0.15/dispatch_cmd", test_dispatch_cmd);
    g_test_add_func("/0.15/dispatch_cmd_error", test_dispatch_cmd_error);
    g_test_add_func("/0.15/dispatch_cmd_io", test_dispatch_cmd_io);
    g_test_add_func("/0.15/dealloc_types", test_dealloc_types);
    g_test_add_func("/0.15/dealloc_partial", test_dealloc_partial);

    module_call_init(MODULE_INIT_QAPI);
    g_test_run();

    return 0;
}
