#include <jni.h>
#include <string>
#include "jvmti.h"
#include <android/log.h>
#include <slicer/dex_ir_builder.h>
#include <slicer/code_ir.h>
#include <slicer/reader.h>
#include <slicer/writer.h>
#include <sstream>

#define LOG_TAG "jvmti"
using namespace dex;
using namespace lir;

#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
static jvmtiEnv *localJvmtiEnv;


// Add a label before instructionAfter
static void
addLabel(CodeIr &c,
         lir::Instruction *instructionAfter,
         Label *returnTrueLabel) {
    c.instructions.InsertBefore(instructionAfter, returnTrueLabel);
}

// Add a byte code before instructionAfter
static void
addInstr(CodeIr &c,
         lir::Instruction *instructionAfter,
         Opcode opcode,
         const std::list<Operand *> &operands) {
    auto instruction = c.Alloc<Bytecode>();

    instruction->opcode = opcode;

    for (auto it = operands.begin(); it != operands.end(); it++) {
        instruction->operands.push_back(*it);
    }

    c.instructions.InsertBefore(instructionAfter, instruction);
}


static void
addCall(ir::Builder &b,
        CodeIr &c,
        lir::Instruction *instructionAfter,
        Opcode opcode,
        ir::Type *type,
        const char *methodName,
        ir::Type *returnType,
        const std::vector<ir::Type *> &types,
        const std::list<int> &regs) {
    auto proto = b.GetProto(returnType, b.GetTypeList(types));
    auto method = b.GetMethodDecl(b.GetAsciiString(methodName), proto, type);

    VRegList *param_regs = c.Alloc<VRegList>();
    for (auto it = regs.begin(); it != regs.end(); it++) {
        param_regs->registers.push_back(*it);
    }

    addInstr(c, instructionAfter, opcode, {param_regs, c.Alloc<Method>(method,
                                                                       method->orig_index)});
}

static std::string
ClassNameToDescriptor(const char *class_name) {
    std::stringstream ss;
    ss << "L";
    for (auto p = class_name; *p != '\0'; ++p) {
        ss << (*p == '.' ? '/' : *p);
    }
    ss << ";";
    return ss.str();
}

static size_t
getNumParams(ir::EncodedMethod *method) {
    if (method->decl->prototype->param_types == NULL) {
        return 0;
    }

    return method->decl->prototype->param_types->types.size();
}

