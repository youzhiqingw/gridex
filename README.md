# Gridex

<p align="right">
  <a href="./README_EN.md">English</a> | <b>中文</b>
</p>

一款支持 macOS 和 Windows 的原生数据库 IDE，内置 AI 聊天功能。可从单一应用连接 PostgreSQL、MySQL、SQLite、Redis、MongoDB 和 SQL Server。

![macOS 14+](https://img.shields.io/badge/macOS-14%2B-blue)
![Windows 10+](https://img.shields.io/badge/Windows-10%2B-0078D4)
![Swift 5.10](https://img.shields.io/badge/Swift-5.10-orange)
![License](https://img.shields.io/badge/license-Apache%202.0-blue)

<p align="center">
  <img src="assets/screenshot.jpg" alt="Gridex 截图" width="100%">
</p>

## 功能特性

### 数据库支持

| 数据库 | 驱动 | 亮点 |
|----------|--------|------------|
| **PostgreSQL** | [PostgresNIO](https://github.com/vapor/postgres-nio) | 参数化查询、SSL/TLS、序列、完整结构检查 |
| **MySQL** | [MySQLNIO](https://github.com/vapor/mysql-nio) | 字符集处理、参数化查询、SSL |
| **SQLite** | 系统 `libsqlite3` | 文件型、WAL 模式、零配置 |
| **Redis** | [RediStack](https://github.com/swift-server/RediStack) | 键浏览器、SCAN 过滤、服务器 INFO 仪表板、慢日志查看器 |
| **MongoDB** | [MongoKitten](https://github.com/orlandos-nl/MongoKitten) | 文档编辑器、NDJSON 导出/恢复、聚合 |
| **SQL Server** | [CosmoSQLClient](https://github.com/vkuttyp/CosmoSQLClient-Swift) | TDS 7.4 协议、原生 BACKUP DATABASE、存储过程 |

### AI 聊天

内置 AI 助手，理解你的数据库结构并为你编写 SQL。

- **Anthropic Claude** — 流式响应，直连 API
- **OpenAI GPT** — 完整 API 支持
- **Google Gemini** — Flash 模型支持
- **Ollama** — 本地大模型，无需 API 密钥

所有请求直接从你的机器发送到提供商。Gridex 从不代理提示词。API 密钥存储在 macOS 钥匙串中。

### 数据网格

- 行内单元格编辑，支持类型感知解析
- 排序、过滤和分页大型数据集
- 添加/删除行，支持待更改跟踪和提交工作流
- 列调整大小、多列排序
- 复制行、导出为 CSV/JSON/SQL

### 查询编辑器

- 多标签页，支持按连接分组的 Chrome 风格标签栏
- 语法高亮（关键字、字符串、数字、注释、函数）
- 通过 SwiftData 持久化的完整查询历史
- 执行选中内容、执行全部
- Redis CLI 模式

### 结构工具

- 表结构查看器（列、索引、外键、约束）
- ER 图表，支持自动布局、缩放、平移和 FK 关系线
- 函数检查器，支持源代码和参数签名
- 存储过程支持（MSSQL）

### SSH 隧道

- 密码和私钥认证
- 通过跳板机进行本地端口转发
- 由 `SSHTunnelService` actor 管理（线程安全，async/await）

### 导入与导出

- **导出**：CSV、JSON、SQL（INSERT 语句）、SQL（完整 DDL，含序列/索引）、结构 DDL
- **导入**：CSV（支持列映射）、SQL 转储

### 备份与恢复

| 数据库 | 备份方法 |
|----------|---------------|
| PostgreSQL | pg_dump（自定义、SQL、tar）/ pg_restore |
| MySQL | mysqldump / mysql CLI |
| SQLite | 文件复制 |
| MongoDB | NDJSON（每行一个文档） |
| Redis | 通过 SCAN 的 JSON 快照 |
| SQL Server | 原生 `BACKUP DATABASE` |

支持选择性表备份、压缩选项和进度回调。

### Redis 管理

- 基于 SCAN 的虚拟 "Keys" 表浏览
- 键详情视图 — 编辑哈希字段、列表项、集合/有序集合成员
- 基于模式的过滤栏（glob 语法：`user:*`、`cache:?`）
- 带自动刷新的服务器 INFO 仪表板
- 慢日志查看器、刷新数据库、键重命名/复制、TTL 管理

### MongoDB 文档编辑器

- 支持语法感知的 NDJSON 文档编辑器
- 文档插入/更新/删除
- 聚合管道支持

### 其他

- 多窗口支持（Cmd+N）
- macOS 钥匙串存储所有凭证
- 原生深色模式
- 连接保存/加载，支持 SSH 隧道配置和 SSL/TLS 选项

## 系统要求

### macOS

- macOS 14.0 (Sonoma) 或更高版本
- Swift 5.10+ / Xcode 15+

### Windows

- Windows 10 或更高版本（64位）
- Visual Studio 2022+、.NET 8 SDK、vcpkg

## 构建与运行

### macOS

```bash
git clone https://github.com/gridex/gridex.git
cd gridex

# 调试（临时签名，快速本地测试）
swift build
.build/debug/Gridex

# 或构建 .app 包
./scripts/build-app.sh
open dist/Gridex.app
```

### 发布

```bash
# Apple Silicon
./scripts/release.sh
# → dist/Gridex-<version>-arm64.dmg

# Intel
ARCH=x86_64 ./scripts/release.sh
# → dist/Gridex-<version>-x86_64.dmg

# 双架构
./scripts/release-all.sh
```

发布流程：`swift build` → `.app` 包 → 代码签名 → 公证 → 装订 → DMG → 签名 DMG → 公证 DMG。

**发布环境变量：**

| 变量 | 描述 |
|----------|-------------|
| `ARCH` | `arm64` 或 `x86_64`（默认：主机） |
| `SIGN_IDENTITY` | Developer ID 证书 SHA-1（或在 `.env` 中设置） |
| `NOTARY_PROFILE` | `notarytool` 钥匙串配置文件（默认：`gridex-notarize`） |
| `NOTARIZE` | 设置为 `0` 跳过公证 |

## 架构

Clean Architecture，5 层结构。依赖指向内部 — Presentation → Services → Data → Domain → Core。

```
gridex/
├── macos/                    macOS 应用（Swift、AppKit）
│   ├── App/                  生命周期、DI 容器、AppState、常量
│   ├── Core/                 协议、模型、枚举、错误 — 零依赖
│   │   ├── Protocols/        DatabaseAdapter、LLMService、SchemaInspectable
│   │   ├── Models/           RowValue、ConnectionConfig、QueryResult 等
│   │   └── Enums/            DatabaseType、GridexError
│   ├── Domain/               用例、仓库协议
│   ├── Data/                 适配器、SwiftData 持久化、钥匙串
│   │   ├── Adapters/         SQLite、PostgreSQL、MySQL、MongoDB、Redis、MSSQL
│   │   ├── Persistence/      SwiftData 模型（连接、查询历史）
│   │   └── Keychain/         macOS 钥匙串服务
│   ├── Services/             横切关注点
│   │   ├── QueryEngine/      QueryEngine actor、ConnectionManager、QueryBuilder
│   │   ├── AI/               Anthropic、OpenAI、Ollama、Gemini 提供商
│   │   ├── SSH/              SSHTunnelService（NIOSSH 端口转发）
│   │   └── Export/           ExportService、BackupService
│   └── Presentation/         AppKit 视图、SwiftUI 设置、ViewModels
│       ├── Views/            18 个视图组（DataGrid、QueryEditor、AIChat 等）
│       └── Windows/          macOS 窗口管理
├── windows/                  Windows 应用（C++、WinUI 3）
├── scripts/                  构建和发布自动化
└── Package.swift             SPM 清单
```

### 关键协议

- **`DatabaseAdapter`** — 约 50 个方法，涵盖连接、查询、结构、CRUD、事务、分页。所有 6 个数据库适配器都遵循此协议。
- **`LLMService`** — 通过 `AsyncThrowingStream` 流式传输 AI 响应。所有 4 个 AI 提供商都遵循此协议。
- **`SchemaInspectable`** — 完整结构快照（表、视图、索引、约束）。

### 并发

- `actor` 用于线程安全服务：`QueryEngine`、`ConnectionManager`、`SSHTunnelService`、`BackupService`、`SchemaInspectorService`
- 全程使用 `async/await` — 无完成处理程序
- 所有数据模型都有 `Sendable` 约束

### 依赖注入

`DependencyContainer`（单例）管理服务创建。SwiftData `ModelContainer` 跨窗口共享。服务通过 SwiftUI 环境注入。

## 依赖

| 包 | 版本 | 用途 |
|---------|---------|---------|
| [postgres-nio](https://github.com/vapor/postgres-nio) | 1.21.0+ | PostgreSQL 驱动 |
| [mysql-nio](https://github.com/vapor/mysql-nio) | 1.7.0+ | MySQL 驱动 |
| [swift-nio-ssl](https://github.com/apple/swift-nio-ssl) | 2.27.0+ | NIO 连接的 TLS |
| [swift-nio-ssh](https://github.com/apple/swift-nio-ssh) | 0.8.0+ | SSH 隧道支持 |
| [RediStack](https://github.com/swift-server/RediStack) | 1.6.0+ | Redis 驱动 |
| [MongoKitten](https://github.com/orlandos-nl/MongoKitten) | 7.9.0+ | MongoDB 驱动（纯 Swift） |
| [CosmoSQLClient-Swift](https://github.com/vkuttyp/CosmoSQLClient-Swift) | main | 通过 TDS 7.4 连接 MSSQL（无 FreeTDS） |

系统库：`libsqlite3`（构建时链接）。

## 贡献

欢迎贡献。请先开 issue 讨论你想更改的内容。

请参阅 [CONTRIBUTING.md](CONTRIBUTING.md) 了解指南和 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) 了解社区标准。

## 许可证

根据 [Apache License, Version 2.0](LICENSE) 授权。

版权所有 © 2026 Thinh Nguyen。

你可以使用、修改和分发本软件 — 包括商业或闭源产品 — 前提是保留版权声明和 NOTICE 文件。有关完整条款，请参阅 LICENSE。
