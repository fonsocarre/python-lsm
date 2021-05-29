#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <structmember.h>
#include <string.h>
#include <assert.h>

#include "lz4/lib/lz4.h"
#include "zstd/lib/zstd.h"
#include "lsm1/lsm.h"

#define IS_64_BIT (sizeof(void*)==8)
#define LSM_MAX_AUTOFLUSH 1048576
#define PYLSM_DEFAULT_COMPRESS_LEVEL -65535

#define LZ4_COMP_LEVEL_DEFAULT 16
#define LZ4_COMP_LEVEL_MAX 16


typedef struct {
	PyObject_HEAD
	char         *path;
	lsm_db       *lsm;
	int          state;
	int          compressed;
	unsigned int compressor_id;
	int          autoflush;
	int          page_size;
	int          block_size;
	int          safety;
	int          autowork;
	int          mmap;
	int          use_log;
	int          automerge;
	int          max_freelist;
	int          multiple_processes;
	int          autocheckpoint;
	int          readonly;
	int 	     tx_level;
	int          compress_level;
	char		 binary;
	PyObject     *logger;
	lsm_compress lsm_compress;
	lsm_env      *lsm_env;
	lsm_mutex    *lsm_mutex;
} LSM;


typedef struct {
	PyObject_HEAD
	uint8_t		state;
	lsm_cursor* cursor;
	LSM*        db;
	int 		seek_mode;
} LSMCursor;


typedef struct {
	PyObject_HEAD
	LSM *db;
	lsm_cursor *cursor;
} LSMIterView;


typedef struct {
	PyObject_HEAD
	LSM *db;
	lsm_cursor *cursor;

	PyObject *start;
	char* pStart;
	int nStart;

	PyObject *stop;
	char* pStop;
	int nStop;

	int state;

	long step;
	char direction;

	ssize_t counter;
} LSMSliceView;


static PyTypeObject LSMType;
static PyTypeObject LSMCursorType;
static PyTypeObject LSMKeysType;
static PyTypeObject LSMValuesType;
static PyTypeObject LSMItemsType;


static PyObject* LSMCursor_new(PyTypeObject*);


enum {
	PY_LSM_INITIALIZED = 0,
	PY_LSM_OPENED = 1,
	PY_LSM_CLOSED = 2
};

enum {
	PY_LSM_SLICE_FORWARD = 0,
	PY_LSM_SLICE_BACKWARD = 1
};

enum {
	PY_LSM_COMPRESSOR_NONE = LSM_COMPRESSION_NONE,
	PY_LSM_COMPRESSOR_LZ4 = 1024,
	PY_LSM_COMPRESSOR_ZSTD = 2048,
};


static int pylsm_error(int rc) {
	switch (rc) {
		case LSM_OK:
			break;
		case LSM_ERROR:
			PyErr_SetString(PyExc_RuntimeError, "Error occurred");
			break;
		case LSM_BUSY:
			PyErr_SetString(PyExc_RuntimeError, "Busy");
			break;
		case LSM_NOMEM:
			PyErr_SetNone(PyExc_MemoryError);
			break;
		case LSM_READONLY:
			PyErr_SetString(PyExc_PermissionError, "Read only");
			break;
		case LSM_IOERR:
			PyErr_SetString(PyExc_OSError, "IO error");
			break;
		case LSM_CORRUPT:
			PyErr_SetString(PyExc_RuntimeError, "Corrupted");
			break;
		case LSM_FULL:
			PyErr_SetString(PyExc_RuntimeError, "Full");
			break;
		case LSM_CANTOPEN:
			PyErr_SetString(PyExc_FileNotFoundError, "Can not open");
			break;
		case LSM_PROTOCOL:
			PyErr_SetString(PyExc_FileNotFoundError, "Protocol error");
			break;
		case LSM_MISUSE:
			PyErr_SetString(PyExc_RuntimeError, "Misuse");
			break;
		case LSM_MISMATCH:
			PyErr_SetString(PyExc_RuntimeError, "Mismatch");
			break;
		case LSM_IOERR_NOENT:
			PyErr_SetString(PyExc_SystemError, "NOENT");
			break;
		default:
			PyErr_Format(PyExc_RuntimeError, "Unhandled error: %d", rc);
			break;
	}

	return rc;
}


static int LSM_MutexLock(LSM* self) {
	self->lsm_env->xMutexEnter(self->lsm_mutex);
	return LSM_OK;
}


static int LSM_MutexLeave(LSM* self) {
	self->lsm_env->xMutexLeave(self->lsm_mutex);
	return LSM_OK;
}


static int pylsm_lz4_xBound(LSM* self, int nIn) {
	int rc = LZ4_compressBound(nIn);
	assert(rc > 0);
	return rc;
}


static int pylsm_lz4_xCompress(LSM* self, char *pOut, int *pnOut, const char *pIn, int nIn) {
	int acceleration = (2 << (15 - self->compress_level)) + 1;
	int rc = LZ4_compress_fast((const char*)pIn, pOut, nIn, *pnOut, acceleration);
	assert(rc > 0);
	*pnOut = rc;
	return LSM_OK;
}


static int pylsm_lz4_xUncompress(LSM* self, char *pOut, int *pnOut, const char *pIn, int nIn) {
	int rc = LZ4_decompress_safe((const char*)pIn, (char*)pOut, nIn, *pnOut);
	assert(rc > 0);
	*pnOut = rc;
	return LSM_OK;
}


static int pylsm_zstd_xBound(LSM* self, int nIn) {
	return ZSTD_compressBound(nIn);
}


static int pylsm_zstd_xCompress(LSM* self, char *pOut, int *pnOut, const char *pIn, int nIn) {
	size_t rc = ZSTD_compress(pOut, *pnOut, pIn, nIn, self->compress_level);

	assert(!ZSTD_isError(rc));

	*pnOut = rc;
	return LSM_OK;
}


static int pylsm_zstd_xUncompress(LSM* self, char *pOut, int *pnOut, const char *pIn, int nIn) {
  size_t rc = ZSTD_decompress((char*)pOut, *pnOut, (const char*)pIn, nIn);
  assert(!ZSTD_isError(rc));
  *pnOut = rc;
  return 0;
}


static uint32_t is_power_of_two(uint32_t n) {
   if (n==0) return 0;
   return (ceil(log2(n)) == floor(log2(n)));
}


static void pylsm_logger(LSM* self, int rc, const char * message) {
	if (self->logger == NULL) return;

	PyGILState_STATE state = PyGILState_Ensure();
	PyObject_CallFunction(self->logger, "sI", message, rc);
	PyErr_Print();
	PyGILState_Release(state);
}


static int pylsm_seek_mode_check(int seek_mode) {
	switch (seek_mode) {
		case LSM_SEEK_EQ:
			return 0;
		case LSM_SEEK_LE:
			return 0;
		case LSM_SEEK_GE:
			return 0;
		case LSM_SEEK_LEFAST:
			return 0;
		default:
			PyErr_Format(
				PyExc_ValueError,
				"\"seek_mode\" should be one of SEEK_LEFAST (%d), SEEK_LE (%d), SEEK_EQ(%d) or SEEK_GE (%d) not %d",
				LSM_SEEK_LEFAST, LSM_SEEK_LE, LSM_SEEK_EQ, LSM_SEEK_GE, seek_mode
			);
			return -1;
	}
}


static Py_ssize_t pylsm_csr_length(lsm_cursor* cursor, Py_ssize_t *result) {
	Py_ssize_t counter = 0;
	int rc = 0;

	if (rc = lsm_csr_first(cursor)) return rc;

	while (lsm_csr_valid(cursor)) {
		counter++;
		if (rc = lsm_csr_next(cursor)) break;
	}

	*result = counter;
	return rc;
}


static Py_ssize_t pylsm_length(lsm_db* lsm, Py_ssize_t *result) {
	Py_ssize_t counter = 0;
	int rc = 0;
	lsm_cursor *cursor;

	if (rc = lsm_csr_open(lsm, &cursor)) return rc;
	rc = pylsm_csr_length(cursor, result);
	lsm_csr_close(cursor);
	return rc;
}


static int pylsm_getitem(
	lsm_db* lsm,
	const char * pKey,
	int nKey,
	char** ppVal,
	int* pnVal,
	int seek_mode
) {
	int rc;
	lsm_cursor *cursor;
	char* value_buff = NULL;
	int* value_len = 0;
	char* result = NULL;

	if (rc = lsm_csr_open(lsm, &cursor)) return rc;
	if (rc = lsm_csr_seek(cursor, pKey, nKey, seek_mode)) {
		lsm_csr_close(cursor);
		return rc;
	}
	if (!lsm_csr_valid(cursor)) {
		lsm_csr_close(cursor);
		return -1;
	}
	if (rc = lsm_csr_value(cursor, (const void **)&value_buff, &value_len)) {
		lsm_csr_close(cursor);
		return rc;
	}

	result = calloc(value_len, sizeof(char));
	memcpy(result, value_buff, value_len);
	lsm_csr_close(cursor);

	*ppVal = result;
	*pnVal = value_len;
	return 0;
}


