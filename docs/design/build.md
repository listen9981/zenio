# 构建、符号导出与测试

状态: 草稿
细化程度: 全部已细化 (M1、M2)
依据: S22、S23; A12、A14; D2

## 1. 职责与边界

本文件规定 CMake 构建的约定: 目标、选项、编译标志、公开头文件的导出, 以及怎么配置、构建、跑测试。

编码风格 (命名、注释、include 顺序、格式) 见 `docs/coding-style.md` 和仓库根目录的 `.clang-format`, 本文件不重复。

不在本文件: 各个源文件实现什么, 见各子系统文档。

## 2. 对外接口

构建系统没有 C++ 接口。这一节列出使用者 (包括测试) 能看到的东西。

### 2.1 CMake 选项

| 选项 | 默认 | 作用 |
|------|------|------|
| `ZENIO_BUILD_TESTS` | OFF | 构建 `zenio_unittest` 和 API 编译检查 |
| `ZENIO_BUILD_EXAMPLES` | OFF | 构建示例 |
| `ZENIO_BUILD_BENCHMARKS` | OFF | 构建基准 |
| `ZENIO_ENABLE_ASAN` | OFF | 开启 AddressSanitizer |
| `ZENIO_ENABLE_TSAN` | OFF | 开启 ThreadSanitizer |
| `ZENIO_BUILD_SHARED_LIBS` | ON | 构建共享库; 关闭则构建静态库 |
| `ZENIO_INSTALL` | OFF | 生成安装规则 |

两个 sanitizer 选项互斥, 同时开启时 ASAN 优先。

### 2.2 目标

| 目标 | 类型 | 说明 |
|------|------|------|
| `zenio_obj` | OBJECT 库 | 库的全部源文件编译出的目标文件, 只编译一次, 库和单测都用它 |
| `zenio` | 库 | 由 `zenio_obj` 的目标文件链接而成, 共享还是静态由选项决定。别名 `zenio::zenio` |
| `zenio_unittest` | 可执行文件 | 单元测试, 直接链接 `zenio_obj` 的目标文件和 `GTest::gtest_main`, 不经过 `zenio` |
| `zenio_api_compile_check` | OBJECT 库 | 只编译不链接的 API 编译检查 |

单测链接目标文件而不是链接 `zenio` (S28): 测试因此可以直接使用 `zenio::detail` 里的内部实体, 而这些实体不必导出。

### 2.3 `ZENIO_EXPORT` 宏

定义在 `zenio/detail/export.h`:

```cpp
#ifndef ZENIO_DETAIL_EXPORT_H
#define ZENIO_DETAIL_EXPORT_H

// 公开的类和函数标注这个宏。
// 库按 -fvisibility=hidden 编译, 不标注的符号在库外不可见。
// v1 只支持 Linux 和 macOS, 两边都是同一个写法。
#define ZENIO_EXPORT __attribute__((visibility("default")))

#endif  // ZENIO_DETAIL_EXPORT_H
```

哪些东西要标 (S23):

- 公开的非模板类: 标在类上, 如 `class ZENIO_EXPORT TcpListener`。成员函数不用再标。
- 公开的非模板函数: 标在声明上。
- 公开的类模板和函数模板: 也标上。它们的实例化发生在使用者的代码里, 标注是为了规则统一。
- `detail` 命名空间里的东西一律不标 (S28)。单元测试能用内部符号, 靠的是直接链接目标文件 (见 2.2), 不是导出。

### 2.4 公开头文件清单

使用者只包含 `<zenio/zenio.h>` (S22)。它包含下面全部头文件:

```
zenio/version.h        构建时生成
zenio/error.h
zenio/result.h
zenio/task.h
zenio/try.h
zenio/cancel.h
zenio/spawn.h
zenio/join_set.h
zenio/select.h
zenio/time.h
zenio/runtime.h
zenio/signal.h
zenio/net/address.h
zenio/net/tcp.h
zenio/net/udp.h
```

公开头文件依赖的内部头, 随公开头一起导出:

```
zenio/detail/export.h
zenio/detail/context.h
zenio/detail/awaiters.h
zenio/detail/fd.h
zenio/detail/io_object.h
```

