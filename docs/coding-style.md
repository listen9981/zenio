# ZENIO 编码风格规范

本项目统一遵循以下命名与格式约定。

## 命名

| 对象          | 风格                                                                                                         | 示例                                 |
|---------------|--------------------------------------------------------------------------------------------------------------|--------------------------------------|
| 文件          | `snake_case.h` / `snake_case.cpp`                                                                            | `string.h` / `vector_unittest.cpp`   |
| 类(类型)      | `PascalCase`                                                                                                 | `ScopedString`                       |
| 成员变量      | `snake_case_`(下划线后缀, public 成员变量不带下划线后缀)                                                     | `data_ptr_` (`data_ptr`)             |
| 全局变量      | `g_snake_case`                                                                                               | `g_screen_size`                      |
| 静态变量      | `s_snake_case`                                                                                               | `s_instance_count`                   |
| 函数          | `PascalCase`(getter/setter 一律 `snake_case`: getter 取属性名词, setter 加 `set_` 前缀; 谓词 `is_x`/`has_x`) | `InvariantHold` (`user`/`set_user`)  |
| 枚举          | `UPPER_SNAKE_CASE`                                                                                           | `TYPE_INFO`                          |
| 常量          | `kPascalCase`                                                                                                | `kStringMinCapacity`                 |
| 导出为 C 符号 | `snake_case`                                                                                                 | `string_create` / `vector_push_back` |

补充约定:

- C 接口类型采用不透明句柄模式:头文件中只写前置声明与句柄 typedef (如 `struct String;` +
  `typedef struct String* string_t;`),完整结构体定义私有于 .cpp 文件。注意 C 语言中标签名前必须保留 `struct` 关键字,否则无法通过
  C 编译。
- 文件内 static 辅助函数不导出,按 `PascalCase` 命名 (如 `StringGrow`、`VectorSelfOffset`)。

## Include 顺序

`.cpp` 对应的自身头文件必须是第一个 include; 其后按 **项目头文件 → 标准库头文件 → 第三方头文件 (如 gtest)** 分组,
组内按字母序。项目头在前能让"项目头文件缺少自给依赖"的问题在编译本仓库时即暴露,
而不是被外部使用方先包含标准头的巧合掩盖。

平台头文件 (POSIX 系统头, 如 `<unistd.h>`、`<fcntl.h>`、`<arpa/inet.h>`、`<sys/...>`) 单独一组, 排在标准库头文件之后、
第三方头文件之前, 即 **项目头文件 → 标准库头文件 → 平台头文件 → 第三方头文件**。平台头只允许出现在 `.cpp` 里,
公开头文件不能包含它们。

注: `.clang-format` 目前没有平台头的分类, clang-format 会把平台头并回标准库那一组。在配置补上分类之前, 这条规则
不由工具强制, 现有代码保持 clang-format 的排序。

## 格式

- `if` / `for` / `while` **必须**使用大括号 `{}`,即使循环体只有一条语句。
- 其余格式化 (clang-format)以仓库既有代码为准。

## 注释风格

参考如下：