static int pylsm_delitem(
	lsm_db* lsm,
	const char * pKey,
	int nKey
) {
	int rc = 0;
	lsm_cursor *cursor;

	if (rc = lsm_csr_open(lsm, &cursor)) return rc;
	if (rc = lsm_csr_seek(cursor, pKey, nKey, LSM_SEEK_EQ)) {
		lsm_csr_close(cursor);
		return rc;
	}
	if (!lsm_csr_valid(cursor)) {
		lsm_csr_close(cursor);
		return -1;
	}
	lsm_csr_close(cursor);
	if (rc = lsm_delete(lsm, pKey, nKey)) return rc;
	return 0;
}


static int pylsm_contains(lsm_db* lsm, const char* pKey, int nKey) {
	int rc;
	lsm_cursor *cursor;

	if (rc = lsm_csr_open(lsm, &cursor)) return rc;
	if (rc = lsm_csr_seek(cursor, pKey, nKey, LSM_SEEK_EQ)) {
		lsm_csr_close(cursor);
		return rc;
	}

	if (!lsm_csr_valid(cursor)) { rc = -1; } else { rc = 0; }
	lsm_csr_close(cursor);
	return rc;
}


static int pylsm_ensure_opened(LSM* self) {
	if (self->state == PY_LSM_OPENED) return 0;

	PyErr_SetString(PyExc_RuntimeError, "Database has not opened");
	return -1;
}

static int pylsm_ensure_csr_opened(LSMCursor* self) {
	if (self->state == PY_LSM_OPENED) return 0;
	if (pylsm_ensure_opened(self->db)) return 0;
	PyErr_SetString(PyExc_RuntimeError, "Cursor closed");
	return -1;
}


int pylsm_slice_first(LSMSliceView* self) {
	int rc;
	int cmp_res;

	if (self->pStop != NULL) {
		if (rc = lsm_csr_cmp(self->cursor, self->pStop, self->nStop, &cmp_res)) return rc;
		if (cmp_res == 0) return -1;
	}

	if (!lsm_csr_valid(self->cursor)) return -1;

	return 0;
}


int pylsm_slice_next(LSMSliceView* self) {
	int rc;
	int cmp_res = -65535;

	while (lsm_csr_valid(self->cursor)) {
		switch (self->direction) {
			case PY_LSM_SLICE_FORWARD:
				if (rc = lsm_csr_next(self->cursor)) return rc;
				break;
			case PY_LSM_SLICE_BACKWARD:
				if (rc = lsm_csr_prev(self->cursor)) return rc;
				break;
		}

		if (self->pStop != NULL) {
			if (rc = lsm_csr_cmp(self->cursor, self->pStop, self->nStop, &cmp_res)) return rc;
			if (cmp_res == 0) return -1;
		}

		self->counter++;
		if ((self->counter % self->step) == 0) return 0;
	}

	return -1;
}


static inline int pylsm_seek_mode_direction(int direction) {
	return (direction == PY_LSM_SLICE_FORWARD) ? LSM_SEEK_GE : LSM_SEEK_LE;
}


static int pylsm_slice_view_iter(LSMSliceView *self) {
	int rc;

	if (rc = lsm_csr_open(self->db->lsm, &self->cursor)) return rc;

	const char* pKey;
	int nKey;
	int seek_mode = pylsm_seek_mode_direction(self->direction);

	if (self->pStart != NULL) {
		if (rc = lsm_csr_seek(self->cursor, self->pStart, self->nStart, seek_mode)) return rc;
	} else {
		switch (self->direction) {
			case PY_LSM_SLICE_FORWARD:
				if (rc =lsm_csr_first(self->cursor)) return rc;
				break;
			case PY_LSM_SLICE_BACKWARD:
				if (rc = lsm_csr_last(self->cursor)) return rc;
				break;
		}
	}

	return LSM_OK;
}


static int str_or_bytes_check(char binary, PyObject* pObj, const char** ppBuff, ssize_t* nBuf) {
	const char * buff = NULL;
	ssize_t buff_len = 0;

	if (binary) {
		if (PyBytes_Check(pObj)) {
			buff_len = PyBytes_GET_SIZE(pObj);
			buff = PyBytes_AS_STRING(pObj);
		} else {
			PyErr_Format(PyExc_ValueError, "bytes expected not %R", PyObject_Type(pObj));
			return -1;
		}
	} else {
		if (PyUnicode_Check(pObj)) {
			buff = PyUnicode_AsUTF8AndSize(pObj, &buff_len);
			if (buff == NULL) return -1;
		} else {
			PyErr_Format(PyExc_ValueError, "str expected not %R", PyObject_Type(pObj));
			return -1;
		}
	}

	*ppBuff = buff;
	*nBuf = buff_len;

	return 0;
}


static PyObject* LSMIterView_new(PyTypeObject *type) {
	LSMIterView *self;
	self = (LSMIterView *) type->tp_alloc(type, 0);
	Py_INCREF(self);
	return (PyObject *) self;
}


static void LSMIterView_dealloc(LSMIterView *self) {
	if (self->db == NULL) return;

	if (self->cursor != NULL) {
		LSM_MutexLock(self->db);
		lsm_csr_close(self->cursor);
		LSM_MutexLeave(self->db);
	}

	Py_DECREF(self->db);
	self->cursor = NULL;
	self->db = NULL;
}


static int LSMIterView_init(LSMIterView *self, LSM* lsm) {
	if (pylsm_ensure_opened(lsm)) return -1;

	self->db = lsm;
	Py_INCREF(self->db);
	return 0;
}


static int LSMIterView_len(LSMIterView* self) {
	if (pylsm_ensure_opened(self->db)) return -1;

	Py_ssize_t result = 0;
	int rc = 0;

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self->db);
	rc = pylsm_length(self->db->lsm, &result);
	LSM_MutexLeave(self->db);
	Py_END_ALLOW_THREADS

	if (rc) return -1;
	return result;
}

static LSMIterView* LSMIterView_iter(LSMIterView* self) {
	if (pylsm_ensure_opened(self->db)) return NULL;

	LSM_MutexLock(self->db);
	if (pylsm_error(lsm_csr_open(self->db->lsm, &self->cursor))) {
		LSM_MutexLeave(self->db);
	    return NULL;
	}

	if (pylsm_error(lsm_csr_first(self->cursor))) {
		LSM_MutexLeave(self->db);
		return NULL;
	}

	LSM_MutexLeave(self->db);
	return self;
}


static PyObject* LSMKeysView_next(LSMIterView *self) {
	if (pylsm_ensure_opened(self->db)) return NULL;

	PyObject *result;
	char *pKey = NULL;
	ssize_t nKey = 0;

	if (!lsm_csr_valid(self->cursor)) {
		PyErr_SetNone(PyExc_StopIteration);
		return NULL;
	}

	LSM_MutexLock(self->db);

	if (pylsm_error(lsm_csr_key(self->cursor, (const void **) &pKey, &nKey))) {
		LSM_MutexLeave(self->db);
		return NULL;
	}

	if (pylsm_error(lsm_csr_next(self->cursor))) {
		LSM_MutexLeave(self->db);
		return NULL;
	};

	LSM_MutexLeave(self->db);

	if (self->db->binary) {
		result = PyBytes_FromStringAndSize(pKey, nKey);
	} else {
		result = PyUnicode_FromStringAndSize(pKey, nKey);
	}

	return result;
}


static PyObject* LSMValuesView_next(LSMIterView *self) {
	if (pylsm_ensure_opened(self->db)) return NULL;

	PyObject *result;
	char *pValue = NULL;
	ssize_t nValue = 0;

	if (!lsm_csr_valid(self->cursor)) {
		PyErr_SetNone(PyExc_StopIteration);
		return NULL;
	}

	LSM_MutexLock(self->db);

	if (pylsm_error(lsm_csr_value(self->cursor, &pValue, &nValue))) {
		LSM_MutexLeave(self->db);
		return NULL;
	}

	if (pylsm_error(lsm_csr_next(self->cursor))) {
		LSM_MutexLeave(self->db);
		return NULL;
	};

	LSM_MutexLeave(self->db);

	if (self->db->binary) {
		result = PyBytes_FromStringAndSize(pValue, nValue);
	} else {
		result = PyUnicode_FromStringAndSize(pValue, nValue);
	}

	return result;
}


