# JVMTI_Sample

该例子主要展示了JVM TI支持的几种功能：

1. Memory Alloc Tracker
2. GC Event Tracker
3. JNI Method Rebind
4. Class Retransform

注意：例子最好在9.0上测试，支持模拟器，8.0下修改 Class 功能无效

运行界面
======

![](Screenshot.png)

产生的日志可以在 Logcat 中查看

```
I/jvmti: ==========alloc callback======= [I {size:32}
I/jvmti: ==========alloc callback======= java.lang.ref.WeakReference {size:24}
I/jvmti: ==========alloc callback======= java.lang.ref.WeakReference {size:24}
I/jvmti: ==========alloc callback======= [Ljava.lang.Object; {size:16}
I/jvmti: ==========触发 GCStart=======
I/jvmti: ==========触发 GCFinish=======
```