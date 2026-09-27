# CLAUDE.md

本仓库的协作约定统一写在 [AGENTS.md](./AGENTS.md)，请以它为准。

**首要一条：了解代码优先用 `codegraph`，不要一上来就全仓库 grep/read。**
常用：`codegraph explore "<关键词>"`、`codegraph query <符号>`、
`codegraph callers|callees|impact <符号>`、`codegraph context "<任务>"`、`codegraph sync`。
索引已配置为只含本工程代码（`codegraph.json` 排除 `src/third_party/`）。