static PyObject* LSMItemsView_next(LSMIterView *self) {
	if (pylsm_ensure_opened(self->db)) return NULL;

	char *pKey = NULL;
	ssize_t nKey = 0;

	char *pValue = NULL;
	ssize_t nValue = 0;

	if (!lsm_csr_valid(self->cursor)) {
		PyErr_SetNone(PyExc_StopIteration);
		return NULL;
	}

	LSM_MutexLock(self->db);

	if (pylsm_error(lsm_csr_key(self->cursor, &pKey, &nKey))) {
		LSM_MutexLeave(self->db);
		return NULL;
	}

	if (pylsm_error(lsm_csr_value(self->cursor, &pValue, &nValue))) {
		LSM_MutexLeave(self->db);
		return NULL;
	}

	if (pylsm_error(lsm_csr_next(self->cursor))) {
		LSM_MutexLeave(self->db);
		return NULL;
	};

	LSM_MutexLeave(self->db);

	PyObject *result;
	PyObject *key;
	PyObject *value;

	if (self->db->binary) {
		key = PyBytes_FromStringAndSize(pKey, nKey);
	} else {
		key = PyUnicode_FromStringAndSize(pKey, nKey);
	}

	if (self->db->binary) {
		value = PyBytes_FromStringAndSize(pValue, nValue);
	} else {
		value = PyUnicode_FromStringAndSize(pValue, nValue);
	}

	return PyTuple_Pack(2, key, value);
}


static int LSM_contains(LSM *self, PyObject *key);

static int LSMKeysView_contains(LSMIterView* self, PyObject* key) {
	return LSM_contains(self->db, key);
}

static PySequenceMethods LSMKeysView_sequence = {
	.sq_length = (lenfunc) LSMIterView_len,
	.sq_contains = (objobjproc) LSMKeysView_contains
};


static int LSMIterView_contains(LSMIterView* self, PyObject* key) {
	PyErr_SetNone(PyExc_NotImplementedError);
	return NULL;
}


static PySequenceMethods LSMIterView_sequence = {
	.sq_length = (lenfunc) LSMIterView_len,
	.sq_contains = (objobjproc) LSMIterView_contains
};

static PyTypeObject LSMKeysType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "lsm_keys",
	.tp_basicsize = sizeof(LSMIterView),
	.tp_itemsize = 0,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_dealloc = (destructor) LSMIterView_dealloc,
	.tp_iter = (getiterfunc) LSMIterView_iter,
	.tp_iternext = (iternextfunc) LSMKeysView_next,
	.tp_as_sequence = &LSMKeysView_sequence
};


static PyTypeObject LSMItemsType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "lsm_items",
	.tp_basicsize = sizeof(LSMIterView),
	.tp_itemsize = 0,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_dealloc = (destructor) LSMIterView_dealloc,
	.tp_iter = (getiterfunc) LSMIterView_iter,
	.tp_iternext = (iternextfunc) LSMItemsView_next,
	.tp_as_sequence = &LSMIterView_sequence
};


static PyTypeObject LSMValuesType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "lsm_values",
	.tp_basicsize = sizeof(LSMIterView),
	.tp_itemsize = 0,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_dealloc = (destructor) LSMIterView_dealloc,
	.tp_iter = (getiterfunc) LSMIterView_iter,
	.tp_iternext = (iternextfunc) LSMValuesView_next,
	.tp_as_sequence = &LSMIterView_sequence
};


static PyObject* LSMSliceView_new(PyTypeObject *type) {
	LSMSliceView *self;
	self = (LSMSliceView *) type->tp_alloc(type, 0);
	Py_INCREF(self);
	return (PyObject *) self;
}


static void LSMSliceView_dealloc(LSMSliceView *self) {
	if (self->db == NULL) return;

	if (self->cursor != NULL) {
		LSM_MutexLock(self->db);
		lsm_csr_close(self->cursor);
		LSM_MutexLeave(self->db);
	}

	if (self->db != NULL) Py_DECREF(self->db);
	if (self->start != NULL) Py_DECREF(self->start);
	if (self->stop != NULL) Py_DECREF(self->stop);

	self->cursor = NULL;
	self->db = NULL;
	self->pStart = NULL;
	self->pStop = NULL;
	self->stop = NULL;
}


static int LSMSliceView_init(
	LSMSliceView *self,
	LSM* lsm,
	PyObject* start,
	PyObject* stop,
	PyObject* step
) {
	assert(lsm != NULL);
	if (pylsm_ensure_opened(lsm)) return -1;

	if (step == Py_None) {
		self->step = 1;
	} else {
		if (!PyLong_Check(step)) {
			PyErr_Format(
				PyExc_ValueError,
				"step must be int not %R",
				PyObject_Type(step)
			);
			return -1;
		}
		self->step = PyLong_AsLong(step);
	}

	self->direction = (self->step > 0) ? PY_LSM_SLICE_FORWARD : PY_LSM_SLICE_BACKWARD;

	self->db = lsm;

	switch (self->direction) {
		case PY_LSM_SLICE_FORWARD:
			self->stop = stop;
			self->start = start;
			break;
		case PY_LSM_SLICE_BACKWARD:
			self->stop = start;
			self->start = stop;
			break;
	}

	self->pStop = NULL;
	self->nStop = 0;
	self->counter = 0;

	if (self->stop != Py_None) {
		if (str_or_bytes_check(self->db->binary, self->stop, &self->pStop, &self->nStop)) return -1;
		Py_INCREF(self->stop);
	}

	if (self->start != Py_None) {
		if (str_or_bytes_check(self->db->binary, self->start, &self->pStart, &self->nStart)) return -1;
		Py_INCREF(self->start);
	}

	Py_INCREF(self->db);

	self->state = PY_LSM_INITIALIZED;
	return 0;
}


static LSMSliceView* LSMSliceView_iter(LSMSliceView* self) {
	if (pylsm_ensure_opened(self->db)) return NULL;

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self->db);

	if (pylsm_error(pylsm_slice_view_iter(self))) return NULL;

	LSM_MutexLeave(self->db);
	Py_END_ALLOW_THREADS

	return self;
}


static PyObject* LSMSliceView_next(LSMSliceView *self) {
	if (pylsm_ensure_opened(self->db)) return NULL;

	if (self->state == PY_LSM_CLOSED) {
		PyErr_SetNone(PyExc_StopIteration);
		return NULL;
	}

	if (!lsm_csr_valid(self->cursor)) {
		PyErr_SetNone(PyExc_StopIteration);
		return NULL;
	}

	int rc;

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self->db);

	if (self->state == PY_LSM_INITIALIZED) {
		self->state = PY_LSM_OPENED;
		rc = pylsm_slice_first(self);
	} else {
		rc = pylsm_slice_next(self);
	}

	LSM_MutexLeave(self->db);
	Py_END_ALLOW_THREADS

	if (rc == -1) {
		self->state = PY_LSM_CLOSED;
		if (!lsm_csr_valid(self->cursor)) {
			PyErr_SetNone(PyExc_StopIteration);
			return NULL;
		}
		rc = 0;
	}

	if (pylsm_error(rc)) return NULL;

	PyObject *result;
	PyObject *key;
	PyObject *value;

	char *pKey = NULL;
	ssize_t nKey = 0;

	char *pValue = NULL;
	ssize_t nValue = 0;

	if (!lsm_csr_valid(self->cursor)) {
		PyErr_SetNone(PyExc_StopIteration);
		return NULL;
	}

	if (rc = pylsm_error(lsm_csr_key(self->cursor, &pKey, &nKey))) return rc;
	if (rc = pylsm_error(lsm_csr_value(self->cursor, &pValue, &nValue))) return rc;

	if (self->db->binary) {
		key = PyBytes_FromStringAndSize(pKey, nKey);
	} else {
		key = PyUnicode_FromStringAndSize(pKey, nKey);
	}

	if (self->db->binary) {
		value = PyBytes_FromStringAndSize(pValue, nValue);
	} else {
		value = PyUnicode_FromStringAndSize(pValue, nValue);
	}

	return PyTuple_Pack(2, key, value);
}


static PyTypeObject LSMSliceType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "lsm_slice",
	.tp_basicsize = sizeof(LSMSliceView),
	.tp_itemsize = 0,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_dealloc = (destructor) LSMSliceView_dealloc,
	.tp_iter = (getiterfunc) LSMSliceView_iter,
	.tp_iternext = (iternextfunc) LSMSliceView_next
};


static PyObject* LSM_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
	LSM *self;
	self = (LSM *) type->tp_alloc(type, 0);
	return (PyObject *) self;
}


static void LSM_dealloc(LSM *self) {
	if (self->state != PY_LSM_CLOSED && self->lsm != NULL) {
		LSM_MutexLock(self);
		lsm_close(self->lsm);
		LSM_MutexLeave(self);
	}

	if (self->lsm_mutex != NULL) self->lsm_env->xMutexDel(self->lsm_mutex);

	self->lsm = NULL;
	self->lsm_env = NULL;
	self->lsm_mutex = NULL;

	if (self->logger != NULL) Py_DECREF(self->logger);
	if (self->path != NULL) PyMem_Free(self->path);
}