其余 `zenio/detail/` 下的头文件不导出。

## 3. 内部结构

### 3.1 文件布局

```
CMakeLists.txt                     根: 选项、全局编译标志、sanitizer
src/CMakeLists.txt                 只有 add_subdirectory(zenio)
src/zenio/CMakeLists.txt           zenio_obj 和库目标; 安装时用 FILE_SET 导出 2.4 的头文件
tests/CMakeLists.txt               找 GTest, 加入 zenio 和 compile_check 两个子目录
tests/zenio/CMakeLists.txt         zenio_unittest, GLOB_RECURSE 收集 *.cpp
tests/zenio/*_unittest.cpp         单元测试
tests/compile_check/CMakeLists.txt zenio_api_compile_check
tests/compile_check/api_compile_check.cpp   内容是 design.md 1.8 的目标用法
```

### 3.2 需要改动的 CMake 内容

**根 `CMakeLists.txt`** (M2): 在选项定义之后加全局编译标志, 只对 GCC 和 Clang 生效:

```cmake
if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -fvisibility=hidden)
endif ()
```

**`src/zenio/CMakeLists.txt`**:

- 源文件继续用 `GLOB_RECURSE` 收集。
- 先建 OBJECT 库 `zenio_obj`, 收全部源文件和头文件, 再用它的目标文件建库目标 `zenio` (S28):

  ```cmake
  add_library(zenio_obj OBJECT ${zenio_srcs} ${zenio_hdrs})
  add_library(zenio ${ZENIO_LIB_TYPE} $<TARGET_OBJECTS:zenio_obj>)
  add_library(zenio::zenio ALIAS zenio)
  ```

  编译选项和 `target_include_directories` 都挂在 `zenio_obj` 上。根 `CMakeLists.txt` 已经全局开了 `CMAKE_POSITION_INDEPENDENT_CODE`, 目标文件可以直接进共享库。`VERSION`、`SOVERSION` 这些属性照旧挂在 `zenio` 上。
- 安装块里的 `FILE_SET` 列表换成 2.4 的清单。`version.h` 在构建目录里, 所以 `BASE_DIRS` 除了 `${CMAKE_SOURCE_DIR}/src` 还要加上 `${CMAKE_BINARY_DIR}/src`。

**`tests/CMakeLists.txt`**:

```cmake
find_package(GTest REQUIRED)
include(GoogleTest)

add_subdirectory(zenio)
add_subdirectory(compile_check)
```

`include(GoogleTest)` 是为了下面的 `gtest_discover_tests`。

**`tests/zenio/CMakeLists.txt`**: 源文件照旧用 `GLOB_RECURSE` 收集。链接目标从 `zenio::zenio` 换成 `zenio_obj`; 用 `gtest_discover_tests` 把每个用例注册成 ctest 里的一项:

```cmake
target_link_libraries(zenio_unittest PRIVATE GTest::gtest_main zenio_obj)

gtest_discover_tests(zenio_unittest
        DISCOVERY_MODE POST_BUILD
        PROPERTIES TIMEOUT 60
)
```

不要给测试设置 `ASAN_OPTIONS` 之类的 sanitizer 环境变量 (S29)。

**`tests/compile_check/CMakeLists.txt`** (M1 新建):

```cmake
add_library(zenio_api_compile_check OBJECT api_compile_check.cpp)

target_include_directories(zenio_api_compile_check PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/src
)
```

OBJECT 库不产生链接步骤, 所以头文件只有声明时它也能构建成功。

## 4. 关键流程

都在仓库根目录执行。

**日常构建和测试** (M2 起):

```
cmake -S . -B cmake-build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DZENIO_BUILD_TESTS=ON
cmake --build cmake-build-debug
ctest --test-dir cmake-build-debug --output-on-failure
```

**ThreadSanitizer** (M6 起, 涉及并发的里程碑必须跑):

```
cmake -S . -B cmake-build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DZENIO_BUILD_TESTS=ON -DZENIO_ENABLE_TSAN=ON
cmake --build cmake-build-tsan
ctest --test-dir cmake-build-tsan --output-on-failure
```

