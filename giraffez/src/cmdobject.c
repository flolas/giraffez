/*
 * Copyright 2016 Capital One Services, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "cmdobject.h"

// Python 2/3 C API and Windows compatibility
#include "_compat.h"

#include "errors.h"
#include "tdcli.h"
#include "columns.h"
#include "pytypes.h"
#include "unpack.h"
#include "util.h"


#define MAX_PARCEL_ATTEMPTS 5

// CLIv2 control flow exceptions
PyObject *EndStatementError;
PyObject *EndRequestError;


PyObject* check_parcel_error(TeradataConnection *conn) {
    TeradataErr *err;
    switch (conn->dbc->fet_parcel_flavor) {
    case PclSUCCESS:
        Py_RETURN_NONE;
    case PclFAILURE:
        err = tdcli_read_failure(conn->dbc->fet_data_ptr);
        PyErr_Format(TeradataError, "%d: %s", err->Code, err->Msg);
        return NULL;
    case PclERROR:
        err = tdcli_read_error(conn->dbc->fet_data_ptr);
        PyErr_Format(TeradataError, "%d: %s", err->Code, err->Msg);
        return NULL;
    }
    return NULL;
}

PyObject* check_error(TeradataConnection *conn) {
    if (conn->result == REQEXHAUST && conn->connected) {
        if (tdcli_end_request(conn) != OK) {
            PyErr_Format(TeradataError, "%d: %s", conn->result, conn->dbc->msg_text);
            return NULL;
        }
        conn->connected = NOT_CONNECTED;
    } else if (conn->result != OK) {
        PyErr_Format(TeradataError, "%d: %s", conn->result, conn->dbc->msg_text);
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject* teradata_close(TeradataConnection *conn) {
    if (conn == NULL) {
        Py_RETURN_NONE;
    }
    if (tdcli_end_request(conn) != OK) {
        PyErr_Format(TeradataError, "%d: %s", conn->result, conn->dbc->msg_text);
        return NULL;
    }
    tdcli_close(conn);
    tdcli_free(conn);
    conn = NULL;
    Py_RETURN_NONE;
}

TeradataConnection* teradata_connect(const char *host, const char *username,
        const char *password) {
    int status;
    TeradataConnection *conn;
    conn = tdcli_new();
    if ((status = tdcli_init(conn)) != OK) {
        PyErr_Format(TeradataError, "%d: CLIv2[init]: %s", conn->result, conn->dbc->msg_text);
        tdcli_free(conn);
        return NULL;
    }
    if ((status = tdcli_connect(conn, host, username, password)) != OK) {
        PyErr_Format(TeradataError, "%d: CLIv2[connect]: %s", conn->result, conn->dbc->msg_text);
        tdcli_free(conn);
        return NULL;
    }
    if ((status = tdcli_fetch(conn)) != OK) {
        PyErr_Format(TeradataError, "%d: CLIv2[fetch]: %s", conn->result, conn->dbc->msg_text);
        tdcli_free(conn);
        return NULL;
    } else {
        if (check_parcel_error(conn) == NULL) {
            tdcli_free(conn);
            return NULL;
        }
    }
    if (tdcli_end_request(conn) != OK) {
        PyErr_Format(TeradataError, "%d: CLIv2[end_request]: %s", conn->result, conn->dbc->msg_text);
        tdcli_free(conn);
        return NULL;
    }
    conn->connected = CONNECTED;
    return conn;
}

PyObject* teradata_execute(TeradataConnection *conn, TeradataEncoder *e, const char *command) {
    PyObject *row = NULL;
    size_t count;
    if (tdcli_execute(conn, command) != OK) {
        PyErr_Format(TeradataError, "%d: CLIv2[execute_init]: %s", conn->dbc->msg_text);
        return NULL;
    }
    conn->dbc->i_sess_id = conn->dbc->o_sess_id;
    conn->dbc->i_req_id = conn->dbc->o_req_id;
    conn->dbc->func = DBFFET;
    count = 0;
    // Find first statement info parcel only
    while (tdcli_fetch_record(conn) == OK && count < MAX_PARCEL_ATTEMPTS) {
        if ((row = teradata_handle_record(e, conn->dbc->fet_parcel_flavor,
                (unsigned char**)&conn->dbc->fet_data_ptr,
                conn->dbc->fet_ret_data_len)) == NULL) {
            return NULL;
        }
        if (e != NULL && e->Columns != NULL) {
            Py_RETURN_NONE;
        }
        count++;
    }
    return check_error(conn);
}

PyObject* teradata_execute_rc(TeradataConnection *conn, TeradataEncoder *e, const char *command, int *rc) {
    TeradataErr *err;
    if (tdcli_execute(conn, command) != OK) {
        PyErr_Format(TeradataError, "%d: CLIv2[execute_init]: %s", conn->result, conn->dbc->msg_text);
        return NULL;
    }
    conn->dbc->i_sess_id = conn->dbc->o_sess_id;
    conn->dbc->i_req_id = conn->dbc->o_req_id;
    conn->dbc->func = DBFFET;
    while (tdcli_fetch_record(conn) == OK) {
        switch (conn->dbc->fet_parcel_flavor) {
            case PclFAILURE:
                err = tdcli_read_failure(conn->dbc->fet_data_ptr);
                *rc = err->Code;
                PyErr_Format(TeradataError, "%d: %s", err->Code, err->Msg);
                return NULL;
            case PclERROR:
                err = tdcli_read_error(conn->dbc->fet_data_ptr);
                *rc = err->Code;
                PyErr_Format(TeradataError, "%d: %s", err->Code, err->Msg);
                return NULL;
        }
    }
    if (conn->result == TD_ERROR_REQUEST_EXHAUSTED && conn->connected == CONNECTED) {
        if (tdcli_end_request(conn) != OK) {
            *rc = conn->result;
            PyErr_Format(TeradataError, "%d: %s", conn->result, conn->dbc->msg_text);
            return NULL;
        }
    } else if (conn->result != OK) {
        *rc = conn->result;
        PyErr_Format(TeradataError, "%d: %s", conn->result, conn->dbc->msg_text);
        return NULL;
    }
    *rc = conn->result;
    Py_RETURN_NONE;
}

PyObject* teradata_execute_p(TeradataConnection *conn, TeradataEncoder *e, const char *command) {
    PyObject *result;
    const char tmp = conn->dbc->req_proc_opt;
    conn->dbc->req_proc_opt = 'P';
    result = teradata_execute(conn, e, command);
    conn->dbc->req_proc_opt = tmp;
    return result;
}

PyObject* teradata_handle_record(TeradataEncoder *e, const uint32_t parcel_t, unsigned char **data, const uint32_t length) {
    TeradataErr *err;
    PyGILState_STATE state;
    PyObject *row = NULL;
    switch (parcel_t) {
        case PclRECORD:
            state = PyGILState_Ensure();
            if ((row = e->UnpackRowFunc(e, data, length)) == NULL) {
                return NULL;
            }
            PyGILState_Release(state);
            return row;
        case PclSTATEMENTINFO:
            encoder_clear(e);
            e->Columns = e->UnpackStmtInfoFunc(data, length);
            break;
        case PclSTATEMENTINFOEND:
            break;
        case PclENDSTATEMENT:
            PyErr_SetNone(EndStatementError);
            return NULL;
        case PclENDREQUEST:
            PyErr_SetNone(EndRequestError);
            return NULL;
        case PclFAILURE:
            err = tdcli_read_failure((char*)*data);
            PyErr_Format(TeradataError, "%d: %s", err->Code, err->Msg);
            return NULL;
        case PclERROR:
            err = tdcli_read_error((char*)*data);
            PyErr_Format(TeradataError, "%d: %s", err->Code, err->Msg);
            return NULL;
    }
    Py_RETURN_NONE;
}

static void Cmd_dealloc(Cmd *self) {
    if (self->conn != NULL) {
        tdcli_free(self->conn);
        self->conn = NULL;
    }
    if (self->encoder != NULL) {
        encoder_free(self->encoder);
        self->encoder = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* Cmd_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    Cmd *self;
    self = (Cmd*)type->tp_alloc(type, 0);
    self->conn = NULL;
    self->encoder = NULL;
    return (PyObject*)self;
}

static int Cmd_init(Cmd *self, PyObject *args, PyObject *kwargs) {
    char *host=NULL, *username=NULL, *password=NULL;
    uint32_t settings;

    static char *kwlist[] = {"host", "username", "password", "encoder_settings", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sss|i", kwlist, &host, &username, &password,
            &settings)) {
        return -1;
    }
    if ((self->conn = teradata_connect(host, username, password)) == NULL) {
        return -1;
    }

    self->encoder = encoder_new(NULL, settings);
    if (self->encoder == NULL) {
        PyErr_Format(PyExc_ValueError, "Could not create encoder, settings value 0x%06x is invalid.", settings);
        return -1;
    }
    return 0;
}

static PyObject* Cmd_close(Cmd *self) {
    tdcli_close(self->conn);
    self->conn->connected = NOT_CONNECTED;
    Py_RETURN_NONE;
}

static PyObject* Cmd_columns(Cmd *self, PyObject *args, PyObject *kwargs) {
    PyObject *debug = NULL;
    static char *kwlist[] = {"debug", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O", kwlist, &debug)) {
        return NULL;
    }
    if (self->encoder == NULL || self->encoder->Columns == NULL) {
        Py_RETURN_NONE;
    }
    if (debug != NULL && PyBool_Check(debug) && debug == Py_True) {
        RawStatementInfo *raw = self->encoder->Columns->raw;
        return PyBytes_FromStringAndSize(raw->data, raw->length);
    }
    return columns_to_pylist(self->encoder->Columns);
}

static PyObject* Cmd_execute(Cmd *self, PyObject *args, PyObject *kwargs) {
    char *command = NULL;
    int prepare_only = 0;
    static char *kwlist[] = {"command", "prepare_only", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|i", kwlist, &command, &prepare_only)) {
        return NULL;
    }
    if (self->conn->connected == NOT_CONNECTED) {
        PyErr_SetString(TeradataError, "1: Connection not established.");
        return NULL;
    }
    encoder_clear(self->encoder);
    if (prepare_only) {
        self->conn->dbc->req_proc_opt = 'P';
    } else {
        self->conn->dbc->req_proc_opt = 'B';
    }
    if (teradata_execute(self->conn, self->encoder, command) == NULL) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject* Cmd_fetchone(Cmd *self) {
    PyObject* row = NULL;
    while (tdcli_fetch_record(self->conn) == OK) {
        if ((row = teradata_handle_record(self->encoder, self->conn->dbc->fet_parcel_flavor,
                (unsigned char**)&self->conn->dbc->fet_data_ptr,
                self->conn->dbc->fet_ret_data_len)) == NULL) {
            return NULL;
        } else if (row != Py_None) {
            return row;
        }
    }
    return check_error(self->conn);
}

static PyMethodDef Cmd_methods[] = {
    {"close", (PyCFunction)Cmd_close, METH_NOARGS, ""},
    {"columns", (PyCFunction)Cmd_columns, METH_VARARGS|METH_KEYWORDS, ""},
    {"execute", (PyCFunction)Cmd_execute, METH_VARARGS|METH_KEYWORDS, ""},
    {"fetchone", (PyCFunction)Cmd_fetchone, METH_NOARGS, ""},
    {NULL}  /* Sentinel */
};