static int LSM_init(LSM *self, PyObject *args, PyObject *kwds) {
	self->autocheckpoint = 2048;
	self->autoflush = 1024;
	self->automerge = 4;
	self->autowork = 1;
	self->mmap = (IS_64_BIT ? 1 : 32768);
	self->block_size = 1024;
	self->max_freelist = 24;
	self->multiple_processes = 1;
	self->page_size = 4 * 1024;
	self->readonly = 0;
	self->safety = LSM_SAFETY_NORMAL;
	self->use_log = 1;
	self->tx_level = 0;
	self->compressed = 0;
	self->logger = NULL;
	self->compress_level = PYLSM_DEFAULT_COMPRESS_LEVEL;
	self->path = NULL;
	self->binary = 1;
	memset(&self->lsm_compress, 0, sizeof(lsm_compress));

	static char* kwlist[] = {
		"path",
		"autoflush",
		"page_size",
		"safety",
		"block_size",
		"automerge",
		"max_freelist",
		"autocheckpoint",
		"autowork",
		"mmap",
		"use_log",
		"multiple_processes",
		"readonly",
		"binary",
		"logger",
		"compress",
		"compress_level",
		NULL
	};

	PyObject* compress = Py_None;
	int compressor_id = LSM_COMPRESSION_NONE;

	char *path;
	Py_ssize_t path_len;

	if (!PyArg_ParseTupleAndKeywords(
		args, kwds, "s#|IIIIIIIppppppOOi", kwlist,
		&path, &path_len,
		&self->autoflush,
		&self->page_size,
		&self->safety,
		&self->block_size,
		&self->automerge,
		&self->max_freelist,
		&self->autocheckpoint,
		&self->autowork,
		&self->mmap,
		&self->use_log,
		&self->multiple_processes,
		&self->readonly,
		&self->binary,
		&self->logger,
		&compress,
		&self->compress_level
	)) return -1;

	self->path = PyMem_Calloc(sizeof(char), path_len + 1);
	memcpy(self->path, path, path_len);

	self->state = PY_LSM_INITIALIZED;

	if (self->autoflush > LSM_MAX_AUTOFLUSH) {
		PyErr_Format(
			PyExc_ValueError,
			"The maximum allowable value for autoflush parameter "
			"is 1048576 (1GB). Not %d",
			self->autoflush
		);
		return -1;
	}

	if (self->autocheckpoint == 0) {
		PyErr_SetString(
			PyExc_ValueError,
			"autocheckpoint is not able to be zero"
		);
		return -1;
	}

	if (!(
		is_power_of_two(self->block_size) &&
		self->block_size > 64 &&
		self->block_size < 65536
	)) {
		PyErr_Format(
			PyExc_ValueError,
			"block_size parameter must be power of two between "
			"64 and 65536. Not %d",
			self->block_size
		);
		return -1;
	}

	switch (self->safety) {
		case LSM_SAFETY_OFF:
			break;
		case LSM_SAFETY_NORMAL:
			break;
		case LSM_SAFETY_FULL:
			break;
		default:
			PyErr_Format(
				PyExc_ValueError,
				"safety parameter must be SAFETY_OFF SAFETY_NORMAL "
				"or SAFETY_FULL. Not %d", self->safety
			);
			return -1;
	}

	if (compress == Py_None) {
		compressor_id = PY_LSM_COMPRESSOR_NONE;
	} else if (!PyUnicode_Check(compress)) {
		PyErr_Format(PyExc_ValueError, "str expected not %R", PyObject_Type(compress));
		return -1;
	} else if (PyUnicode_CompareWithASCIIString(compress, "none") == 0) {
		compressor_id = PY_LSM_COMPRESSOR_NONE;
	} else if (PyUnicode_CompareWithASCIIString(compress, "lz4") == 0) {
		compressor_id = PY_LSM_COMPRESSOR_LZ4;

		if (self->compress_level == PYLSM_DEFAULT_COMPRESS_LEVEL) {
			self->compress_level = LZ4_COMP_LEVEL_DEFAULT;
		}

		if (self->compress_level > LZ4_COMP_LEVEL_MAX || self->compress_level < 1) {
			PyErr_Format(
				PyExc_ValueError,
				"compress_level for lz4 must be between 1 and %d",
				 LZ4_COMP_LEVEL_MAX
			);
			return -1;
		}
	} else if (PyUnicode_CompareWithASCIIString(compress, "zstd") == 0) {
		compressor_id = PY_LSM_COMPRESSOR_ZSTD;
		if (self->compress_level == PYLSM_DEFAULT_COMPRESS_LEVEL) {
			self->compress_level = ZSTD_CLEVEL_DEFAULT;
		}

		if (self->compress_level > ZSTD_maxCLevel() || self->compress_level < 1) {
			PyErr_Format(
				PyExc_ValueError,
				"compress_level for zstd must be between 1 and %d", ZSTD_maxCLevel()
			);
			return -1;
		}

	} else {
		PyErr_Format(
			PyExc_ValueError,
			"compressor argument must be one of \"none\" (or None) \"lz4\" or \"zstd\", but not %R",
			compress
		);
		return -1;
	}

	if (compressor_id != PY_LSM_COMPRESSOR_NONE) self->compressed = 1;

	if (self->logger != NULL && !PyCallable_Check(self->logger)) {
		PyErr_Format(PyExc_ValueError, "object %R is not callable", self->logger);
		return -1;
	}

	if (self->logger != NULL) Py_INCREF(self->logger);
	if (pylsm_error(lsm_new(NULL, &self->lsm))) return -1;

	self->lsm_env = lsm_get_env(self->lsm);

	if (pylsm_error(self->lsm_env->xMutexNew(self->lsm_env, &self->lsm_mutex))) return -1;

	if (self->logger != NULL) {
		lsm_config_log(self->lsm, pylsm_logger, self);
	} else {
		lsm_config_log(self->lsm, NULL, NULL);
	}

	if (self->lsm == NULL) {
		PyErr_SetString(PyExc_MemoryError, "Can not allocate memory");
		return -1;
	}

	// Only before lsm_open
	if (self->compressed) {
		self->lsm_compress.pCtx = self;
		self->lsm_compress.iId = compressor_id;

		switch (compressor_id) {
			case PY_LSM_COMPRESSOR_LZ4:
				self->lsm_compress.xCompress = pylsm_lz4_xCompress;
				self->lsm_compress.xUncompress = pylsm_lz4_xUncompress;
				self->lsm_compress.xBound = pylsm_lz4_xBound;
				self->lsm_compress.xFree = NULL;
				break;
			case PY_LSM_COMPRESSOR_ZSTD:
				self->lsm_compress.xCompress = pylsm_zstd_xCompress;
				self->lsm_compress.xUncompress = pylsm_zstd_xUncompress;
				self->lsm_compress.xBound = pylsm_zstd_xBound;
				self->lsm_compress.xFree = NULL;
				break;
		}

		if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_SET_COMPRESSION, &self->lsm_compress))) return -1;
	}

	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_BLOCK_SIZE, &self->block_size))) return -1;
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_MULTIPLE_PROCESSES, &self->multiple_processes))) return -1;
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_PAGE_SIZE, &self->page_size))) return -1;
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_READONLY, &self->readonly))) return -1;

	// Not only before lsm_open
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_AUTOCHECKPOINT, &self->autocheckpoint))) return -1;
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_AUTOFLUSH, &self->autoflush))) return -1;
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_AUTOMERGE, &self->automerge))) return -1;
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_AUTOWORK, &self->autowork))) return -1;
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_MAX_FREELIST, &self->max_freelist))) return -1;
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_MMAP, &self->mmap))) return -1;
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_SAFETY, &self->safety))) return -1;
	if (pylsm_error(lsm_config(self->lsm, LSM_CONFIG_USE_LOG, &self->use_log))) return -1;

	if (PyErr_Occurred()) return -1;

	return 0;
}


static PyObject* LSM_open(LSM *self) {
	if (self->state == PY_LSM_OPENED) {
		PyErr_SetString(PyExc_RuntimeError, "Database already opened");
		return NULL;
	}

	if (self->state == PY_LSM_CLOSED) {
		PyErr_SetString(PyExc_RuntimeError, "Database closed");
		return NULL;
	}

	int result;
	Py_BEGIN_ALLOW_THREADS
	result = lsm_open(self->lsm, self->path);
	Py_END_ALLOW_THREADS

	if (pylsm_error(result)) return NULL;

	if (self->readonly == 0) {
		Py_BEGIN_ALLOW_THREADS
		result = lsm_flush(self->lsm);
		Py_END_ALLOW_THREADS

		if (pylsm_error(result)) return NULL;

		Py_BEGIN_ALLOW_THREADS
		result = lsm_work(self->lsm, self->automerge, self->page_size, NULL);
		Py_END_ALLOW_THREADS

		if (pylsm_error(result)) return NULL;
	}

	self->state = PY_LSM_OPENED;
	Py_RETURN_TRUE;
}

static int _LSM_close(LSM* self) {
	int result;
	LSM_MutexLock(self);
	result = lsm_close(self->lsm);
	LSM_MutexLeave(self);
	self->lsm = NULL;
	self->state = PY_LSM_CLOSED;
	return result;
}