**AddressSanitizer** (M4 起, 涉及对象生命周期的里程碑必须跑): 同上, 把 `ZENIO_ENABLE_TSAN` 换成 `ZENIO_ENABLE_ASAN`, 目录名换成 `cmake-build-asan`。

**检查安装配置** (M2): 只配置, 不执行安装。

```
cmake -S . -B cmake-build-install -G Ninja -DZENIO_INSTALL=ON
```

## 5. 不变量

- 只包含 `<zenio/zenio.h>` 就能使用全部公开 API。
- 公开头文件不包含任何平台头, 包括 `<sys/...>`、`<netinet/...>`、`<arpa/...>`。
- `zenio_api_compile_check` 不在 `tests/zenio/` 目录下 (那里的 `*.cpp` 会被收进单元测试)。
- sanitizer 构建使用系统自带的 Apple Clang, 即命令行里的 `/usr/bin/c++`。

## 6. 已知的坑

- **`tmp.cpp` 占位文件**: 库目标至少要有一个源文件。M1 阶段库只有头文件, `src/zenio/tmp.cpp` 必须留着; M2 有了真正的源文件后再删掉它和 `tests/zenio/tmp.cpp`。
- **`GLOB_RECURSE` 的范围**: `tests/zenio/` 下的每个 `*.cpp` 都会进 `zenio_unittest`。编译检查的源文件放在那个目录下会因为缺符号而链接失败。
- **安装前缀**: 根 `CMakeLists.txt` 在没有指定前缀时把安装前缀设成仓库自身, 所以执行 `cmake --install` 会把 `include/` 和 `lib/` 写进仓库。任务书只要求配置成功, 不要求执行安装。
- **CLion 的构建目录**: `cmake-build-debug` 是 CLion 管理的。从命令行在同一个目录里改配置选项, 可能和 CLion 的配置冲突。命令行验证时用自己的目录名, 比如上面命令里的那些。
- **sanitizer 运行库**: 如果用 Homebrew 的 LLVM 做 sanitizer 构建, 运行时可能找不到运行库。改用系统自带的 Apple Clang。
- **`-Wall -Wextra` 下的第三方头**: `GTest::gtest_main` 的头文件目录是系统目录, 不会报警告。
- **ASAN 报 container-overflow 时先确认构建目录是干净的** (S29): 带和不带 sanitizer 的产物混在同一个目录里, 或者换了编译器没有重新配置, 都可能出看似误报的报告。M2 审查时在全新的 ASAN 构建目录里用默认选项跑全量单测, 没有任何报告。确认能复现之后, 记录复现步骤并登记到债务, 不要直接降低检测项。
- **单测里的内部符号来自目标文件**: `zenio_unittest` 链接的是 `zenio_obj`, 和共享库 `zenio` 是两份代码。单测通过不代表共享库导出了这些符号; 共享库的导出情况要用 `nm` 单独看 (S28)。
- **平台头的分组还没有工具保证**: `coding-style.md` 规定平台头单独一组 (S30), 但 `.clang-format` 没有对应的分类, clang-format 会把平台头并回标准库那一组。配置补上之前, 代码里保持 clang-format 的排序。

## 7. 验收测试

### M1

- 用上面的日常构建命令配置并构建成功, 包括 `zenio`、`zenio_unittest`、`zenio_api_compile_check` 三个目标。
- `api_compile_check.cpp` 只包含 `<zenio/zenio.h>` (以及 `<cstdio>`) 就能编译。

### M2

- `ctest` 输出里 `zenio_unittest` 通过, 包含 design/error.md 和 design/net.md 列出的全部单测。
- `zenio_api_compile_check` 仍然构建成功。
- `ZENIO_INSTALL=ON` 配置成功。
- 共享库不导出任何 `zenio::detail` 的符号: `nm -gU <库文件> | grep 'zenio::detail'` 没有输出 (S28)。
- 在全新的目录里开 `ZENIO_ENABLE_ASAN` 构建, 不设任何 `ASAN_OPTIONS`, 直接运行 `zenio_unittest`, 全部通过且没有 sanitizer 报告 (S29)。
- D2 清偿: `cmake/version.h.in` 的注释改成只描述本文件。