PyTypeObject CmdType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_cli.Cmd",                                     /* tp_name */
    sizeof(Cmd),                                    /* tp_basicsize */
    0,                                              /* tp_itemsize */
    (destructor)Cmd_dealloc,                        /* tp_dealloc */
    0,                                              /* tp_print */
    0,                                              /* tp_getattr */
    0,                                              /* tp_setattr */
    0,                                              /* tp_compare */
    0,                                              /* tp_repr */
    0,                                              /* tp_as_number */
    0,                                              /* tp_as_sequence */
    0,                                              /* tp_as_mapping */
    0,                                              /* tp_hash */
    0,                                              /* tp_call */
    0,                                              /* tp_str */
    0,                                              /* tp_getattro */
    0,                                              /* tp_setattro */
    0,                                              /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,       /* tp_flags */
    "GiraffeCmd objects",                           /* tp_doc */
    0,                                              /* tp_traverse */
    0,                                              /* tp_clear */
    0,                                              /* tp_richcompare */
    0,                                              /* tp_weaklistoffset */
    0,                                              /* tp_iter */
    0,                                              /* tp_iternext */
    Cmd_methods,                                    /* tp_methods */
    0,                                              /* tp_members */
    0,                                              /* tp_getset */
    0,                                              /* tp_base */
    0,                                              /* tp_dict */
    0,                                              /* tp_descr_get */
    0,                                              /* tp_descr_set */
    0,                                              /* tp_dictoffset */
    (initproc)Cmd_init,                             /* tp_init */
    0,                                              /* tp_alloc */
    Cmd_new,                                        /* tp_new */
};