static PyObject* LSM_close(LSM *self) {
	if (self->state == PY_LSM_CLOSED) {
		PyErr_SetString(PyExc_RuntimeError, "Database already closed");
		return NULL;
	}

	int result;
	result = _LSM_close(self);

	if (pylsm_error(result)) return NULL;
	Py_RETURN_TRUE;
}


static PyObject* LSM_info(LSM *self) {
	if (pylsm_ensure_opened(self)) return NULL;

	int32_t nwrite; int nwrite_result;
	int32_t nread; int nread_result;
	int checkpoint_size, checkpoint_size_result;
	int tree_size_old, tree_size_current, tree_size_result;

	LSM_MutexLock(self);
	nwrite_result = lsm_info(
		self->lsm, LSM_INFO_NWRITE, &nwrite
	);
	nread_result = lsm_info(
		self->lsm, LSM_INFO_NREAD, &nread
	);
	checkpoint_size_result = lsm_info(
		self->lsm, LSM_INFO_CHECKPOINT_SIZE, &checkpoint_size
	);
	tree_size_result = lsm_info(
		self->lsm, LSM_INFO_TREE_SIZE, &tree_size_old, &tree_size_current
	);
	LSM_MutexLeave(self);

	if (pylsm_error(nwrite_result)) return NULL;
	if (pylsm_error(nread_result)) return NULL;
	if (pylsm_error(checkpoint_size_result)) return NULL;
	if (pylsm_error(tree_size_result)) return NULL;

	PyObject *result = PyDict_New();

	if (PyDict_SetItemString(result, "nwrite", PyLong_FromLong(nwrite))) return NULL;
	if (PyDict_SetItemString(result, "nread", PyLong_FromLongLong(nread))) return NULL;
	if (PyDict_SetItemString(result, "checkpoint_size_result", PyLong_FromLong(checkpoint_size))) return NULL;

	PyObject *tree_size = PyDict_New();
	if (PyDict_SetItemString(tree_size, "old", PyLong_FromLong(tree_size_old))) return NULL;
	if (PyDict_SetItemString(tree_size, "current", PyLong_FromLong(tree_size_current))) return NULL;
	if (PyDict_SetItemString(result, "tree_size", tree_size)) return NULL;
	return result;
}


static PyObject* LSM_ctx_enter(LSM *self) {
	if (self->state == PY_LSM_OPENED) return (PyObject*) self;

	LSM_open(self);
	if (PyErr_Occurred()) return NULL;

	Py_INCREF(self);

	return (PyObject*) self;
}


static PyObject* LSM_ctx_exit(LSM *self) {
	if (self->state == PY_LSM_CLOSED) { Py_RETURN_NONE; };

	_LSM_close(self);
	Py_RETURN_NONE;
}


static PyObject* LSM_work(LSM *self, PyObject *args, PyObject *kwds) {
	if (pylsm_ensure_opened(self)) return NULL;

	static char *kwlist[] = {"nmerge", "nkb", "complete", NULL};

	char complete = 1;
	int nmerge = self->automerge;
	int nkb = self->page_size;

	if (!PyArg_ParseTupleAndKeywords(
		args, kwds, "|IIp", kwlist, &nmerge, &nkb, &complete
	)) return NULL;

	int result;
	int total_written = 0;
	int written = 0;

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);

	do {
		result = lsm_work(self->lsm, nmerge, nkb, &written);
		total_written += written;
		if (nmerge < self->automerge) nmerge++;
	} while (complete && written > 0);

	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (pylsm_error(result)) return NULL;
	return PyLong_FromLong(total_written);
}


static PyObject* LSM_flush(LSM *self) {
	if (pylsm_ensure_opened(self)) return NULL;

	int rc;

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);
	rc = lsm_flush(self->lsm);
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (pylsm_error(rc)) return NULL;
	Py_RETURN_TRUE;
}

static PyObject* LSM_checkpoint(LSM *self) {
	if (pylsm_ensure_opened(self)) return NULL;

	int result;
	int bytes_written = 0;

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);
	result = lsm_checkpoint(self->lsm, &bytes_written);
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (pylsm_error(result)) return NULL;
	return PyLong_FromLong(bytes_written);
}

static PyObject* LSM_cursor(LSM *self, PyObject *args, PyObject *kwds) {
	if (pylsm_ensure_opened(self)) return NULL;

	LSMCursor* cursor = (LSMCursor*) LSMCursor_new(&LSMCursorType);
	cursor->db = self;

	int rc;

	LSM_MutexLock(self);
	rc = lsm_csr_open(cursor->db->lsm, &cursor->cursor);
	LSM_MutexLeave(self);

	if(pylsm_error(rc)) return NULL;
	cursor->state = PY_LSM_OPENED;

	Py_INCREF(cursor->db);
	Py_INCREF(cursor);

	return (PyObject*) cursor;
}


static PyObject* LSM_insert(LSM *self, PyObject *args, PyObject *kwds) {
	if (pylsm_ensure_opened(self)) return NULL;

	static char *kwlist[] = {"key", "value", NULL};

	PyObject* key = NULL;
	PyObject* val = NULL;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", kwlist, &key, &val)) return NULL;

	const char* pKey = NULL;
	Py_ssize_t nKey = 0;

	const char* pVal = NULL;
	Py_ssize_t nVal = 0;

	if (str_or_bytes_check(self->binary, key, &pKey, &nKey)) return NULL;
	if (str_or_bytes_check(self->binary, val, &pVal, &nVal)) return NULL;

	int result;

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);
	result = lsm_insert(self->lsm, pKey, nKey, pVal, nVal);
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (pylsm_error(result)) return NULL;
	Py_RETURN_NONE;
}


static PyObject* LSM_delete(LSM *self, PyObject *args, PyObject *kwds) {
	if (pylsm_ensure_opened(self)) return NULL;

	static char *kwlist[] = {"key", NULL};

	PyObject* key = NULL;
	const char* pKey = NULL;
	Py_ssize_t nKey = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &key)) return NULL;
	if (str_or_bytes_check(self->binary, key, &pKey, &nKey)) return NULL;

	int result;
	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);
	result = lsm_delete(self->lsm, pKey, nKey);
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (pylsm_error(result)) return NULL;
	Py_RETURN_NONE;
}


static PyObject* LSM_delete_range(LSM *self, PyObject *args, PyObject *kwds) {
	if (pylsm_ensure_opened(self)) return NULL;

	static char *kwlist[] = {"start", "end", NULL};

	PyObject* key_start = NULL;
	PyObject* key_end = NULL;

	const char* pKeyStart = NULL;
	Py_ssize_t nKeyStart = 0;

	const char* pKeyEnd = NULL;
	Py_ssize_t nKeyEnd = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", kwlist, &key_start, &key_end)) return NULL;
	if (str_or_bytes_check(self->binary, key_start, &pKeyStart, &nKeyStart)) return NULL;
	if (str_or_bytes_check(self->binary, key_end, &pKeyEnd, &nKeyEnd)) return NULL;

	int result;
	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);
	result = lsm_delete_range(self->lsm, pKeyStart, nKeyStart, pKeyEnd, nKeyEnd);
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (pylsm_error(result)) return NULL;
	Py_RETURN_NONE;
}


static PyObject* LSM_begin(LSM *self) {
	if (pylsm_ensure_opened(self)) return NULL;

	int level = self->tx_level + 1;

	int result;
	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);
	result = lsm_begin(self->lsm, level);
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (pylsm_error(result)) return NULL;
	self->tx_level = level;
	Py_RETURN_TRUE;
}


static PyObject* LSM_commit(LSM *self) {
	if (pylsm_ensure_opened(self)) return NULL;

	self->tx_level--;

	int result;
	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);
	result = lsm_commit(self->lsm, self->tx_level);
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (pylsm_error(result)) return NULL;
	if (self->tx_level < 0) self->tx_level = 0;
	Py_RETURN_TRUE;
}


static PyObject* LSM_getitem(LSM *self, PyObject *arg) {
	if (pylsm_ensure_opened(self)) return NULL;

	PyObject* key = arg;
	const char* pKey = NULL;
	Py_ssize_t nKey = 0;
	ssize_t tuple_size;
	int seek_mode = LSM_SEEK_EQ;

	if (PySlice_Check(arg)) {
		PySliceObject* slice = (PySliceObject*) arg;

		LSMSliceView* view = (LSMSliceView*) LSMSliceView_new(&LSMSliceType);
		if (LSMSliceView_init(view, self, slice->start, slice->stop, slice->step)) return NULL;
		return view;
	}

	if (PyTuple_Check(arg)) {
		tuple_size = PyTuple_GET_SIZE(arg);
		if (tuple_size != 2) {
			PyErr_Format(
				PyExc_ValueError,
				"tuple argument must be pair of key and seek_mode passed tuple has size %d",
				tuple_size
			);
			return NULL;
		}

		key = PyTuple_GetItem(arg, 0);
		PyObject* seek_mode_obj = PyTuple_GetItem(arg, 1);

		if (!PyLong_Check(seek_mode_obj)) {
			PyErr_Format(
				PyExc_ValueError,
				"second tuple argument must be int not %R",
				PyObject_Type(seek_mode_obj)
			);
			return NULL;
		}

		seek_mode = PyLong_AsLong(seek_mode_obj);
	}

	if (pylsm_seek_mode_check(seek_mode)) return NULL;

	if (str_or_bytes_check(self->binary, key, &pKey, &nKey)) return NULL;

	int result;
	char *value_buff = NULL;
	int value_len = 0;

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);

	result = pylsm_getitem(
		self->lsm,
		pKey,
		nKey,
		&value_buff,
		&value_len,
		seek_mode
	);

	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (result == -1) {
		PyErr_Format(
			PyExc_KeyError,
			"Key %R was not found",
			key
		);
		return NULL;
	}

	if (pylsm_error(result)) return NULL;

	PyObject* py_value;
	if (self->binary) {
		py_value = PyBytes_FromStringAndSize(value_buff, value_len);
	} else {
		py_value = PyUnicode_FromStringAndSize(value_buff, value_len);
	}

	free(value_buff);

	return py_value;
}


