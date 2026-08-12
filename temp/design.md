# quickjs-go

# 用法

```
1. 跑 JS 脚本

   1.1 只执行
       RunFile()
       → error

   1.2 执行并取得 Eval result
       RunFile()
       → JSValue


2. 传入变量，让 JS 修改

   GoValue/GoStruct
       ↓
   JSValue/JSClass
       ↓
   script / function 修改
       ↓
   GoValue/GoStruct

3. 传入函数，让 JS 调用

   GoFunction
       ↓
   JSFunction
       ↓
   script / function 调用
       ↓
   GoFunction 执行

```

# API

- VM 是对外的最高层封装，用户直接使用
- 底层抽象仍然是 JSContext 和 JSRuntime

```go
type VM struct {
    // C 层核心
    rt      *C.JSRuntime      // 1 个（堆内存、GC、全局中断器）
    
    // 上下文池（逻辑上多个，物理上串行使用）
    ctxPool chan *C.JSContext // 容量可配置（默认 4）
    
    // 并发控制
    mu      sync.Mutex        // 保护 rt 和 ctxPool 的互斥操作
    
    // 安全阀
    interruptMs int64         // 执行超时阈值
}
```