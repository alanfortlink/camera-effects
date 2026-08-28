// Resolve onnxruntime at runtime instead of linking it.
//
// The daemon uses onnxruntime only through its header-only C++ wrapper, whose
// single entry point into the library is OrtGetApiBase(). Linking that symbol
// records the version node of the release it was built against, and onnxruntime
// renames that node on every release (VERS_1.28.0 -> VERS_1.29.0, keeping no
// old node), so any onnxruntime update left an installed daemon that the loader
// refused to start. dlsym has no such notion of versions — it takes the default
// definition — and the C API struct it hands back carries its own version field,
// which is how onnxruntime supports older callers. So the binary now survives an
// onnxruntime upgrade without a rebuild.
#include <onnxruntime_c_api.h>

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>

extern "C" const OrtApiBase* ORT_API_CALL OrtGetApiBase(void) NO_EXCEPTION {
  static const OrtApiBase* base = [] {
    // RTLD_GLOBAL to match what linking would have done: onnxruntime dlopens its
    // own provider libraries and expects its symbols to be visible to them.
    void* h = dlopen("libonnxruntime.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libonnxruntime.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
      fprintf(stderr, "cannot load libonnxruntime.so.1 (%s) — install the onnxruntime package\n", dlerror());
      exit(127);   // the same code the loader used to give: the shell plugin knows it
    }
    auto fn = reinterpret_cast<const OrtApiBase* (*)(void)>(dlsym(h, "OrtGetApiBase"));
    if (!fn) {
      fprintf(stderr, "libonnxruntime.so.1 has no OrtGetApiBase — unusable onnxruntime build\n");
      exit(127);
    }
    return fn();
  }();
  return base;
}