static int LSM_set_del_item(LSM* self, PyObject* key, PyObject* value) {
	if (pylsm_ensure_opened(self)) return -1;

	int rc;
	const char* pKey = NULL;
	Py_ssize_t nKey = 0;

	const char* pVal = NULL;
	Py_ssize_t nVal = 0;

	// Delete slice
	if (PySlice_Check(key)) {

		if (value != NULL) {
			PyErr_SetString(PyExc_NotImplementedError, "setting range doesn't supported yet");
			return -1;
		}

		PySliceObject* slice = (PySliceObject*) key;

		long step = 1;

		if (slice->step != Py_None) {
			PyErr_SetString(PyExc_ValueError, "Stepping not allowed in delete_range operation");
			return -1;
		}

		if (slice->start == Py_None || slice->stop == Py_None) {
			PyErr_SetString(PyExc_ValueError, "You must provide range start and range stop values");
			return -1;
		}

		char *pStop = NULL;
		char *pStart = NULL;
		ssize_t nStart = 0;
		ssize_t nStop = 0;

		if (str_or_bytes_check(self->binary, slice->start, &pStart, &nStart)) return -1;
		if (str_or_bytes_check(self->binary, slice->stop, &pStop, &nStop)) return -1;

		Py_INCREF(slice->start);
		Py_INCREF(slice->stop);

		int rc;

		Py_BEGIN_ALLOW_THREADS
		LSM_MutexLock(self);
		rc = lsm_delete_range(self->lsm, pStart, nStart, pStop, nStop);
		LSM_MutexLeave(self);
		Py_END_ALLOW_THREADS

		Py_DECREF(slice->start);
		Py_DECREF(slice->stop);

		if (pylsm_error(rc)) return -1;

		return 0;
	}

	if (str_or_bytes_check(self->binary, key, &pKey, &nKey)) return -1;
	if (value != NULL) { if (str_or_bytes_check(self->binary, value, &pVal, &nVal)) return -1; }

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);
	if (pVal == NULL) {
		rc = pylsm_delitem(self->lsm, pKey, nKey);
	} else {
		rc = lsm_insert(self->lsm, pKey, nKey, pVal, nVal);
	}
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (rc == -1) {
		PyErr_Format(
    		PyExc_KeyError,
    		"Key %R was not found",
    		key
    	);
		return -1;
	}

	if (pylsm_error(rc)) return -1;

	return 0;
}


static int LSM_contains(LSM *self, PyObject *key) {
	if (pylsm_ensure_opened(self)) return NULL;

	const char* pKey = NULL;
	Py_ssize_t nKey = 0;

	if (str_or_bytes_check(self->binary, key, &pKey, &nKey)) return NULL;

	int rc;

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);
	rc = pylsm_contains(self->lsm, pKey, nKey);
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (rc == -1) return 0;
	if (rc == 0) return 1;

	pylsm_error(rc);
	return -1;
}


static PyObject* LSM_rollback(LSM *self) {
	if (pylsm_ensure_opened(self)) return NULL;

	int result;
	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);
	result = lsm_rollback(self->lsm, self->tx_level);
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	self->tx_level--;
	if (pylsm_error(result)) return NULL;
	if (self->tx_level < 0) self->tx_level = 0;
	Py_RETURN_TRUE;
}


static PyObject* LSM_compress_get(LSM* self) {
	switch (self->lsm_compress.iId) {
		case PY_LSM_COMPRESSOR_NONE:
			Py_RETURN_NONE;
		case PY_LSM_COMPRESSOR_LZ4:
			return PyUnicode_FromString("lz4");
		case PY_LSM_COMPRESSOR_ZSTD:
			return PyUnicode_FromString("zstd");
	}

	PyErr_SetString(PyExc_RuntimeError, "invalid compressor");
	return NULL;
}


static PyObject* LSM_repr(LSM *self) {
	char * path = self->path;
	if (path == NULL) path = "<NULL>";
	return PyUnicode_FromFormat(
		"<%s at \"%s\" as %p>", Py_TYPE(self)->tp_name, path, self
	);
}


static Py_ssize_t LSM_length(LSM *self) {
	Py_ssize_t result = 0;
	int rc = 0;

	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self);

	rc = pylsm_length(self->lsm, &result);
	LSM_MutexLeave(self);
	Py_END_ALLOW_THREADS

	if (rc) return -1;
	return result;
}


static LSMIterView* LSM_keys(LSM* self) {
	if (pylsm_ensure_opened(self)) return NULL;

	LSMIterView* view = (LSMIterView*) LSMIterView_new(&LSMKeysType);
	if (LSMIterView_init(view, self)) return NULL;
	return view;
}

static LSMIterView* LSM_values(LSM* self) {
	if (pylsm_ensure_opened(self)) return NULL;

	LSMIterView* view = (LSMIterView*) LSMIterView_new(&LSMValuesType);
	if (LSMIterView_init(view, self)) return NULL;
	return view;
}

static LSMIterView* LSM_items(LSM* self) {
	if (pylsm_ensure_opened(self)) return NULL;

	LSMIterView* view = (LSMIterView*) LSMIterView_new(&LSMItemsType);
	if (LSMIterView_init(view, self)) return NULL;
	return view;
}

static LSMIterView* LSM_iter(LSM* self) {
	if (pylsm_ensure_opened(self)) return NULL;

	LSMIterView* view = (LSMIterView*) LSMIterView_new(&LSMKeysType);
	if (LSMIterView_init(view, self)) return NULL;
	return LSMIterView_iter(view);
}

static PyObject* LSM_update(LSM* self, PyObject *args) {
	if (pylsm_ensure_opened(self)) return NULL;

	PyObject * value = NULL;
	if (!PyArg_ParseTuple(args, "O", &value)) return NULL;
	if (!PyMapping_Check(value)) {
		PyErr_Format(
			PyExc_ValueError,
			"Mapping expected not %R",
			PyObject_Type(value)
		);
		return NULL;
	}

	PyObject* items = PyMapping_Items(value);

	if (!PyList_Check(items)) {
		PyErr_Format(
			PyExc_ValueError,
			"Iterable expected not %R",
			PyObject_Type(items)
		);
		return NULL;
	}

	int mapping_size = PyMapping_Length(value);

	PyObject **objects = PyMem_Calloc(mapping_size * 2, sizeof(PyObject*));
	char **keys = PyMem_Calloc(mapping_size, sizeof(char*));
	char **values = PyMem_Calloc(mapping_size, sizeof(char*));
	int *key_sizes = PyMem_Calloc(mapping_size, sizeof(int*));
	int *value_sizes = PyMem_Calloc(mapping_size, sizeof(int*));

	PyObject *item;
	size_t count = 0;
	PyObject *iterator = PyObject_GetIter(items);

	PyObject* obj;

	unsigned short is_ok = 1;

	while ((item = PyIter_Next(iterator))) {
		if (PyTuple_Size(item) != 2) {
			Py_DECREF(item);
			PyErr_Format(
				PyExc_ValueError,
				"Mapping items must be tuple with pair not %R",
				item
			);
			is_ok = 0;
			break;
		}

		obj = PyTuple_GET_ITEM(item, 0);
		if (str_or_bytes_check(self->binary, obj, &keys[count], &key_sizes[count])) {
			Py_DECREF(item);
			is_ok = 0;
			break;
		}

		objects[count * 2] = obj;
		Py_INCREF(obj);

		obj = PyTuple_GET_ITEM(item, 1);
		if (str_or_bytes_check(self->binary, obj, &values[count], &value_sizes[count])) {
			Py_DECREF(item);
			is_ok = 0;
			break;
		}

		objects[(count * 2) + 1] = obj;
		Py_INCREF(value);

		Py_DECREF(item);
		count++;
    }

    int rc;

	if (is_ok) {
		Py_BEGIN_ALLOW_THREADS
		LSM_MutexLock(self);
		for (size_t i=0; i < mapping_size; i++) {
			if (rc = lsm_insert(self->lsm, keys[i], key_sizes[i], values[i], value_sizes[i])) break;
		}
		LSM_MutexLeave(self);
		Py_END_ALLOW_THREADS

		if (pylsm_error(rc)) is_ok = 0;
	}

	for (size_t i = 0; i < mapping_size && objects[i] != NULL; i++) {
		Py_DECREF(objects[i]);
	}

	PyMem_Free(key_sizes);
	PyMem_Free(value_sizes);
	PyMem_Free(keys);
	PyMem_Free(values);
	PyMem_Free(objects);

	if (is_ok) {
		Py_RETURN_NONE;
	} else {
		return NULL;
	}
}