static void
ClassTransform(jvmtiEnv *jvmti_env,
               JNIEnv *env,
               jclass classBeingRedefined,
               jobject loader,
               const char *name,
               jobject protectionDomain,
               jint classDataLen,
               const unsigned char *classData,
               jint *newClassDataLen,
               unsigned char **newClassData) {

    if (!strcmp(name, "android/app/Activity")) {
        if (loader == nullptr) {
            ALOGI("==========bootclassloader=============");
        }
        ALOGI("==========ClassTransform %s=======", name);

        struct Allocator : public Writer::Allocator {
            virtual void *Allocate(size_t size) { return ::malloc(size); }

            virtual void Free(void *ptr) { ::free(ptr); }
        };

        Reader reader(classData, classDataLen);
        reader.CreateClassIr(0);
        std::shared_ptr<ir::DexFile> dex_ir = reader.GetIr();

        ir::Builder b(dex_ir);

        for (auto &method : dex_ir->encoded_methods) {
            std::string type = method->decl->parent->Decl();
            ir::String *methodName = method->decl->name;
            std::string prototype = method->decl->prototype->Signature();

            ALOGI("==========ClassTransform method %s=======", methodName->c_str());

            if (!strcmp("onCreate", methodName->c_str()) &&
                !strcmp(prototype.c_str(), "(Landroid/os/Bundle;)V")) {
                ALOGI("==========Method modify %s %s=======", methodName->c_str(),
                      prototype.c_str());

                CodeIr c(method.get(), dex_ir);
                int originalNumRegisters = method->code->registers -
                                           method->code->ins_count;//v=寄存器p+v数量减去入参p,此例子中v=3 p=2
                int numAdditionalRegs = std::max(0, 1 - originalNumRegisters);
                int thisReg = numAdditionalRegs + method->code->registers
                              - method->code->ins_count;
                ALOGI("origin:%d  addreg:%d", originalNumRegisters, numAdditionalRegs);
                if (numAdditionalRegs > 0) {
                    c.ir_method->code->registers += numAdditionalRegs;
                }

                ir::Type *stringT = b.GetType("Ljava/lang/String;");
                ir::Type *activityT = b.GetType("Landroid/app/Activity;");
                ir::Type *jvmtiHelperT = b.GetType("Lcom/dodola/jvmtilib/JVMTIHelper;");
                ir::Type *voidT = b.GetType("V");
                std::stringstream ss;
                ss << method->decl->parent->Decl() << "#" << method->decl->name->c_str() << "(";
                bool first = true;
                if (method->decl->prototype->param_types != NULL) {
                    for (const auto &type : method->decl->prototype->param_types->types) {
                        if (first) {
                            first = false;
                        } else {
                            ss << ",";
                        }

                        ss << type->Decl().c_str();
                    }
                }
                ss << ")";
                std::string methodDescStr = ss.str();
                ir::String *methodDesc = b.GetAsciiString(methodDescStr.c_str());

                //该例子中不影响原寄存器数量
                lir::Instruction *fi = *(c.instructions.begin());
                VReg *v0 = c.Alloc<VReg>(0);
                VReg *v1 = c.Alloc<VReg>(1);
                VReg *v2 = c.Alloc<VReg>(2);
                VReg *thiz = c.Alloc<VReg>(thisReg);

                addInstr(c, fi, OP_MOVE_OBJECT, {v0, thiz});
                addInstr(c, fi, OP_CONST_STRING,
                         {v1, c.Alloc<String>(methodDesc, methodDesc->orig_index)});//对于插入到前面的指令来说
                addCall(b, c, fi, OP_INVOKE_STATIC, jvmtiHelperT, "printEnter", voidT,
                        {activityT, stringT},
                        {0, 1});
                c.Assemble();

                Allocator allocator;
                Writer writer2(dex_ir);
                jbyte *transformed(
                        (jbyte *) writer2.CreateImage(&allocator,
                                                      reinterpret_cast<size_t *>(newClassDataLen)));

                jvmti_env->Allocate(*newClassDataLen, newClassData);
                std::memcpy(*newClassData, transformed, *newClassDataLen);
                break;
            }
        }

    }
}

void SetAllCapabilities(jvmtiEnv *jvmti) {
    jvmtiCapabilities caps;
    jvmtiError error;
    error = jvmti->GetPotentialCapabilities(&caps);
    error = jvmti->AddCapabilities(&caps);
}

jvmtiEnv *CreateJvmtiEnv(JavaVM *vm) {
    jvmtiEnv *jvmti_env;
    jint result = vm->GetEnv((void **) &jvmti_env, JVMTI_VERSION_1_2);
    if (result != JNI_OK) {
        return nullptr;
    }

    return jvmti_env;

}

void ObjectAllocCallback(jvmtiEnv *jvmti, JNIEnv *jni,
                         jthread thread, jobject object,
                         jclass klass, jlong size) {
    jclass cls = jni->FindClass("java/lang/Class");
    jmethodID mid_getName = jni->GetMethodID(cls, "getName", "()Ljava/lang/String;");
    jstring name = static_cast<jstring>(jni->CallObjectMethod(klass, mid_getName));
    const char* className=jni->GetStringUTFChars(name,JNI_FALSE);
    ALOGI("==========alloc callback======= %s {size:%d}",className,size);
    jni->ReleaseStringUTFChars(name,className);
}

void GCStartCallback(jvmtiEnv *jvmti) {
    ALOGI("==========触发 GCStart=======");

}

void GCFinishCallback(jvmtiEnv *jvmti) {
    ALOGI("==========触发 GCFinish=======");

}

void SetEventNotification(jvmtiEnv *jvmti, jvmtiEventMode mode,
                          jvmtiEvent event_type) {
    jvmtiError err = jvmti->SetEventNotificationMode(mode, event_type, nullptr);
}

