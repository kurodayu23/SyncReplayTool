# 收尾清理清单

> 打包/交付后执行 `forge_gate(action="clean")`（默认只预览），确认后再带 confirm 真正删除。
> 一行一个相对路径或通配符（* 不跨目录，** 跨目录）；# 开头是注释。
> 只删这里列出的东西 —— 清单里没有的一律不动。

# 子代理与主代理留下的临时探测脚本、日志、样例输出
tmp/**
scratch/**
*.bak-*
*.tmp
probe-*.ps1
probe-*.mjs
spill/**
logs/**
coverage/**
dist/**/*.map