static PyMemberDef LSM_members[] = {
	{
		"path",
		T_STRING,
		offsetof(LSM, path),
		READONLY,
		"path"
	},
	{
		"compressed",
		T_BOOL,
		offsetof(LSM, compressed),
		READONLY,
		"compressed"
	},
	{
		"state",
		T_INT,
		offsetof(LSM, state),
		READONLY,
		"state"
	},
	{
		"page_size",
		T_INT,
		offsetof(LSM, page_size),
		READONLY,
		"page_size"
	},
	{
		"block_size",
		T_INT,
		offsetof(LSM, block_size),
		READONLY,
		"block_size"
	},
	{
		"safety",
		T_INT,
		offsetof(LSM, safety),
		READONLY,
		"safety"
	},
	{
		"autowork",
		T_BOOL,
		offsetof(LSM, autowork),
		READONLY,
		"autowork"
	},
	{
		"autocheckpoint",
		T_INT,
		offsetof(LSM, autocheckpoint),
		READONLY,
		"autocheckpoint"
	},
	{
		"mmap",
		T_BOOL,
		offsetof(LSM, mmap),
		READONLY,
		"mmap"
	},
	{
		"use_log",
		T_BOOL,
		offsetof(LSM, use_log),
		READONLY,
		"use_log"
	},
	{
		"automerge",
		T_INT,
		offsetof(LSM, automerge),
		READONLY,
		"automerge"
	},
	{
		"max_freelist",
		T_INT,
		offsetof(LSM, max_freelist),
		READONLY,
		"max_freelist"
	},
	{
		"multiple_processes",
		T_BOOL,
		offsetof(LSM, multiple_processes),
		READONLY,
		"multiple_processes"
	},
	{
		"readonly",
		T_BOOL,
		offsetof(LSM, readonly),
		READONLY,
		"readonly"
	},
	{
		"binary",
		T_BOOL,
		offsetof(LSM, binary),
		READONLY,
		"binary"
	},
	{
		"compress_level",
		T_INT,
		offsetof(LSM, compress_level),
		READONLY,
		"compress_level"
	},
	{NULL}  /* Sentinel */
};


static PyMethodDef LSM_methods[] = {
	{
		"__enter__",
		(PyCFunction) LSM_ctx_enter, METH_NOARGS,
		"Enter context"
	},
	{
		"__exit__",
		(PyCFunction) LSM_ctx_exit, METH_VARARGS | METH_KEYWORDS,
		"Exit context"
	},
	{
		"open",
		(PyCFunction) LSM_open, METH_NOARGS,
		"Open database"
	},
	{
		"close",
		(PyCFunction) LSM_close, METH_NOARGS,
		"Close database"
	},
	{
		"info",
		(PyCFunction) LSM_info, METH_NOARGS,
		"Database info"
	},
	{
		"work",
		(PyCFunction) LSM_work, METH_VARARGS | METH_KEYWORDS,
		"Explicit Database work"
	},
	{
		"flush",
		(PyCFunction) LSM_flush, METH_NOARGS,
		"Explicit Database flush"
	},
	{
		"checkpoint",
		(PyCFunction) LSM_checkpoint, METH_NOARGS,
		"Explicit Database checkpointing"
	},
	{
		"cursor",
		(PyCFunction) LSM_cursor, METH_NOARGS,
		"Create a cursor"
	},
	{
		"insert",
		(PyCFunction) LSM_insert, METH_VARARGS | METH_KEYWORDS,
		"Insert key and value"
	},
	{
		"delete",
		(PyCFunction) LSM_delete, METH_VARARGS | METH_KEYWORDS,
		"Delete value by key"
	},
	{
		"delete_range",
		(PyCFunction) LSM_delete_range, METH_VARARGS | METH_KEYWORDS,
		"Delete values by range"
	},
	{
		"begin",
		(PyCFunction) LSM_begin, METH_NOARGS,
		"Start transaction"
	},
	{
		"commit",
		(PyCFunction) LSM_commit, METH_NOARGS,
		"Commit transaction"
	},
	{
		"rollback",
		(PyCFunction) LSM_rollback, METH_NOARGS,
		"Rollback transaction"
	},
	{
		"keys",
		(PyCFunction) LSM_keys, METH_NOARGS,
		"Returns lsm_keys instance"
	},
	{
		"values",
		(PyCFunction) LSM_values, METH_NOARGS,
		"Returns lsm_keys instance"
	},
	{
		"items",
		(PyCFunction) LSM_items, METH_NOARGS,
		"Returns lsm_keys instance"
	},
	{
		"update",
		(PyCFunction) LSM_update, METH_VARARGS,
		"dict-like update method"

	},
	{NULL}  /* Sentinel */
};


static PyGetSetDef LSMTypeGetSet[] = {
	{
		.name = "compress",
		.get = LSM_compress_get,
		.doc = "Compression algorithm"
	},
	{NULL} /* Sentinel */
};

static PyMappingMethods LSMTypeMapping = {
	.mp_subscript = (binaryfunc) LSM_getitem,
	.mp_ass_subscript = (objobjargproc) LSM_set_del_item,
	.mp_length = (lenfunc) LSM_length
};


static PySequenceMethods LSMTypeSequence = {
	.sq_contains = (objobjproc) LSM_contains
};


static PyTypeObject LSMType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "LSM",
	.tp_doc = "",
	.tp_basicsize = sizeof(LSM),
	.tp_itemsize = 0,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_new = LSM_new,
	.tp_init = (initproc) LSM_init,
	.tp_dealloc = (destructor) LSM_dealloc,
	.tp_members = LSM_members,
	.tp_methods = LSM_methods,
	.tp_repr = (reprfunc) LSM_repr,
	.tp_as_mapping = &LSMTypeMapping,
	.tp_as_sequence = &LSMTypeSequence,
	.tp_getset = &LSMTypeGetSet,
	.tp_iter = (getiterfunc) LSM_iter
};


static PyObject* LSMCursor_new(PyTypeObject *type) {
	LSMCursor *self;

	self = (LSMCursor *) type->tp_alloc(type, 0);
	self->state = PY_LSM_INITIALIZED;

	return (PyObject *) self;
}


static void LSMCursor_dealloc(LSMCursor *self) {
	if (self->state != PY_LSM_CLOSED && self->cursor != NULL) {
		lsm_csr_close(self->cursor);
		self->cursor = NULL;
		self->state = PY_LSM_CLOSED;
	}

	if (self->db != NULL) {
		Py_DECREF(self->db);
		self->db = NULL;
	}
}


static PyObject* LSMCursor_first(LSMCursor *self) {
	if (pylsm_ensure_csr_opened(self)) return NULL;
	int result;
	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self->db);
	result = lsm_csr_first(self->cursor);
	LSM_MutexLeave(self->db);
	Py_END_ALLOW_THREADS

	if (pylsm_error(result)) return NULL;
	self->state = PY_LSM_OPENED;
	Py_RETURN_NONE;
}


static PyObject* LSMCursor_last(LSMCursor *self) {
	if (pylsm_ensure_csr_opened(self)) return NULL;
	int result;
	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self->db);
	result = lsm_csr_last(self->cursor);
	LSM_MutexLeave(self->db);
	Py_END_ALLOW_THREADS

	if (pylsm_error(result)) return NULL;
	self->state = PY_LSM_OPENED;
	Py_RETURN_NONE;
}


static PyObject* LSMCursor_close(LSMCursor *self) {
	if (pylsm_ensure_csr_opened(self)) return NULL;
	int result;
	result = lsm_csr_close(self->cursor);

	if (pylsm_error(result)) return NULL;

	if (self->db != NULL) Py_DECREF(self->db);
	self->db = NULL;

	self->cursor = NULL;
	self->state = PY_LSM_CLOSED;
	Py_RETURN_NONE;
}

