# 05-内置对象 (Standard Built-in Objects)

参考规范：**ES2021 Chapters 18 - 28**

QppJS 引擎需要在 C++ 层提供这些基础的内置对象及其所有的原型方法。这通常占引擎开发工作量的很大一部分。

## 1. 全局对象 (The Global Object)

- **值属性:** `NaN`, `Infinity`, `undefined`, `globalThis`。
- **函数属性:** `eval`, `parseInt`, `parseFloat`, `isNaN`, `isFinite`, `encodeURI` 等。
- **构造器属性:** 指向 `Object`, `Function`, `Array` 等。

## 2. 基础对象 (Fundamental Objects)

- **Object:** 核心基类。提供 `Object.create()`, `Object.keys()`, `Object.defineProperty()` 等静态方法，以及 `Object.prototype.toString()` 等实例方法。
- **Function:** 所有函数的基类。提供 `Function.prototype.call()`, `apply()`, `bind()`。
- **Boolean / Symbol / Error:** 原型对象。

## 3. 数字与日期 (Numbers and Dates)

- **Number:** IEEE 754 包装对象。
- **BigInt:** 任意精度大整数。
- **Math:** 提供数学常量和计算方法（`Math.sin`, `Math.max`）。
- **Date:** 时间和日期处理。

## 4. 文本处理 (Text Processing)

- **String:** 字符串包装对象，包含极大量的方法（`indexOf`, `slice`, `replace`, `match`）。
- **RegExp:** 正则表达式对象（实现通常依赖底层的正则引擎库，例如 PCRE 或自定义的 NFA/DFA 引擎）。

## 5. 索引集合 (Indexed Collections)

- **Array:** 数组对象，JS 引擎中需要对紧凑数组（Dense Array）和稀疏数组（Sparse Array）做底层优化。
- **TypedArray:** `Int32Array`, `Float64Array` 等，直接映射到底层的一段连续内存（ArrayBuffer）。

## 6. 键控集合 (Keyed Collections)

- **Map / Set:** 能够保持插入顺序的哈希集合。
- **WeakMap / WeakSet:** 弱引用集合，依赖引擎底层的 GC 系统支持。

## 7. 控制流抽象 (Control Abstraction Objects)

- **Promise:** 异步编程核心，引擎需要实现微任务队列（Microtask Queue/Job Queue）机制。
- **Generator / AsyncFunction:** 协程机制抽象。
- **Iterator / AsyncIterator:** 迭代器协议（`next()`, `return()`, `throw()`）。

## 8. 结构化数据 (Structured Data)

- **ArrayBuffer:** 固定长度的原始二进制数据缓冲区。
- **SharedArrayBuffer:** 可共享的二进制缓冲区（用于多线程 WebWorker 场景）。
- **DataView:** 提供底层二进制数据的读写视图（getInt32, setFloat64 等）。
- **Atomics:** 原子操作对象，用于多线程同步。

## 9. 反射 (Reflection)

- **Reflect:** 提供与对象内部方法同名的方法（`Reflect.get`, `Reflect.set`），用于函数式风格的元编程。
- **Proxy:** 对象代理，拦截所有内部方法（13 种陷阱，如 `get`, `set`, `has`, `apply`），是 QppJS 实现响应式系统和测试框架的关键。

## 10. 其他 (Other)

- **JSON:** `JSON.parse()`, `JSON.stringify()` 序列化与反序列化。
- **console (Host-defined):** 虽然不是 ECMA 标准的一部分，但所有宿主环境（浏览器/Node.js）都实现了它，用于调试输出。
