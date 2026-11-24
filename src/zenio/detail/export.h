#ifndef ZENIO_DETAIL_EXPORT_H
#define ZENIO_DETAIL_EXPORT_H

// 公开的类和函数标注这个宏。
// 库按 -fvisibility=hidden 编译, 不标注的符号在库外不可见。
// v1 只支持 Linux 和 macOS, 两边都是同一个写法。
#define ZENIO_EXPORT __attribute__((visibility("default")))

#endif    // ZENIO_DETAIL_EXPORT_H
