# tight Lite Mode 设计文档索引

本文档集针对 `tight/` 可靠 UDP 协议栈的 **lite mode（客户端精简模式）** 实现，
从需求、架构、API、安全、内存优化五个维度进行完整总结。

| 文档 | 内容 |
|---|---|
| [01_requirements.md](01_requirements.md) | 需求分析：目标场景、功能性/非功能性需求、约束与验收指标 |
| [02_architecture.md](02_architecture.md) | 架构设计：分层模型、线程模型、可靠传输机制、线格式 |
| [03_api.md](03_api.md) | API 设计：公共接口、配置项、lite mode 开关语义与容量钳制 |
| [04_security.md](04_security.md) | 安全设计：密钥交换、AEAD 加密、完整性校验、防御性设计 |
| [05_memory_optimization.md](05_memory_optimization.md) | 内存优化思路与业务逻辑方案：已有优化、优化路线、业务侧配合方案 |

代码位置约定：`tight.hpp` = `tight/include/tight/tight.hpp`，`transport.cpp` = `tight/src/core/transport.cpp`，以此类推。
