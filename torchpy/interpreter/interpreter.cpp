#include <dlfcn.h>

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <iostream>
#include "interpreter_impl.h"

#include <assert.h>
#include <pybind11/embed.h>
#include <stdio.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/csrc/jit/python/pybind_utils.h>
#include <iostream>
#include <map>
#include <thread>

namespace py = pybind11;

// TODO this should come from cmake
#define DEBUG 1

#if (DEBUG == 1)
#define PYOBJ_ASSERT(obj) \
  if (NULL == obj) {      \
    PyErr_Print();        \
  }                       \
  assert(NULL != obj);
#elif (DEBUG == 0)
#define PYOBJ_ASSERT(obj) assert(NULL != obj);
#endif

// This is generated by cmake with the path to the compiled cpython
#include "python_path.h"

static wchar_t* program;

#define FOREACH_LIBRARY(_) \
  _(array)                 \
  _(_asyncio)              \
  _(audioop)               \
  _(binascii)              \
  _(_bisect)               \
  _(_blake2)               \
  _(_bz2)                  \
  _(cmath)                 \
  _(_codecs_cn)            \
  _(_codecs_hk)            \
  _(_codecs_iso2022)       \
  _(_codecs_jp)            \
  _(_codecs_kr)            \
  _(_codecs_tw)            \
  _(_contextvars)          \
  _(_crypt)                \
  _(_csv)                  \
  _(_ctypes)               \
  _(_ctypes_test)          \
  _(_curses)               \
  _(_curses_panel)         \
  _(_datetime)             \
  _(_decimal)              \
  _(_elementtree)          \
  _(fcntl)                 \
  _(grp)                   \
  _(_hashlib)              \
  _(_heapq)                \
  _(_json)                 \
  _(_lsprof)               \
  _(_lzma)                 \
  _(math)                  \
  _(_md5)                  \
  _(mmap)                  \
  _(_multibytecodec)       \
  _(_multiprocessing)      \
  _(nis)                   \
  _(_opcode)               \
  _(ossaudiodev)           \
  _(parser)                \
  _(_pickle)               \
  _(_posixsubprocess)      \
  _(pyexpat)               \
  _(_queue)                \
  _(_random)               \
  _(readline)              \
  _(resource)              \
  _(select)                \
  _(_sha1)                 \
  _(_sha256)               \
  _(_sha3)                 \
  _(_sha512)               \
  _(_socket)               \
  _(spwd)                  \
  _(_ssl)                  \
  _(_struct)               \
  _(syslog)                \
  _(termios)               \
  _(_testbuffer)           \
  _(_testcapi)             \
  _(_testimportmultiple)   \
  _(_testmultiphase)       \
  _(unicodedata)           \
  _(xxlimited)             \
  _(_xxtestfuzz)           \
  _(zlib)

#define DECLARE_LIBRARY_INIT(name) extern "C" PyObject* PyInit_##name(void);
FOREACH_LIBRARY(DECLARE_LIBRARY_INIT)
#undef DECLARE_LIBRARY_INIT

const char* finder = R"RAW(
import sys
class F:
    def find_spec(self, fullname, path, target=None):
        if fullname == 'torch._C':
            return sys.meta_path[1].find_spec('torch._C', None, None)
        return None
sys.meta_path.insert(0, F())

# make loader importable
sys.path.append('torchpy')
)RAW";

extern "C" PyObject* initModule(void);
/**
 * These functions each run inside their own copy of an interpreter, so
 * acquiring the gil here is the same as locking just that interpreter.
 *
 */
// PyThreadState* mainThreadState = NULL;
static std::atomic<size_t> s_id;
std::map<size_t, py::object> forwards;

__attribute__((constructor)) void init() {
  // some dependency in mkl requires this...
  void* result = dlopen("libz.so", RTLD_GLOBAL | RTLD_LAZY);
  assert(result);

  program = Py_DecodeLocale("main", NULL);
  if (program == NULL) {
    fprintf(stderr, "Fatal error: cannot decode argv[0]\n");
    exit(1);
  }
  Py_SetProgramName(program);
#define APPEND_INIT(name) PyImport_AppendInittab(#name, PyInit_##name);
  FOREACH_LIBRARY(APPEND_INIT)
#undef APPEND_INIT
  PyImport_AppendInittab("torch._C", initModule);
  Py_Initialize();
  PyRun_SimpleString(PY_PATH_STRING);
  PyRun_SimpleString(finder);
  PyEval_ReleaseThread(PyThreadState_Get());
}
static void teardown() {
  PyGILState_Ensure();

  forwards.clear();
  if (Py_FinalizeEx() < 0) {
    std::cout << "IT BROKE SO WE ARE EXITING\n";
    exit(120);
  }
  PyMem_RawFree(program);
}

__attribute__((destructor)) void deinit() {}

static void run_some_python(const char* code) {
  PyGILState_STATE gstate = PyGILState_Ensure();

  if (PyRun_SimpleString(code) == -1) {
    throw std::runtime_error("python eval failed\n");
  }

  PyGILState_Release(gstate);
}

static void run_python_file(const char* code) {
  PyGILState_STATE gstate = PyGILState_Ensure();

  FILE* f = fopen(code, "r");
  if (PyRun_SimpleFile(f, code) == -1) {
    throw std::runtime_error("python eval failed\n");
  }
  fclose(f);

  PyGILState_Release(gstate);
}

extern "C" void initialize_interface(InterpreterImpl* s) {
#define INITIALIZE_MEMBER(func) s->func = func;
  FOREACH_INTERFACE_FUNCTION(INITIALIZE_MEMBER)
#undef INITIALIZE_MEMBER
}


static size_t load_model(const char* filename) {
  PyGILState_STATE gstate = PyGILState_Ensure();
  // PyEval_RestoreThread(mainThreadState); // Acquire GIL, resume our state
  // assert(PyGILState_Check() == 1);

  auto loader = py::module::import("loader");
  auto load = loader.attr("load");

  auto py_model = load(filename);
  auto forward = py_model.attr("forward");
  auto id = ++s_id;
  forwards[id] = forward;

  // mainThreadState = PyEval_SaveThread(); // save our state, release GIL
  PyGILState_Release(gstate);
  return id;
}

static at::Tensor forward_model(size_t model_id, at::Tensor input) {
  at::Tensor output;
  PyGILState_STATE gil_state = PyGILState_Ensure();
  {
    auto forward = forwards[model_id];
    py::object py_output = forward(input);
    // TODO is this going to leak?
    // added it to prevent crash wehn using 'output' tensor in callee of
    // forward()
    py_output.inc_ref();
    output = py::cast<at::Tensor>(py_output);
  }
  PyGILState_Release(gil_state);

  return output;
}