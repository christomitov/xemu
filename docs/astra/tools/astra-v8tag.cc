#include <node.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>
static void Tag(const v8::FunctionCallbackInfo<v8::Value>& a) {
    char s[160];
    snprintf(s, sizeof(s), "{\"pid\":%d,\"tid\":%ld,\"isolate\":\"%p\"}",
             getpid(), syscall(SYS_gettid), static_cast<void*>(a.GetIsolate()));
    a.GetReturnValue().Set(v8::String::NewFromUtf8(a.GetIsolate(), s).ToLocalChecked());
}
NODE_MODULE_INIT() { NODE_SET_METHOD(exports, "tag", Tag); }