static PyObject* LSMCursor_seek(LSMCursor *self, PyObject* args, PyObject* kwds) {
	if (pylsm_ensure_csr_opened(self)) return NULL;
	static char *kwlist[] = {"key", "seek_mode", NULL};

	self->seek_mode = LSM_SEEK_EQ;

	PyObject* key = NULL;
	const char* pKey = NULL;
	Py_ssize_t nKey = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|I", kwlist, &key, &self->seek_mode)) return NULL;
	if (pylsm_seek_mode_check(self->seek_mode)) return NULL;

	int rc;

	if (str_or_bytes_check(self->db->binary, key, &pKey, &nKey)) return NULL;
	Py_BEGIN_ALLOW_THREADS
	LSM_MutexLock(self->db);
	rc = lsm_csr_seek(self->cursor, pKey, nKey, self->seek_mode);
	LSM_MutexLeave(self->db);
	Py_END_ALLOW_THREADS

	if (pylsm_error(rc)) return NULL;
	if (lsm_csr_valid(self->cursor)) { Py_RETURN_TRUE; } else { Py_RETURN_FALSE; }
}


static PyObject* LSMCursor_compare(LSMCursor *self, PyObject* args, PyObject* kwds) {
	if (pylsm_ensure_csr_opened(self)) return NULL;
	static char *kwlist[] = {"key", NULL};

	PyObject * key = NULL;
	const char* pKey = NULL;
	Py_ssize_t nKey = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &key)) return NULL;
	if (str_or_bytes_check(self->db->binary, key, &pKey, &nKey)) return NULL;

	int cmp_result = 0;
	int result;

	LSM_MutexLock(self->db);
	result = lsm_csr_cmp(self->cursor, pKey, nKey, &cmp_result);
	LSM_MutexLeave(self->db);

	if (pylsm_error(result)) return NULL;
	return PyLong_FromLong(cmp_result);
}

static PyObject* LSMCursor_retrieve(LSMCursor *self) {
	if (pylsm_ensure_csr_opened(self)) return NULL;
	if(!lsm_csr_valid(self->cursor)) { Py_RETURN_NONE; }

	char* key_buff = NULL;
	char* value_buff = NULL;
	int key_len = 0;
	int value_len = 0;

	LSM_MutexLock(self->db);
	lsm_csr_key(self->cursor, (const void **)&key_buff, &key_len);
	lsm_csr_value(self->cursor, (const void **)&value_buff, &value_len);
	LSM_MutexLeave(self->db);

	PyObject* key;
	PyObject* value;

	if (self->db->binary) {
		key = PyBytes_FromStringAndSize(key_buff, key_len);
		value = PyBytes_FromStringAndSize(value_buff, value_len);
	} else {
		key = PyUnicode_FromStringAndSize(key_buff, key_len);
		value = PyUnicode_FromStringAndSize(value_buff, value_len);
	}
	return PyTuple_Pack(2, key, value);
}


static PyObject* LSMCursor_next(LSMCursor *self) {
	if (pylsm_ensure_csr_opened(self)) return NULL;
	if (self->seek_mode == LSM_SEEK_EQ) Py_RETURN_FALSE;
	if (!lsm_csr_valid(self->cursor)) Py_RETURN_FALSE;

	LSM_MutexLock(self->db);
	if (pylsm_error(lsm_csr_next(self->cursor))) return NULL;
	LSM_MutexLeave(self->db);

	if (!lsm_csr_valid(self->cursor)) Py_RETURN_FALSE;
	Py_RETURN_TRUE;
}


static PyObject* LSMCursor_prev(LSMCursor *self) {
	if (pylsm_ensure_csr_opened(self)) return NULL;
	if (self->seek_mode == LSM_SEEK_EQ) Py_RETURN_FALSE;
	if (!lsm_csr_valid(self->cursor)) Py_RETURN_FALSE;

	int res = 0;

	LSM_MutexLock(self->db);
	if (pylsm_error(lsm_csr_prev(self->cursor))) return NULL;
	LSM_MutexLeave(self->db);

	if (!lsm_csr_valid(self->cursor)) Py_RETURN_FALSE;
	Py_RETURN_TRUE;
}


static PyObject* LSMCursor_ctx_enter(LSMCursor *self) {
	if (pylsm_ensure_csr_opened(self)) return NULL;
	return (PyObject*) self;
}


static PyObject* LSMCursor_ctx_exit(LSMCursor *self) {
	if (self->state == PY_LSM_CLOSED) { Py_RETURN_NONE; };
	LSMCursor_close(self);
	Py_RETURN_NONE;
}


static PyObject* LSMCursor_repr(LSMCursor *self) {
	return PyUnicode_FromFormat(
		"<%s as %p>",
		Py_TYPE(self)->tp_name, self
	);
}


static PyMemberDef LSMCursor_members[] = {
	{
		"state",
		T_INT,
		offsetof(LSMCursor, state),
		READONLY,
		"state"
	},
	{
		"seek_mode",
		T_INT,
		offsetof(LSMCursor, seek_mode),
		READONLY,
		"seek_mode"
	},
	{NULL}  /* Sentinel */
};


static PyMethodDef LSMCursor_methods[] = {
	{
		"__enter__",
		(PyCFunction) LSMCursor_ctx_enter, METH_NOARGS,
		"Enter context"
	},
	{
		"__exit__",
		(PyCFunction) LSMCursor_ctx_exit, METH_VARARGS | METH_KEYWORDS,
		"Exit context"
	},
	{
		"close",
		(PyCFunction) LSMCursor_close, METH_NOARGS,
		"Close database"
	},
	{
		"first",
		(PyCFunction) LSMCursor_first, METH_NOARGS,
		"Move cursor to first item"
	},
	{
		"last",
		(PyCFunction) LSMCursor_last, METH_NOARGS,
		"Move cursor to last item"
	},
	{
		"seek",
		(PyCFunction) LSMCursor_seek, METH_VARARGS | METH_KEYWORDS,
		"Seek to key"
	},
	{
		"retrieve",
		(PyCFunction) LSMCursor_retrieve, METH_NOARGS,
		"Retrieve key and value"
	},
	{
		"next",
		(PyCFunction) LSMCursor_next, METH_NOARGS,
		"Seek next"
	},
	{
		"prev",
		(PyCFunction) LSMCursor_prev, METH_NOARGS,
		"Seek prev"
	},
	{
		"compare",
		(PyCFunction) LSMCursor_compare, METH_VARARGS | METH_KEYWORDS,
		"Compare current position against key"
	},

	{NULL}  /* Sentinel */
};

static PyTypeObject LSMCursorType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "Cursor",
	.tp_doc = "",
	.tp_basicsize = sizeof(LSMCursor),
	.tp_itemsize = 0,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_dealloc = (destructor) LSMCursor_dealloc,
	.tp_members = LSMCursor_members,
	.tp_methods = LSMCursor_methods,
	.tp_repr = (reprfunc) LSMCursor_repr
};


static PyModuleDef lsm_module = {
	PyModuleDef_HEAD_INIT,
	.m_name = "lsm",
	.m_doc = "LSM DB python binding",
	.m_size = -1,
};


PyMODINIT_FUNC PyInit_lsm(void) {
	PyObject *m;

	m = PyModule_Create(&lsm_module);

	if (m == NULL) return NULL;

	if (PyType_Ready(&LSMType) < 0) return NULL;
	Py_INCREF(&LSMType);

	if (PyModule_AddObject(m, "LSM", (PyObject *) &LSMType) < 0) {
		Py_XDECREF(&LSMType);
		Py_XDECREF(m);
		return NULL;
	}

	if (PyType_Ready(&LSMCursorType) < 0) return NULL;
	Py_INCREF(&LSMCursorType);

	if (PyModule_AddObject(m, "Cursor", (PyObject *) &LSMCursorType) < 0) {
		Py_XDECREF(&LSMCursorType);
		Py_XDECREF(m);
		return NULL;
	}

	if (PyType_Ready(&LSMItemsType) < 0) return NULL;
	Py_INCREF(&LSMItemsType);

	if (PyType_Ready(&LSMValuesType) < 0) return NULL;
	Py_INCREF(&LSMValuesType);

	if (PyType_Ready(&LSMKeysType) < 0) return NULL;
	Py_INCREF(&LSMKeysType);

	if (PyType_Ready(&LSMSliceType) < 0) return NULL;
	Py_INCREF(&LSMSliceType);

	PyModule_AddIntConstant(m, "SAFETY_OFF", LSM_SAFETY_OFF);
	PyModule_AddIntConstant(m, "SAFETY_NORMAL", LSM_SAFETY_NORMAL);
	PyModule_AddIntConstant(m, "SAFETY_FULL", LSM_SAFETY_FULL);

	PyModule_AddIntConstant(m, "STATE_INITIALIZED", PY_LSM_INITIALIZED);
	PyModule_AddIntConstant(m, "STATE_OPENED", PY_LSM_OPENED);
	PyModule_AddIntConstant(m, "STATE_CLOSED", PY_LSM_CLOSED);

	PyModule_AddIntConstant(m, "SEEK_EQ", LSM_SEEK_EQ);
	PyModule_AddIntConstant(m, "SEEK_LE", LSM_SEEK_LE);
	PyModule_AddIntConstant(m, "SEEK_GE", LSM_SEEK_GE);
	PyModule_AddIntConstant(m, "SEEK_LEFAST", LSM_SEEK_LEFAST);

	return m;
}
