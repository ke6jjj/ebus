/*
 * Copyright 2018 Helium Systems Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ebus_message.h"
#include "ebus_shared.h"
#include "ebus_watch.h"
#include <stdio.h> // TODO: REMOVE
#include <string.h>

static ErlNifResourceType * DBUS_CONNECTION_RESOURCE;

ERL_NIF_TERM ATOM_OK;
ERL_NIF_TERM ATOM_ERROR;
ERL_NIF_TERM ATOM_ENOMEM;
ERL_NIF_TERM ATOM_TRUE;
ERL_NIF_TERM ATOM_FALSE;
ERL_NIF_TERM ATOM_UNDEFINED;

ERL_NIF_TERM ATOM_ADD_WATCH;
ERL_NIF_TERM ATOM_REMOVE_WATCH;
ERL_NIF_TERM ATOM_TOGGLE_WATCH;

typedef struct
{
    ErlNifEnv *      env;
    DBusConnection * connection;
    ErlNifPid        handler;
} dbus_connection;

static void
dbus_connection_dtor(ErlNifEnv * env, void * obj)
{
    (void)env;
    dbus_connection * conn = (dbus_connection *)obj;
    dbus_connection_unref(conn->connection);
}

static bool
get_dbus_connection(ErlNifEnv * env, ERL_NIF_TERM term, DBusConnection ** dest)
{
    dbus_connection * conn;
    if (!enif_get_resource(env, term, DBUS_CONNECTION_RESOURCE, (void **)&conn))
    {
        return false;
    }
    *dest = conn->connection;
    return true;
}

//
// Watch Callbacks
//

static dbus_bool_t
add_watch(DBusWatch * watch, void * data)
{
    dbus_connection * state = (dbus_connection *)data;
    ERL_NIF_TERM      msg =
        enif_make_tuple2(state->env, ATOM_ADD_WATCH, mk_dbus_watch(state->env, watch));
    enif_send(state->env, &state->handler, NULL, msg);
    enif_keep_resource(data);
    return TRUE;
}

static void
remove_watch(DBusWatch * watch, void * data)
{
    dbus_connection * state = (dbus_connection *)data;
    ERL_NIF_TERM      msg =
        enif_make_tuple2(state->env, ATOM_REMOVE_WATCH, mk_dbus_watch(state->env, watch));
    enif_send(state->env, &state->handler, NULL, msg);
    enif_release_resource(data);
}

static void
toggle_watch(DBusWatch * watch, void * data)
{
    dbus_connection * state = (dbus_connection *)data;
    ERL_NIF_TERM      msg =
        enif_make_tuple2(state->env, ATOM_TOGGLE_WATCH, mk_dbus_watch(state->env, watch));
    enif_send(state->env, &state->handler, NULL, msg);
}


static ERL_NIF_TERM
ebus_get(ErlNifEnv * env, int argc, const ERL_NIF_TERM argv[])
{
    if (argc != 2)
    {
        return enif_make_badarg(env);
    }

    int bus_type;
    if (!enif_get_int(env, argv[0], &bus_type) || bus_type < 0
        || bus_type > DBUS_BUS_STARTER)
    {
        return enif_make_badarg(env);
    }

    ErlNifPid handler_pid;
    if (!enif_get_local_pid(env, argv[1], &handler_pid))
    {
        return enif_make_badarg(env);
    }

    DBusError error;
    dbus_error_init(&error);

    DBusConnection * connection = dbus_bus_get(bus_type, &error);
    if (dbus_error_is_set(&error))
    {
        ERL_NIF_TERM message = enif_make_string(env, error.message, ERL_NIF_LATIN1);
        dbus_error_free(&error);
        return enif_make_tuple2(env, ATOM_ERROR, message);
    }

    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    dbus_connection * res =
        enif_alloc_resource(DBUS_CONNECTION_RESOURCE, sizeof(dbus_connection));
    res->connection = connection;
    res->handler    = handler_pid;
    res->env        = env;

    if (!dbus_connection_set_watch_functions(
            connection, add_watch, remove_watch, toggle_watch, res, enif_release_resource))
    {
        enif_release_resource(res);
        return enif_make_tuple2(env, ATOM_ERROR, ATOM_ENOMEM);
    }

    return enif_make_tuple2(env, ATOM_OK, enif_make_resource(env, res));
}

#define CHECK_INT_ERR(F)                                                                 \
    {                                                                                    \
        DBusError error;                                                                 \
        dbus_error_init(&error);                                                         \
        int result = (F);                                                                \
        if (dbus_error_is_set(&error))                                                   \
        {                                                                                \
            ERL_NIF_TERM message = enif_make_string(env, error.message, ERL_NIF_LATIN1); \
            dbus_error_free(&error);                                                     \
            return enif_make_tuple2(env, ATOM_ERROR, message);                           \
        }                                                                                \
        else                                                                             \
        {                                                                                \
            return enif_make_tuple2(env, ATOM_OK, enif_make_int(env, result));           \
        }                                                                                \
    }

static ERL_NIF_TERM
ebus_unique_name(ErlNifEnv * env, int argc, const ERL_NIF_TERM argv[])
{
    DBusConnection * connection;
    if (argc < 1 || !get_dbus_connection(env, argv[0], &connection))
    {
        return enif_make_badarg(env);
    }

    const char * name = dbus_bus_get_unique_name(connection);
    return enif_make_string(env, name, ERL_NIF_LATIN1);
}

static ERL_NIF_TERM
ebus_release_name(ErlNifEnv * env, int argc, const ERL_NIF_TERM argv[])
{
    if (argc != 2)
    {
        return enif_make_badarg(env);
    }

    DBusConnection * connection;
    if (!get_dbus_connection(env, argv[0], &connection))
    {
        return enif_make_badarg(env);
    }

    GET_STR(name, argv[1]);

    if (!dbus_validate_bus_name(name, NULL))
    {
        return enif_make_badarg(env);
    }

    CHECK_INT_ERR(dbus_bus_release_name(connection, name, &error));
}

static ERL_NIF_TERM
ebus_request_name(ErlNifEnv * env, int argc, const ERL_NIF_TERM argv[])
{
    if (argc != 3)
    {
        return enif_make_badarg(env);
    }

    DBusConnection * connection;
    if (!get_dbus_connection(env, argv[0], &connection))
    {
        return enif_make_badarg(env);
    }

    GET_STR(name, argv[1]);

    if (!dbus_validate_bus_name(name, NULL))
    {
        return enif_make_badarg(env);
    }

    int flags;
    if (!enif_get_int(env, argv[2], &flags))
    {
        return enif_make_badarg(env);
    }

    CHECK_INT_ERR(dbus_bus_request_name(connection, name, flags, &error));
}

static ERL_NIF_TERM
ebus_add_match(ErlNifEnv * env, int argc, const ERL_NIF_TERM argv[])
{
    if (argc != 2)
    {
        return enif_make_badarg(env);
    }

    DBusConnection * connection;
    if (!get_dbus_connection(env, argv[0], &connection))
    {
        return enif_make_badarg(env);
    }

    GET_STR(rule, argv[1]);

    DBusError error;
    dbus_error_init(&error);
    dbus_bus_add_match(connection, rule, &error);
    if (dbus_error_is_set(&error))
    {
        ERL_NIF_TERM message = enif_make_string(env, error.message, ERL_NIF_LATIN1);
        dbus_error_free(&error);
        return enif_make_tuple2(env, ATOM_ERROR, message);
    }
    else
    {
        return ATOM_OK;
    }
}

static ErlNifFunc nif_funcs[] = {
    {"bus", 2, ebus_get, 0},
    {"unique_name", 1, ebus_unique_name, 0},
    {"request_name", 3, ebus_request_name, 0},
    {"release_name", 2, ebus_release_name, 0},
    {"add_match", 2, ebus_add_match, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"message_new_signal", 3, ebus_message_new_signal, 0},
    {"message_new_call", 4, ebus_message_new_call, 0},
    {"message_append_args", 3, ebus_message_append_args, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"message_get_args", 1, ebus_message_get_args, ERL_NIF_DIRTY_JOB_CPU_BOUND},

    {"watch_equals", 2, ebus_watch_equals, 0},
    {"watch_handle", 2, ebus_watch_handle, 0},
};

#define ATOM(Id, Value)                                                                  \
    {                                                                                    \
        Id = enif_make_atom(env, Value);                                                 \
    }

static int
load(ErlNifEnv * env, void ** priv_data, ERL_NIF_TERM load_info)
{
    (void)priv_data;
    (void)load_info;


    int flags = ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER;
    DBUS_CONNECTION_RESOURCE =
        enif_open_resource_type(env, NULL, "dbus_connection", dbus_connection_dtor, flags, NULL);
    if (DBUS_CONNECTION_RESOURCE == NULL)
    {
        return -1;
    }

    ATOM(ATOM_OK, "ok");
    ATOM(ATOM_ERROR, "error");
    ATOM(ATOM_ENOMEM, "enomem");
    ATOM(ATOM_TRUE, "true");
    ATOM(ATOM_FALSE, "false");
    ATOM(ATOM_UNDEFINED, "undefined");

    ATOM(ATOM_ADD_WATCH, "add_watch");
    ATOM(ATOM_REMOVE_WATCH, "remove_watch");
    ATOM(ATOM_TOGGLE_WATCH, "toggle_watch");

    ebus_message_load(env);
    ebus_watch_load(env);

    return 0;
}

ERL_NIF_INIT(ebus_nif, nif_funcs, load, NULL, NULL, NULL);