```c++

/**
 * @brief JDBC 风格的数据库连接抽象接口
 * @note 连接负责管理事务边界、自动提交、只读状态与超时配置, 并作为 Statement / PreparedStatement / DatabaseMetaData
 * 的工厂。
 * @note 线程安全: 各驱动实现内部通过互斥锁串行化底层访问, 但建议调用方对同一连接仍按单线程使用, 以获得可预期的行为。
 * @note 生命周期: Close() 之后调用任何方法都会抛出 SQLNonTransientConnectionException; 连接由 shared_ptr 管理,
 * 底层驱动负责在析构时释放原生数据库句柄。
 */
class Connection {
public:
    /**
     * @brief 虚析构函数
     * @note 子类析构时必须释放底层数据库句柄; 调用方应优先使用 Close() 显式关闭连接, 而不是依赖析构。
     */
    virtual ~Connection() = default;

    /**
     * @brief 创建一个执行静态 SQL 的语句对象
     * @return 新创建的 Statement 对象
     * @note 语句对象持有连接的强引用, 保证连接在语句使用期间存活; 语句使用完毕后应调用 Close() 释放。
     */
    virtual StatementPtr CreateStatement() = 0;

    /**
     * @brief 预编译一条 SQL 语句
     * @param sql 待预编译的 SQL, 参数使用 '?' 占位符
     * @return 新创建的 PreparedStatement 对象
     * @note SQL 在 prepare 阶段被解析/编译, 语法错误会抛出 SQLSyntaxErrorException; 占位符个数由 parameter_count()
     * 返回, 参数索引从 1 开始。
     */
    virtual PreparedStatementPtr PrepareStatement(std::string_view sql) = 0;

    /**
     * @brief 提交当前事务
     * @note 仅在 autoCommit=false 且存在未提交事务时生效, 否则为空操作; 提交后下一条语句执行时会隐式开启新事务。
     */
    virtual void Commit() = 0;

    /**
     * @brief 回滚当前事务
     * @note 仅在 autoCommit=false 且存在未提交事务时生效, 否则为空操作; 回滚会丢弃事务期间的所有未提交修改。
     */
    virtual void Rollback() = 0;

    /**
     * @brief 关闭连接
     * @note 存在未提交事务时自动回滚; 重复调用 Close() 是幂等安全的。
     * @note 对连接池借出的连接, Close 表示"归还"而非物理关闭——池化连接的物理生命周期由连接池独占管理,
     * 物理关闭只发生在池关闭或失效剔除等池内路径上; 直接创建的裸驱动连接则是真正的关闭。
     */
    virtual void Close() = 0;

    /**
     * @brief 独占使用期结束通知
     * @return 无
     * @note 表达"当前持有方对连接的独占借用结束"。默认实现为空操作——直接持有裸驱动连接时,
     * 连接的生命周期仍归创建方管理; 池化实现覆写此方法以立即归还连接池并将当前视角作废
     * (之后经由该连接的调用抛出异常), 使归还时机与引用计数解耦。
     */
    virtual void EndUse() {}

    /**
     * @brief 查询连接是否已关闭
     * @return true 表示连接已关闭, 后续操作将抛出异常
     */
    [[nodiscard]] virtual bool is_closed() const = 0;

    /**
     * @brief 设置自动提交模式
     * @param auto_commit true 表示每条语句执行后自动提交, false 表示由 Commit() / Rollback() 手动控制事务
     * @note 默认值为 true; 从 false 切回 true 时, 若存在未提交事务会先提交。
     */
    virtual void set_auto_commit(bool auto_commit) = 0;

    /**
     * @brief 查询当前自动提交模式
     * @return true 表示自动提交, false 表示手动提交
     */
    [[nodiscard]] virtual bool auto_commit() const = 0;

    /**
     * @brief 设置事务隔离级别
     * @param isolation 目标隔离级别, 取值见 TransactionIsolation 枚举
     * @note 具体驱动可能不支持所有级别, 实现可以选择忽略或映射为最接近的级别, 因此设置后应以 transaction_isolation()
     * 的结果为准。
     */
    virtual void set_transaction_isolation(TransactionIsolation isolation) = 0;

    /**
     * @brief 查询当前事务隔离级别
     * @return 当前生效的隔离级别
     */
    [[nodiscard]] virtual TransactionIsolation transaction_isolation() const = 0;

    /**
     * @brief 设置连接为只读
     * @param read_only true 时拒绝执行 INSERT / UPDATE / DELETE 等写语句
     * @note 只读状态下执行写语句会抛出 SQLException; SELECT 查询不受影响。
     */
    virtual void set_read_only(bool read_only) = 0;

    /**
     * @brief 查询连接是否只读
     * @return true 表示只读
     */
    [[nodiscard]] virtual bool is_read_only() const = 0;

    /**
     * @brief 设置语句超时时间
     * @param seconds 超时秒数, 0 表示不限制
     * @note 由各驱动映射到底层机制, 例如 SQLite 的 busy_timeout、MySQL 客户端的读写超时; 之后创建的语句对象会继承该值。
     */
    virtual void set_query_timeout(int seconds) = 0;

    /**
     * @brief 查询语句超时时间
     * @return 超时秒数, 0 表示不限制
     */
    [[nodiscard]] virtual int query_timeout() const = 0;

    /**
     * @brief 获取数据库元数据
     * @return DatabaseMetaData 对象, 用于查询表、列、主键等结构信息
     * @note 连接关闭后该对象不可再使用。
     */
    virtual DatabaseMetaDataPtr metadata() = 0;

    /**
     * @brief 获取创建连接时使用的 URL
     * @return CDBC URL 字符串, 例如 cdbc:mysql://127.0.0.1:3306/db
     */
    [[nodiscard]] virtual std::string url() const = 0;

    /**
     * @brief 获取最近一次 INSERT 自动生成的主键值
     * @return 生成键; 无生成键或驱动不支持时返回空 optional
     * @note 供 ORM 主键回填使用: SQLite 基于 last_insert_rowid, MySQL 基于 mysql_insert_id; 内存驱动不支持, 返回空。
     */
    virtual std::optional<Value> last_generated_key() { return std::nullopt; }

    /**
     * @brief 探测连接是否仍可用
     * @return true 表示连接可用; false 表示已失效(实现应同时内部标记为关闭)
     * @note 由连接池在闲置超过 max_idle_time 后调用, 用于剔除被服务端 wait_timeout / 网络中断杀死的闲置连接。
     * 默认实现不探测(视为可用), 各驱动按自身机制覆盖: MySQL 用 mysql_ping, 其他驱动可用 "SELECT 1" 等。
     */
    virtual bool is_valid() { return true; }

    /**
     * @brief 获取连接的用户名
     * @return 登录用户名, 未指定时为空字符串
     */
    [[nodiscard]] virtual std::string user_name() const = 0;

private:
    int member_; // xxxx
};

```