extern "C" JNIEXPORT void JNICALL retransformClasses(JNIEnv *env,
                                                     jclass clazz,
                                                     jobjectArray classes) {
    jsize numTransformedClasses = env->GetArrayLength(classes);
    jclass *transformedClasses = (jclass *) malloc(numTransformedClasses * sizeof(jclass));

    for (int i = 0; i < numTransformedClasses; i++) {
        transformedClasses[i] = (jclass) env->NewGlobalRef(env->GetObjectArrayElement(classes, i));
    }
    ALOGI("==============retransformClasses ===============");

    jvmtiError error = localJvmtiEnv->RetransformClasses(numTransformedClasses,
                                                         transformedClasses);

    for (int i = 0; i < numTransformedClasses; i++) {
        env->DeleteGlobalRef(transformedClasses[i]);
    }
    free(transformedClasses);
}

void JNICALL
JvmTINativeMethodBind(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jthread thread, jmethodID method,
                      void *address, void **new_address_ptr) {
    ALOGI("===========NativeMethodBind===============");

    jclass clazz = jni_env->FindClass("com/dodola/jvmtilib/JVMTIHelper");
    jmethodID methodid = jni_env->GetStaticMethodID(clazz, "retransformClasses",
                                                    "([Ljava/lang/Class;)V");
    if (methodid == method) {
        *new_address_ptr = reinterpret_cast<void *>(&retransformClasses);
    }
    //绑定 package code 到BootClassLoader 里
    jfieldID packageCodePathId = jni_env->GetStaticFieldID(clazz, "packageCodePath",
                                                           "Ljava/lang/String;");
    jstring packageCodePath = static_cast<jstring>(jni_env->GetStaticObjectField(clazz,
                                                                                 packageCodePathId));
    const char *pathChar = jni_env->GetStringUTFChars(packageCodePath, JNI_FALSE);
    ALOGI("===========add to boot classloader %s===============", pathChar);
    jvmti_env->AddToBootstrapClassLoaderSearch(pathChar);
    jni_env->ReleaseStringUTFChars(packageCodePath,pathChar);

}

extern "C" JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char *options,
                                                 void *reserved) {
    jvmtiEnv *jvmti_env = CreateJvmtiEnv(vm);

    if (jvmti_env == nullptr) {
        return JNI_ERR;
    }
    localJvmtiEnv = jvmti_env;
    SetAllCapabilities(jvmti_env);

    jvmtiEventCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.ClassFileLoadHook = &ClassTransform;

    callbacks.VMObjectAlloc = &ObjectAllocCallback;
    callbacks.NativeMethodBind = &JvmTINativeMethodBind;

    callbacks.GarbageCollectionStart = &GCStartCallback;
    callbacks.GarbageCollectionFinish = &GCFinishCallback;
    int error = jvmti_env->SetEventCallbacks(&callbacks, sizeof(callbacks));

    SetEventNotification(jvmti_env, JVMTI_ENABLE,
                         JVMTI_EVENT_GARBAGE_COLLECTION_START);
    SetEventNotification(jvmti_env, JVMTI_ENABLE,
                         JVMTI_EVENT_GARBAGE_COLLECTION_FINISH);
    SetEventNotification(jvmti_env, JVMTI_ENABLE,
                         JVMTI_EVENT_NATIVE_METHOD_BIND);
    SetEventNotification(jvmti_env, JVMTI_ENABLE,
                         JVMTI_EVENT_VM_OBJECT_ALLOC);
    SetEventNotification(jvmti_env, JVMTI_ENABLE,
                         JVMTI_EVENT_OBJECT_FREE);
    SetEventNotification(jvmti_env, JVMTI_ENABLE,
                         JVMTI_EVENT_CLASS_FILE_LOAD_HOOK);
    ALOGI("==========Agent_OnAttach=======");
    return JNI_OK;

}

extern "C" JNIEXPORT void JNICALL tempRetransformClasses(JNIEnv *env,
                                                         jclass clazz,
                                                         jobjectArray classes) {
}

static JNINativeMethod methods[] = {
        {"retransformClasses", "([Ljava/lang/Class;)V", reinterpret_cast<void *>(tempRetransformClasses)}
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    ALOGI("==============library load====================");
    jclass clazz = env->FindClass("com/dodola/jvmtilib/JVMTIHelper");
    env->RegisterNatives(clazz, methods, 1);
    return JNI_VERSION_1_6;
}